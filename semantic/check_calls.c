#include "check.h"
#include "check_internal.h"

#include <string.h>

#include "builtins.h"

/* ── calls ────────────────────────────────────────────────────────────────── */

void check_call_args(SnChecker *c, SnExpr *call_expr, const SnSymbol *callee,
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
        arg_tys[i] = sn_check_adopt_literal_type(c, SN_LIST_AT(call_expr->args, SnExpr, i),
                                        arg_tys[i], expected);
        if (sn_check_argument_type_clash(c, arg_tys[i], expected)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, call_expr->span,
                        "argument %zu (`%s`) has the wrong type", i + 1, p->name);
        }
    }
}

/* Calling a VALUE of function type: a lambda held in a local
 * (`let double = (x: int) -> x * 2; double(n)`), a function-typed parameter, a
 * function-typed field. The signature comes from the type, not from a
 * declaration, so there are no parameter names to quote. */
void check_call_against_functype(SnChecker *c, SnExpr *call_expr,
                                        const SnTypeRep *ft, SnTypeRep **arg_tys,
                                        size_t nargs) {
    if (ft->nargs != nargs) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARITY_MISMATCH, call_expr->span,
                    "expected %u argument(s), found %zu", ft->nargs, nargs);
        return;
    }
    for (size_t i = 0; i < nargs; i++) {
        arg_tys[i] = sn_check_adopt_literal_type(c, SN_LIST_AT(call_expr->args, SnExpr, i),
                                        arg_tys[i], ft->args[i]);
        if (sn_check_argument_type_clash(c, arg_tys[i], ft->args[i])) {
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
SnTypeRep *named_with_type_args(SnChecker *c, SnSymbol *type_sym,
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
void check_constructor_call(SnChecker *c, SnExpr *call_expr, SnSymbol *type_sym,
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
        arg_tys[fi] = sn_check_adopt_literal_type(c, SN_LIST_AT(call_expr->args, SnExpr, fi),
                                         arg_tys[fi], expected);
        if (sn_check_argument_type_clash(c, arg_tys[fi], expected)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, call_expr->span,
                        "argument %zu (`%s`) has the wrong type", fi + 1, m->name);
        }
        fi++;
    }
}

