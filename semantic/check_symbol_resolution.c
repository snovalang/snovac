#include "check.h"
#include "check_internal.h"

#include <string.h>

#include "builtins.h"

/* Resolves what an IDENT or MEMBER expression refers to, checking/caching
 * it (and, for MEMBER, its receiver chain) along the way. Shared by
 * sn_check_expr's own IDENT/MEMBER cases and by CALL, which needs the same
 * resolution for its callee without checking it twice. */
const char *resolve_package_prefix(SnChecker *c, SnScope *local, SnExpr *e) {
    if (e->kind == SN_EXPR_IDENT) {
        const char *head_i = sn_intern_cstr(c->intern, e->text);
        if (local && sn_scope_lookup(local, head_i)) return NULL;
        if (c->enclosing_type) {
            SnScope *ms = sn_resolver_type_scope(c->resolver, c->enclosing_type);
            if (ms && sn_scope_lookup_local(ms, head_i)) return NULL;
        }
        SnScope *pkg_scope = sn_resolver_package_scope(c->resolver, c->current_package);
        if (pkg_scope && sn_scope_lookup_local(pkg_scope, head_i)) return NULL;

        if (c->current_imports) {
            for (size_t i = 0; i < c->current_imports->len; i++) {
                const char *imp_pkg = SN_LIST_AT(*c->current_imports, const char, i);
                const char *last = strrchr(imp_pkg, '.');
                const char *stem = last ? last + 1 : imp_pkg;
                if (strcmp(stem, e->text) == 0) {
                    return imp_pkg;
                }
            }
        }
        if (sn_pkggraph_find(c->resolver->graph, head_i)) return head_i;
        return NULL;
    }
    if (e->kind == SN_EXPR_MEMBER) {
        const char *parent_pkg = resolve_package_prefix(c, local, e->lhs);
        if (parent_pkg) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s.%s", parent_pkg, e->text);
            const char *candidate = sn_intern_cstr(c->intern, buf);
            if (sn_pkggraph_find(c->resolver->graph, candidate)) return candidate;
        }
    }
    return NULL;
}

/* `Array<Post>` / `Partial<User>` written in EXPRESSION position — a
 * reference to an intrinsic TYPE, not a value. The parser leaves these as an
 * IDENT carrying `type_args` (parse_expr.c, "the generic arguments belong to
 * the type"), so there is no symbol to resolve and sn_resolve_ident would
 * report SNOVA0023 for a name that is perfectly real. Returns the type, or
 * NULL when `e` is not one of these. */
SnTypeRep *intrinsic_generic_ref(SnChecker *c, const char *name,
                                        const SnList *type_args) {
    if (!name) {
        return NULL;
    }
    const char *iname = sn_intern_cstr(c->intern, name);
    if (!sn_builtin_is_generic_name(c->intern, iname)) {
        return NULL;
    }
    SnTypeRep **args = NULL;
    if (type_args->len) {
        args = (SnTypeRep **)sn_arena_alloc(c->arena,
                                            type_args->len * sizeof(SnTypeRep *));
        for (size_t i = 0; i < type_args->len; i++) {
            args[i] = sn_check_resolve_type(c, SN_LIST_AT(*type_args, SnType, i));
        }
    }
    return sn_builtin_generic(c->types, c->intern, iname, args, (uint32_t)type_args->len);
}

SnTypeRep *intrinsic_type_ref(SnChecker *c, SnExpr *e) {
    if (e->kind != SN_EXPR_IDENT || !e->text) {
        return NULL;
    }
    return intrinsic_generic_ref(c, e->text, &e->type_args);
}

