#include "resolve.h"
#include "resolve_internal.h"

#include <string.h>

SnSymbol *sn_resolve_member_path(SnResolver *r, const char *current_package,
                                  const SnScope *local, const SnDecl *enclosing_type,
                                  const SnList *imports, const SnList *segments,
                                  SnSpan span) {
    if (segments->len == 0) {
        return NULL;
    }

    const char *head = SN_LIST_AT(*segments, const char, 0);
    const char *head_i = sn_intern_cstr(r->intern, head);

    /* 1. try as a value first — variable/type-member/package-level, in that
     * order, WITHOUT emitting a diagnostic on failure (a package prefix is
     * still a legitimate possibility; only step 2 failing is a real error). */
    if (local) {
        SnSymbol *sym = sn_scope_lookup(local, head_i);
        if (sym) {
            return sym; /* rest of `segments`: check.c's job, see resolve.h note 4 */
        }
    }
    if (enclosing_type) {
        SnScope *ms = sn_resolver_type_scope(r, enclosing_type);
        if (ms) {
            SnSymbol *sym = sn_scope_lookup_local(ms, head_i);
            if (sym) {
                return sym;
            }
        }
    }
    SnScope *pkg_scope = sn_resolver_package_scope(r, current_package);
    if (pkg_scope) {
        SnSymbol *sym = sn_scope_lookup_local(pkg_scope, head_i);
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
            SnSymbol *sym = sn_scope_lookup_local(imp_scope, head_i);
            if (sym) {
                return sym;
            }
        }
    }
    if (r->prelude_scope) {
        SnSymbol *sym = sn_scope_lookup_local(r->prelude_scope, head_i);
        if (sym) {
            return sym;
        }
    }

    /* 2. try as a package prefix: greedily match the longest run of leading
     * segments that names a real package in the graph, then resolve the
     * next segment (if any) as a top-level symbol of that package. */
    char buf[512];
    size_t n = 0;
    size_t matched_upto = 0;
    const char *matched_pkg = NULL;
    for (size_t i = 0; i < segments->len; i++) {
        const char *seg = SN_LIST_AT(*segments, const char, i);
        size_t sl = strlen(seg);
        if (i > 0 && n + 1u < sizeof(buf)) {
            buf[n++] = '.';
        }
        if (n + sl < sizeof(buf)) {
            memcpy(buf + n, seg, sl);
            n += sl;
        }
        buf[n] = '\0';
        const char *candidate = sn_intern_cstr(r->intern, buf);
        if (sn_pkggraph_find(r->graph, candidate)) {
            matched_upto = i + 1u;
            matched_pkg = candidate;
        }
    }

    /* 2.5. try as an imported package alias: if `head` matches the last
     * segment of an imported package, treat it as that package. `imp_pkg`
     * itself is often PACKAGE.SYMBOL rather than a real package name (the
     * common `import a.b.RealSymbol` spelling), so the scope lookup below
     * needs the real, collected package name — resolve_import_package_name
     * finds it the same way resolve_import_scope does elsewhere in this
     * file. */
    if (!matched_pkg && imports) {
        for (size_t i = 0; i < imports->len; i++) {
            const char *imp_pkg = SN_LIST_AT(*imports, const char, i);
            const char *last = strrchr(imp_pkg, '.');
            const char *stem = last ? last + 1 : imp_pkg;
            if (strcmp(stem, head) == 0) {
                matched_pkg = resolve_import_package_name(r, imp_pkg);
                if (matched_pkg) {
                    matched_upto = 1;
                    break;
                }
            }
        }
    }

    if (matched_pkg) {
        if (matched_upto < segments->len) {
            const char *next_seg = SN_LIST_AT(*segments, const char, matched_upto);
            const char *next_i = sn_intern_cstr(r->intern, next_seg);
            SnScope *matched_scope = sn_resolver_package_scope(r, matched_pkg);
            SnSymbol *sym =
                matched_scope ? sn_scope_lookup_local(matched_scope, next_i) : NULL;
            if (sym) {
                return sym;
            }
            sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_UNDECLARED_NAME, span,
                        "`%s` is not defined in this scope", next_seg);
            return NULL;
        }
        /* the whole path names a package with nothing left to resolve to a
         * symbol; not itself an error at this layer. */
        return NULL;
    }

    sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_UNDECLARED_NAME, span,
                "`%s` is not defined in this scope", head);
    return NULL;
}
