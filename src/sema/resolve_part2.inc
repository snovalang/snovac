size_t sn_resolver_collect(SnResolver *r) {
    size_t skipped = 0;
    ExtEntry *pending_ext = NULL;

    for (SnPackageNode *node = r->graph->nodes; node; node = node->next) {
        SnScope *pkg_scope = package_scope_find_or_create(r, node->name);

        for (SnPackageFile *pf = node->files; pf; pf = pf->next) {
            /* Multi-section files (resolve.h note 1): the same physical path
             * appears under more than one SnPackageFile across the graph.
             * O(n^2) in file count, fine at this corpus's scale. */
            int occurrences = 0;
            for (SnPackageNode *n2 = r->graph->nodes; n2; n2 = n2->next) {
                for (SnPackageFile *pf2 = n2->files; pf2; pf2 = pf2->next) {
                    if (strcmp(pf2->path, pf->path) == 0) {
                        occurrences++;
                    }
                }
            }
            if (occurrences > 1) {
                skipped++;
                continue;
            }

            size_t len = 0;
            char *src = read_whole_file(r->arena, pf->path, &len);
            if (!src) {
                continue;
            }

            /* Arena-allocated so it outlives this loop: every symbol defined
             * below keeps a pointer to it, and check.c reads it much later. */
            SnDiagFile *origin = (SnDiagFile *)sn_arena_alloc(r->arena, sizeof(SnDiagFile));
            origin->path = pf->path;
            origin->src = src;
            origin->src_len = len;
            r->current_origin = origin;
            SnDiagFile outer = sn_diag_set_file(r->diag, *origin);

            SnTokenVec toks;
            memset(&toks, 0, sizeof(toks));
            sn_lex(r->arena, r->diag, src, len, &toks);

            SnUnit unit;
            sn_parse(r->arena, r->diag, &toks, &unit);

            for (size_t i = 0; i < unit.decls.len; i++) {
                SnDecl *d = SN_LIST_AT(unit.decls, SnDecl, i);

                if (d->kind == SN_DECL_EXTENSION) {
                    ExtEntry *ee = (ExtEntry *)sn_arena_alloc(r->arena, sizeof(ExtEntry));
                    ee->decl = d;
                    ee->package = node->name;
                    ee->origin = origin;
                    ee->next = pending_ext;
                    pending_ext = ee;
                    continue;
                }

                /* `method` needs an owning type; at the top level it must be
                 * `func`. Reported before the symbol is defined so the name
                 * still enters scope and callers don't cascade. */
                if (d->kind == SN_DECL_METHOD) {
                    sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_METHOD_AT_TOP_LEVEL,
                                 d->span,
                                 "`method` is not valid at the top level -- use `func` "
                                 "for module-level functions.");
                }

                SnSymbolKind sk;
                if (!top_level_symbol_kind(d->kind, &sk)) {
                    continue;
                }

                const char *name = sn_intern_cstr(r->intern, d->name);
                SnSymbol *sym = define_with_origin(r, pkg_scope, name, sk, d, d->span);
                if (!sym) {
                    SnSymbol *existing = sn_scope_lookup_local(pkg_scope, name);
                    if (existing && existing->kind == sk &&
                        existing->decl && d && existing->decl->kind == d->kind &&
                        existing->origin && origin && existing->origin->path && origin->path &&
                        strcmp(existing->origin->path, origin->path) != 0) {
                        /* Duplicate identical declaration from redundant dependency roots; reuse existing */
                        continue;
                    }
                    if (existing && existing->kind == sk && sk == SN_SYM_FUNC &&
                        !overload_chain_has_sig(existing, d)) {
                        chain_overload(r, existing, name, sk, d, d->span);
                        continue;
                    }
                    sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_DUPLICATE_DECL,
                                 d->span, "`%s` is already declared in package `%s`",
                                 d->name, node->name);
                    continue;
                }

                if (decl_kind_has_member_scope(d->kind)) {
                    SnScope *member_scope = type_scope_create(r, d);
                    for (size_t j = 0; j < d->members.len; j++) {
                        SnDecl *m = SN_LIST_AT(d->members, SnDecl, j);
                        collect_member(r, member_scope, d, m, NULL);
                    }
                    for (size_t j = 0; j < d->variants.len; j++) {
                        SnDecl *v = SN_LIST_AT(d->variants, SnDecl, j);
                        const char *vname = sn_intern_cstr(r->intern, v->name);
                        SnSymbol *vsym = define_with_origin(r, member_scope, vname,
                                                           SN_SYM_VARIANT, v, v->span);
                        if (!vsym) {
                            sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_DUPLICATE_DECL,
                                         v->span, "`%s` is already declared in `%s`",
                                         v->name, d->name);
                        }
                    }
                }
            }

            sn_diag_set_file(r->diag, outer);
            r->current_origin = NULL;
        }
    }

    /* Pass B: extension members, now that every type in this run has a
     * member scope to merge into. This is how Option/Result actually get
     * isSome()/unwrap()/map()/... — the enum body only has variants. */
    for (ExtEntry *ee = pending_ext; ee; ee = ee->next) {
        r->current_origin = ee->origin;
        SnDiagFile outer = sn_diag_set_file(r->diag, *ee->origin);
        collect_extension_members(r, ee);
        sn_diag_set_file(r->diag, outer);
        r->current_origin = NULL;
    }

    return skipped;
}


