#include "package.h"
#include "package_internal.h"
#include "driver_utils.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "lex.h"

#define SN_PKG_PATH_MAX 4096

SnPackageNode *sn_pkggraph_find_or_create(SnPackageGraph *g, const char *name) {
    SnPackageNode *n = sn_pkggraph_find(g, name);
    if (n) {
        return n;
    }
    n = (SnPackageNode *)sn_arena_calloc(g->arena, sizeof(SnPackageNode));
    n->name = name;
    n->next = g->nodes;
    g->nodes = n;
    g->node_count++;
    return n;
}

/* ── header-only scan ─────────────────────────────────────────────────────
 *
 * Reuses the real lexer (sn_lex) but not the real parser: header parsing only
 * needs `package <qualified>` once, then `import <qualified>` repeated, which
 * is far narrower than the parser's declaration grammar. Mirrors
 * parse_type.c's parse_qualified() (name ('.' name)*, where a "name" is any
 * identifier or contextual keyword — plan.md's fact 3) but works directly off
 * a SnTokenVec instead of the parser's private `P` state, so this module
 * stays decoupled from parse_internal.h. */

const char *sn_pkggraph_scan_qualified(SnArena *arena, const SnTokenVec *toks,
                                   size_t *pos, SnSpan *span_out) {
    const SnToken *t0 = &toks->data[*pos];
    if (t0->kind != SN_TOK_IDENT && !sn_tok_is_keyword(t0->kind)) {
        return NULL;
    }

    char buf[512];
    size_t n = strlen(t0->text);
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1u;
    }
    memcpy(buf, t0->text, n);

    SnSpan span = t0->span;
    size_t p = *pos + 1u;

    while (toks->data[p].kind == SN_TOK_DOT &&
           (toks->data[p + 1u].kind == SN_TOK_IDENT ||
            sn_tok_is_keyword(toks->data[p + 1u].kind))) {
        p++; /* consume '.' */
        const SnToken *part = &toks->data[p];
        size_t pl = strlen(part->text);
        if (n + 1u + pl < sizeof(buf)) {
            buf[n++] = '.';
            memcpy(buf + n, part->text, pl);
            n += pl;
        }
        span.len = (part->span.offset + part->span.len) - span.offset;
        p++;
    }
    buf[n] = '\0';

    *pos = p;
    if (span_out) {
        *span_out = span;
    }
    return sn_arena_strndup(arena, buf, n);
}

/* `builtin.metadata` is the one bootstrap package whose declared name does
 * NOT repeat its file stem (`builtin/metadata/Documented.snova` declares
 * `package builtin.metadata`, where every other builtin declares
 * `builtin.<lower(Stem)>.<Stem>`). Its importers still spell it
 * `import builtin.metadata.Documented` — Types/DateTime/Collections all do,
 * and the file's own doc comment instructs it. That is deliberate, not a
 * corpus typo: the same exception is hardcoded in
 * `crates/snovalang/src/packages.rs::canonical_package_name` and in
 * `tools/launcher/snova.c::builtin_stdlib_package_name`, because `Documented`
 * is also one of the two native packages in packages_gen.c.
 *
 * Folding the import back to the declared name here (rather than aliasing the
 * node) keeps the exception in one place: link, package scopes and import
 * lookup all then match on the single name the file actually declares. */
const char *canonical_import_name(const char *name) {
    if (!name) return name;
    static const char PREFIX[] = "builtin.metadata.";
    if (strncmp(name, PREFIX, sizeof(PREFIX) - 1u) == 0) {
        return "builtin.metadata";
    }
    return name;
}

