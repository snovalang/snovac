static int try_string_method(SnEvalInterp *in, SnEvalEnv *env, SnEvalValue recv,
                             const SnExpr *call, SnEvalValue *out) {
    const SnExpr *callee = call->lhs;
    const char *m = callee->text;
    const char *s = recv.as.s ? recv.as.s : "";
    if (strcmp(m, "toUpper") == 0) {
        size_t len = strlen(s);
        char *buf = (char *)sn_arena_alloc(in->arena, len + 1);
        for (size_t i = 0; i < len; i++) buf[i] = (char)toupper((unsigned char)s[i]);
        buf[len] = '\0';
        *out = v_str(buf);
        return 1;
    }
    if (strcmp(m, "toLower") == 0) {
        size_t len = strlen(s);
        char *buf = (char *)sn_arena_alloc(in->arena, len + 1);
        for (size_t i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)s[i]);
        buf[len] = '\0';
        *out = v_str(buf);
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
        *out = v_str(buf);
        return 1;
    }
    if (strcmp(m, "startsWith") == 0) {
        if (call->args.len > 0) {
            SnEvalValue arg = sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            const char *prefix = (arg.kind == V_STRING && arg.as.s) ? arg.as.s : "";
            size_t plen = strlen(prefix);
            *out = v_bool(strncmp(s, prefix, plen) == 0);
        } else {
            *out = v_bool(0);
        }
        return 1;
    }
    if (strcmp(m, "endsWith") == 0) {
        if (call->args.len > 0) {
            SnEvalValue arg = sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            const char *suffix = (arg.kind == V_STRING && arg.as.s) ? arg.as.s : "";
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
            SnEvalValue arg = sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            const char *sub = (arg.kind == V_STRING && arg.as.s) ? arg.as.s : "";
            *out = v_bool(strstr(s, sub) != NULL);
        } else {
            *out = v_bool(0);
        }
        return 1;
    }
    if (strcmp(m, "indexOf") == 0) {
        if (call->args.len > 0) {
            SnEvalValue arg = sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            const char *sub = (arg.kind == V_STRING && arg.as.s) ? arg.as.s : "";
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
            SnEvalValue arg = sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            long long idx = sn_eval_as_int(in, arg, call->span);
            size_t slen = strlen(s);
            if (idx >= 0 && (size_t)idx < slen) {
                char chbuf[2] = { s[idx], '\0' };
                SnEvalValue chVal = v_str(sn_arena_strndup(in->arena, chbuf, 1));
                SnList payload = {0};
                SnEvalValue *boxed = (SnEvalValue *)sn_arena_alloc(in->arena, sizeof(SnEvalValue));
                *boxed = chVal;
                sn_list_push(in->arena, &payload, boxed);
                *out = sn_eval_variant(in, "Some", payload);
            } else {
                SnList empty = {0};
                *out = sn_eval_variant(in, "None", empty);
            }
        } else {
            SnList empty = {0};
            *out = sn_eval_variant(in, "None", empty);
        }
        return 1;
    }
    if (strcmp(m, "substring") == 0) {
        size_t slen = strlen(s);
        long long start = 0;
        long long end = (long long)slen;
        if (call->args.len > 0) {
            start = sn_eval_as_int(in, sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]), call->span);
            if (start < 0) start = 0;
            if ((size_t)start > slen) start = (long long)slen;
        }
        if (call->args.len > 1) {
            end = sn_eval_as_int(in, sn_eval_expr(in, env, (const SnExpr *)call->args.items[1]), call->span);
            if (end < start) end = start;
            if ((size_t)end > slen) end = (long long)slen;
        }
        char *buf = sn_arena_strndup(in->arena, s + start, (size_t)(end - start));
        *out = v_str(buf);
        return 1;
    }
    if (strcmp(m, "replaceFirst") == 0) {
        if (call->args.len >= 2) {
            SnEvalValue target = sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            SnEvalValue repl = sn_eval_expr(in, env, (const SnExpr *)call->args.items[1]);
            const char *tstr = (target.kind == V_STRING && target.as.s) ? target.as.s : "";
            const char *rstr = (repl.kind == V_STRING && repl.as.s) ? repl.as.s : "";
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
                *out = v_str(buf);
            } else {
                *out = v_str(s);
            }
        } else {
            *out = v_str(s);
        }
        return 1;
    }
    return 0;
}

/* Option/Result (and general variant) instance methods: the functional
 * unwrapping surface — `isSome`, `unwrap`, `unwrapOr`, `unwrapOrElse`,
 * `unwrapTo`, `unwrapIf`, `map`. Returns 1 when handled. */