/* Option/Result's own variants (Some/None/Ok/Err) are also usable with no
 * import — builtin/Types.snl's own doc comment says so explicitly
 * ("construction (Some(v), None, Ok(v), Err(e))... is usable in every
 * Snovalang program with no import"). Found while testing variant
 * construction (`Some(1)`) against the checker: registering only the type
 * names Option/Result in the prelude scope left their variants reachable
 * only via `Option.Some(...)`, which is not how the corpus/spec actually
 * uses them. */
static void add_variants_to_prelude(SnResolver *r, SnSymbol *type_sym) {
    SnScope *member_scope = sn_resolver_type_scope(r, type_sym->decl);
    if (!member_scope) {
        return;
    }
    for (size_t i = 0; i < member_scope->nbuckets; i++) {
        for (SnSymbol *sym = member_scope->buckets[i]; sym; sym = sym->next) {
            if (sym->kind == SN_SYM_VARIANT) {
                sn_scope_define(r->prelude_scope, sym->name, sym->kind, sym->decl,
                                sym->span);
            }
        }
    }
}

/* Copies every top-level TYPE declared by `package_name` into the prelude
 * scope. A name already present is left alone: the packages are folded in
 * source order below, so the first one to claim a name keeps it. */
static void add_package_types_to_prelude(SnResolver *r, const char *package_name) {
    SnScope *src = sn_resolver_package_scope(r, sn_intern_cstr(r->intern, package_name));
    if (!src) {
        return; /* not among the roots scanned this run — the names simply
                 * need an explicit import, which is not a crash */
    }
    for (size_t i = 0; i < src->nbuckets; i++) {
        for (SnSymbol *sym = src->buckets[i]; sym; sym = sym->next) {
            if (sym->kind != SN_SYM_TYPE) {
                continue;
            }
            if (sn_scope_lookup_local(r->prelude_scope, sym->name)) {
                continue;
            }
            sn_scope_define(r->prelude_scope, sym->name, sym->kind, sym->decl, sym->span);
        }
    }
}

