static SnTypeRep *check_binary(SnChecker *c, SnExpr *e, SnTypeRep *lt, SnTypeRep *rt) {
    if (is_error(lt) || is_error(rt)) {
        return sn_type_error(c->types); /* plan.md §5.1: silent propagation */
    }
    if (sn_type_is_any(lt) || sn_type_is_any(rt)) {
        /* types.h: `any` never takes part in a mismatch, in either direction.
         * A comparison still yields bool; everything else stays `any` rather
         * than guessing which operand's type wins. */
        return is_comparison_op(e->op) ? sn_type_bool(c->types) : sn_type_any(c->types);
    }
    switch (e->op) {
    case SN_TOK_PLUS:
        if (is_text_type(lt) && is_text_type(rt)) {
            return sn_type_string(c->types);
        }
        /* fallthrough */
    case SN_TOK_MINUS:
    case SN_TOK_STAR:
    case SN_TOK_SLASH:
    case SN_TOK_PERCENT:
    case SN_TOK_AMP:
    case SN_TOK_PIPE:
    case SN_TOK_CARET:
    case SN_TOK_SHL:
    case SN_TOK_SHR:
        if (is_numeric(lt) && lt == rt) {
            return lt;
        }
        break;
    case SN_TOK_LT:
    case SN_TOK_GT:
    case SN_TOK_LE:
    case SN_TOK_GE:
        if (is_numeric(lt) && lt == rt) {
            return sn_type_bool(c->types);
        }
        break;
    case SN_TOK_EQ:
    case SN_TOK_NE:
        if (lt == rt || sn_ptr_null_cmp(lt, rt)) {
            return sn_type_bool(c->types);
        }
        break;
    case SN_TOK_ANDAND:
    case SN_TOK_OROR:
        if (lt->tag == SN_T_BOOL && rt->tag == SN_T_BOOL) {
            return sn_type_bool(c->types);
        }
        break;
    case SN_TOK_QQ: {
        int is_opt = (lt->tag == SN_T_NAMED && lt->decl &&
                      strcmp(lt->decl->name, "Option") == 0 && lt->nargs == 1);
        if (!is_opt) {
            sn_diag_emit(c->diag, SN_DIAG_WARNING, SNOVA_REDUNDANT_NULL_COALESCING, e->span,
                         "redundant null-coalescing operator `??` applied to non-optional type");
            return lt;
        }
        SnTypeRep *inner = lt->args[0];
        if (rt != inner) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_MISMATCHED_FALLBACK, e->span,
                         "null-coalescing fallback type mismatch: expected inner type of Option");
            return sn_type_error(c->types);
        }
        return inner;
    }
    default:
        break;
    }
    sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_BINARY_TYPE_MISMATCH, e->span,
                "operator `%s` cannot be applied to these operand types "
                "(no implicit numeric promotion -- operands must match exactly)",
                sn_tok_name(e->op));
    return sn_type_error(c->types);
}

static SnTypeRep *check_unary(SnChecker *c, SnExpr *e, SnTypeRep *operand) {
    SnTypeRep *ptr_ty = NULL;
    if (sn_check_ptr_unary(c, e, operand, &ptr_ty)) {
        return ptr_ty;
    }
    if (is_error(operand)) {
        return sn_type_error(c->types);
    }
    if (sn_type_is_any(operand)) {
        return operand; /* types.h: `any` never takes part in a mismatch */
    }

    if (e->op == SN_TOK_QUESTION) {
        /* PROVISIONAL — see check.h file header. plan.md §7 item 6 was left
         * unratified by resolve.c; real corpus usage is essentially absent. */
        if (operand->tag == SN_T_NAMED && operand->decl && operand->nargs >= 1 &&
            (operand->decl->name == sn_intern_cstr(c->intern, "Option") ||
             operand->decl->name == sn_intern_cstr(c->intern, "Result"))) {
            return operand->args[0];
        }
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNARY_TYPE_MISMATCH, e->span,
                    "`?` requires an Option or Result operand");
        return sn_type_error(c->types);
    }

    switch (e->op) {
    case SN_TOK_BANG:
        if (operand->tag == SN_T_BOOL) {
            return operand;
        }
        break;
    case SN_TOK_MINUS:
    case SN_TOK_PLUS:
        if (is_numeric(operand)) {
            return operand;
        }
        break;
    case SN_TOK_TILDE:
        if (sn_type_integer_width(operand) > 0) {
            return operand;
        }
        break;
    default:
        break;
    }
    sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNARY_TYPE_MISMATCH, e->span,
                "invalid operand type for unary `%s`", sn_tok_name(e->op));
    return sn_type_error(c->types);
}

/* ── calls ────────────────────────────────────────────────────────────────── */

