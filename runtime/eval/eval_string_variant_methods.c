#include "eval_internal.h"

int eval_try_string_method(Interp *in, Env *env, Value recv,
                             const SnExpr *call, Value *out) {
    const SnExpr *callee = call->lhs;
    const char *m = callee->text;
    const char *s = recv.as.str.data ? recv.as.str.data : "";
    if (strcmp(m, "toUpper") == 0) {
        size_t len = strlen(s);
        char *buf = (char *)sn_arena_alloc(in->arena, len + 1);
        for (size_t i = 0; i < len; i++) buf[i] = (char)toupper((unsigned char)s[i]);
        buf[len] = '\0';
        *out = v_str_in_region(buf, in->current_region);
        return 1;
    }
    if (strcmp(m, "toLower") == 0) {
        size_t len = strlen(s);
        char *buf = (char *)sn_arena_alloc(in->arena, len + 1);
        for (size_t i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)s[i]);
        buf[len] = '\0';
        *out = v_str_in_region(buf, in->current_region);
        return 1;
    }
    if (strcmp(m, "len") == 0 || strcmp(m, "length") == 0) {
        *out = v_int((long long)strlen(s));
        return 1;
    }
    if (strcmp(m, "isEmpty") == 0) {
        *out = v_bool(strlen(s) == 0);
        return 1;
    }
    if (strcmp(m, "trim") == 0) {
        size_t len = strlen(s);
        size_t start = 0;
        while (start < len && isspace((unsigned char)s[start])) start++;
        size_t end = len;
        while (end > start && isspace((unsigned char)s[end - 1])) end--;
        char *buf = sn_arena_strndup(in->arena, s + start, end - start);
        *out = v_str_in_region(buf, in->current_region);
        return 1;
    }
    if (strcmp(m, "startsWith") == 0) {
        if (call->args.len > 0) {
            Value arg = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            const char *prefix = (arg.kind == V_STRING && arg.as.str.data) ? arg.as.str.data : "";
            size_t plen = strlen(prefix);
            *out = v_bool(strncmp(s, prefix, plen) == 0);
        } else {
            *out = v_bool(0);
        }
        return 1;
    }
    if (strcmp(m, "endsWith") == 0) {
        if (call->args.len > 0) {
            Value arg = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            const char *suffix = (arg.kind == V_STRING && arg.as.str.data) ? arg.as.str.data : "";
            size_t slen = strlen(s);
            size_t suflen = strlen(suffix);
            *out = v_bool(slen >= suflen && strcmp(s + (slen - suflen), suffix) == 0);
        } else {
            *out = v_bool(0);
        }
        return 1;
    }
    if (strcmp(m, "contains") == 0) {
        if (call->args.len > 0) {
            Value arg = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            const char *sub = (arg.kind == V_STRING && arg.as.str.data) ? arg.as.str.data : "";
            *out = v_bool(strstr(s, sub) != NULL);
        } else {
            *out = v_bool(0);
        }
        return 1;
    }
    if (strcmp(m, "indexOf") == 0) {
        if (call->args.len > 0) {
            Value arg = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            const char *sub = (arg.kind == V_STRING && arg.as.str.data) ? arg.as.str.data : "";
            char *p = strstr((char *)s, sub);
            if (p) {
                *out = v_int((long long)(p - s));
            } else {
                *out = v_int(-1);
            }
        } else {
            *out = v_int(-1);
        }
        return 1;
    }
    if (strcmp(m, "charAt") == 0) {
        if (call->args.len > 0) {
            Value arg = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            long long idx = as_int(in, arg, call->span);
            size_t slen = strlen(s);
            if (idx >= 0 && (size_t)idx < slen) {
                char chbuf[2] = { s[idx], '\0' };
                Value chVal = v_str_in_region(sn_arena_strndup(in->arena, chbuf, 1), in->current_region);
                SnList payload = {0};
                Value *boxed = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
                *boxed = chVal;
                sn_list_push(in->arena, &payload, boxed);
                *out = v_variant(in, "Some", payload);
            } else {
                SnList empty = {0};
                *out = v_variant(in, "None", empty);
            }
        } else {
            SnList empty = {0};
            *out = v_variant(in, "None", empty);
        }
        return 1;
    }
    if (strcmp(m, "substring") == 0) {
        size_t slen = strlen(s);
        long long start = 0;
        long long end = (long long)slen;
        if (call->args.len > 0) {
            start = as_int(in, eval_expr(in, env, (const SnExpr *)call->args.items[0]), call->span);
            if (start < 0) start = 0;
            if ((size_t)start > slen) start = (long long)slen;
        }
        if (call->args.len > 1) {
            end = as_int(in, eval_expr(in, env, (const SnExpr *)call->args.items[1]), call->span);
            if (end < start) end = start;
            if ((size_t)end > slen) end = (long long)slen;
        }
        char *buf = sn_arena_strndup(in->arena, s + start, (size_t)(end - start));
        *out = v_str_in_region(buf, in->current_region);
        return 1;
    }
    if (strcmp(m, "replaceFirst") == 0) {
        if (call->args.len >= 2) {
            Value target = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            Value repl = eval_expr(in, env, (const SnExpr *)call->args.items[1]);
            const char *tstr = (target.kind == V_STRING && target.as.str.data) ? target.as.str.data : "";
            const char *rstr = (repl.kind == V_STRING && repl.as.str.data) ? repl.as.str.data : "";
            char *found = strstr((char *)s, tstr);
            if (found && tstr[0]) {
                size_t prefix_len = (size_t)(found - s);
                size_t tlen = strlen(tstr);
                size_t rlen = strlen(rstr);
                size_t rest_len = strlen(found + tlen);
                char *buf = (char *)sn_arena_alloc(in->arena, prefix_len + rlen + rest_len + 1);
                memcpy(buf, s, prefix_len);
                memcpy(buf + prefix_len, rstr, rlen);
                memcpy(buf + prefix_len + rlen, found + tlen, rest_len + 1);
                *out = v_str_in_region(buf, in->current_region);
            } else {
                *out = recv;
            }
        } else {
            *out = recv;
        }
        return 1;
    }
    return 0;
}

