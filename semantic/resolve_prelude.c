#include "resolve.h"

#include <string.h>

#define SN_PRELUDE_PACKAGE "builtin.core.types.values"

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
     * and `builtin.core.types.values` in unconditionally (BUILTIN_PRELUDE_PACKAGES)
     * and `builtin.collections.collections.types` on demand
     * (COLLECTIONS_PRELUDE_PACKAGE), and
     * specs/20260711/builtin-prelude-types/ plus
     * tests/compile-pass/packages/builtin_prelude_no_import.snova state the
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
     * `builtin.text.parsing.results`, which no prelude list names, so calling it
     * without an import stays an error. */
    add_package_types_to_prelude(r, SN_PRELUDE_PACKAGE);
    add_package_types_to_prelude(r, "builtin.datetime.DateTime");
    add_package_types_to_prelude(r, "builtin.collections.collections.types");

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
