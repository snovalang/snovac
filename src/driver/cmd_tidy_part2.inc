int sn_cmd_tidy_project(const char *path) {
    SnProject proj;
    sn_project_discover(path, &proj);

    char manifest_path[SNOVAC_PATH_MAX + 64];
    char proj_root[SNOVAC_PATH_MAX];
    if (proj.has_manifest) {
        sn_driver_dirname_into(proj.source_root, proj_root, sizeof(proj_root));
        snprintf(manifest_path, sizeof(manifest_path), "%s/mod.sns", proj_root);
        if (!sn_driver_path_is_file(manifest_path)) {
            char alt[SNOVAC_PATH_MAX + 64];
            snprintf(alt, sizeof(alt), "%s/snova.sns", proj_root);
            if (sn_driver_path_is_file(alt)) {
                snprintf(manifest_path, sizeof(manifest_path), "%s", alt);
            }
        }
    } else {
        snprintf(proj_root, sizeof(proj_root), "%s", path);
        snprintf(manifest_path, sizeof(manifest_path), "%s/mod.sns", path);
    }

    char mod_name[256];
    char mod_url[256];
    char snova_ver[64];
    int is_root = 0;
    read_module_header(manifest_path, mod_name, sizeof(mod_name), mod_url, sizeof(mod_url), snova_ver, sizeof(snova_ver), &is_root);

    SnArena arena;
    sn_arena_init(&arena, 4 * 1024 * 1024);
    SnInternTable intern;
    sn_intern_init(&intern, &arena);
    SnDiagSink diag;
    sn_diag_init(&diag, path, "", 0);

    /* 1. Discover all installed modules in .snovalang/deps dynamically */
    DiscoveredModule discovered[MAX_DISCOVERED_MODULES];
    size_t discovered_count = 0;
    if (proj.deps_root[0] && sn_driver_path_is_dir(proj.deps_root)) {
        discover_deps_recursive(proj.deps_root, proj.deps_root, discovered, &discovered_count);
    }

    /* 2. Scan project source root and collect declared local packages and imported packages */
    SnPackageGraph graph;
    sn_pkggraph_init(&graph, &arena, &intern, &diag);
    sn_pkggraph_scan_root(&graph, proj.source_root);

    const char *used_modules[MAX_DISCOVERED_MODULES];
    size_t used_mod_count = 0;

    for (SnPackageNode *node = graph.nodes; node; node = node->next) {
        for (SnPackageFile *pf = node->files; pf; pf = pf->next) {
            SnDiagFile self = {pf->path, pf->src, pf->src_len};
            SnDiagFile outer = sn_diag_set_file(&diag, self);

            SnTokenVec toks;
            sn_lex(&arena, &diag, pf->src, pf->src_len, &toks);

            SnUnit unit;
            sn_parse(&arena, &diag, &toks, &unit);

            for (size_t ii = 0; ii < unit.imports.len; ii++) {
                const char *imp = SN_LIST_AT(unit.imports, const char, ii);
                if (!imp) continue;

                /* Check if this import is local to project */
                int is_local = 0;
                for (SnPackageNode *pn = graph.nodes; pn; pn = pn->next) {
                    if (pn->name && strcmp(pn->name, imp) == 0) {
                        is_local = 1;
                        break;
                    }
                }
                if (is_local) continue;

                /* Match against dynamically discovered dependency modules */
                for (size_t m = 0; m < discovered_count; m++) {
                    int matched = 0;
                    for (size_t p = 0; p < discovered[m].package_count; p++) {
                        const char *pkg = discovered[m].provided_packages[p];
                        size_t pkg_len = strlen(pkg);
                        if (strcmp(imp, pkg) == 0 ||
                            (strncmp(imp, pkg, pkg_len) == 0 && (imp[pkg_len] == '.' || imp[pkg_len] == '\0'))) {
                            matched = 1;
                            break;
                        }
                    }
                    if (matched) {
                        int already = 0;
                        for (size_t u = 0; u < used_mod_count; u++) {
                            if (strcmp(used_modules[u], discovered[m].module_id) == 0) {
                                already = 1;
                                break;
                            }
                        }
                        if (!already && used_mod_count < MAX_DISCOVERED_MODULES) {
                            used_modules[used_mod_count++] = sn_intern_cstr(&intern, discovered[m].module_id);
                        }
                    }
                }
            }
            sn_diag_set_file(&diag, outer);
        }
    }

    /* 3. Collect indirect edges dynamically from used modules' dependencies */
    const char *indirect_edges[128];
    size_t indirect_count = 0;

    for (size_t u = 0; u < used_mod_count; u++) {
        const char *from_id = used_modules[u];
        for (size_t m = 0; m < discovered_count; m++) {
            if (strcmp(discovered[m].module_id, from_id) == 0) {
                for (size_t s = 0; s < discovered[m].sub_dep_count; s++) {
                    const char *to_id = discovered[m].sub_deps[s];
                    char edge_buf[512];
                    snprintf(edge_buf, sizeof(edge_buf), "%s -> %s", from_id, to_id);
                    const char *interned_edge = sn_intern_cstr(&intern, edge_buf);

                    int already = 0;
                    for (size_t i = 0; i < indirect_count; i++) {
                        if (strcmp(indirect_edges[i], interned_edge) == 0) {
                            already = 1;
                            break;
                        }
                    }
                    if (!already && indirect_count < sizeof(indirect_edges)/sizeof(indirect_edges[0])) {
                        indirect_edges[indirect_count++] = interned_edge;
                    }
                }
            }
        }
    }

    /* 4. Write updated declarative mod.sns */
    FILE *out = fopen(manifest_path, "w");
    if (!out) {
        fprintf(stderr, "error: cannot write manifest to %s\n", manifest_path);
        sn_arena_free(&arena);
        return 1;
    }

    if (mod_url[0] != '\0') {
        fprintf(out, "module %s %s\n\n", mod_name, mod_url);
    } else {
        fprintf(out, "module %s\n\n", mod_name);
    }
    if (is_root) {
        fprintf(out, "root true\n\n");
    }
    fprintf(out, "snova \"%s\"\n\n", snova_ver);
    fprintf(out, "dependencies(\n");
    fprintf(out, "    direct = [\n");
    for (size_t u = 0; u < used_mod_count; u++) {
        fprintf(out, "        \"%s@1.0.0\"%s\n", used_modules[u], (u + 1 < used_mod_count) ? "," : "");
    }
    fprintf(out, "    ],\n");
    fprintf(out, "    indirect = [\n");
    for (size_t i = 0; i < indirect_count; i++) {
        fprintf(out, "        \"%s\"%s\n", indirect_edges[i], (i + 1 < indirect_count) ? "," : "");
    }
    fprintf(out, "    ]\n");
    fprintf(out, ")\n");
    fclose(out);

    printf("snovac tidy: updated %s (%zu direct, %zu indirect dependencies)\n",
           manifest_path, used_mod_count, indirect_count);

    sn_arena_free(&arena);

    /* 5. Automatically fetch, install, and reconcile dependencies into .snovalang/deps */
    return sn_cmd_get_project(proj_root, NULL, NULL);
}
