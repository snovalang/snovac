static SnSymbol *find_variant_owner_symbol(SnResolver *r, const SnSymbol *variant_sym) {
    const SnDecl *owner_decl = NULL;
    for (SnTypeScopeEntry *e = r->type_scopes; e; e = e->next) {
        SnSymbol *found = sn_scope_lookup_local(e->member_scope, variant_sym->name);
        if (found == variant_sym) {
            owner_decl = e->type_decl;
            break;
        }
    }
    return owner_decl ? find_type_symbol_for_decl(r, owner_decl) : NULL;
}

static SnTypeRep *func_like_type(SnChecker *c, const SnDecl *decl) {
    SnTypeRep **params = NULL;
    if (decl->params.len) {
        params =
            (SnTypeRep **)sn_arena_alloc(c->arena, decl->params.len * sizeof(SnTypeRep *));
        for (size_t i = 0; i < decl->params.len; i++) {
            SnParam *p = SN_LIST_AT(decl->params, SnParam, i);
            params[i] = sn_check_resolve_type(c, p->type);
        }
    }
    SnTypeRep *ret = sn_check_resolve_type(c, decl->ret);
    return sn_type_func(c->types, params, (uint32_t)decl->params.len, ret);
}

static SnTypeRep *symbol_type_in_scope(SnChecker *c, SnSymbol *sym);
static void bind_pattern_names(SnChecker *c, SnScope *scope, const SnPattern *pat);
static void check_match_exhaustiveness(SnChecker *c, const SnTypeRep *target_ty,
                                       const SnList *arms, SnSpan span);

/* True when `sym` was declared in a file other than the one being checked.
 * Resolving such a symbol's declared types re-runs name resolution in the
 * CURRENT context, where the other file's imports and type parameters do not
 * exist — `T` in builtin/Errors.snl's `func try<T, E>` is not in scope at a
 * call site in a fixture. Whatever it reports there would be both wrong and
 * attributed to the wrong file, and the real problem (if any) is reported
 * where that declaration's own body is checked. */
static int symbol_is_foreign(const SnChecker *c, const SnSymbol *sym) {
    return sym && sym->origin && sym->origin->path != c->diag->file.path;
}

/* Resolves a type as written in `sym`'s declaration. Silent when `sym` is
 * foreign: the resolution runs in the current file's scope, which is the
 * wrong one for it, so anything it would report is noise here. */
static SnTypeRep *resolve_declared_type(SnChecker *c, const SnSymbol *sym,
                                        const SnType *t) {
    int foreign = symbol_is_foreign(c, sym);
    c->diag->quiet += foreign;
    SnTypeRep *ty = sn_check_resolve_type(c, t);
    c->diag->quiet -= foreign;
    return ty;
}

static SnTypeRep *symbol_type(SnChecker *c, SnSymbol *sym) {
    if (!sym) {
        return sn_type_error(c->types);
    }
    int foreign = symbol_is_foreign(c, sym);
    c->diag->quiet += foreign;
    SnTypeRep *ty = symbol_type_in_scope(c, sym);
    c->diag->quiet -= foreign;
    return ty;
}

static SnTypeRep *symbol_type_in_scope(SnChecker *c, SnSymbol *sym) {
    switch (sym->kind) {
    case SN_SYM_LOCAL:
    case SN_SYM_PARAM:
        return sym->value_type ? sym->value_type : sn_type_error(c->types);
    case SN_SYM_FIELD:
    case SN_SYM_CONST:
        return sn_check_resolve_type(c, sym->decl->type);
    case SN_SYM_FUNC:
    case SN_SYM_METHOD:
        return func_like_type(c, sym->decl);
    case SN_SYM_TYPE:
        return sn_type_named(c->types, sym, NULL, 0);
    case SN_SYM_VARIANT:
        /* A bare reference to a variant name without calling it (no corpus
         * evidence this happens — variants are always constructed,
         * `Some(x)`/`None`) has no clean type here; the CALL path handles
         * construction separately via find_variant_owner_symbol. */
        return sn_type_error(c->types);
    case SN_SYM_PACKAGE:
        return sn_type_error(c->types); /* not a value */
    }
    return sn_type_error(c->types);
}

static int is_mutable_symbol(const SnChecker *c, const SnSymbol *sym) {
    if (!sym) {
        return 1; /* unresolved: already diagnosed elsewhere, don't cascade */
    }
    switch (sym->kind) {
    case SN_SYM_LOCAL:
        return sym->is_mutable;
    case SN_SYM_FIELD:
        /* A `let` field is immutable everywhere EXCEPT the constructor, which
         * is where it gets its one value — `this.description = description` in
         * tests/compile-pass/p1_syntax_additions.snl. Assigning it twice is
         * a definite-assignment question, and flow analysis is P4. */
        return (sym->decl && sym->decl->is_mutable) || c->in_constructor;
    default:
        return 0;
    }
}