SnSymbol *resolve_value_symbol(SnChecker *c, SnScope *local, SnExpr *e) {
    if (e->kind == SN_EXPR_IDENT) {
        SnSymbol *sym = sn_resolve_ident(c->resolver, c->current_package, local,
                                         c->enclosing_type, c->current_imports, e->text,
                                         e->span);
        if (sym && c->lambda_scope &&
            (sym->kind == SN_SYM_LOCAL || sym->kind == SN_SYM_PARAM)) {
            int defined_inside_lambda = 0;
            for (SnScope *s = local; s; s = s->parent) {
                if (sn_scope_lookup_local(s, sym->name) == sym) {
                    defined_inside_lambda = 1;
                    break;
                }
                if (s == c->lambda_scope) break;
            }
            if (!defined_inside_lambda) {
                sym->is_captured = 1;
                if (sym->is_borrowed) {
                    sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW,
                                 e->span,
                                 "borrowed parameter `%s` cannot escape through a closure",
                                 sym->name);
                }
            }
        }
        e->resolved_symbol = sym;
        e->resolved_type = symbol_type(c, sym);
        return sym;
    }
    if (e->kind == SN_EXPR_MEMBER) {
        /* `Array<Post>.new()` — receiver is an intrinsic type, not a value.
         * Checked before the package-prefix walk so the receiver is never
         * evaluated as an expression (which would report SNOVA0023 for it). */
        SnTypeRep *recv_ty = intrinsic_type_ref(c, e->lhs);
        if (recv_ty) {
            e->lhs->resolved_type = recv_ty;
            const char *sname = sn_intern_cstr(c->intern, e->text);
            SnTypeRep *sm = sn_builtin_static_member(c->types, c->intern, recv_ty, sname);
            if (!sm) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNKNOWN_MEMBER, e->span,
                            "type has no member `%s`", e->text);
                sm = sn_type_error(c->types);
            }
            e->resolved_type = sm;
            return NULL; /* intrinsic: no symbol — see the CALL case */
        }

        const char *pkg_prefix = resolve_package_prefix(c, local, e->lhs);
        if (pkg_prefix) {
            SnScope *pkg_scope = sn_resolver_package_scope(c->resolver, pkg_prefix);
            if (pkg_scope) {
                const char *mname = sn_intern_cstr(c->intern, e->text);
                SnSymbol *msym = sn_scope_lookup_local(pkg_scope, mname);
                if (msym) {
                    e->lhs->resolved_type = sn_type_error(c->types); /* package prefix is not a value */
                    e->resolved_type = symbol_type(c, msym);
                    return msym;
                }
            }
        }

        SnTypeRep *base_ty = sn_check_expr(c, local, e->lhs);
        if (sn_check_is_error(base_ty)) {
            e->resolved_type = sn_type_error(c->types);
            return NULL;
        }
        const char *mname = sn_intern_cstr(c->intern, e->text);
        int is_qdot = (e->op == SN_TOK_QDOT);
        int is_opt_base = (base_ty->tag == SN_T_NAMED && base_ty->decl &&
                           strcmp(base_ty->decl->name, "Option") == 0 && base_ty->nargs == 1);
        if (is_qdot && !is_opt_base) {
            sn_diag_emit(c->diag, SN_DIAG_WARNING, SNOVA_OPTIONAL_CHAINING_NON_OPTIONAL, e->span,
                         "redundant optional chaining `?.` applied to non-optional type");
        }
        SnTypeRep *target_ty = (is_qdot && is_opt_base) ? base_ty->args[0] : base_ty;

        if (target_ty->tag != SN_T_NAMED || !target_ty->decl) {
            /* Arrays, strings and scalars carry members that no .snova file
             * declares (`arr.len()`, `n.toString()`) — builtins.c owns them. */
            SnTypeRep *bm = sn_builtin_member(c->types, c->intern, target_ty, mname);
            if (bm) {
                if (is_qdot && is_opt_base && !sn_check_is_error(bm)) {
                    bm = sn_builtin_generic(c->types, c->intern, sn_intern_cstr(c->intern, "Option"), &bm, 1);
                }
                e->resolved_type = bm;
                return NULL; /* intrinsic: no symbol — see the CALL case */
            }
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNKNOWN_MEMBER, e->span,
                        "type has no member `%s`", e->text);
            e->resolved_type = sn_type_error(c->types);
            return NULL;
        }
        SnSymbol *msym = lookup_member_with_inheritance(c, target_ty->decl->decl, mname);
        if (!msym) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNKNOWN_MEMBER, e->span,
                        "Unknown member `%s` on `%s`", e->text, target_ty->decl->name);
            e->resolved_type = sn_type_error(c->types);
            return NULL;
        }
        if (msym->decl && msym->decl->vis == SN_VIS_PRIVATE &&
            c->enclosing_type != target_ty->decl->decl) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PRIVATE_ACCESS, e->span,
                        "`%s` is private to `%s`", e->text, target_ty->decl->name);
        }
        SnTypeRep *mty = symbol_type(c, msym);
        /* A zero-payload enum variant used as `Direction.North` is already a
         * value of its owning enum. Payload-bearing variants remain
         * constructors and are typed by the CALL case. */
        if (msym->kind == SN_SYM_VARIANT && msym->decl &&
            msym->decl->params.len == 0) {
            SnSymbol *owner = find_variant_owner_symbol(c->resolver, msym);
            if (owner) mty = sn_type_named(c->types, owner, NULL, 0);
        }
        if (target_ty->nargs > 0 && target_ty->decl && target_ty->decl->decl &&
            target_ty->decl->decl->generics.len > 0) {
            uint32_t count = (uint32_t)target_ty->decl->decl->generics.len;
            if (count > target_ty->nargs) {
                count = target_ty->nargs;
            }
            const char **param_names = (const char **)sn_arena_alloc(c->arena, count * sizeof(const char *));
            for (uint32_t gi = 0; gi < count; gi++) {
                param_names[gi] = SN_LIST_AT(target_ty->decl->decl->generics, const char, gi);
            }
            mty = sn_type_subst_names(c->types, mty, param_names, target_ty->args, count);
        }
        if (is_qdot && is_opt_base && !sn_check_is_error(mty)) {
            mty = sn_builtin_generic(c->types, c->intern, sn_intern_cstr(c->intern, "Option"), &mty, 1);
        }
        e->resolved_type = mty;
        return msym;
    }
    sn_check_expr(c, local, e);
    return NULL;
}

