#include "eval_internal.h"

Value eval_binary(Interp *in, Env *env, const SnExpr *e) {
    Value a = eval_expr(in, env, e->lhs);

    /* Short-circuit before evaluating the right operand. */
    if (e->op == SN_TOK_ANDAND) {
        if (!truthy(in, a, e->span)) return v_bool(0);
        return v_bool(truthy(in, eval_expr(in, env, e->rhs), e->span));
    }
    if (e->op == SN_TOK_OROR) {
        if (truthy(in, a, e->span)) return v_bool(1);
        return v_bool(truthy(in, eval_expr(in, env, e->rhs), e->span));
    }

    Value b = eval_expr(in, env, e->rhs);

    if (e->op == SN_TOK_EQ) {
        return v_bool(value_equals(a, b));
    }
    if (e->op == SN_TOK_NE) {
        return v_bool(!value_equals(a, b));
    }

    /* `+` concatenates when either side is a string. */
    if (e->op == SN_TOK_PLUS && (a.kind == V_STRING || b.kind == V_STRING)) {
        return v_str_in_region(str_concat(in, to_string(in, a, e->span),
                                          to_string(in, b, e->span)),
                               in->current_region);
    }

    if (a.kind == V_DOUBLE || b.kind == V_DOUBLE) {
        double x = (a.kind == V_DOUBLE) ? a.as.d : (double)as_int(in, a, e->span);
        double y = (b.kind == V_DOUBLE) ? b.as.d : (double)as_int(in, b, e->span);
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
        int c = strcmp(a.as.str.data, b.as.str.data);
        switch (e->op) {
        case SN_TOK_EQ: return v_bool(c == 0);
        case SN_TOK_NE: return v_bool(c != 0);
        case SN_TOK_LT: return v_bool(c < 0);
        case SN_TOK_GT: return v_bool(c > 0);
        default: break;
        }
    }

    long long x = as_int(in, a, e->span);
    long long y = as_int(in, b, e->span);
    switch (e->op) {
    case SN_TOK_PLUS:  return v_int(x + y);
    case SN_TOK_MINUS: return v_int(x - y);
    case SN_TOK_STAR:  return v_int(x * y);
    case SN_TOK_SLASH:
        if (y == 0) {
            rt_error(in, SNOVA_TYPE_ERROR, e->span, "division by zero");
            return v_int(0);
        }
        return v_int(x / y);
    case SN_TOK_PERCENT:
        if (y == 0) {
            rt_error(in, SNOVA_TYPE_ERROR, e->span, "division by zero");
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
        rt_error(in, SNOVA_UNSUPPORTED, e->span, "operator not supported yet");
        return v_unit();
    }
}

Value eval_unary(Interp *in, Env *env, const SnExpr *e) {
    if (e->op == SN_TOK_BANG) {
        return v_bool(!truthy(in, eval_expr(in, env, e->lhs), e->span));
    }
    if (e->op == SN_TOK_MINUS) {
        Value v = eval_expr(in, env, e->lhs);
        if (v.kind == V_DOUBLE) return v_double(-v.as.d);
        return v_int(-as_int(in, v, e->span));
    }
    if (e->op == SN_TOK_PLUS) {
        return eval_expr(in, env, e->lhs);
    }
    rt_error(in, SNOVA_UNSUPPORTED, e->span, "unary operator not supported yet");
    return v_unit();
}

Value eval_assign(Interp *in, Env *env, const SnExpr *e) {
    Value val = eval_expr(in, env, e->rhs);
    Value *slot = NULL;
    Value container = v_unit();
    int created_container_field = 0;
    if (e->lhs->kind == SN_EXPR_IDENT) {
        EvalRegion *slot_region = NULL;
        slot = env_lookup_owned(env, e->lhs->text, &slot_region);
        if (in->current_region && eval_value_is_heap_owned(val) &&
            slot && slot_region != in->current_region)
            eval_value_mark_escaping(in, val);
    } else if (e->lhs->kind == SN_EXPR_MEMBER) {
        if (e->lhs->lhs && e->lhs->lhs->kind == SN_EXPR_IDENT && e->lhs->lhs->text &&
            !env_lookup(env, e->lhs->lhs->text)) {
            const SnDecl *cls = find_type(in, e->lhs->lhs->text);
            if (cls) {
                char key[512];
                snprintf(key, sizeof(key), "%s.%s", e->lhs->lhs->text, e->lhs->text);
                EvalRegion *slot_region = NULL;
                slot = env_lookup_owned(in->globals, key, &slot_region);
                if (!slot) {
                    slot = env_define(in, in->globals, key, val);
                } else if (in->current_region && eval_value_is_heap_owned(val) &&
                           slot_region != in->current_region) {
                    eval_value_mark_escaping(in, val);
                }
            }
        }
        if (!slot) {
            Value recv = eval_expr(in, env, e->lhs->lhs);
            if (recv.kind == V_OBJECT) {
                container = recv;
                slot = object_field(recv.as.o, e->lhs->text);
                if (!slot && recv.as.o) {
                    /* The new field slot and list backing belong to the
                     * current arena even when the assigned value is scalar. */
                    if (recv.as.o->owner_region != in->current_region)
                        eval_region_mark_current(in);
                    Value *new_slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
                    *new_slot = v_unit();
                    sn_list_push(in->arena, &recv.as.o->names, (void *)e->lhs->text);
                    sn_list_push(in->arena, &recv.as.o->slots, new_slot);
                    slot = new_slot;
                    created_container_field = 1;
                }
            }
        }
    } else if (e->lhs->kind == SN_EXPR_INDEX) {
        Value recv = eval_expr(in, env, e->lhs->lhs);
        if (recv.kind == V_ARRAY) {
            container = recv;
            long long idx = as_int(in, eval_expr(in, env, e->lhs->rhs), e->span);
            if (idx >= 0 && (size_t)idx < recv.as.arr->len) {
                slot = &recv.as.arr->items[idx];
            }
        }
    }
    if (!slot) {
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span,
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
    if (container.kind != V_UNIT) {
        int destination_outlives_region =
            container.kind != V_ARRAY ||
            container.as.arr->owner_region != in->current_region;
        if (in->current_region && destination_outlives_region &&
            eval_value_is_heap_owned(val))
            eval_value_mark_escaping(in, val);
        int cycle = eval_would_create_cycle(container, val);
        if (cycle != 0) {
            if (created_container_field) {
                container.as.o->names.len--;
                container.as.o->slots.len--;
            }
            rt_error(in, SNOVA_TYPE_ERROR, e->span,
                     cycle > 0
                         ? "assignment would create a strong reference cycle"
                         : "cannot verify assignment safety because of memory pressure");
            return v_unit();
        }
    }
    *slot = val;
    return val;
}

