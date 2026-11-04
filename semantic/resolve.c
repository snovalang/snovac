#include "resolve.h"
#include "resolve_internal.h"

#include <string.h>

#include "lex.h"
#include "parse.h"

/* Option/Result's real package (see resolve.h note 3). */

/* ── declaration collection ───────────────────────────────────────────────── */

static char *read_whole_file(SnArena *a, const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)sn_arena_alloc(a, (size_t)n + 1u);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

/* sn_scope_define() plus the provenance stamp — see SnSymbol.origin. Every
 * definition made while walking a file must go through here, or a diagnostic
 * raised for that symbol later has no file to be measured against. */
static SnSymbol *define_with_origin(SnResolver *r, SnScope *scope, const char *name,
                                     SnSymbolKind kind, const SnDecl *decl,
                                     SnSpan span) {
    SnSymbol *sym = sn_scope_define(scope, name, kind, decl, span);
    if (sym) {
        sym->origin = r->current_origin;
    }
    return sym;
}

static void collect_member(SnResolver *r, SnScope *member_scope, const SnDecl *owner,
                            const SnDecl *m, const char *via_extension) {
    /* `func` belongs at the top level or in an `extension` body; inside a
     * class/struct/interface it must be `method`. Checked before
     * member_symbol_kind so it is reported whether or not a top-level-shaped
     * declaration has a member symbol kind at all. */
    if (!via_extension && m->kind == SN_DECL_FUNC) {
        sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_FUNC_IN_TYPE_BODY, m->span,
                     "`func` is not valid inside a class body -- use `method`.");
    }

    SnSymbolKind msk;
    if (!member_symbol_kind(m->kind, &msk)) {
        return;
    }
    const char *mname = sn_intern_cstr(r->intern, m->name);

    /* A private backing field and its same-named public accessor method
     * (`private let path: Path` + `method path(): Path { return path }`,
     * both real, in builtin/FileSystem.snova's `File`) is a legitimate,
     * idiomatic pattern — not a collision. Found while running this
     * collector against the real corpus (2026-07-25); not one of plan.md
     * §7's ratified ambiguities, so treated as its own narrow rule: a
     * FIELD/METHOD pair sharing a name is allowed, and whichever was
     * collected first wins the name in the member scope (the practical
     * effect: within-class references to the bare name still resolve to
     * *something* real; disambiguating "the field" from "a call to the
     * method" needs call-site syntax, which is check.c's job, not this
     * scope table's). Any other same-kind collision remains a real error. */
    SnSymbol *existing = sn_scope_lookup_local(member_scope, mname);
    if (existing && ((existing->kind == SN_SYM_FIELD && msk == SN_SYM_METHOD) ||
                      (existing->kind == SN_SYM_METHOD && msk == SN_SYM_FIELD))) {
        return;
    }

    SnSymbol *msym = define_with_origin(r, member_scope, mname, msk, m, m->span);
    if (!msym) {
        SnSymbol *existing_m = sn_scope_lookup_local(member_scope, mname);
        if (existing_m && existing_m->kind == msk &&
            existing_m->origin && r->current_origin &&
            existing_m->origin->path && r->current_origin->path &&
            strcmp(existing_m->origin->path, r->current_origin->path) != 0) {
            return;
        }
        if (via_extension) {
            sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_DUPLICATE_DECL, m->span,
                         "`%s` is already declared in `%s` (via extension)",
                         m->name, owner->name);
        } else {
            sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_DUPLICATE_DECL, m->span,
                         "`%s` is already declared in `%s`", m->name, owner->name);
        }
    }
}

typedef struct ExtEntry ExtEntry;
struct ExtEntry {
    const SnDecl *decl; /* SN_DECL_EXTENSION */
    const char *package;
    const SnDiagFile *origin; /* file the extension block was parsed from */
    ExtEntry *next;
};

/* Merges one `extension` block's members into the same-package type it
 * targets. Silently does nothing when the target isn't a type of that package
 * — there is no corpus evidence of cross-package extension, so the whole
 * graph is not searched for one. */
static void collect_extension_members(SnResolver *r, const ExtEntry *ee) {
    const char *target_name = sn_intern_cstr(r->intern, ee->decl->name);
    SnScope *pkg_scope = sn_resolver_package_scope(r, ee->package);
    SnSymbol *target_sym =
        pkg_scope ? sn_scope_lookup_local(pkg_scope, target_name) : NULL;
    if (!target_sym || target_sym->kind != SN_SYM_TYPE) {
        return;
    }
    SnScope *member_scope = sn_resolver_type_scope(r, target_sym->decl);
    if (!member_scope) {
        return;
    }
    for (size_t j = 0; j < ee->decl->members.len; j++) {
        SnDecl *m = SN_LIST_AT(ee->decl->members, SnDecl, j);
        collect_member(r, member_scope, ee->decl, m, ee->decl->name);
    }
}

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
 * import — builtin/Types.snova's own doc comment says so explicitly
 * ("construction (Some(v), None, Ok(v), Err(e))... is usable in every
 * Snovalang program with no import"). Found while testing variant
 * construction (`Some(1)`) against the checker: registering only the type
 * names Option/Result in the prelude scope left their variants reachable
 * only via `Option.Some(...)`, which is not how the corpus/spec actually
 * uses them. */
