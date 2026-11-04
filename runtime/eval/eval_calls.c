#include "eval_internal.h"

/* Console.print / Console.printline — the only intrinsics wired up so far.
 * Returns 1 when the call was an intrinsic and `out` was set. */
static int try_intrinsic(Interp *in, Env *env, const SnExpr *call, Value *out) {
    const SnExpr *callee = call->lhs;
    if (!callee || callee->kind != SN_EXPR_MEMBER || !callee->lhs ||
        callee->lhs->kind != SN_EXPR_IDENT || !callee->lhs->text) {
        return 0;
    }
    if (strcmp(callee->lhs->text, "Console") != 0) {
        return 0;
    }

    int newline;
    if (strcmp(callee->text, "printline") == 0)  newline = 1;
    else if (strcmp(callee->text, "print") == 0) newline = 0;
    else if (strcmp(callee->text, "err") == 0)   newline = 2;
    else if (strcmp(callee->text, "warn") == 0)  newline = 2;
    else {
        rt_error(in, SNOVA_UNKNOWN_INTRINSIC, call->span,
                 "Console.%s is not implemented yet", callee->text);
        *out = v_unit();
        return 1;
    }

    const char *text = "";
    if (call->args.len > 0) {
        Value v = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
        text = to_string(in, v, call->span);
    }
    FILE *dst = (newline == 2) ? stderr : stdout;
    fputs(text, dst);
    if (newline) {
        fputc('\n', dst);
    }
    fflush(dst);
    *out = v_unit();
    return 1;
}

/* Builds a variant value from a constructor call's evaluated arguments. */
static Value make_variant_from_args(Interp *in, Env *env, const char *name,
                                    const SnList *args) {
    SnList payload = {0};
    for (size_t i = 0; i < args->len; i++) {
        Value v = eval_expr(in, env, (const SnExpr *)args->items[i]);
        Value *slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
        *slot = v;
        sn_list_push(in->arena, &payload, slot);
    }
    return v_variant(in, name, payload);
}

/* `Some` / `Ok` / `Err` are built-in variant constructors; any other
 * capitalized name is looked up among declared enums' variants so user enums
 * construct the same way. */