int find_nearest_manifest_rel_pkg(const char *file_path, char *out, size_t out_sz) {
    if (!file_path || !file_path[0]) return 0;
    char norm_path[SN_PKG_PATH_MAX];
    snprintf(norm_path, sizeof(norm_path), "%s", file_path);
    for (char *c = norm_path; *c; c++) {
        if (*c == '\\') *c = '/';
    }

    char cur_dir[SN_PKG_PATH_MAX];
    const char *last_slash = strrchr(norm_path, '/');
    if (!last_slash) return 0;
    size_t dlen = (size_t)(last_slash - norm_path);
    if (dlen >= sizeof(cur_dir)) dlen = sizeof(cur_dir) - 1;
    memcpy(cur_dir, norm_path, dlen);
    cur_dir[dlen] = '\0';

    char check_dir[SN_PKG_PATH_MAX];
    snprintf(check_dir, sizeof(check_dir), "%s", cur_dir);

    char manifest_dir[SN_PKG_PATH_MAX] = {0};
    int found_manifest = 0;

    for (int depth = 0; depth < 32; depth++) {
        char mod_cand[SN_PKG_PATH_MAX];
        snprintf(mod_cand, sizeof(mod_cand), "%s/mod.sno", check_dir);
        struct stat st;
        if (stat(mod_cand, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(manifest_dir, sizeof(manifest_dir), "%s", check_dir);
            found_manifest = 1;
            break;
        }
        char *p_slash = strrchr(check_dir, '/');
        if (!p_slash || p_slash == check_dir) break;
        *p_slash = '\0';
    }

    if (!found_manifest) return 0;

    /* Relative path between manifest_dir and cur_dir */
    size_t mlen = strlen(manifest_dir);
    if (strlen(cur_dir) < mlen) return 0;
    const char *rel = cur_dir + mlen;
    if (*rel == '/') rel++;
    if (!*rel) return 0;

    /* If rel starts with "src/", skip "src/" because src is conventional root */
    if (strncmp(rel, "src/", 4) == 0) rel += 4;
    else if (strcmp(rel, "src") == 0) return 0;

    /* Convert slashes to dots */
    char pkg_buf[SN_PKG_PATH_MAX];
    snprintf(pkg_buf, sizeof(pkg_buf), "%s", rel);
    for (char *c = pkg_buf; *c; c++) {
        if (*c == '/' || *c == '\\') *c = '.';
    }
    snprintf(out, out_sz, "%s", pkg_buf);
    return 1;
}

/* A file may contain more than one `package` section — parse.c documents
 * this precedent (tests/compile-fail/visibility_internal_cross_package.snova
 * puts a provider and a consumer package in one file to test cross-package
 * visibility without an import). Each section is scanned and grouped
 * independently; only the tokens between sections (declaration bodies) are
 * skipped, by fast-forwarding to the next `package`/`import`/EOF. */
static void skip_to_next_header_token(const SnTokenVec *toks, size_t *pos) {
    while (toks->data[*pos].kind != SN_TOK_EOF &&
           toks->data[*pos].kind != SN_TOK_PACKAGE &&
           toks->data[*pos].kind != SN_TOK_IMPORT) {
        (*pos)++;
    }
}

void sn_pkggraph_scan_header(SnPackageGraph *g, const char *path, const char *src,
                         size_t len) {
    SnArena *a = g->arena;
    char norm_path[SN_PKG_PATH_MAX];
    normalize_path_into(path, norm_path, sizeof(norm_path));
    SnDiagFile self = {norm_path, src, len};
    SnDiagFile outer = sn_diag_set_file(g->diag, self);

    SnTokenVec toks;
    memset(&toks, 0, sizeof(toks));
    sn_lex(a, g->diag, src, len, &toks); /* lexical errors reported via diag already */

    size_t pos = 0;
    int any_section = 0;
    while (toks.data[pos].kind != SN_TOK_EOF) {
        if (toks.data[pos].kind == SN_TOK_PACKAGE) {
            any_section |= sn_pkggraph_scan_section(g, &self, &toks, &pos);
            continue;
        }
        if (toks.data[pos].kind == SN_TOK_IMPORT) {
            /* imports with no preceding `package` in this section: still
             * scan them (advances pos correctly) but they attach to nothing,
             * matching parse.c which only starts recording once a package is
             * seen. */
            (void)sn_pkggraph_scan_section(g, &self, &toks, &pos);
            continue;
        }
        skip_to_next_header_token(&toks, &pos);
    }

    if (!any_section) {
        size_t path_len = strlen(path);
        int is_sno = (path_len >= 4u && strcmp(path + path_len - 4u, ".sno") == 0);
        if (is_sno) {
            /* .sno files are Snovalang script files; an explicit package header is optional.
             * They default to the implicit "main" script package. */
            SnPackageFile *pf = (SnPackageFile *)sn_arena_calloc(a, sizeof(SnPackageFile));
            pf->path = sn_arena_strndup(a, path, strlen(path));
            pf->src = src;
            pf->src_len = len;
            pf->package = sn_intern_cstr(g->intern, "main");

            size_t p = 0;
            while (toks.data[p].kind != SN_TOK_EOF) {
                if (toks.data[p].kind == SN_TOK_IMPORT) {
                    p++;
                    SnSpan span;
                    const char *name = sn_pkggraph_scan_qualified(a, &toks, &p, &span);
                    if (name) {
                        if (toks.data[p].kind == SN_TOK_SEMI) p++;
                        sn_list_push(a, &pf->imports,
                                     (void *)sn_intern_cstr(g->intern, canonical_import_name(name)));
                        SnSpan *sp = (SnSpan *)sn_arena_alloc(a, sizeof(SnSpan));
                        *sp = span;
                        sn_list_push(a, &pf->import_spans, sp);
                    }
                } else {
                    p++;
                }
            }

            SnPackageNode *node = sn_pkggraph_find_or_create(g, pf->package);
            pf->next = node->files;
            node->files = pf;
            g->file_count++;
        } else {
            SnSpan zero;
            memset(&zero, 0, sizeof(zero));
            zero.line = 1;
            zero.col = 1;
            sn_diag_emit(g->diag, SN_DIAG_ERROR, SNOVA_MISSING_PACKAGE_DECL, zero,
                         "%s: file has no `package` declaration", path);
        }
    }

    sn_diag_set_file(g->diag, outer);
}

