#include "package.h"
#include "package_internal.h"
#include "driver_utils.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "lex.h"

#define SN_PKG_PATH_MAX 4096

/* ── directory walk ───────────────────────────────────────────────────────
 *
 * POSIX (dirent.h/sys/stat.h): matches the project's current platform
 * support (arm64 macOS primary, Linux backend planned — see
 * specs/20260719/snovac-c-toolchain/llm.md fact 6/7). No Windows path exists
 * anywhere else in snovac/ yet either. */

static char *read_source_file(SnArena *a, const char *path, size_t *out_len) {
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

static int has_snova_suffix(const char *path) {
    size_t n = strlen(path);
    if (n >= 6u && strcmp(path + n - 6u, ".snova") == 0) {
        return 1;
    }
    if (n >= 4u && strcmp(path + n - 4u, ".sno") == 0) {
        const char *slash = strrchr(path, '/');
        const char *filename = slash ? slash + 1 : path;
        if (strcmp(filename, "mod.sno") == 0 || strcmp(filename, "snova.sno") == 0) {
            return 0;
        }
        return 1;
    }
    return 0;
}

/* Scans exactly one file, unlike sn_pkggraph_scan_root() which recursively
 * pulls in every `*.snova` under a directory — needed by callers (the CLI's
 * `check` command) that want ONE file's own declarations without silently
 * absorbing every sibling fixture in the same directory. Returns 1 if the
 * file was read and header-scanned, 0 if it couldn't be opened. */
int sn_pkggraph_scan_single_file(SnPackageGraph *g, const char *path) {
    size_t len = 0;
    char *src = read_source_file(g->arena, path, &len);
    if (!src) {
        return 0;
    }
    sn_pkggraph_scan_header(g, path, src, len);
    return 1;
}

size_t sn_pkggraph_scan_root(SnPackageGraph *g, const char *root) {
    size_t count = 0;
    DIR *d = opendir(root);
    if (!d) {
        /* A missing/unreadable root is a configuration problem for whoever
         * assembles the root list (snova.toml, CLI args) — not this
         * function's job to diagnose. */
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char path[SN_PKG_PATH_MAX];
        int written = snprintf(path, sizeof(path), "%s/%s", root, ent->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            continue; /* path too long to represent faithfully; skip rather
                       * than silently scan a truncated (wrong) path */
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            count += sn_pkggraph_scan_root(g, path);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !has_snova_suffix(path)) {
            continue;
        }

        count++;
        size_t flen = 0;
        char *src = read_source_file(g->arena, path, &flen);
        if (!src) {
            continue;
        }
        sn_pkggraph_scan_header(g, path, src, flen);
    }
    closedir(d);
    return count;
}

size_t sn_pkggraph_scan_root_fallback(SnPackageGraph *g, const char *root) {
    size_t count = 0;
    DIR *d = opendir(root);
    if (!d) {
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char path[SN_PKG_PATH_MAX];
        int written = snprintf(path, sizeof(path), "%s/%s", root, ent->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            count += sn_pkggraph_scan_root_fallback(g, path);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !has_snova_suffix(path)) {
            continue;
        }

        size_t flen = 0;
        char *src = read_source_file(g->arena, path, &flen);
        if (!src) {
            continue;
        }

        SnTokenVec toks;
        sn_lex(g->arena, g->diag, src, flen, &toks);
        size_t pos = 0;
        if (toks.len > 0 && toks.data[0].kind == SN_TOK_PACKAGE) {
            pos++;
            SnSpan span;
            const char *pname = sn_pkggraph_scan_qualified(g->arena, &toks, &pos, &span);
            if (pname) {
                const char *interned = sn_intern_cstr(g->intern, pname);
                SnPackageNode *existing = sn_pkggraph_find(g, interned);
                if (existing && existing->files) {
                    if (strncmp(existing->files->path, root, strlen(root)) != 0) {
                        continue;
                    }
                }
            }
        }

        count++;
        sn_pkggraph_scan_header(g, path, src, flen);
    }
    closedir(d);
    return count;
}
