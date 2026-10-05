SnEvalValue sn_eval_expr(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e) {
    if (!e || in->failed) {
        return v_unit();
    }

    switch (e->kind) {
    case SN_EXPR_INT:
        return v_int(strtoll(e->text, NULL, 0));
    case SN_EXPR_LONG: {
        SnEvalValue lv = v_int(strtoll(e->text, NULL, 0));
        lv.iwidth = 64;
        return lv;
    }
    case SN_EXPR_DOUBLE:
    case SN_EXPR_DECIMAL:
        return v_double(strtod(e->text, NULL));
    case SN_EXPR_BOOL:
        return v_bool(strcmp(e->text, "true") == 0);
    case SN_EXPR_STRING:
        return v_str(sn_eval_decode_string(in, env, e));
    case SN_EXPR_CHAR: {
        const char *t = e->text ? e->text : "";
        if (t[0] == '\'' && t[1] && t[2] == '\'') {
            return v_char((unsigned char)t[1]);
        }
        if (t[0] == '\'') {
            return v_char((unsigned char)t[1]);
        }
        return v_char(t[0] ? (unsigned char)t[0] : 0);
    }
    case SN_EXPR_THIS: {
        SnEvalValue *v = sn_eval_env_lookup(env, "this");
        return v ? *v : v_unit();
    }
    case SN_EXPR_IDENT: {
        SnEvalValue *v = sn_eval_env_lookup(env, e->text);
        if (v) {
            return *v;
        }
        /* Payload-less variants read as bare identifiers: `None`, or a
         * user enum's empty variant. */
        if (e->text && sn_eval_is_variant_ctor(in, e->text)) {
            SnList empty = {0};
            return sn_eval_variant(in, e->text, empty);
        }
        sn_rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "undefined name `%s`",
                 e->text);
        return v_unit();
    }
    case SN_EXPR_MEMBER: {
        /* Enum variant access: `State.Done` where `State` is a declared enum
         * type, not a local binding — mirrors the static-call check in
         * eval_call() for the analogous `Foo.bar()` case. Must run before
         * sn_eval_expr(e->lhs) below: a bare type name is not itself an
         * executable value. */
        if (e->lhs->kind == SN_EXPR_IDENT && e->lhs->text && e->text &&
            !sn_eval_env_lookup(env, e->lhs->text)) {
            const SnDecl *en = sn_eval_find_top(in, e->lhs->text, SN_DECL_ENUM);
            if (en) {
                for (size_t i = 0; i < en->variants.len; i++) {
                    const SnDecl *v = (const SnDecl *)en->variants.items[i];
                    if (v->name && strcmp(v->name, e->text) == 0) {
                        SnList empty = {0};
                        return sn_eval_variant(in, v->name, empty);
                    }
                }
            }
            const SnDecl *cls = sn_eval_find_type(in, e->lhs->text);
            if (cls) {
                char key[512];
                snprintf(key, sizeof(key), "%s.%s", e->lhs->text, e->text);
                SnEvalValue *slot = sn_eval_env_lookup(in->globals, key);
                if (slot) {
                    return *slot;
                }
                const SnDecl *f = sn_eval_find_member(cls, e->text);
                if (f) {
                    SnEvalValue v = f->init ? sn_eval_expr(in, env, f->init) : sn_eval_default_for(f->type);
                    sn_eval_env_define(in, in->globals, key, v);
                    return v;
                }
            }
            if (en) {
                sn_rt_error(in, SNOVA_UNDEFINED_NAME, e->span,
                         "enum `%s` has no variant `%s`", e->lhs->text,
                         e->text);
                return v_unit();
            }
        }
        SnEvalValue recv = sn_eval_expr(in, env, e->lhs);
        if (recv.kind == V_OBJECT) {
            SnEvalValue *f = sn_eval_object_field(recv.as.o, e->text);
            if (f) {
                return *f;
            }
        }
        sn_rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "no field `%s`", e->text);
        return v_unit();
    }
    case SN_EXPR_CALL:
        return eval_call(in, env, e);
    case SN_EXPR_UNARY:
        return eval_unary(in, env, e);
    case SN_EXPR_BINARY:
        return eval_binary(in, env, e);
    case SN_EXPR_ASSIGN:
        return eval_assign(in, env, e);
    case SN_EXPR_LAMBDA: {
        SnEvalLambda *lam = (SnEvalLambda *)sn_arena_calloc(in->arena,
                                                      sizeof(SnEvalLambda));
        lam->expr = e;
        lam->env = env;
        SnEvalValue v;
        v.kind = V_LAMBDA;
        v.as.lam = lam;
        return v;
    }
    case SN_EXPR_AWAIT:
        /* The smoke-path interpreter is synchronous: `await e` is `e`. */
        return sn_eval_expr(in, env, e->lhs);
    case SN_EXPR_STRUCT_LIT: {
        if (!e->lhs || e->lhs->kind != SN_EXPR_IDENT || !e->lhs->text) {
            sn_rt_error(in, SNOVA_UNSUPPORTED, e->span,
                     "struct literal target is not a plain type name");
            return v_unit();
        }
        const SnDecl *cls = sn_eval_find_type(in, e->lhs->text);
        if (!cls) {
            sn_rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "unknown type `%s`",
                     e->lhs->text);
            return v_unit();
        }
        /* Named (`Point { x: 3 }`) and positional (`Point{3, 4}`) share the
         * constructor filler. A NULL field name is positional. */
        SnEvalValue v;
        v.kind = V_OBJECT;
        v.as.o = sn_eval_instantiate(in, cls, (SnList *)&e->args,
                                    (SnList *)&e->field_names, env, e->span);
        return v;
    }
    case SN_EXPR_MATCH: {
        SnEvalValue subject = sn_eval_expr(in, env, e->lhs);
        if (in->failed) {
            return v_unit();
        }
        SnEvalEnv *arm_env = NULL;
        const SnMatchArm *arm =
            sn_eval_match_arm(in, env, &e->arms, subject, &arm_env);
        if (!arm) {
            sn_rt_error(in, SNOVA_TYPE_ERROR, e->span,
                     "no match arm matched value `%s`",
                     sn_eval_to_string(in, subject, e->span));
            return v_unit();
        }
        if (arm->value) {
            return sn_eval_expr(in, arm_env, arm->value);
        }
        if (arm->body) {
            /* Block-bodied arm in expression position: run it for effect; a
             * `return` inside is delivered as the surrounding function's
             * return value by the caller's exec loop. */
            SnEvalFlow f = sn_eval_exec_stmt(in, arm_env, arm->body);
            (void)f;
        }
        return v_unit();
    }
    case SN_EXPR_NULL:
        return sn_rt_null(in);
    case SN_EXPR_INDEX:
        return sn_rt_eval_index(in, env, e);
    case SN_EXPR_ARRAY:
        return sn_rt_new_array(in, env, e);
    default:
        sn_rt_error(in, SNOVA_UNSUPPORTED, e->span,
                 "this expression form is not executable yet");
        return v_unit();
    }
}