int is_variant_constructor(Interp *in, const char *name) {
    const char *vname = strrchr(name, '.') ? strrchr(name, '.') + 1 : name;
    if (strcmp(vname, "Some") == 0 || strcmp(vname, "Ok") == 0 ||
        strcmp(vname, "Err") == 0 || strcmp(vname, "None") == 0) {
        return 1;
    }
    for (size_t i = 0; i < in->unit->decls.len; i++) {
        const SnDecl *d = (const SnDecl *)in->unit->decls.items[i];
        if (d->kind != SN_DECL_ENUM) {
            continue;
        }
        for (size_t j = 0; j < d->variants.len; j++) {
            const SnDecl *v = (const SnDecl *)d->variants.items[j];
            if (v->name && strcmp(v->name, vname) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

Value eval_call(Interp *in, Env *env, const SnExpr *e) {
    Value out;
    if (try_intrinsic(in, env, e, &out)) {
        return out;
    }

    const SnExpr *callee = e->lhs;
    if (!callee) {
        rt_error(in, SNOVA_NOT_CALLABLE, e->span,
                 "call target is not an expression");
        return v_unit();
    }

    if (callee->kind == SN_EXPR_IDENT && callee->text) {
        if (strcmp(callee->text, "type") == 0) {
            /* type(T) expression returns type name as string */
            if (e->args.len > 0) {
                const SnExpr *arg0 = (const SnExpr *)e->args.items[0];
                if (arg0->text) {
                    char *type_name = sn_arena_strndup(in->arena, arg0->text,
                                                       strlen(arg0->text));
                    return v_str_in_region(type_name, in->current_region);
                }
                Value av = eval_expr(in, env, arg0);
                if (av.kind == V_STRING) return av;
                return v_str_in_region(to_string(in, av, e->span), in->current_region);
            }
            return v_str("type");
        }
        Value *local = env_lookup(env, callee->text);
        if (local && local->kind == V_LAMBDA) {
            return call_lambda(in, local->as.lam, (SnList *)&e->args, env,
                               e->span);
        }
        const SnDecl *fn = find_top(in, callee->text, SN_DECL_FUNC);
        if (fn) {
            return call_function(in, fn, (SnList *)&e->args, env, NULL, e->span);
        }
        if (strcmp(callee->text, "Array") == 0) {
            Value v;
            v.kind = V_ARRAY;
            v.as.arr = array_new(in);
            return v;
        }
        const SnDecl *cls = find_type(in, callee->text);
        if (cls) {
            Value v;
            v.kind = V_OBJECT;
            v.as.o = instantiate(in, cls, (SnList *)&e->args, env, e->span);
            return v;
        }
        if (is_variant_constructor(in, callee->text)) {
            return make_variant_from_args(in, env, callee->text, &e->args);
        }
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "unknown function `%s`",
                 callee->text);
        return v_unit();
    }

    /* Method call on a receiver. */
    if (callee->kind == SN_EXPR_MEMBER && callee->lhs) {
        /* Static method: `Foo.bar()` where Foo is a declared type, not a
         * local binding. This must be checked before evaluating `callee->lhs`
         * as a value expression below: a bare type name is not itself an
         * executable value, so eval_expr would raise "undefined name" for
         * any static call before this function ever got a chance to
         * recognize it as one. */
        if (callee->lhs->kind == SN_EXPR_IDENT && callee->lhs->text &&
            !env_lookup(env, callee->lhs->text)) {
            if (strcmp(callee->lhs->text, "Array") == 0) {
                Value v;
                v.kind = V_ARRAY;
                v.as.arr = array_new(in);
                return v;
            }
            const SnDecl *cls = find_type(in, callee->lhs->text);
            if (cls) {
                const SnDecl *m = find_member_inherited(in, cls, callee->text);
                if (m) {
                    if (strcmp(m->name, "new") == 0) {
                        /* Canonical constructor: public static method new(...) { this.x = x } */
                        Object *obj = instantiate(in, cls, NULL, env, e->span);
                        Value res = call_method(in, obj, m, (SnList *)&e->args, env, e->span);
                        if (res.kind == V_OBJECT) {
                            return res;
                        }
                        Value out_obj;
                        out_obj.kind = V_OBJECT;
                        out_obj.as.o = obj;
                        return out_obj;
                    }
                    return call_function(in, m, (SnList *)&e->args, env, NULL,
                                         e->span);
                }
                if (strcmp(callee->text, "new") == 0) {
                    Value v;
                    v.kind = V_OBJECT;
                    v.as.o = instantiate(in, cls, (SnList *)&e->args, env, e->span);
                    return v;
                }
                rt_error(in, SNOVA_UNDEFINED_NAME, e->span,
                         "unknown method `%s`",
                         callee->text ? callee->text : "?");
                return v_unit();
            }
        }



        Value recv = eval_expr(in, env, callee->lhs);
        if (recv.kind == V_STRING && callee->text &&
            eval_try_string_method(in, env, recv, e, &out)) {
            return out;
        }
        if (recv.kind == V_ARRAY && callee->text &&
            eval_try_array_method(in, env, recv, e, &out)) {
            return out;
        }
        if (recv.kind == V_VARIANT && callee->text &&
            eval_try_variant_method(in, env, recv, e, &out)) {
            return out;
        }
        if (recv.kind == V_OBJECT) {
            const SnDecl *m = find_member_inherited(in, recv.as.o->cls, callee->text);
            if (m) {
                return call_method(in, recv.as.o, m, (SnList *)&e->args, env,
                                   e->span);
            }
            /* A field that holds a lambda value is callable through the
             * member: `handler.onHit(entry)`. */
            Value *f = object_field(recv.as.o, callee->text);
            if (f && f->kind == V_LAMBDA) {
                return call_lambda(in, f->as.lam, (SnList *)&e->args, env,
                                   e->span);
            }
        }
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "unknown method `%s`",
                 callee->text ? callee->text : "?");
        return v_unit();
    }

    rt_error(in, SNOVA_NOT_CALLABLE, e->span, "expression is not callable");
    return v_unit();
}

