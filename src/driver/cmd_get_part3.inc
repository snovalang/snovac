
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

static DepNode *graph_find_node(DepGraph *g, const char *id) {
    for (DepNode *n = g->nodes; n; n = n->next) {
        if (strcmp(n->id, id) == 0) return n;
    }
    return NULL;
}

static DepNode *graph_add_node(DepGraph *g, const char *id, const char *url,
                               const char *version, const char *local_dir,
                               int is_direct, int is_root) {
    DepNode *n = graph_find_node(g, id);
    if (n) {
        if (is_direct) n->is_direct = 1;
        if (version && version[0] && (!n->version[0] || strcmp(n->version, "1.0.0") == 0)) {
            snprintf(n->version, sizeof(n->version), "%s", version);
        }
        return n;
    }
    n = (DepNode *)calloc(1, sizeof(DepNode));
    snprintf(n->id, sizeof(n->id), "%s", id);
    snprintf(n->url, sizeof(n->url), "%s", url ? url : "");
    snprintf(n->version, sizeof(n->version), "%s", version ? version : "1.0.0");
    snprintf(n->local_dir, sizeof(n->local_dir), "%s", local_dir ? local_dir : "");
    n->is_direct = is_direct;
    n->is_root = is_root;
    n->state = DEP_STATE_UNVISITED;
    n->next = g->nodes;
    g->nodes = n;
    return n;
}

static void graph_add_edge(DepGraph *g, const char *from, const char *to) {
    for (DepEdge *e = g->edges; e; e = e->next) {
        if (strcmp(e->from, from) == 0 && strcmp(e->to, to) == 0) return;
    }
    DepEdge *e = (DepEdge *)calloc(1, sizeof(DepEdge));
    snprintf(e->from, sizeof(e->from), "%s", from);
    snprintf(e->to, sizeof(e->to), "%s", to);
    e->next = g->edges;
    g->edges = e;
}

static int graph_resolve_node(DepGraph *g, DepNode *node) {
    /* Cycle detection */
    for (int i = 0; i < g->stack_depth; i++) {
        if (g->stack[i] == node) {
            g->has_cycle = 1;
            snprintf(g->cycle_msg, sizeof(g->cycle_msg), "dependency cycle detected: ");
            for (int j = i; j < g->stack_depth; j++) {
                strncat(g->cycle_msg, g->stack[j]->id, sizeof(g->cycle_msg) - strlen(g->cycle_msg) - 1);
                strncat(g->cycle_msg, " -> ", sizeof(g->cycle_msg) - strlen(g->cycle_msg) - 1);
            }
            strncat(g->cycle_msg, node->id, sizeof(g->cycle_msg) - strlen(g->cycle_msg) - 1);
            return 1;
        }
    }

    if (node->state == DEP_STATE_VISITED) {
        return 0;
    }

    if (g->stack_depth >= 120) {
        fprintf(stderr, "error: dependency graph too deep\n");
        return 1;
    }

    g->stack[g->stack_depth++] = node;
    node->state = DEP_STATE_VISITING;

    if (node->is_root) {
        for (DepNode *n = g->nodes; n; n = n->next) {
            if (n->is_direct && !n->is_root) {
                int rc = graph_resolve_node(g, n);
                if (rc != 0) {
                    g->stack_depth--;
                    return rc;
                }
            }
        }
    } else {
        if (!sn_driver_path_is_dir(node->local_dir)) {
            int rc = fetch_dependency(node->url, node->version, node->local_dir);
            if (rc != 0) {
                fprintf(stderr, "error: failed to fetch dependency from %s\n", node->url);
                g->stack_depth--;
                return 1;
            }
        }

        char mod_path[SNOVAC_PATH_MAX + 32];
        SnManifest manifest;
        if (find_dir_manifest(node->local_dir, mod_path, sizeof(mod_path)) && manifest_read(mod_path, &manifest)) {
            for (SnManifestDep *d = manifest.direct; d; d = d->next) {
                char d_id[256];
                char d_ver[64];
                char d_url[512];
                char d_rel[SNOVAC_PATH_MAX];
                normalize_module_info(d->raw, d->version, d_id, sizeof(d_id), d_ver, sizeof(d_ver),
                                      d_url, sizeof(d_url), d_rel, sizeof(d_rel));

                char d_local[SNOVAC_PATH_MAX];
                snprintf(d_local, sizeof(d_local), "%s/%s", g->deps_root, d_rel);

                graph_add_edge(g, node->id, d_id);

                DepNode *d_node = graph_add_node(g, d_id, d_url, d_ver, d_local, 0, 0);
                int rc = graph_resolve_node(g, d_node);
                if (rc != 0) {
                    manifest_free(&manifest);
                    g->stack_depth--;
                    return rc;
                }
            }
            manifest_free(&manifest);
        }
    }

    g->stack_depth--;
    node->state = DEP_STATE_VISITED;
    return 0;
}

