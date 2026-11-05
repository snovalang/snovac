/* rt_defer.c — LIFO `defer` calls, run before the owner's frame is freed. */
#include "eval_internal.h"

#include <stdint.h>

void sn_rt_defer_schedule(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *call) {
    if (!call || call->kind != SN_EXPR_CALL) {
        sn_rt_error(in, SNOVA_UNSUPPORTED, call ? call->span : (SnSpan){0},
                 "defer requires a function call");
        return;
    }
    SnEvalDefer *d = (SnEvalDefer *)calloc(1, sizeof(SnEvalDefer));
    if (!d) {
        sn_rt_error(in, SNOVA_OUT_OF_MEMORY, call->span,
                    "out of memory scheduling defer");
        return;
    }
    d->call = call;
    d->env = env;
    d->nargs = call->args.len;
    if (d->nargs) {
        if (d->nargs > SIZE_MAX / sizeof(*d->args)) {
            free(d);
            sn_rt_error(in, SNOVA_OUT_OF_MEMORY, call->span,
                        "defer argument list is too large");
            return;
        }
        d->args = (SnEvalValue *)calloc(d->nargs, sizeof(SnEvalValue));
        if (!d->args) {
            free(d);
            sn_rt_error(in, SNOVA_OUT_OF_MEMORY, call->span,
                        "out of memory capturing defer arguments");
            return;
        }
        for (size_t i = 0; i < d->nargs; i++) {
            d->args[i] = sn_rt_eval_arg(in, env, (const SnExpr *)call->args.items[i]);
        }
    }
    if (in->failed) {
        free(d->args);
        free(d);
        return;
    }
    d->next = in->defers;
    in->defers = d;
}

static SnEvalValue invoke_defer(SnEvalInterp *in, SnEvalDefer *d) {
    const SnExpr *call = d->call;
    const SnExpr *callee = call->lhs;
    if (!callee || callee->kind != SN_EXPR_IDENT || !callee->text) {
        sn_rt_error(in, SNOVA_UNSUPPORTED, call->span,
                 "defer call must name a function");
        return v_unit();
    }
    const SnDecl *fn = sn_eval_find_top(in, callee->text, SN_DECL_FUNC);
    if (!fn) {
        sn_rt_error(in, SNOVA_UNDEFINED_NAME, call->span, "unknown function `%s`",
                 callee->text);
        return v_unit();
    }
    SnEvalEnv *local = sn_eval_env_new(in, NULL);
    size_t n = fn->params.len < d->nargs ? fn->params.len : d->nargs;
    for (size_t i = 0; i < n; i++) {
        const SnParam *p = (const SnParam *)fn->params.items[i];
        sn_eval_env_define(in, local, p->name, d->args[i]);
    }
    return sn_rt_finish_call(in, local, d->env, fn->body);
}

void sn_rt_run_defers(SnEvalInterp *in) {
    SnEvalFlow pending_flow = in->flow;
    SnEvalValue pending_throw = in->thrown;
    int owner_threw = pending_flow == FLOW_THROW;
    while (in->defers) {
        SnEvalDefer *d = in->defers;
        in->defers = d->next;
        if (!in->failed) {
            /* The owner may be unwinding a throw. Run each deferred call with
             * FLOW_NORMAL so older defers still run. Keep the owner's throw
             * after the list finishes. A throw from a defer replaces an
             * earlier pending throw only when the owner itself did not throw.
             */
            in->flow = FLOW_NORMAL;
            invoke_defer(in, d);
            if (in->flow == FLOW_THROW && !owner_threw) {
                pending_flow = FLOW_THROW;
                pending_throw = in->thrown;
            }
        }
        free(d->args);
        free(d);
    }
    in->flow = pending_flow;
    if (pending_flow == FLOW_THROW) in->thrown = pending_throw;
}

SnEvalValue sn_rt_finish_call(SnEvalInterp *in, SnEvalEnv *local, SnEvalEnv *caller, const SnStmt *body) {
    SnEvalDefer *saved = in->defers;
    in->defers = NULL;
    sn_rt_frame_push(in);
    SnEvalValue saved_ret = in->ret;
    in->ret = v_unit();
    SnEvalFlow f = body ? sn_eval_exec_stmt(in, local, body) : FLOW_NORMAL;
    if (f == FLOW_THROW) in->flow = FLOW_THROW;
    sn_rt_run_defers(in);
    SnEvalValue r = (f == FLOW_RETURN) ? in->ret : v_unit();
    int threw = (in->flow == FLOW_THROW);
    in->ret = saved_ret;
    sn_rt_frame_pop(in, caller, threw ? in->thrown : r);
    in->defers = saved;
    if (threw) {
        in->flow = FLOW_THROW;
    } else if (!in->failed) {
        in->flow = FLOW_NORMAL;
    }
    return r;
}
