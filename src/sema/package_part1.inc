#include "package.h"
#include "driver_utils.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "lex.h"

#define SN_PKG_PATH_MAX 4096

void sn_pkggraph_init(SnPackageGraph *g, SnArena *a, SnInternTable *it,
                       SnDiagSink *diag) {
    g->arena = a;
    g->intern = it;
    g->diag = diag;
    g->nodes = NULL;
    g->node_count = 0;
    g->file_count = 0;
    memset(&g->native_manifest, 0, sizeof(g->native_manifest));
    g->native_manifest_loaded = 0;
    sn_pkggraph_load_native_manifest(g, NULL);
}

void sn_pkggraph_load_native_manifest(SnPackageGraph *g, const char *builtin_dir) {
    if (builtin_dir && builtin_dir[0]) {
        char path[SN_PKG_PATH_MAX];
        snprintf(path, sizeof(path), "%s/native-packages.list", builtin_dir);
        FILE *f = fopen(path, "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                size_t n = strlen(line);
                while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                                 line[n - 1] == ' ' || line[n - 1] == '\t')) {
                    line[--n] = '\0';
                }
                if (n == 0 || line[0] == '#') {
                    continue;
                }
                const char *name = sn_intern_cstr(g->intern, line);
                sn_list_push(g->arena, &g->native_manifest, (void *)name);
            }
            fclose(f);
            g->native_manifest_loaded = 1;
            return;
        }
    }
    // Embedded default native packages in pure C
    static const char *const DEFAULT_NATIVES[] = {
        "builtin.metadata.Documented",
        "builtin.syntax.Syntax",
        "builtin.console.Console"
    };
    for (size_t i = 0; i < sizeof(DEFAULT_NATIVES) / sizeof(DEFAULT_NATIVES[0]); i++) {
        const char *name = sn_intern_cstr(g->intern, DEFAULT_NATIVES[i]);
        sn_list_push(g->arena, &g->native_manifest, (void *)name);
    }
    g->native_manifest_loaded = 1;
}

SnPackageNode *sn_pkggraph_find(const SnPackageGraph *g, const char *name) {
    for (SnPackageNode *n = g->nodes; n; n = n->next) {
        if (n->name == name) {
            return n;
        }
    }
    return NULL;
}

static SnPackageNode *find_or_create(SnPackageGraph *g, const char *name) {
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
 * parse_type.c's sn_parse_qualified() (name ('.' name)*, where a "name" is any
 * identifier or contextual keyword — plan.md's fact 3) but works directly off
 * a SnTokenVec instead of the parser's private `P` state, so this module
 * stays decoupled from parse_internal.h. */

static const char *scan_qualified(SnArena *arena, const SnTokenVec *toks,
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
 * NOT repeat its file stem (`builtin/metadata/Documented.snl` declares
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
static const char *canonical_import_name(const char *name) {
    if (!name) return name;
    static const char PREFIX[] = "builtin.metadata.";
    if (strncmp(name, PREFIX, sizeof(PREFIX) - 1u) == 0) {
        return "builtin.metadata";
    }
    return name;
}

static int find_nearest_manifest_rel_pkg(const char *file_path, char *out, size_t out_sz) {
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
        static const char *const manifest_names[] = {"mod.sns", "snova.sns"};
        for (size_t mi = 0; mi < sizeof(manifest_names) / sizeof(manifest_names[0]); mi++) {
            snprintf(mod_cand, sizeof(mod_cand), "%s/%s", check_dir, manifest_names[mi]);
            struct stat st;
            if (stat(mod_cand, &st) == 0 && S_ISREG(st.st_mode)) {
                snprintf(manifest_dir, sizeof(manifest_dir), "%s", check_dir);
                found_manifest = 1;
                break;
            }
        }
        if (found_manifest) {
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

/* Scans one (package? import*) section starting at *pos and advances past
 * it, stopping at whatever ends the header — a declaration keyword, another
 * `package` (the next section), or EOF. Returns 1 if the section is usable
 * (had a package name), 0 otherwise (nothing added to the graph for it). */
static int scan_section(SnPackageGraph *g, const SnDiagFile *file,
                        const SnTokenVec *toks, size_t *pos) {
    SnArena *a = g->arena;
    SnPackageFile *pf = (SnPackageFile *)sn_arena_calloc(a, sizeof(SnPackageFile));
    pf->path = sn_arena_strndup(a, file->path, strlen(file->path));
    pf->src = file->src;
    pf->src_len = file->src_len;

    if (toks->data[*pos].kind == SN_TOK_PACKAGE) {
        (*pos)++;
        SnSpan span;
        const char *name = scan_qualified(a, toks, pos, &span);
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
        const char *name = scan_qualified(a, toks, pos, &span);
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

    /* Enforce physical directory path matching package declaration relative to nearest mod.sns checkpoint */
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
                         "package declaration '%s' does not match physical directory path relative to nearest 'mod.sns' or 'snova.sns' manifest (expected '%s')",
                         pf->package, checkpoint_expected);
        }
    }

    SnPackageNode *node = find_or_create(g, pf->package);
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

/* A file may contain more than one `package` section — parse.c documents
 * this precedent (tests/compile-fail/visibility_internal_cross_package.snl
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

