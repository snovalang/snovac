#include "eval_internal.h"

/* Calls a single-parameter lambda with an already-evaluated value. */
Value eval_call_lambda_with_value(Interp *in, const LambdaVal *lam,
                                    Value arg, SnSpan span) {
    (void)span;
    EvalRegion *region = eval_region_begin(in);
    Env *local = env_new(in, lam->env);
    if (lam->expr->params.len > 0) {
        const SnParam *p = (const SnParam *)lam->expr->params.items[0];
        env_define(in, local, p->name, arg);
    }
    Value result = v_unit();
    if (lam->expr->value) {
        result = eval_expr(in, local, lam->expr->value);
    } else if (lam->expr->body) {
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

int eval_try_array_method(Interp *in, Env *env, Value recv,
                            const SnExpr *call, Value *out) {
    const SnExpr *callee = call->lhs;
    const char *m = callee->text;
    if (!recv.as.arr) {
        return 0;
    }
    if (strcmp(m, "len") == 0 || strcmp(m, "length") == 0) {
        *out = v_int((long long)recv.as.arr->len);
        return 1;
    }
    if (strcmp(m, "isEmpty") == 0) {
        *out = v_bool(recv.as.arr->len == 0);
        return 1;
    }
    if (strcmp(m, "push") == 0) {
        if (call->args.len > 0) {
            Value item = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            if (in->failed) {
                *out = v_unit();
                return 1;
            }
            int cycle = eval_would_create_cycle(recv, item);
            if (cycle != 0) {
                rt_error(in, SNOVA_TYPE_ERROR, call->span,
                         cycle > 0
                             ? "array push would create a strong reference cycle"
                             : "cannot verify array push safety because of memory pressure");
                *out = v_unit();
                return 1;
            }
            if (!array_push(in, recv.as.arr, item, call->span)) {
                *out = v_unit();
                return 1;
            }
        }
        *out = v_int((long long)recv.as.arr->len);
        return 1;
    }
    if (strcmp(m, "pop") == 0) {
        if (recv.as.arr->len > 0) {
            size_t index = --recv.as.arr->len;
            *out = recv.as.arr->items[index];
            recv.as.arr->items[index] = v_unit();
            if (in->array_used_bytes < sizeof(Value)) {
                fputs("snovac runtime: live array memory accounting underflow\n", stderr);
                abort();
            }
            in->array_used_bytes -= sizeof(Value);
            if (recv.as.arr->len == 0) {
                array_release_storage(in, recv.as.arr);
            } else if (recv.as.arr->capacity > 4u &&
                       recv.as.arr->len <= recv.as.arr->capacity / 4u) {
                size_t capacity = recv.as.arr->capacity / 2u;
                if (capacity < 4u) capacity = 4u;
                Value *items = (Value *)realloc(recv.as.arr->items,
                                                capacity * sizeof(Value));
                if (items) {
                    in->array_reserved_bytes -=
                        (recv.as.arr->capacity - capacity) * sizeof(Value);
                    recv.as.arr->items = items;
                    recv.as.arr->capacity = capacity;
                }
            }
        } else {
            *out = v_unit();
        }
        return 1;
    }
    if (strcmp(m, "get") == 0) {
        if (call->args.len > 0) {
            long long idx = as_int(in, eval_expr(in, env, (const SnExpr *)call->args.items[0]), call->span);
            if (idx >= 0 && (size_t)idx < recv.as.arr->len) {
                *out = recv.as.arr->items[idx];
            } else {
                *out = v_unit();
            }
        } else {
            *out = v_unit();
        }
        return 1;
    }
    if (strcmp(m, "clear") == 0) {
        array_release_storage(in, recv.as.arr);
        *out = v_int(0);
        return 1;
    }
    return 0;
}