void sn_resolver_build_prelude(SnResolver *r) {
    r->prelude_scope = (SnScope *)sn_arena_alloc(r->arena, sizeof(SnScope));
    sn_scope_init(r->prelude_scope, r->arena, NULL);

    /* The prelude's membership is already decided elsewhere in this repo, and
     * this mirrors that decision rather than inventing one:
     * crates/snovalang/src/modules/resolve.rs folds `builtin.datetime.DateTime`
     * and `builtin.types.Types` in unconditionally (BUILTIN_PRELUDE_PACKAGES)
     * and `builtin.collections.Collections` on demand
     * (COLLECTIONS_PRELUDE_PACKAGE), and
     * specs/20260711/builtin-prelude-types/ plus
     * tests/compile-pass/packages/builtin_prelude_no_import.snl state the
     * same three.
     *
     * snovac folds Collections in unconditionally instead of on demand: the
     * "only when referenced" rule exists there because folding it in injects
     * its SOURCE into the compiled bundle, which the frozen Stage 0
     * type-checker cannot handle for its generic methods. Here the prelude is
     * a scope, not a source fold — nothing is injected, so a name that is
     * never referenced simply is never looked up. What that package declares
     * is unchanged either way.
     *
     * Free functions are NOT folded in: `parseInt` lives in
     * `builtin.strings.Strings`, which no prelude list names, so calling it
     * without an import stays an error. */
    add_package_types_to_prelude(r, SN_PRELUDE_PACKAGE);
    add_package_types_to_prelude(r, "builtin.datetime.DateTime");
    add_package_types_to_prelude(r, "builtin.collections.Collections");

    /* Option/Result additionally publish their VARIANTS into the prelude. */
    SnScope *types_pkg =
        sn_resolver_package_scope(r, sn_intern_cstr(r->intern, SN_PRELUDE_PACKAGE));
    if (types_pkg) {
        const char *const WITH_VARIANTS[] = {"Option", "Result"};
        for (size_t i = 0; i < sizeof(WITH_VARIANTS) / sizeof(WITH_VARIANTS[0]); i++) {
            SnSymbol *sym =
                sn_scope_lookup_local(types_pkg, sn_intern_cstr(r->intern, WITH_VARIANTS[i]));
            if (sym) {
                add_variants_to_prelude(r, sym);
            }
        }
    }

    /* Ensure fundamental Option / Result variants are unconditionally available */
    static const char *const BUILTIN_VARIANTS[] = {"Some", "None", "Ok", "Err"};
    for (size_t i = 0; i < sizeof(BUILTIN_VARIANTS) / sizeof(BUILTIN_VARIANTS[0]); i++) {
        const char *vname = sn_intern_cstr(r->intern, BUILTIN_VARIANTS[i]);
        if (!sn_scope_lookup_local(r->prelude_scope, vname)) {
            SnDecl *vd = (SnDecl *)sn_arena_calloc(r->arena, sizeof(SnDecl));
            vd->kind = SN_DECL_VARIANT;
            vd->name = vname;
            if (strcmp(BUILTIN_VARIANTS[i], "Some") == 0 ||
                strcmp(BUILTIN_VARIANTS[i], "Ok") == 0 ||
                strcmp(BUILTIN_VARIANTS[i], "Err") == 0) {
                SnParam *p = (SnParam *)sn_arena_calloc(r->arena, sizeof(SnParam));
                p->name = sn_intern_cstr(r->intern, "value");
                sn_list_push(r->arena, &vd->params, p);
            }
            SnSymbol *sym = (SnSymbol *)sn_arena_calloc(r->arena, sizeof(SnSymbol));
            sym->kind = SN_SYM_VARIANT;
            sym->name = vname;
            sym->decl = vd;
            sn_scope_define(r->prelude_scope, vname, SN_SYM_VARIANT, vd, (SnSpan){0, 0, 0, 0});
        }
    }
}

/* ── name resolution ──────────────────────────────────────────────────────── */

#include "builtins.h"

static SnTypeRep *primitive_by_name(SnResolver *r, const char *iname) {
    /* `iname` is interned, so it is a stable C string. The catalog is the
     * same table the language server completes and highlights. */
    return sn_builtin_primitive_type(r->types, iname);
}

/* `Int`/`String`/... — accepted, mapped to the primitive, and warned about.
 * Returns NULL when `iname` is not one of these. Tried only after every real
 * lookup has failed, so a package that genuinely declares a type named `Int`
 * still wins.
 *
 * The first six are exactly the pairs
 * crates/snovalang/src/native/selfcheck/mod.rs warns about under SNOVA011.
 * The last four complete the set over the primitives snovac actually has a
 * tag for: `Float` alone accounts for 13 of the errors this gate attributes to
 * tests/compile-pass/data_class.snl, which writes `public let x: Float`.
 * The Rust table stops at six only because it is a hardcoded list, not because
 * the capitalized spelling means something else there — so the same advice is
 * given, rather than SNOVA0027 for a type the shipped `snova check` accepts. */