static void check_call_args(SnChecker *c, SnExpr *call_expr, const SnSymbol *callee,
                            const SnList *params, SnTypeRep **arg_tys, size_t nargs) {
    if (!params) {
        return;
    }
    if (params->len != nargs) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARITY_MISMATCH, call_expr->span,
                    "expected %zu argument(s), found %zu", params->len, nargs);
        return; /* don't also report per-argument mismatches against a
                 * signature that's already known not to match */
    }

    const char **param_names = NULL;
    SnTypeRep **subst_tys = NULL;
    uint32_t ngen = 0;
    if (callee && callee->decl && callee->decl->generics.len > 0) {
        ngen = (uint32_t)callee->decl->generics.len;
        param_names = (const char **)sn_arena_alloc(c->arena, ngen * sizeof(const char *));
        subst_tys = (SnTypeRep **)sn_arena_alloc(c->arena, ngen * sizeof(SnTypeRep *));
        for (uint32_t gi = 0; gi < ngen; gi++) {
            param_names[gi] = SN_LIST_AT(callee->decl->generics, const char, gi);
            subst_tys[gi] = (gi < call_expr->type_args.len)
                ? sn_check_resolve_type(c, SN_LIST_AT(call_expr->type_args, SnType, gi))
                : sn_type_any(c->types);
        }
    }

    for (size_t i = 0; i < nargs; i++) {
        SnParam *p = SN_LIST_AT(*params, SnParam, i);
        SnTypeRep *expected = resolve_declared_type(c, callee, p->type);
        if (ngen > 0 && expected) {
            expected = sn_type_subst_names(c->types, expected, param_names, subst_tys, ngen);
        }
        arg_tys[i] = adopt_literal_type(c, SN_LIST_AT(call_expr->args, SnExpr, i),
                                        arg_tys[i], expected);
        arg_tys[i] = sn_ptr_adapt_arg(c, SN_LIST_AT(call_expr->args, SnExpr, i),
                                      arg_tys[i], expected);
        if (types_clash(c, arg_tys[i], expected)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, call_expr->span,
                        "argument %zu (`%s`) has the wrong type", i + 1, p->name);
        }
    }
}

/* Calling a VALUE of function type: a lambda held in a local
 * (`let double = (x: int) -> x * 2; double(n)`), a function-typed parameter, a
 * function-typed field. The signature comes from the type, not from a
 * declaration, so there are no parameter names to quote. */
static void check_call_against_functype(SnChecker *c, SnExpr *call_expr,
                                        const SnTypeRep *ft, SnTypeRep **arg_tys,
                                        size_t nargs) {
    if (ft->nargs != nargs) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARITY_MISMATCH, call_expr->span,
                    "expected %u argument(s), found %zu", ft->nargs, nargs);
        return;
    }
    for (size_t i = 0; i < nargs; i++) {
        arg_tys[i] = adopt_literal_type(c, SN_LIST_AT(call_expr->args, SnExpr, i),
                                        arg_tys[i], ft->args[i]);
        arg_tys[i] = sn_ptr_adapt_arg(c, SN_LIST_AT(call_expr->args, SnExpr, i),
                                      arg_tys[i], ft->args[i]);
        if (types_clash(c, arg_tys[i], ft->args[i])) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, call_expr->span,
                        "argument %zu has the wrong type", i + 1);
        }
    }
}

/* The named type a constructor call produces, with any explicit generic
 * arguments threaded through so `Deferred<int, string>` and
 * `Deferred<string, int>` stay distinct (types.c hash-consing). A call that
 * writes none gets the bare named type — inferring generic arguments from the
 * argument list is not modeled (check.h's generics gap). */
static SnTypeRep *named_with_type_args(SnChecker *c, SnSymbol *type_sym,
                                       const SnExpr *call) {
    if (call->type_args.len == 0) {
        return sn_type_named(c->types, type_sym, NULL, 0);
    }
    if (type_sym->decl && call->type_args.len != type_sym->decl->generics.len) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_GENERIC_ARITY_MISMATCH, call->span,
                    "`%s` expects %zu type argument(s), found %zu",
                    type_sym->decl->name ? type_sym->decl->name : "<type>",
                    type_sym->decl->generics.len, call->type_args.len);
    }
    SnTypeRep **args =
        (SnTypeRep **)sn_arena_alloc(c->arena, call->type_args.len * sizeof(SnTypeRep *));
    for (size_t i = 0; i < call->type_args.len; i++) {
        args[i] = sn_check_resolve_type(c, SN_LIST_AT(call->type_args, SnType, i));
    }
    return sn_type_named(c->types, type_sym, args, (uint32_t)call->type_args.len);
}

/* Construction: `Point(3.0, 4.0)`, `Counter(0)`, `HealthCheck()`.
 *
 * ast.h already records the shape ("Construction has no `new` keyword"), and
 * no fixture or builtin file declares a constructor member — the class/struct
 * FIELD list, in source order, IS the signature. Fields carrying an
 * initializer are optional, so an accepted call passes between "fields without
 * an initializer" and "all fields" arguments; argument types are only matched
 * positionally when every field was supplied, since a partial call gives no
 * way to tell which fields the arguments belong to.
 *
 * Enums and interfaces are not constructible this way and are left alone. */
