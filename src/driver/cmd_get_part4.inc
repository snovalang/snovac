int sn_cmd_get_project(const char *proj_path, const char *url, const char *version) {
    SnProject proj;
    sn_project_discover(proj_path ? proj_path : ".", &proj);

    char proj_root[SNOVAC_PATH_MAX];
    char manifest_path[SNOVAC_PATH_MAX + 32];
    char deps_root[SNOVAC_PATH_MAX];

    if (proj.has_manifest && proj.manifest_dir[0]) {
        snprintf(proj_root, sizeof(proj_root), "%s", proj.manifest_dir);
    } else if (sn_driver_path_is_dir(proj_path ? proj_path : ".")) {
        snprintf(proj_root, sizeof(proj_root), "%s", proj_path ? proj_path : ".");
    } else {
        sn_driver_dirname_into(proj_path ? proj_path : ".", proj_root, sizeof(proj_root));
    }

    if (!find_dir_manifest(proj_root, manifest_path, sizeof(manifest_path))) {
        snprintf(manifest_path, sizeof(manifest_path), "%s/mod.sns", proj_root);
    }

    snprintf(deps_root, sizeof(deps_root), "%s/.snovalang/deps", proj_root);

    if (!sn_driver_path_is_dir(deps_root)) {
        sn_driver_ensure_parent_dir(deps_root);
#if defined(_WIN32)
        mkdir(deps_root);
#else
        mkdir(deps_root, 0755);
#endif
    }

    SnManifest root_manifest;
    if (sn_driver_path_is_file(manifest_path)) {
        if (!manifest_read(manifest_path, &root_manifest)) {
            fprintf(stderr, "error: failed to read manifest from %s\n", manifest_path);
            return 1;
        }
    } else {
        memset(&root_manifest, 0, sizeof(root_manifest));
        const char *base = strrchr(proj_root, '/');
        snprintf(root_manifest.module_name, sizeof(root_manifest.module_name), "%s", base ? base + 1 : "app");
        snprintf(root_manifest.snova_version, sizeof(root_manifest.snova_version), "1.0.0");
    }

    if (!url && root_manifest.direct_count == 0) {
        fprintf(stderr, "error: get requires a repository url or declared dependencies in mod.sns\n");
        manifest_free(&root_manifest);
        return 2;
    }

    DepGraph graph;
    memset(&graph, 0, sizeof(graph));
    snprintf(graph.proj_root, sizeof(graph.proj_root), "%s", proj_root);
    snprintf(graph.deps_root, sizeof(graph.deps_root), "%s", deps_root);

    /* Add root node */
    DepNode *root_node = graph_add_node(&graph, root_manifest.module_name, NULL, root_manifest.snova_version, proj_root, 0, 1);

    /* If URL is supplied, add/update as root direct dependency */
    if (url && url[0]) {
        char req_id[256];
        char req_ver[64];
        char req_url[512];
        char req_rel[SNOVAC_PATH_MAX];
        normalize_module_info(url, version, req_id, sizeof(req_id), req_ver, sizeof(req_ver),
                              req_url, sizeof(req_url), req_rel, sizeof(req_rel));

        char req_local[SNOVAC_PATH_MAX];
        snprintf(req_local, sizeof(req_local), "%s/%s", deps_root, req_rel);

        graph_add_node(&graph, req_id, req_url, req_ver, req_local, 1, 0);

        /* Add to root_manifest if not present */
        int found = 0;
        for (SnManifestDep *d = root_manifest.direct; d; d = d->next) {
            if (strcmp(d->module, req_id) == 0) {
                found = 1;
                if (req_ver[0]) snprintf(d->version, sizeof(d->version), "%s", req_ver);
                snprintf(d->raw, sizeof(d->raw), "%s@%s", req_id, d->version[0] ? d->version : "1.0.0");
                break;
            }
        }
        if (!found) {
            SnManifestDep *dep = (SnManifestDep *)calloc(1, sizeof(SnManifestDep));
            snprintf(dep->module, sizeof(dep->module), "%s", req_id);
            snprintf(dep->version, sizeof(dep->version), "%s", req_ver[0] ? req_ver : "1.0.0");
            snprintf(dep->raw, sizeof(dep->raw), "%s@%s", req_id, dep->version);
            dep->next = root_manifest.direct;
            root_manifest.direct = dep;
            root_manifest.direct_count++;
        }
    }

    /* Add all existing direct deps from root_manifest */
    for (SnManifestDep *d = root_manifest.direct; d; d = d->next) {
        char d_id[256];
        char d_ver[64];
        char d_url[512];
        char d_rel[SNOVAC_PATH_MAX];
        normalize_module_info(d->raw, d->version, d_id, sizeof(d_id), d_ver, sizeof(d_ver),
                              d_url, sizeof(d_url), d_rel, sizeof(d_rel));

        char d_local[SNOVAC_PATH_MAX];
        snprintf(d_local, sizeof(d_local), "%s/%s", deps_root, d_rel);

        graph_add_node(&graph, d_id, d_url, d_ver, d_local, 1, 0);
    }

    /* Resolve graph starting from root node */
    int rc = graph_resolve_node(&graph, root_node);
    if (rc != 0) {
        if (graph.has_cycle) {
            fprintf(stderr, "error: %s\n", graph.cycle_msg);
        }
        manifest_free(&root_manifest);
        graph_free(&graph);
        return 1;
    }

    /* Print structured dependency hierarchy and vulnerability audit */
    int vuln_count = 0;
    print_dependency_graph(&graph, &vuln_count);

    /* Reconstruct root manifest dependencies */
    manifest_free(&root_manifest);
    memset(&root_manifest.direct, 0, sizeof(root_manifest.direct));
    root_manifest.direct_count = 0;
    root_manifest.indirect = NULL;
    root_manifest.indirect_count = 0;

    /* Populate direct */
    for (DepNode *n = graph.nodes; n; n = n->next) {
        if (n->is_direct && !n->is_root) {
            SnManifestDep *d = (SnManifestDep *)calloc(1, sizeof(SnManifestDep));
            snprintf(d->module, sizeof(d->module), "%s", n->id);
            snprintf(d->version, sizeof(d->version), "%s", n->version[0] ? n->version : "1.0.0");
            snprintf(d->raw, sizeof(d->raw), "%s@%s", n->id, d->version);
            d->next = root_manifest.direct;
            root_manifest.direct = d;
            root_manifest.direct_count++;
        }
    }

    /* Populate indirect */
    for (DepEdge *e = graph.edges; e; e = e->next) {
        SnManifestDep *d = (SnManifestDep *)calloc(1, sizeof(SnManifestDep));
        d->is_edge = 1;
        snprintf(d->from, sizeof(d->from), "%s", e->from);
        snprintf(d->module, sizeof(d->module), "%s", e->to);
        snprintf(d->raw, sizeof(d->raw), "%s -> %s", e->from, e->to);
        d->next = root_manifest.indirect;
        root_manifest.indirect = d;
        root_manifest.indirect_count++;
    }

    /* Write updated manifest */
    if (!manifest_write(manifest_path, &root_manifest)) {
        manifest_free(&root_manifest);
        graph_free(&graph);
        return 1;
    }

    printf("snova: updated %s (%d direct, %d indirect dependencies, %d vulnerabilities)\n",
           manifest_path, root_manifest.direct_count, root_manifest.indirect_count, vuln_count);

    manifest_free(&root_manifest);
    graph_free(&graph);
    return 0;
}

int sn_cmd_get(const char *url, const char *version) {
    return sn_cmd_get_project(".", url, version);
}
