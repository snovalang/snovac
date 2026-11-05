static int dfs_cycle(SnPackageGraph *g, SnPackageNode *node, DfsPath *path,
                      SnList *cycle_out) {
    node->visited = 1;
    node->on_stack = 1;
    path->items[path->depth++] = node;

    for (size_t i = 0; i < node->edges.len; i++) {
        const char *target_name = SN_LIST_AT(node->edges, const char, i);
        SnPackageNode *target = sn_pkggraph_find(g, target_name);
        if (!target) {
            continue; /* sn_pkggraph_link() already reported unresolved edges */
        }
        if (target->on_stack) {
            size_t start = 0;
            for (size_t k = 0; k < path->depth; k++) {
                if (path->items[k] == target) {
                    start = k;
                    break;
                }
            }
            for (size_t k = start; k < path->depth; k++) {
                sn_list_push(g->arena, cycle_out, (void *)path->items[k]->name);
            }
            sn_list_push(g->arena, cycle_out, (void *)target->name);
            return 1;
        }
        if (!target->visited && dfs_cycle(g, target, path, cycle_out)) {
            return 1;
        }
    }

    node->on_stack = 0;
    path->depth--;
    return 0;
}

int sn_pkggraph_find_cycle(SnPackageGraph *g, SnList *cycle_out) {
    for (SnPackageNode *n = g->nodes; n; n = n->next) {
        n->visited = 0;
        n->on_stack = 0;
    }

    DfsPath path;
    path.items = (SnPackageNode **)sn_arena_alloc(
        g->arena, (g->node_count ? g->node_count : 1u) * sizeof(SnPackageNode *));
    path.depth = 0;

    for (SnPackageNode *n = g->nodes; n; n = n->next) {
        if (!n->visited && dfs_cycle(g, n, &path, cycle_out)) {
            return 1;
        }
    }
    return 0;
}