/* ── member lookup with single-inheritance walk (mirrors sn_resolve_ident's,
 * kept local since resolve.c doesn't expose its internal walk as a
 * standalone function) ──────────────────────────────────────────────────── */

static SnSymbol *lookup_member_with_inheritance(SnChecker *c, const SnDecl *type_decl,
                                                const char *name) {
    const SnDecl *cur = type_decl;
    int depth = 0;
    while (cur && depth < 64) {
        SnScope *ms = sn_resolver_type_scope(c->resolver, cur);
        if (ms) {
            SnSymbol *sym = sn_scope_lookup_local(ms, name);
            if (sym) {
                return sym;
            }
        }
        if (cur->supertypes.len == 0) {
            break;
        }
        SnSymbol *super_sym = NULL;
        for (size_t i = 0; i < cur->supertypes.len; i++) {
            SnType *super_ty = SN_LIST_AT(cur->supertypes, SnType, i);
            if (super_ty->kind != SN_TYPE_NAME) {
                continue;
            }
            const char *super_name = sn_intern_cstr(c->intern, super_ty->name);
            super_sym = find_type_symbol_anywhere(c, cur, super_name);
            if (super_sym && super_sym->kind == SN_SYM_TYPE) {
                SnScope *sms = sn_resolver_type_scope(c->resolver, super_sym->decl);
                if (sms) {
                    SnSymbol *sym = sn_scope_lookup_local(sms, name);
                    if (sym) {
                        return sym;
                    }
                }
                break;
            }
        }
        cur = (super_sym && super_sym->kind == SN_SYM_TYPE) ? super_sym->decl : NULL;
        depth++;
    }
    return NULL;
}

/* Resolves what an IDENT or MEMBER expression refers to, checking/caching
 * it (and, for MEMBER, its receiver chain) along the way. Shared by
 * sn_check_expr's own IDENT/MEMBER cases and by CALL, which needs the same
 * resolution for its callee without checking it twice. */
static const char *resolve_package_prefix(SnChecker *c, SnScope *local, SnExpr *e) {
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
static SnTypeRep *intrinsic_generic_ref(SnChecker *c, const char *name,
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

static SnTypeRep *intrinsic_type_ref(SnChecker *c, SnExpr *e) {
    if (e->kind != SN_EXPR_IDENT || !e->text) {
        return NULL;
    }
    return intrinsic_generic_ref(c, e->text, &e->type_args);
}

static SnSymbol *resolve_value_symbol(SnChecker *c, SnScope *local, SnExpr *e) {
    if (e->kind == SN_EXPR_IDENT) {
        SnSymbol *sym = sn_resolve_ident(c->resolver, c->current_package, local,
                                         c->enclosing_type, c->current_imports, e->text,
                                         e->span);
        e->resolved_type = symbol_type(c, sym);
        return sym;
    }
    if (e->kind == SN_EXPR_MEMBER) {
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT && e->lhs->text && e->text &&
            strcmp(e->lhs->text, "Console") == 0 &&
            (strcmp(e->text, "print") == 0 || strcmp(e->text, "printline") == 0 ||
             strcmp(e->text, "println") == 0 || strcmp(e->text, "err") == 0 ||
             strcmp(e->text, "warn") == 0)) {
            SnTypeRep *any_ty = sn_type_any(c->types);
            e->lhs->resolved_type = sn_type_unit(c->types);
            e->resolved_type = sn_type_func(c->types, &any_ty, 1, sn_type_unit(c->types));
            return NULL;
        }
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

        SnTypeRep *base_ty = sn_ptr_peel(e->lhs, sn_check_expr(c, local, e->lhs));
        if (is_error(base_ty)) {
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
            /* Arrays, strings and scalars carry members that no .snl file
             * declares (`arr.len()`, `n.toString()`) — builtins.c owns them. */
            SnTypeRep *bm = sn_builtin_member(c->types, c->intern, target_ty, mname);
            if (bm) {
                if (is_qdot && is_opt_base && !is_error(bm)) {
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
        if (is_qdot && is_opt_base && !is_error(mty)) {
            mty = sn_builtin_generic(c->types, c->intern, sn_intern_cstr(c->intern, "Option"), &mty, 1);
        }
        e->resolved_type = mty;
        return msym;
    }
    sn_check_expr(c, local, e);
    return NULL;
}

/* ── operators ────────────────────────────────────────────────────────────── */

static int is_comparison_op(SnTokKind op) {
    switch (op) {
    case SN_TOK_LT: case SN_TOK_GT: case SN_TOK_LE: case SN_TOK_GE:
    case SN_TOK_EQ: case SN_TOK_NE:
        return 1;
    default:
        return 0;
    }
}