static void graph_free(DepGraph *g) {
    DepNode *n = g->nodes;
    while (n) {
        DepNode *next = n->next;
        free(n);
        n = next;
    }
    g->nodes = NULL;

    DepEdge *e = g->edges;
    while (e) {
        DepEdge *next = e->next;
        free(e);
        e = next;
    }
    g->edges = NULL;
}

static void print_dep_subtree(DepGraph *g, const char *pkg_id, int depth,
                              const char *path_stack[], int stack_depth, int *vuln_count) {
    DepNode *node = graph_find_node(g, pkg_id);
    if (!node) return;

    char prefix[64] = {0};
    if (depth == 0) {
        prefix[0] = '\0';
    } else if (depth == 1) {
        snprintf(prefix, sizeof(prefix), "-> ");
    } else {
        int eq_count = depth - 1;
        if (eq_count > 20) eq_count = 20;
        for (int i = 0; i < eq_count; i++) prefix[i] = '=';
        prefix[eq_count] = '>';
        prefix[eq_count + 1] = ' ';
        prefix[eq_count + 2] = '\0';
    }

    char vuln_tag[128] = {0};
    if (strncmp(node->url, "http://", 7) == 0) {
        snprintf(vuln_tag, sizeof(vuln_tag), " [vulnerability: unencrypted http transport]");
        (*vuln_count)++;
    }

    /* Check cycle in active print stack */
    int is_cycle = 0;
    for (int i = 0; i < stack_depth; i++) {
        if (strcmp(path_stack[i], pkg_id) == 0) {
            is_cycle = 1;
            break;
        }
    }

    if (is_cycle) {
        printf("%s%s@%s [cyclic reference]%s\n", prefix, node->id,
               node->version[0] ? node->version : "1.0.0", vuln_tag);
        return;
    }

    printf("%s%s@%s%s\n", prefix, node->id, node->version[0] ? node->version : "1.0.0", vuln_tag);

    if (stack_depth < 25) {
        path_stack[stack_depth] = pkg_id;

        /* Collect and sort outgoing child edges */
        const char *children[64];
        int child_count = 0;
        for (DepEdge *e = g->edges; e && child_count < 64; e = e->next) {
            if (strcmp(e->from, pkg_id) == 0) {
                children[child_count++] = e->to;
            }
        }
        for (int i = 0; i < child_count - 1; i++) {
            for (int j = i + 1; j < child_count; j++) {
                if (strcmp(children[i], children[j]) > 0) {
                    const char *tmp = children[i];
                    children[i] = children[j];
                    children[j] = tmp;
                }
            }
        }

        for (int i = 0; i < child_count; i++) {
            print_dep_subtree(g, children[i], depth + 1, path_stack, stack_depth + 1, vuln_count);
        }
    }
}

static void print_dependency_graph(DepGraph *g, int *out_vulns) {
    printf("snova: dependency:\n\n");
    int vulns = 0;

    /* Collect direct root dependencies */
    DepNode *directs[128];
    int direct_count = 0;
    for (DepNode *n = g->nodes; n && direct_count < 128; n = n->next) {
        if (n->is_direct && !n->is_root) {
            directs[direct_count++] = n;
        }
    }

    /* Sort direct dependencies alphabetically */
    for (int i = 0; i < direct_count - 1; i++) {
        for (int j = i + 1; j < direct_count; j++) {
            if (strcmp(directs[i]->id, directs[j]->id) > 0) {
                DepNode *tmp = directs[i];
                directs[i] = directs[j];
                directs[j] = tmp;
            }
        }
    }

    const char *path_stack[32];
    for (int i = 0; i < direct_count; i++) {
        print_dep_subtree(g, directs[i]->id, 0, path_stack, 0, &vulns);
    }
    printf("\n");

    if (out_vulns) *out_vulns = vulns;
}

/* ── CLI Command Implementation ──────────────────────────────────────────── */

