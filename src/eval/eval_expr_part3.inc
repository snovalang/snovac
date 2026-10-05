static SnEvalValue eval_call(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e) {
    if (e->lhs && e->lhs->kind == SN_EXPR_IDENT && sn_eval_probe_name(e->lhs->text)) {
        return v_int(sn_eval_probe_value(e->lhs->text));
    }
    SnEvalValue out;
    if (try_intrinsic(in, env, e, &out)) {
        return out;
    }

    const SnExpr *callee = e->lhs;
    if (!callee) {
        sn_rt_error(in, SNOVA_NOT_CALLABLE, e->span,
                 "call target is not an expression");
        return v_unit();
    }

    if (callee->kind == SN_EXPR_IDENT && callee->text) {
        if (strcmp(callee->text, "read_bytes") == 0) {
            if (e->args.len != 1) {
                sn_rt_error(in, SNOVA_TYPE_ERROR, e->span, "read_bytes expects a path");
                return v_unit();
            }
            SnEvalValue path = sn_eval_expr(in, env, (const SnExpr *)e->args.items[0]);
            if (in->failed) {
                return v_unit();
            }
            return eval_read_bytes(in, path, e->span);
        }
        if (strcmp(callee->text, "write_bytes") == 0) {
            if (e->args.len != 2) {
                sn_rt_error(in, SNOVA_TYPE_ERROR, e->span, "write_bytes expects a path and an int array");
                return v_unit();
            }
            SnEvalValue path = sn_eval_expr(in, env, (const SnExpr *)e->args.items[0]);
            SnEvalValue data = sn_eval_expr(in, env, (const SnExpr *)e->args.items[1]);
            if (in->failed) {
                return v_unit();
            }
            return eval_write_bytes(in, path, data, e->span);
        }
        if (strcmp(callee->text, "type") == 0) {
            /* type(T) expression returns type name as string */
            if (e->args.len > 0) {
                const SnExpr *arg0 = (const SnExpr *)e->args.items[0];
                if (arg0->text) {
                    return v_str(arg0->text);
                }
                SnEvalValue av = sn_eval_expr(in, env, arg0);
                if (av.kind == V_STRING) return av;
                return v_str(sn_eval_to_string(in, av, e->span));
            }
            return v_str("type");
        }
        SnEvalValue *local = sn_eval_env_lookup(env, callee->text);
        if (local && local->kind == V_LAMBDA) {
            return sn_eval_call_lambda(in, local->as.lam, (SnList *)&e->args, env,
                               e->span);
        }
        const SnDecl *fn = sn_eval_find_top(in, callee->text, SN_DECL_FUNC);
        if (fn) {
            return sn_eval_call_function(in, fn, (SnList *)&e->args, env, NULL, e->span);
        }
        if (strcmp(callee->text, "Array") == 0) {
            SnEvalValue v;
            v.kind = V_ARRAY;
            v.as.arr = (ArrayVal *)sn_arena_calloc(in->arena, sizeof(ArrayVal));
            return v;
        }
        const SnDecl *cls = sn_eval_find_type(in, callee->text);
        if (cls) {
            SnEvalValue v;
            v.kind = V_OBJECT;
            v.as.o = sn_eval_instantiate(in, cls, (SnList *)&e->args, (SnList *)&e->field_names,
                                env, e->span);
            return v;
        }
        if (sn_eval_is_variant_ctor(in, callee->text)) {
            return make_variant_from_args(in, env, callee->text, &e->args);
        }
        sn_rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "unknown function `%s`",
                 callee->text);
        return v_unit();
    }

    /* Method call on a receiver. */
    if (callee->kind == SN_EXPR_MEMBER && callee->lhs) {
        /* Static method: `Foo.bar()` where Foo is a declared type, not a
         * local binding. This must be checked before evaluating `callee->lhs`
         * as a value expression below: a bare type name is not itself an
         * executable value, so sn_eval_expr would raise "undefined name" for
         * any static call before this function ever got a chance to
         * recognize it as one. */
        if (callee->lhs->kind == SN_EXPR_IDENT && callee->lhs->text &&
            !sn_eval_env_lookup(env, callee->lhs->text)) {
            if (strcmp(callee->lhs->text, "Array") == 0) {
                SnEvalValue v;
                v.kind = V_ARRAY;
                v.as.arr = (ArrayVal *)sn_arena_calloc(in->arena, sizeof(ArrayVal));
                return v;
            }
            const SnDecl *cls = sn_eval_find_type(in, callee->lhs->text);
            if (cls) {
                const SnDecl *m = sn_eval_find_overload(in, env, cls, callee->text, &e->args);
                if (m) {
                    if (strcmp(m->name, "new") == 0) {
                        /* Canonical constructor: public static method new(...) { this.x = x } */
                        SnEvalObject *obj = sn_eval_instantiate(in, cls, NULL, NULL, env, e->span);
                        SnEvalValue res = sn_eval_call_method(in, obj, m, (SnList *)&e->args, env, e->span);
                        if (res.kind == V_OBJECT) {
                            return res;
                        }
                        SnEvalValue out_obj;
                        out_obj.kind = V_OBJECT;
                        out_obj.as.o = obj;
                        return out_obj;
                    }
                    return sn_eval_call_function(in, m, (SnList *)&e->args, env, NULL,
                                         e->span);
                }
                if (strcmp(callee->text, "new") == 0) {
                    SnEvalValue v;
                    v.kind = V_OBJECT;
                    v.as.o = sn_eval_instantiate(in, cls, (SnList *)&e->args, (SnList *)&e->field_names,
                                env, e->span);
                    return v;
                }
                sn_rt_error(in, SNOVA_UNDEFINED_NAME, e->span,
                         "unknown method `%s`",
                         callee->text ? callee->text : "?");
                return v_unit();
            }
        }



        SnEvalValue recv = sn_rt_autoderef(in, sn_eval_expr(in, env, callee->lhs), callee->span);
        if (recv.kind == V_STRING && callee->text &&
            try_string_method(in, env, recv, e, &out)) {
            return out;
        }
        if (recv.kind == V_ARRAY && callee->text &&
            try_array_method(in, env, recv, e, &out)) {
            return out;
        }
        if (recv.kind == V_VARIANT && callee->text &&
            try_variant_method(in, env, recv, e, &out)) {
            return out;
        }
        if (recv.kind == V_OBJECT) {
            const SnDecl *m = sn_eval_find_overload(in, env, recv.as.o->cls, callee->text, &e->args);
            if (m) {
                return sn_eval_call_method(in, recv.as.o, m, (SnList *)&e->args, env,
                                   e->span);
            }
            /* A field that holds a lambda value is callable through the
             * member: `handler.onHit(entry)`. */
            SnEvalValue *f = sn_eval_object_field(recv.as.o, callee->text);
            if (f && f->kind == V_LAMBDA) {
                return sn_eval_call_lambda(in, f->as.lam, (SnList *)&e->args, env,
                                   e->span);
            }
        }
        sn_rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "unknown method `%s`",
                 callee->text ? callee->text : "?");
        return v_unit();
    }

    if (callee->kind == SN_EXPR_LAMBDA) {
        SnEvalValue fn = sn_eval_expr(in, env, callee);
        if (in->failed) {
            return v_unit();
        }
        if (fn.kind == V_LAMBDA && fn.as.lam) {
            return sn_eval_call_lambda(in, fn.as.lam, (SnList *)&e->args, env, e->span);
        }
    }

    sn_rt_error(in, SNOVA_NOT_CALLABLE, e->span, "expression is not callable");
    return v_unit();
}

