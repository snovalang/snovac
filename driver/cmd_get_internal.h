#ifndef SNOVAC_CMD_GET_INTERNAL_H
#define SNOVAC_CMD_GET_INTERNAL_H
#include "driver_utils.h"
#include <stddef.h>
typedef struct SnManifestDep { char raw[512]; char from[256]; char module[256]; char version[64]; int is_edge; struct SnManifestDep *next; } SnManifestDep;
typedef struct SnManifest { char module_name[256]; char module_url[512]; char snova_version[64]; int is_root; SnManifestDep *direct; SnManifestDep *indirect; int direct_count; int indirect_count; } SnManifest;
typedef enum {
    DEP_STATE_UNVISITED = 0,
    DEP_STATE_VISITING,
    DEP_STATE_VISITED
} DepVisitState;

typedef struct DepNode {
    char id[256];
    char url[512];
    char version[64];
    char local_dir[SNOVAC_PATH_MAX];
    int is_direct;
    int is_root;
    DepVisitState state;
    struct DepNode *next;
} DepNode;

typedef struct DepEdge {
    char from[256];
    char to[256];
    struct DepEdge *next;
} DepEdge;

typedef struct {
    char proj_root[SNOVAC_PATH_MAX];
    char deps_root[SNOVAC_PATH_MAX];
    DepNode *nodes;
    DepEdge *edges;
    DepNode *stack[128];
    int stack_depth;
    int has_cycle;
    char cycle_msg[512];
} DepGraph;

void graph_add_edge(DepGraph *, const char *, const char *);
int graph_resolve_node(DepGraph *, DepNode *);
void graph_free(DepGraph *);
void print_dependency_graph(DepGraph *, int *);
DepNode *graph_add_node(DepGraph *, const char *, const char *, const char *, const char *, int, int);
int find_dir_manifest(const char *, char *, size_t);
int manifest_read(const char *, SnManifest *);
int manifest_write(const char *, const SnManifest *);
void manifest_free(SnManifest *);
void parse_dep_string(const char *, SnManifestDep *);
void normalize_module_info(const char *, const char *, char *, size_t, char *, size_t, char *, size_t, char *, size_t);
int fetch_dependency(const char *, const char *, const char *);

#endif