static SnTypeRep *legacy_primitive_alias(SnResolver *r, const char *iname,
                                          const char *name, SnSpan span) {
    static const char *const ALIASES[][2] = {
        {"String", "string"}, {"Int", "int"},   {"Long", "long"},
        {"Bool", "bool"},     {"Unit", "unit"}, {"Decimal", "decimal"},
        {"Float", "float"},   {"Double", "double"},
        {"Byte", "byte"},     {"Char", "char"},
    };
    for (size_t i = 0; i < sizeof(ALIASES) / sizeof(ALIASES[0]); i++) {
        if (iname != sn_intern_cstr(r->intern, ALIASES[i][0])) {
            continue;
        }
        sn_diag_emit(r->diag, SN_DIAG_WARNING, SNOVA_LEGACY_TYPE_SPELLING, span,
                    "Prefer `%s` over `%s` in native Snovalang.", ALIASES[i][1], name);
        return primitive_by_name(r, sn_intern_cstr(r->intern, ALIASES[i][1]));
    }
    return NULL;
}

SnSymbol *sn_resolve_ident(SnResolver *r, const char *current_package,
                            const SnScope *local, const SnDecl *enclosing_type,
                            const SnList *imports, const char *name, SnSpan span) {
    const char *iname = sn_intern_cstr(r->intern, name);

    if (local) {
        SnSymbol *sym = sn_scope_lookup(local, iname);
        if (sym) {
            return sym;
        }
    }

    if (enclosing_type) {
        /* Single-inheritance walk (plan.md's ratified ambiguity 5). The
         * supertype name is looked up in the current package's scope only
         * — no corpus evidence of cross-package inheritance to test against;
         * a `depth` guard stands in for cycle detection since supertypes
         * aren't validated acyclic anywhere before this runs. */
        const SnDecl *cur = enclosing_type;
        int depth = 0;
        while (cur && depth < 64) {
            SnScope *ms = sn_resolver_type_scope(r, cur);
            if (ms) {
                SnSymbol *sym = sn_scope_lookup_local(ms, iname);
                if (sym) {
                    return sym;
                }
            }
            if (cur->supertypes.len == 0) {
                break;
            }
            SnType *super_ty = SN_LIST_AT(cur->supertypes, SnType, 0);
            if (super_ty->kind != SN_TYPE_NAME) {
                break;
            }
            const char *super_name = sn_intern_cstr(r->intern, super_ty->name);
            SnScope *pkg_scope = sn_resolver_package_scope(r, current_package);
            SnSymbol *super_sym =
                pkg_scope ? sn_scope_lookup_local(pkg_scope, super_name) : NULL;
            cur = (super_sym && super_sym->kind == SN_SYM_TYPE) ? super_sym->decl : NULL;
            depth++;
        }
    }

    SnScope *pkg_scope = sn_resolver_package_scope(r, current_package);
    if (pkg_scope) {
        SnSymbol *sym = sn_scope_lookup_local(pkg_scope, iname);
        if (sym) {
            return sym;
        }
    }

    if (imports) {
        for (size_t i = 0; i < imports->len; i++) {
            const char *imp_pkg = SN_LIST_AT(*imports, const char, i);
            SnScope *imp_scope = resolve_import_scope(r, imp_pkg);
            if (!imp_scope) {
                continue;
            }
            SnSymbol *sym = sn_scope_lookup_local(imp_scope, iname);
            if (sym) {
                return sym;
            }
        }
    }

    if (r->prelude_scope) {
        SnSymbol *sym = sn_scope_lookup_local(r->prelude_scope, iname);
        if (sym) {
            return sym;
        }
    }

    sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_UNDECLARED_NAME, span,
                "`%s` is not defined in this scope", name);
    return NULL;
}

