/* project.h — project discovery and multi-file roots scanning. */
#ifndef SNOVAC_PROJECT_H
#define SNOVAC_PROJECT_H

#include <stddef.h>
#include "arena.h"
#include "driver_utils.h"
#include "package.h"

typedef struct {
    char source_root[SNOVAC_PATH_MAX]; /* always set */
    char manifest_dir[SNOVAC_PATH_MAX]; /* directory containing manifest, if has_manifest */
    char deps_root[SNOVAC_PATH_MAX];   /* "" when there is nothing vendored */
    char cache_root[SNOVAC_PATH_MAX];  /* offline precompiled binaries cache */
    int has_manifest;
    int use_offline_cache;
} SnProject;

void sn_project_discover(const char *path, SnProject *out);
void sn_project_set_offline_cache(SnProject *proj, const char *cache_dir);
int sn_project_find_builtin_root(const char *source_root, char *out, size_t out_sz);
size_t sn_project_scan_roots(SnPackageGraph *graph, const SnProject *proj);
SnList sn_project_aggregate_imports(SnArena *a, SnPackageNode *node);

#endif /* SNOVAC_PROJECT_H */
