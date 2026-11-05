/* eval.c — runtime diagnostics and function invocation.
 * Execution lifecycle is in eval_run.c; focused operations live in their
 * own translation units, described by eval_internal.h. */
#include "eval_internal.h"
#include "sn_native_dispatch.h"

void rt_error(Interp *in, int code, SnSpan span, const char *fmt, ...) {
    if (in->failed) {
        return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sn_diag_emit(in->diag, SN_DIAG_ERROR, code, span, "%s", buf);
    in->failed = 1;
    in->flow = FLOW_RETURN;
}

/* ── environment ──────────────────────────────────────────────────────────── */

void sn_eval_merge_extensions(SnArena *arena, SnUnit *unit) {
    for (size_t i = 0; i < unit->decls.len; i++) {
        SnDecl *ext = (SnDecl *)unit->decls.items[i];
        if (ext->kind != SN_DECL_EXTENSION || !ext->name) {
            continue;
        }
        for (size_t j = 0; j < unit->decls.len; j++) {
            SnDecl *target = (SnDecl *)unit->decls.items[j];
            if ((target->kind != SN_DECL_CLASS && target->kind != SN_DECL_STRUCT) ||
                !target->name || strcmp(target->name, ext->name) != 0) {
                continue;
            }
            for (size_t k = 0; k < ext->members.len; k++) {
                sn_list_push(arena, &target->members, ext->members.items[k]);
            }
            break;
        }
    }
}

/* ── calls ────────────────────────────────────────────────────────────────── */

Value call_function(Interp *in, const SnDecl *fn, SnList *args, Env *caller,
                    Object *self, SnSpan span) {
    Value out_native;
    if (sn_native_try_dispatch(in, fn, args, caller, self, span, &out_native)) {
        return out_native;
    }

    if (!fn->body) {
        rt_error(in, SNOVA_UNKNOWN_INTRINSIC, span,
                 "`%s` has no body and its @native intrinsic is not implemented yet",
                 fn->name ? fn->name : "?");
        return v_unit();
    }

    EvalRegion *region = eval_region_begin(in);
    Env *local = env_new(in, NULL); /* functions do not close over the caller */

    /* `this` and the receiver's fields are visible inside a method body. */
    if (self) {
        Value tv;
        tv.kind = V_OBJECT;
        tv.as.o = self;
        env_define(in, local, "this", tv);
        for (size_t i = 0; i < self->names.len; i++) {
            sn_list_push(in->arena, &local->names, self->names.items[i]);
            sn_list_push(in->arena, &local->slots, self->slots.items[i]);
        }
    }

    for (size_t i = 0; i < fn->params.len; i++) {
        const SnParam *p = (const SnParam *)fn->params.items[i];
        Value v;
        if (args && i < args->len) {
            v = eval_expr(in, caller, (const SnExpr *)args->items[i]);
        } else if (p->def) {
            v = eval_expr(in, caller, p->def);
        } else {
            v = default_for(p->type);
        }
        env_define(in, local, p->name, v);
    }

    Value saved = in->ret;
    in->ret = v_unit();
    Flow f = exec_stmt(in, local, fn->body);
    Value r = (f == FLOW_RETURN) ? in->ret : v_unit();
    if (eval_value_is_heap_owned(r)) eval_value_mark_escaping(in, r);
    in->ret = saved;
    in->flow = FLOW_NORMAL;
    eval_region_end(in, region);
    return r;
}

Value call_method(Interp *in, Object *self, const SnDecl *m, SnList *args,
                  Env *caller, SnSpan span) {
    return call_function(in, m, args, caller, self, span);
}

Value call_lambda(Interp *in, const LambdaVal *lam, SnList *args, Env *caller,
                  SnSpan span) {
    (void)span;
    EvalRegion *region = eval_region_begin(in);
    Env *local = env_new(in, lam->env); /* lambdas close over their env */
    for (size_t i = 0; i < lam->expr->params.len; i++) {
        const SnParam *p = (const SnParam *)lam->expr->params.items[i];
        Value v;
        if (args && i < args->len) {
            v = eval_expr(in, caller, (const SnExpr *)args->items[i]);
        } else if (p->def) {
            v = eval_expr(in, caller, p->def);
        } else {
            v = default_for(p->type);
        }
        env_define(in, local, p->name, v);
    }
    Value result = v_unit();
    if (lam->expr->value) result = eval_expr(in, local, lam->expr->value);
    if (lam->expr->body) {
        Value saved = in->ret;
        in->ret = v_unit();
        Flow f = exec_stmt(in, local, lam->expr->body);
        result = (f == FLOW_RETURN) ? in->ret : v_unit();
        in->ret = saved;
        in->flow = FLOW_NORMAL;
    }
    if (eval_value_is_heap_owned(result)) eval_value_mark_escaping(in, result);
    eval_region_end(in, region);
    return result;
}