/* Option/Result (and general variant) instance methods: the functional
 * unwrapping surface — `isSome`, `unwrap`, `unwrapOr`, `unwrapOrElse`,
 * `unwrapTo`, `unwrapIf`, `map`. Returns 1 when handled. */
int eval_try_variant_method(Interp *in, Env *env, Value recv,
                              const SnExpr *call, Value *out) {
    const SnExpr *callee = call->lhs;
    const char *m = callee->text;
    const VariantVal *vt = recv.as.vt;
    int has_payload = vt->payload.len > 0;
    int is_ok_like = strcmp(vt->name, "Some") == 0 || strcmp(vt->name, "Ok") == 0;
    Value payload = has_payload ? *(const Value *)vt->payload.items[0] : v_unit();

    if (strcmp(m, "isSome") == 0) { *out = v_bool(strcmp(vt->name, "Some") == 0); return 1; }
    if (strcmp(m, "isNone") == 0) { *out = v_bool(strcmp(vt->name, "None") == 0); return 1; }
    if (strcmp(m, "isOk") == 0)   { *out = v_bool(strcmp(vt->name, "Ok") == 0); return 1; }
    if (strcmp(m, "isErr") == 0)  { *out = v_bool(strcmp(vt->name, "Err") == 0); return 1; }

    if (strcmp(m, "unwrap") == 0) {
        if (!is_ok_like) {
            rt_error(in, SNOVA_TYPE_ERROR, call->span,
                     "called `unwrap` on `%s`", vt->name);
            *out = v_unit();
            return 1;
        }
        *out = payload;
        return 1;
    }
    if (strcmp(m, "unwrapOr") == 0) {
        if (is_ok_like) {
            *out = payload;
        } else if (call->args.len > 0) {
            *out = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
        } else {
            *out = v_unit();
        }
        return 1;
    }
    if (strcmp(m, "unwrapOrElse") == 0) {
        if (is_ok_like) {
            *out = payload;
            return 1;
        }
        if (call->args.len > 0) {
            Value fn = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            if (fn.kind == V_LAMBDA) {
                SnList no_args = {0};
                *out = call_lambda(in, fn.as.lam, &no_args, env, call->span);
                return 1;
            }
            *out = fn;
            return 1;
        }
        *out = v_unit();
        return 1;
    }
    if (strcmp(m, "unwrapTo") == 0) {
        /* `.unwrapTo(Type)` — unwrap asserting the payload's declared type.
         * The argument is a bare type name, so it is validated against the
         * payload instead of being evaluated as an expression. */
        if (!is_ok_like) {
            rt_error(in, SNOVA_TYPE_ERROR, call->span,
                     "called `unwrapTo` on `%s`", vt->name);
            *out = v_unit();
            return 1;
        }
        if (call->args.len > 0) {
            const SnExpr *arg = (const SnExpr *)call->args.items[0];
            if (arg->kind == SN_EXPR_IDENT && arg->text &&
                payload.kind == V_OBJECT && payload.as.o->cls->name &&
                strcmp(payload.as.o->cls->name, arg->text) != 0) {
                rt_error(in, SNOVA_TYPE_ERROR, call->span,
                         "`unwrapTo(%s)` does not match payload of type `%s`",
                         arg->text, payload.as.o->cls->name);
                *out = v_unit();
                return 1;
            }
        }
        *out = payload;
        return 1;
    }
    if (strcmp(m, "unwrapIf") == 0) {
        /* `.unwrapIf((entry) -> { ... })` — run the lambda with the payload
         * when present; the original value is returned so calls chain. */
        if (is_ok_like && call->args.len > 0) {
            Value fn = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            if (fn.kind == V_LAMBDA) {
                (void)eval_call_lambda_with_value(in, fn.as.lam, payload, call->span);
            }
        }
        *out = recv;
        return 1;
    }
    if (strcmp(m, "map") == 0) {
        if (is_ok_like && call->args.len > 0) {
            Value fn = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            if (fn.kind == V_LAMBDA) {
                Value mapped =
                    eval_call_lambda_with_value(in, fn.as.lam, payload, call->span);
                SnList payload_list = {0};
                Value *slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
                *slot = mapped;
                sn_list_push(in->arena, &payload_list, slot);
                *out = v_variant(in, vt->name, payload_list);
                return 1;
            }
        }
        *out = recv;
        return 1;
    }
    return 0;
}

