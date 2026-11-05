/* eval_expr.c — expression evaluation: coercions, binary operators, calls and
 * the intrinsic bridge. */
#include "eval_internal.h"

long long sn_eval_as_int(SnEvalInterp *in, SnEvalValue v, SnSpan span) {
    switch (v.kind) {
    case V_INT:    return v.as.i;
    case V_BOOL:   return v.as.b;
    case V_DOUBLE: return (long long)v.as.d;
    default:
        sn_rt_error(in, SNOVA_TYPE_ERROR, span, "expected a number");
        return 0;
    }
}

int sn_eval_truthy(SnEvalInterp *in, SnEvalValue v, SnSpan span) {
    switch (v.kind) {
    case V_BOOL: return v.as.b;
    case V_INT:  return v.as.i != 0;
    default:
        sn_rt_error(in, SNOVA_TYPE_ERROR, span, "expected a bool condition");
        return 0;
    }
}

static SnEvalValue eval_binary(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e) {
    SnEvalValue a = sn_eval_expr(in, env, e->lhs);

    /* Short-circuit before evaluating the right operand. */
    if (e->op == SN_TOK_ANDAND) {
        if (!sn_eval_truthy(in, a, e->span)) return v_bool(0);
        return v_bool(sn_eval_truthy(in, sn_eval_expr(in, env, e->rhs), e->span));
    }
    if (e->op == SN_TOK_OROR) {
        if (sn_eval_truthy(in, a, e->span)) return v_bool(1);
        return v_bool(sn_eval_truthy(in, sn_eval_expr(in, env, e->rhs), e->span));
    }

    SnEvalValue b = sn_eval_expr(in, env, e->rhs);

    if (e->op == SN_TOK_EQ) {
        return v_bool(sn_eval_value_equals(a, b));
    }
    if (e->op == SN_TOK_NE) {
        return v_bool(!sn_eval_value_equals(a, b));
    }

    /* `+` concatenates string and char only. Other types are not coerced. */
    if (e->op == SN_TOK_PLUS &&
        (a.kind == V_STRING || a.kind == V_CHAR) &&
        (b.kind == V_STRING || b.kind == V_CHAR)) {
        return v_str(sn_eval_str_concat(in, sn_eval_to_string(in, a, e->span),
                                sn_eval_to_string(in, b, e->span)));
    }

    if (a.kind == V_DOUBLE || b.kind == V_DOUBLE) {
        double x = (a.kind == V_DOUBLE) ? a.as.d : (double)sn_eval_as_int(in, a, e->span);
        double y = (b.kind == V_DOUBLE) ? b.as.d : (double)sn_eval_as_int(in, b, e->span);
        switch (e->op) {
        case SN_TOK_PLUS:  return v_double(x + y);
        case SN_TOK_MINUS: return v_double(x - y);
        case SN_TOK_STAR:  return v_double(x * y);
        case SN_TOK_SLASH: return v_double(y == 0 ? 0 : x / y);
        case SN_TOK_LT: return v_bool(x < y);
        case SN_TOK_GT: return v_bool(x > y);
        case SN_TOK_LE: return v_bool(x <= y);
        case SN_TOK_GE: return v_bool(x >= y);
        case SN_TOK_EQ: return v_bool(x == y);
        case SN_TOK_NE: return v_bool(x != y);
        default: break;
        }
    }

    if (a.kind == V_STRING && b.kind == V_STRING) {
        int c = strcmp(a.as.s, b.as.s);
        switch (e->op) {
        case SN_TOK_EQ: return v_bool(c == 0);
        case SN_TOK_NE: return v_bool(c != 0);
        case SN_TOK_LT: return v_bool(c < 0);
        case SN_TOK_GT: return v_bool(c > 0);
        default: break;
        }
    }

    long long x = sn_eval_as_int(in, a, e->span);
    long long y = sn_eval_as_int(in, b, e->span);
    switch (e->op) {
    case SN_TOK_PLUS:  return v_int(x + y);
    case SN_TOK_MINUS: return v_int(x - y);
    case SN_TOK_STAR:  return v_int(x * y);
    case SN_TOK_SLASH:
        if (y == 0) {
            sn_rt_error(in, SNOVA_TYPE_ERROR, e->span, "division by zero");
            return v_int(0);
        }
        return v_int(x / y);
    case SN_TOK_PERCENT:
        if (y == 0) {
            sn_rt_error(in, SNOVA_TYPE_ERROR, e->span, "division by zero");
            return v_int(0);
        }
        return v_int(x % y);
    case SN_TOK_AMP:    return v_int(x & y);
    case SN_TOK_PIPE:   return v_int(x | y);
    case SN_TOK_CARET:  return v_int(x ^ y);
    case SN_TOK_SHL:    return v_int(x << y);
    case SN_TOK_SHR:    return v_int(x >> y);
    case SN_TOK_LT: return v_bool(x < y);
    case SN_TOK_GT: return v_bool(x > y);
    case SN_TOK_LE: return v_bool(x <= y);
    case SN_TOK_GE: return v_bool(x >= y);
    case SN_TOK_EQ: return v_bool(x == y);
    case SN_TOK_NE: return v_bool(x != y);
    default:
        sn_rt_error(in, SNOVA_UNSUPPORTED, e->span, "operator not supported yet");
        return v_unit();
    }
}

