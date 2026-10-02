/* rt_defer.c — LIFO `defer` calls, run before the owner's frame is freed. */
#include "eval_internal.h"

void rt_defer_schedule(Interp *in, Env *env, const SnExpr *call) {
    if (!call || call->kind != SN_EXPR_CALL) {
        rt_error(in, SNOVA_UNSUPPORTED, call ? call->span : (SnSpan){0},
                 "defer requires a function call");
        return;
    }
    DeferCall *d = (DeferCall *)calloc(1, sizeof(DeferCall));
    if (!d) {
        return;
    }
    d->call = call;
    d->env = env;
    d->nargs = call->args.len;
    if (d->nargs) {
        d->args = (Value *)calloc(d->nargs, sizeof(Value));
        for (size_t i = 0; i < d->nargs; i++) {
            d->args[i] = rt_eval_arg(in, env, (const SnExpr *)call->args.items[i]);
        }
    }
    d->next = in->defers;
    in->defers = d;
}

static Value invoke_defer(Interp *in, DeferCall *d) {
    const SnExpr *call = d->call;
    const SnExpr *callee = call->lhs;
    if (!callee || callee->kind != SN_EXPR_IDENT || !callee->text) {
        rt_error(in, SNOVA_UNSUPPORTED, call->span,
                 "defer call must name a function");
        return v_unit();
    }
    const SnDecl *fn = find_top(in, callee->text, SN_DECL_FUNC);
    if (!fn) {
        rt_error(in, SNOVA_UNDEFINED_NAME, call->span, "unknown function `%s`",
                 callee->text);
        return v_unit();
    }
    Env *local = env_new(in, NULL);
    size_t n = fn->params.len < d->nargs ? fn->params.len : d->nargs;
    for (size_t i = 0; i < n; i++) {
        const SnParam *p = (const SnParam *)fn->params.items[i];
        env_define(in, local, p->name, d->args[i]);
    }
    return rt_finish_call(in, local, d->env, fn->body);
}

void rt_run_defers(Interp *in) {
    while (in->defers && !in->failed) {
        DeferCall *d = in->defers;
        in->defers = d->next;
        invoke_defer(in, d);
        free(d->args);
        free(d);
    }
}

Value rt_finish_call(Interp *in, Env *local, Env *caller, const SnStmt *body) {
    DeferCall *saved = in->defers;
    in->defers = NULL;
    rt_frame_push(in);
    Value saved_ret = in->ret;
    in->ret = v_unit();
    Flow f = body ? exec_stmt(in, local, body) : FLOW_NORMAL;
    rt_run_defers(in);
    Value r = (f == FLOW_RETURN) ? in->ret : v_unit();
    int threw = (f == FLOW_THROW);
    in->ret = saved_ret;
    rt_frame_pop(in, caller, r);
    in->defers = saved;
    if (threw) {
        in->flow = FLOW_THROW;
    } else if (!in->failed) {
        in->flow = FLOW_NORMAL;
    }
    return r;
}
