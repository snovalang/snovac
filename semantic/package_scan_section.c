#include "package.h"
#include "package_internal.h"
#include "driver_utils.h"

#include <stdio.h>
#include <string.h>

#define SN_PKG_PATH_MAX 4096

/* Scans one (package? import*) section starting at *pos and advances past
 * it, stopping at whatever ends the header — a declaration keyword, another
 * `package` (the next section), or EOF. Returns 1 if the section is usable
 * (had a package name), 0 otherwise (nothing added to the graph for it). */
int sn_pkggraph_scan_section(SnPackageGraph *g, const SnDiagFile *file,
                        const SnTokenVec *toks, size_t *pos) {
    SnArena *a = g->arena;
    SnPackageFile *pf = (SnPackageFile *)sn_arena_calloc(a, sizeof(SnPackageFile));
    pf->path = sn_arena_strndup(a, file->path, strlen(file->path));
    pf->src = file->src;
    pf->src_len = file->src_len;

    if (toks->data[*pos].kind == SN_TOK_PACKAGE) {
        (*pos)++;
        SnSpan span;
        const char *name = sn_pkggraph_scan_qualified(a, toks, pos, &span);
        if (name) {
            pf->package = sn_intern_cstr(g->intern, name);
            pf->package_span = span;
        }
        if (toks->data[*pos].kind == SN_TOK_SEMI) {
            (*pos)++;
        }
    }

    while (toks->data[*pos].kind == SN_TOK_IMPORT) {
        (*pos)++;
        SnSpan span;
        const char *name = sn_pkggraph_scan_qualified(a, toks, pos, &span);
        if (!name) {
            break; /* malformed import header; P1 owns rejecting this file at
                     * the full-parse gate, not this scanner */
        }
        if (toks->data[*pos].kind == SN_TOK_SEMI) {
            (*pos)++;
        }
        sn_list_push(a, &pf->imports,
                     (void *)sn_intern_cstr(g->intern, canonical_import_name(name)));
        SnSpan *sp = (SnSpan *)sn_arena_alloc(a, sizeof(SnSpan));
        *sp = span;
        sn_list_push(a, &pf->import_spans, sp);
    }

    if (!pf->package) {
        return 0;
    }

    /* Enforce physical directory path matching package declaration relative to nearest mod.sno checkpoint */
    char checkpoint_expected[SN_PKG_PATH_MAX] = {0};
    if (find_nearest_manifest_rel_pkg(file->path, checkpoint_expected, sizeof(checkpoint_expected))) {
        /* Check if declared package matches checkpoint_expected.
         * Allow case-insensitive comparison or matching suffix for module prefixing. */
        char normalized_declared[SN_PKG_PATH_MAX];
        snprintf(normalized_declared, sizeof(normalized_declared), "%s", pf->package);
        for (char *c = normalized_declared; *c; c++) {
            if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + ('a' - 'A'));
        }
        char normalized_expected[SN_PKG_PATH_MAX];
        snprintf(normalized_expected, sizeof(normalized_expected), "%s", checkpoint_expected);
        for (char *c = normalized_expected; *c; c++) {
            if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + ('a' - 'A'));
        }

        /* Check direct match or suffix match */
        size_t decl_len = strlen(normalized_declared);
        size_t exp_len = strlen(normalized_expected);
        int matched = 0;
        if (strcmp(normalized_declared, normalized_expected) == 0) {
            matched = 1;
        } else if (decl_len > exp_len && normalized_declared[decl_len - exp_len - 1] == '.' &&
                   strcmp(normalized_declared + (decl_len - exp_len), normalized_expected) == 0) {
            matched = 1;
        }

        if (!matched) {
            sn_diag_emit(g->diag, SN_DIAG_ERROR, SNOVA_PKG_PATH_MISMATCH, pf->package_span,
                         "package declaration '%s' does not match physical directory path relative to nearest 'mod.sno' checkpoint (expected '%s')",
                         pf->package, checkpoint_expected);
        }
    }

    SnPackageNode *node = sn_pkggraph_find_or_create(g, pf->package);
    const char *p1 = strrchr(pf->path, '/');
    if (!p1) p1 = strrchr(pf->path, '\\');
    const char *name1 = p1 ? p1 + 1 : pf->path;
    for (SnPackageFile *existing = node->files; existing; existing = existing->next) {
        if (strcmp(pf->path, existing->path) == 0) {
            return 1;
        }
        const char *p2 = strrchr(existing->path, '/');
        if (!p2) p2 = strrchr(existing->path, '\\');
        const char *name2 = p2 ? p2 + 1 : existing->path;
        if (strcmp(name1, name2) == 0) {
            return 1;
        }
    }
    pf->next = node->files;
    node->files = pf;
    g->file_count++;
    return 1;
}