/* Console.print / Console.printline — the only intrinsics wired up so far.
 * Returns 1 when the call was an intrinsic and `out` was set. */
static int try_intrinsic(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *call, SnEvalValue *out) {
    const SnExpr *callee = call->lhs;
    if (!callee || callee->kind != SN_EXPR_MEMBER || !callee->lhs ||
        callee->lhs->kind != SN_EXPR_IDENT || !callee->lhs->text) {
        return 0;
    }
    if (strcmp(callee->lhs->text, "Console") != 0) {
        return 0;
    }

    int newline;
    if (strcmp(callee->text, "printline") == 0 || strcmp(callee->text, "println") == 0)
        newline = 1;
    else if (strcmp(callee->text, "print") == 0) newline = 0;
    else if (strcmp(callee->text, "err") == 0)   newline = 2;
    else if (strcmp(callee->text, "warn") == 0)  newline = 2;
    else {
        sn_rt_error(in, SNOVA_UNKNOWN_INTRINSIC, call->span,
                 "Console.%s is not implemented yet", callee->text);
        *out = v_unit();
        return 1;
    }

    const char *text = "";
    if (call->args.len > 0) {
        SnEvalValue v = sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]);
        text = sn_eval_to_string(in, v, call->span);
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
static SnEvalValue make_variant_from_args(SnEvalInterp *in, SnEvalEnv *env, const char *name,
                                    const SnList *args) {
    SnList payload = {0};
    for (size_t i = 0; i < args->len; i++) {
        SnEvalValue v = sn_eval_expr(in, env, (const SnExpr *)args->items[i]);
        SnEvalValue *slot = (SnEvalValue *)sn_arena_alloc(in->arena, sizeof(SnEvalValue));
        *slot = v;
        sn_list_push(in->arena, &payload, slot);
    }
    return sn_eval_variant(in, name, payload);
}

/* `Some` / `Ok` / `Err` are built-in variant constructors; any other
 * capitalized name is looked up among declared enums' variants so user enums
 * construct the same way. */
int sn_eval_is_variant_ctor(SnEvalInterp *in, const char *name) {
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

/* Calls a single-parameter lambda with an already-evaluated value. */
static SnEvalValue call_lambda_with_value(SnEvalInterp *in, const SnEvalLambda *lam,
                                    SnEvalValue arg, SnSpan span) {
    (void)span;
    SnEvalEnv *local = sn_eval_env_new(in, lam->env);
    if (lam->expr->params.len > 0) {
        const SnParam *p = (const SnParam *)lam->expr->params.items[0];
        sn_eval_env_define(in, local, p->name, arg);
    }
    if (lam->expr->value) {
        return sn_eval_expr(in, local, lam->expr->value);
    }
    if (lam->expr->body) {
        SnEvalValue saved = in->ret;
        in->ret = v_unit();
        SnEvalFlow f = sn_eval_exec_stmt(in, local, lam->expr->body);
        SnEvalValue r = (f == FLOW_RETURN) ? in->ret : v_unit();
        in->ret = saved;
        in->flow = FLOW_NORMAL;
        return r;
    }
    return v_unit();
}

static int try_array_method(SnEvalInterp *in, SnEvalEnv *env, SnEvalValue recv,
                            const SnExpr *call, SnEvalValue *out) {
    const SnExpr *callee = call->lhs;
    const char *m = callee->text;
    if (!recv.as.arr) {
        return 0;
    }
    if (strcmp(m, "len") == 0 || strcmp(m, "length") == 0) {
        *out = v_int((long long)recv.as.arr->items.len);
        return 1;
    }
    if (strcmp(m, "isEmpty") == 0) {
        *out = v_bool(recv.as.arr->items.len == 0);
        return 1;
    }
    if (strcmp(m, "push") == 0) {
        if (call->args.len > 0) {
            SnEvalValue item = sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            SnEvalValue *boxed = (SnEvalValue *)sn_arena_alloc(in->arena, sizeof(SnEvalValue));
            *boxed = item;
            sn_list_push(in->arena, &recv.as.arr->items, boxed);
        }
        *out = v_int((long long)recv.as.arr->items.len);
        return 1;
    }
    if (strcmp(m, "pop") == 0) {
        if (recv.as.arr->items.len > 0) {
            SnEvalValue *last = (SnEvalValue *)recv.as.arr->items.items[recv.as.arr->items.len - 1];
            recv.as.arr->items.len--;
            *out = *last;
        } else {
            *out = v_unit();
        }
        return 1;
    }
    if (strcmp(m, "get") == 0) {
        if (call->args.len > 0) {
            long long idx = sn_eval_as_int(in, sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]), call->span);
            if (idx >= 0 && (size_t)idx < recv.as.arr->items.len) {
                SnEvalValue *elem = (SnEvalValue *)recv.as.arr->items.items[idx];
                *out = *elem;
            } else {
                *out = v_unit();
            }
        } else {
            *out = v_unit();
        }
        return 1;
    }
    if (strcmp(m, "clear") == 0) {
        recv.as.arr->items.len = 0;
        *out = v_int(0);
        return 1;
    }
    return 0;
}