static void check_constructor_call(SnChecker *c, SnExpr *call_expr, SnSymbol *type_sym,
                                   SnTypeRep **arg_tys, size_t nargs) {
    const SnDecl *d = type_sym->decl;
    if (!d || (d->kind != SN_DECL_CLASS && d->kind != SN_DECL_STRUCT)) {
        return;
    }

    if (d->is_abstract) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ABSTRACT_INSTANTIATION, call_expr->span,
                    "cannot instantiate abstract class `%s`",
                    d->name ? d->name : "<type>");
        return;
    }

    /* An explicitly written signature wins over the field list: the
     * `class Foo(a: int)` primary-constructor form (parse_decl.c keeps it in
     * `params`), then a `constructor(...)` member (parse_decl.c parses it as a
     * method of that fixed name — "P2 owns its special dispatch"). */
    if (d->params.len) {
        check_call_args(c, call_expr, type_sym, &d->params, arg_tys, nargs);
        return;
    }
    for (size_t i = 0; i < d->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(d->members, SnDecl, i);
        if (m->kind == SN_DECL_METHOD && m->name &&
            sn_intern_cstr(c->intern, m->name) == sn_intern_cstr(c->intern, "constructor")) {
            check_call_args(c, call_expr, type_sym, &m->params, arg_tys, nargs);
            return;
        }
    }

    size_t total = 0, required = 0;
    for (size_t i = 0; i < d->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(d->members, SnDecl, i);
        if (m->kind != SN_DECL_FIELD) {
            continue;
        }
        total++;
        if (!m->init) {
            required++;
        }
    }
    /* `Counter()` with a field that has no initializer is how the run-pass
     * fixtures construct objects; the runtime fills the field from
     * `default_for`. A call that passes some arguments still has to cover
     * every field that lacks an initializer. */
    if (nargs == 0) {
        return;
    }
    int any_named = 0;
    for (size_t ni = 0; ni < call_expr->field_names.len && ni < nargs; ni++) {
        if (SN_LIST_AT(call_expr->field_names, const char, ni)) {
            any_named = 1;
            break;
        }
    }
    if (any_named) {
        if (nargs > total) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARITY_MISMATCH, call_expr->span,
                        "expected %zu argument(s), found %zu", required, nargs);
            return;
        }
        for (size_t ai = 0; ai < nargs; ai++) {
            const char *fname = (ai < call_expr->field_names.len)
                                    ? SN_LIST_AT(call_expr->field_names, const char, ai)
                                    : NULL;
            SnExpr *arg = SN_LIST_AT(call_expr->args, SnExpr, ai);
            if (!fname) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, call_expr->span,
                            "argument %zu must name a field", ai + 1);
                continue;
            }
            const SnDecl *field = NULL;
            for (size_t i = 0; i < d->members.len; i++) {
                const SnDecl *m = SN_LIST_AT(d->members, SnDecl, i);
                if (m->kind == SN_DECL_FIELD && m->name && strcmp(m->name, fname) == 0) {
                    field = m;
                    break;
                }
            }
            if (!field) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNKNOWN_MEMBER, arg->span,
                            "type `%s` has no field named `%s`", d->name, fname);
                continue;
            }
            SnTypeRep *expected = resolve_declared_type(c, type_sym, field->type);
            arg_tys[ai] = adopt_literal_type(c, arg, arg_tys[ai], expected);
            if (types_clash(c, arg_tys[ai], expected)) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, arg->span,
                            "argument `%s` has the wrong type", fname);
            }
        }
        return;
    }
    if (nargs < required || nargs > total) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARITY_MISMATCH, call_expr->span,
                    "expected %zu argument(s), found %zu", required, nargs);
        return;
    }
    if (nargs != total) {
        return;
    }
    const char **param_names = NULL;
    SnTypeRep **subst_tys = NULL;
    uint32_t ngen = 0;
    if (d->generics.len > 0) {
        ngen = (uint32_t)d->generics.len;
        param_names = (const char **)sn_arena_alloc(c->arena, ngen * sizeof(const char *));
        subst_tys = (SnTypeRep **)sn_arena_alloc(c->arena, ngen * sizeof(SnTypeRep *));
        for (uint32_t gi = 0; gi < ngen; gi++) {
            param_names[gi] = SN_LIST_AT(d->generics, const char, gi);
            subst_tys[gi] = (gi < call_expr->type_args.len)
                ? sn_check_resolve_type(c, SN_LIST_AT(call_expr->type_args, SnType, gi))
                : sn_type_any(c->types);
        }
    }

    size_t fi = 0;
    for (size_t i = 0; i < d->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(d->members, SnDecl, i);
        if (m->kind != SN_DECL_FIELD) {
            continue;
        }
        SnTypeRep *expected = resolve_declared_type(c, type_sym, m->type);
        if (ngen > 0 && expected) {
            expected = sn_type_subst_names(c->types, expected, param_names, subst_tys, ngen);
        }
        arg_tys[fi] = adopt_literal_type(c, SN_LIST_AT(call_expr->args, SnExpr, fi),
                                         arg_tys[fi], expected);
        if (types_clash(c, arg_tys[fi], expected)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, call_expr->span,
                        "argument %zu (`%s`) has the wrong type", fi + 1, m->name);
        }
        fi++;
    }
}

/* ── the big expression switch ───────────────────────────────────────────── */

