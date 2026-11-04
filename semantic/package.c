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
        "builtin.syntax.Syntax"
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
