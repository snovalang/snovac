/* eval_expr.c — expression evaluation: coercions, binary operators, calls and
 * the intrinsic bridge. */
#include "eval_internal.h"

long long as_int(Interp *in, Value v, SnSpan span) {
    switch (v.kind) {
    case V_INT:    return v.as.i;
    case V_BOOL:   return v.as.b;
    case V_DOUBLE: return (long long)v.as.d;
    default:
        rt_error(in, SNOVA_TYPE_ERROR, span, "expected a number");
        return 0;
    }
}

int truthy(Interp *in, Value v, SnSpan span) {
    switch (v.kind) {
    case V_BOOL: return v.as.b;
    case V_INT:  return v.as.i != 0;
    default:
        rt_error(in, SNOVA_TYPE_ERROR, span, "expected a bool condition");
        return 0;
    }
}

Value eval_expr(Interp *in, Env *env, const SnExpr *e) {
    if (!e || in->failed) {
        return v_unit();
    }

    switch (e->kind) {
    case SN_EXPR_INT:
    case SN_EXPR_LONG:
        return v_int(strtoll(e->text, NULL, 0));
    case SN_EXPR_DOUBLE:
    case SN_EXPR_DECIMAL:
        return v_double(strtod(e->text, NULL));
    case SN_EXPR_BOOL:
        return v_bool(strcmp(e->text, "true") == 0);
    case SN_EXPR_STRING:
        return v_str_in_region(decode_string(in, env, e), in->current_region);
    case SN_EXPR_CHAR:
        return v_str_in_region(decode_string(in, env, e), in->current_region);
    case SN_EXPR_THIS: {
        Value *v = env_lookup(env, "this");
        return v ? *v : v_unit();
    }
    case SN_EXPR_IDENT: {
        Value *v = env_lookup(env, e->text);
        if (v) {
            if (v->kind == V_MOVED) {
                rt_error(in, SNOVA_USE_AFTER_MOVE, e->span,
                         "`%s` was moved and cannot be used before reinitialization",
                         e->text ? e->text : "<value>");
                return v_unit();
            }
            if (e->implicit_move) {
                Value moved = *v;
                v->kind = V_MOVED;
                v->as.i = 0;
                return moved;
            }
            return *v;
        }
        /* Payload-less variants read as bare identifiers: `None`, or a
         * user enum's empty variant. */
        if (e->text && is_variant_constructor(in, e->text)) {
            SnList empty = {0};
            return v_variant(in, e->text, empty);
        }
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "undefined name `%s`",
                 e->text);
        return v_unit();
    }
    case SN_EXPR_MEMBER: {
        /* Enum variant access: `State.Done` where `State` is a declared enum
         * type, not a local binding — mirrors the static-call check in
         * eval_call() for the analogous `Foo.bar()` case. Must run before
         * eval_expr(e->lhs) below: a bare type name is not itself an
         * executable value. */
        if (e->lhs->kind == SN_EXPR_IDENT && e->lhs->text && e->text &&
            !env_lookup(env, e->lhs->text)) {
            const SnDecl *en = find_top(in, e->lhs->text, SN_DECL_ENUM);
            if (en) {
                for (size_t i = 0; i < en->variants.len; i++) {
                    const SnDecl *v = (const SnDecl *)en->variants.items[i];
                    if (v->name && strcmp(v->name, e->text) == 0) {
                        SnList empty = {0};
                        return v_variant(in, v->name, empty);
                    }
                }
            }
            const SnDecl *cls = find_type(in, e->lhs->text);
            if (cls) {
                char key[512];
                snprintf(key, sizeof(key), "%s.%s", e->lhs->text, e->text);
                Value *slot = env_lookup(in->globals, key);
                if (slot) {
                    return *slot;
                }
                const SnDecl *f = find_member(cls, e->text);
                if (f) {
                    Value v = f->init ? eval_expr(in, env, f->init) : default_for(f->type);
                    env_define(in, in->globals, key, v);
                    return v;
                }
            }
            if (en) {
                rt_error(in, SNOVA_UNDEFINED_NAME, e->span,
                         "enum `%s` has no variant `%s`", e->lhs->text,
                         e->text);
                return v_unit();
            }
        }
        Value recv = eval_expr(in, env, e->lhs);
        if (recv.kind == V_OBJECT) {
            Value *f = object_field(recv.as.o, e->text);
            if (f) {
                return *f;
            }
        }
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "no field `%s`", e->text);
        return v_unit();
    }
    case SN_EXPR_CALL:
        return eval_call(in, env, e);
    case SN_EXPR_UNARY:
        return eval_unary(in, env, e);
    case SN_EXPR_MOVE: {
        if (!e->lhs || e->lhs->kind != SN_EXPR_IDENT || !e->lhs->text) {
            rt_error(in, SNOVA_UNSUPPORTED, e->span,
                     "`move` requires a local variable or parameter name");
            return v_unit();
        }
        Value *source = env_lookup(env, e->lhs->text);
        if (!source) {
            rt_error(in, SNOVA_UNDEFINED_NAME, e->span,
                     "undefined variable `%s`", e->lhs->text);
            return v_unit();
        }
        if (source->kind == V_MOVED) {
            rt_error(in, SNOVA_USE_AFTER_MOVE, e->span,
                     "`%s` was already moved", e->lhs->text);
            return v_unit();
        }
        Value moved = *source;
        source->kind = V_MOVED;
        source->as.i = 0;
        return moved;
    }
    case SN_EXPR_BINARY:
        return eval_binary(in, env, e);
    case SN_EXPR_ASSIGN:
        return eval_assign(in, env, e);
    case SN_EXPR_LAMBDA: {
        LambdaVal *lam = (LambdaVal *)sn_arena_calloc(in->arena,
                                                      sizeof(LambdaVal));
        lam->expr = e;
        /* Capture this frame by sharing its mutable slots, not by retaining
         * the frame that will own the closure binding. This avoids the
         * structural Env -> Lambda -> Env cycle while preserving mutation. */
        Env *capture = env_new(in, env ? env->parent : NULL);
        if (env) {
            capture->captured_region = env->owner_region;
            for (size_t i = 0; i < env->names.len; ++i) {
                sn_list_push(in->arena, &capture->names, env->names.items[i]);
                sn_list_push(in->arena, &capture->slots, env->slots.items[i]);
            }
        }
        lam->env = capture;
        lam->owner_region = in->current_region;
        Value v;
        v.kind = V_LAMBDA;
        v.as.lam = lam;
        return v;
    }
    case SN_EXPR_AWAIT:
        /* The smoke-path interpreter is synchronous: `await e` is `e`. */
        return eval_expr(in, env, e->lhs);
    case SN_EXPR_STRUCT_LIT: {
        if (!e->lhs || e->lhs->kind != SN_EXPR_IDENT || !e->lhs->text) {
            rt_error(in, SNOVA_UNSUPPORTED, e->span,
                     "struct literal target is not a plain type name");
            return v_unit();
        }
        const SnDecl *cls = find_type(in, e->lhs->text);
        if (!cls) {
            rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "unknown type `%s`",
                     e->lhs->text);
            return v_unit();
        }
        Object *o = (Object *)sn_arena_calloc(in->arena, sizeof(Object));
        o->cls = cls;
        o->owner_region = in->current_region;
        for (size_t i = 0; i < cls->members.len; i++) {
            const SnDecl *m = (const SnDecl *)cls->members.items[i];
            if (m->kind != SN_DECL_FIELD) {
                continue;
            }
            Value v = m->init ? eval_expr(in, env, m->init)
                              : default_for(m->type);
            for (size_t j = 0; j < e->field_names.len; j++) {
                const char *fname = (const char *)e->field_names.items[j];
                if (m->name && strcmp(fname, m->name) == 0) {
                    v = eval_expr(in, env, (const SnExpr *)e->args.items[j]);
                    break;
                }
            }
            Value *slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
            *slot = v;
            sn_list_push(in->arena, &o->names, (void *)m->name);
            sn_list_push(in->arena, &o->slots, slot);
        }
        Value v;
        v.kind = V_OBJECT;
        v.as.o = o;
        return v;
    }
    case SN_EXPR_MATCH: {
        EvalRegion *region = eval_region_begin(in);
        Value subject = eval_expr(in, env, e->lhs);
        if (in->failed) {
            eval_region_end(in, region);
            return v_unit();
        }
        Env *arm_env = NULL;
        const SnMatchArm *arm =
            match_select_arm(in, env, &e->arms, subject, &arm_env);
        if (!arm) {
            rt_error(in, SNOVA_TYPE_ERROR, e->span,
                     "no match arm matched value `%s`",
                     to_string(in, subject, e->span));
            eval_region_end(in, region);
            return v_unit();
        }
        Value result = v_unit();
        if (arm->value) {
            result = eval_expr(in, arm_env, arm->value);
        } else if (arm->body) {
            /* Block-bodied arm in expression position: run it for effect; a
             * `return` inside is delivered as the surrounding function's
             * return value by the caller's exec loop. */
            Flow f = exec_stmt(in, arm_env, arm->body);
            if (f == FLOW_RETURN && eval_value_is_heap_owned(in->ret))
                eval_value_mark_escaping(in, in->ret);
        }
        if (eval_value_is_heap_owned(result)) eval_value_mark_escaping(in, result);
        eval_region_end(in, region);
        return result;
    }
    case SN_EXPR_INDEX: {
        Value base = eval_expr(in, env, e->lhs);
        if (base.kind != V_ARRAY) {
            rt_error(in, SNOVA_TYPE_ERROR, e->span, "value is not indexable");
            return v_unit();
        }
        long long idx = as_int(in, eval_expr(in, env, e->rhs), e->span);
        if (idx < 0 || (size_t)idx >= base.as.arr->len) {
            rt_error(in, SNOVA_TYPE_ERROR, e->span,
                     "array index %lld out of bounds (len %zu)", idx,
                     base.as.arr->len);
            return v_unit();
        }
        return base.as.arr->items[idx];
    }
    case SN_EXPR_ARRAY: {
        ArrayVal *arr = array_new(in);
        for (size_t i = 0; i < e->args.len; i++) {
            Value v = eval_expr(in, env, (const SnExpr *)e->args.items[i]);
            if (in->failed || !array_push(in, arr, v, e->span)) return v_unit();
        }
        Value v;
        v.kind = V_ARRAY;
        v.as.arr = arr;
        return v;
    }
    default:
        rt_error(in, SNOVA_UNSUPPORTED, e->span,
                 "this expression form is not executable yet");
        return v_unit();
    }
}
