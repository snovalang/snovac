static SnTypeRep *preview_adopted_type(const SnExpr *e, SnTypeRep *actual,
                                       SnTypeRep *expected) {
    if (!e || !expected || !actual || expected->tag == SN_T_ERROR || expected == actual) {
        return actual;
    }
    int negative = 0;
    long long mag = 0;
    if (sn_type_integer_width(expected) > 0 &&
        integer_literal_magnitude(e, &mag, &negative)) {
        if (integer_magnitude_fits(mag, negative, sn_type_integer_width(expected),
                                   sn_type_integer_signed(expected))) {
            return (SnTypeRep *)expected;
        }
        return actual;
    }
    if (e->kind == SN_EXPR_DOUBLE &&
        (expected->tag == SN_T_DOUBLE || expected->tag == SN_T_FLOAT ||
         expected->tag == SN_T_DECIMAL)) {
        return (SnTypeRep *)expected;
    }
    return actual;
}

/* Picks one method when `primary` has an overload chain. Returns the winner,
 * or NULL after emitting ambiguous / no-match. A symbol with no overloads is
 * not passed here — a single candidate keeps the existing mismatch codes. */
static SnSymbol *select_overload(SnChecker *c, SnExpr *call, SnTypeRep **arg_tys,
                                SnSymbol *primary) {
    SnSymbol *best = NULL;
    int best_score = -1;
    int ties = 0;
    for (SnSymbol *cand = primary; cand; cand = cand->overloads) {
        if (!cand->decl || cand->decl->params.len != call->args.len) {
            continue;
        }
        int score = 0;
        int ok = 1;
        for (size_t i = 0; i < call->args.len; i++) {
            SnParam *p = SN_LIST_AT(cand->decl->params, SnParam, i);
            SnTypeRep *expected = resolve_declared_type(c, cand, p->type);
            SnExpr *arg = SN_LIST_AT(call->args, SnExpr, i);
            SnTypeRep *eff = preview_adopted_type(arg, arg_tys[i], expected);
            if (!eff || !expected || eff->tag == SN_T_ERROR || expected->tag == SN_T_ERROR ||
                sn_type_is_any(eff) || sn_type_is_any(expected)) {
                score += 1;
                continue;
            }
            if (eff == expected) {
                score += 2;
                continue;
            }
            if (!types_clash(c, eff, expected)) {
                score += 1;
                continue;
            }
            ok = 0;
            break;
        }
        if (!ok) {
            continue;
        }
        if (score > best_score) {
            best = cand;
            best_score = score;
            ties = 1;
        } else if (score == best_score) {
            ties++;
        }
    }
    if (!best) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_NO_MATCHING_OVERLOAD, call->span,
                    "no overload of `%s` matches these arguments",
                    primary->name ? primary->name : "<method>");
        return NULL;
    }
    if (ties > 1) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_AMBIGUOUS_OVERLOAD, call->span,
                    "call to `%s` is ambiguous",
                    primary->name ? primary->name : "<method>");
        return NULL;
    }
    return best;
}

/* read_bytes and write_bytes are ordinary calls. They are not tokens and
 * they are not declared in the program. The tree walk and the native runner
 * both implement them. */
static int check_byte_io_call(SnChecker *c, SnScope *local, SnExpr *e, SnTypeRep **out) {
    if (!e->lhs || e->lhs->kind != SN_EXPR_IDENT || !e->lhs->text) {
        return 0;
    }
    int reading = strcmp(e->lhs->text, "read_bytes") == 0;
    int writing = strcmp(e->lhs->text, "write_bytes") == 0;
    if (!reading && !writing) {
        return 0;
    }
    c->in_pulsar_launch = 0;
    size_t need = reading ? 1u : 2u;
    SnTypeRep **arg_tys = NULL;
    if (e->args.len > 0) {
        arg_tys = (SnTypeRep **)sn_arena_alloc(c->arena, e->args.len * sizeof(SnTypeRep *));
    }
    for (size_t i = 0; i < e->args.len; i++) {
        SnExpr *arg = SN_LIST_AT(e->args, SnExpr, i);
        arg_tys[i] = sn_check_expr(c, local, arg);
    }
    if (e->args.len != need) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARITY_MISMATCH, e->span,
                    "expected %zu argument(s), found %zu", need, e->args.len);
        *out = sn_type_error(c->types);
        return 1;
    }
    SnTypeRep *string_ty = sn_type_string(c->types);
    SnTypeRep *bytes_ty = sn_type_array(c->types, sn_type_int(c->types));
    if (arg_tys && types_clash(c, arg_tys[0], string_ty)) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, e->span,
                    "%s path must be a string", e->lhs->text);
    }
    if (writing && arg_tys && types_clash(c, arg_tys[1], bytes_ty)) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, e->span,
                    "write_bytes data must be an int array");
    }
    *out = reading ? bytes_ty : sn_type_unit(c->types);
    return 1;
}