static int try_variant_method(SnEvalInterp *in, SnEvalEnv *env, SnEvalValue recv,
                              const SnExpr *call, SnEvalValue *out) {
    const SnExpr *callee = call->lhs;
    const char *m = callee->text;
    const VariantVal *vt = recv.as.vt;
    int has_payload = vt->payload.len > 0;
    int is_ok_like = strcmp(vt->name, "Some") == 0 || strcmp(vt->name, "Ok") == 0;
    SnEvalValue payload = has_payload ? *(const SnEvalValue *)vt->payload.items[0] : v_unit();

    if (strcmp(m, "isSome") == 0) { *out = v_bool(strcmp(vt->name, "Some") == 0); return 1; }
    if (strcmp(m, "isNone") == 0) { *out = v_bool(strcmp(vt->name, "None") == 0); return 1; }
    if (strcmp(m, "isOk") == 0)   { *out = v_bool(strcmp(vt->name, "Ok") == 0); return 1; }
    if (strcmp(m, "isErr") == 0)  { *out = v_bool(strcmp(vt->name, "Err") == 0); return 1; }

    if (strcmp(m, "unwrap") == 0) {
        if (!is_ok_like) {
            sn_rt_error(in, SNOVA_TYPE_ERROR, call->span,
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
            *out = sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]);
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
            SnEvalValue fn = sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            if (fn.kind == V_LAMBDA) {
                SnList no_args = {0};
                *out = sn_eval_call_lambda(in, fn.as.lam, &no_args, env, call->span);
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
            sn_rt_error(in, SNOVA_TYPE_ERROR, call->span,
                     "called `unwrapTo` on `%s`", vt->name);
            *out = v_unit();
            return 1;
        }
        if (call->args.len > 0) {
            const SnExpr *arg = (const SnExpr *)call->args.items[0];
            if (arg->kind == SN_EXPR_IDENT && arg->text &&
                payload.kind == V_OBJECT && payload.as.o->cls->name &&
                strcmp(payload.as.o->cls->name, arg->text) != 0) {
                sn_rt_error(in, SNOVA_TYPE_ERROR, call->span,
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
            SnEvalValue fn = sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            if (fn.kind == V_LAMBDA) {
                (void)call_lambda_with_value(in, fn.as.lam, payload, call->span);
            }
        }
        *out = recv;
        return 1;
    }
    if (strcmp(m, "map") == 0) {
        if (is_ok_like && call->args.len > 0) {
            SnEvalValue fn = sn_eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            if (fn.kind == V_LAMBDA) {
                SnEvalValue mapped =
                    call_lambda_with_value(in, fn.as.lam, payload, call->span);
                SnList payload_list = {0};
                SnEvalValue *slot = (SnEvalValue *)sn_arena_alloc(in->arena, sizeof(SnEvalValue));
                *slot = mapped;
                sn_list_push(in->arena, &payload_list, slot);
                *out = sn_eval_variant(in, vt->name, payload_list);
                return 1;
            }
        }
        *out = recv;
        return 1;
    }
    return 0;
}

static SnEvalValue eval_read_bytes(SnEvalInterp *in, SnEvalValue path, SnSpan span) {
    if (path.kind != V_STRING || !path.as.s) {
        sn_rt_error(in, SNOVA_TYPE_ERROR, span, "read_bytes expects a string path");
        return v_unit();
    }
    FILE *fp = fopen(path.as.s, "rb");
    if (!fp) {
        sn_rt_error(in, SNOVA_TYPE_ERROR, span, "cannot read '%s'", path.as.s);
        return v_unit();
    }
    ArrayVal *arr = (ArrayVal *)sn_arena_calloc(in->arena, sizeof(ArrayVal));
    if (!arr) {
        fclose(fp);
        sn_rt_error(in, SNOVA_TYPE_ERROR, span, "out of memory");
        return v_unit();
    }
    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        SnEvalValue *slot = (SnEvalValue *)sn_arena_alloc(in->arena, sizeof(SnEvalValue));
        if (!slot) {
            fclose(fp);
            sn_rt_error(in, SNOVA_TYPE_ERROR, span, "out of memory");
            return v_unit();
        }
        *slot = v_int((long long)ch);
        sn_list_push(in->arena, &arr->items, slot);
    }
    if (ferror(fp)) {
        fclose(fp);
        sn_rt_error(in, SNOVA_TYPE_ERROR, span, "cannot read '%s'", path.as.s);
        return v_unit();
    }
    fclose(fp);
    SnEvalValue v;
    v.kind = V_ARRAY;
    v.iwidth = 0;
    v.as.arr = arr;
    return v;
}

static SnEvalValue eval_write_bytes(SnEvalInterp *in, SnEvalValue path, SnEvalValue data,
                                    SnSpan span) {
    if (path.kind != V_STRING || !path.as.s) {
        sn_rt_error(in, SNOVA_TYPE_ERROR, span, "write_bytes expects a string path");
        return v_unit();
    }
    if (data.kind != V_ARRAY || !data.as.arr) {
        sn_rt_error(in, SNOVA_TYPE_ERROR, span, "write_bytes expects an int array");
        return v_unit();
    }
    FILE *fp = fopen(path.as.s, "wb");
    if (!fp) {
        sn_rt_error(in, SNOVA_TYPE_ERROR, span, "cannot write '%s'", path.as.s);
        return v_unit();
    }
    for (size_t i = 0; i < data.as.arr->items.len; i++) {
        const SnEvalValue *item = (const SnEvalValue *)data.as.arr->items.items[i];
        if (!item || item->kind != V_INT) {
            fclose(fp);
            sn_rt_error(in, SNOVA_TYPE_ERROR, span, "write_bytes expects an int array");
            return v_unit();
        }
        if (fputc((int)(item->as.i & 0xff), fp) == EOF) {
            fclose(fp);
            sn_rt_error(in, SNOVA_TYPE_ERROR, span, "cannot write '%s'", path.as.s);
            return v_unit();
        }
    }
    if (fclose(fp) != 0) {
        sn_rt_error(in, SNOVA_TYPE_ERROR, span, "cannot write '%s'", path.as.s);
        return v_unit();
    }
    return v_unit();
}