static SnEvalValue eval_unary(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e) {
    if (e->op == SN_TOK_AMP) {
        return sn_rt_take_ref(in, env, e);
    }
    if (e->op == SN_TOK_STAR) {
        return sn_rt_deref(in, sn_eval_expr(in, env, e->lhs), e->span);
    }
    if (e->op == SN_TOK_BANG) {
        return v_bool(!sn_eval_truthy(in, sn_eval_expr(in, env, e->lhs), e->span));
    }
    if (e->op == SN_TOK_MINUS) {
        SnEvalValue v = sn_eval_expr(in, env, e->lhs);
        if (v.kind == V_DOUBLE) return v_double(-v.as.d);
        return v_int(-sn_eval_as_int(in, v, e->span));
    }
    if (e->op == SN_TOK_PLUS) {
        return sn_eval_expr(in, env, e->lhs);
    }
    sn_rt_error(in, SNOVA_UNSUPPORTED, e->span, "unary operator not supported yet");
    return v_unit();
}

static SnEvalValue eval_assign(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e) {
    SnEvalValue val = sn_eval_expr(in, env, e->rhs);
    SnEvalValue *slot = NULL;
    if (e->lhs->kind == SN_EXPR_IDENT) {
        slot = sn_eval_env_lookup(env, e->lhs->text);
    } else if (e->lhs->kind == SN_EXPR_MEMBER) {
        if (e->lhs->lhs && e->lhs->lhs->kind == SN_EXPR_IDENT && e->lhs->lhs->text &&
            !sn_eval_env_lookup(env, e->lhs->lhs->text)) {
            const SnDecl *cls = sn_eval_find_type(in, e->lhs->lhs->text);
            if (cls) {
                char key[512];
                snprintf(key, sizeof(key), "%s.%s", e->lhs->lhs->text, e->lhs->text);
                slot = sn_eval_env_lookup(in->globals, key);
                if (!slot) {
                    slot = sn_eval_env_define(in, in->globals, key, val);
                }
            }
        }
        if (!slot) {
            SnEvalValue recv = sn_eval_expr(in, env, e->lhs->lhs);
            if (recv.kind == V_OBJECT) {
                slot = sn_eval_object_field(recv.as.o, e->lhs->text);
                if (!slot && recv.as.o) {
                    SnEvalValue *new_slot = (SnEvalValue *)sn_arena_alloc(in->arena, sizeof(SnEvalValue));
                    *new_slot = v_unit();
                    sn_list_push(in->arena, &recv.as.o->names, (void *)e->lhs->text);
                    sn_list_push(in->arena, &recv.as.o->slots, new_slot);
                    slot = new_slot;
                }
            }
        }
    } else if (e->lhs->kind == SN_EXPR_INDEX) {
        if (!sn_rt_index_slot(in, env, e->lhs, &slot)) {
            return v_unit();
        }
    } else if (e->lhs->kind == SN_EXPR_UNARY && e->lhs->op == SN_TOK_STAR) {
        SnEvalValue recv = sn_eval_expr(in, env, e->lhs->lhs);
        if (recv.kind == V_REF && recv.as.ref && !recv.as.ref->is_null) {
            slot = recv.as.ref->slot;
        } else {
            sn_rt_error(in, SNOVA_TYPE_ERROR, e->span, "null pointer dereference");
            return v_unit();
        }
    }
    if (!slot) {
        sn_rt_error(in, SNOVA_UNDEFINED_NAME, e->span,
                 "cannot assign to this target");
        return v_unit();
    }
    if (e->op != SN_TOK_ASSIGN) {
        SnExpr tmp = *e;
        tmp.kind = SN_EXPR_BINARY;
        switch (e->op) {
        case SN_TOK_PLUS_EQ:  tmp.op = SN_TOK_PLUS;  break;
        case SN_TOK_MINUS_EQ: tmp.op = SN_TOK_MINUS; break;
        case SN_TOK_STAR_EQ:  tmp.op = SN_TOK_STAR;  break;
        case SN_TOK_SLASH_EQ: tmp.op = SN_TOK_SLASH; break;
        default: tmp.op = SN_TOK_PLUS; break;
        }
        val = eval_binary(in, env, &tmp);
    }
    *slot = val;
    return val;
}

