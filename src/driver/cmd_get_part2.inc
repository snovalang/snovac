static int manifest_read(const char *manifest_path, SnManifest *m) {
    memset(m, 0, sizeof(*m));
    snprintf(m->module_name, sizeof(m->module_name), "app");
    snprintf(m->snova_version, sizeof(m->snova_version), "1.0.0");

    FILE *f = fopen(manifest_path, "r");
    if (!f) return 0;

    char line[1024];
    int in_direct = 0;
    int in_indirect = 0;
    int in_multiline_comment = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        str_trim(p);

        if (in_multiline_comment) {
            char *end_c = strstr(p, "*/");
            if (end_c) {
                in_multiline_comment = 0;
                p = end_c + 2;
                str_trim(p);
            } else {
                continue;
            }
        }
        char *start_c = strstr(p, "/*");
        if (start_c) {
            char *end_c = strstr(start_c + 2, "*/");
            if (end_c) {
                memmove(start_c, end_c + 2, strlen(end_c + 2) + 1);
                str_trim(p);
            } else {
                *start_c = '\0';
                in_multiline_comment = 1;
                str_trim(p);
            }
        }

        char *comment = strstr(p, "//");
        if (comment) *comment = '\0';
        if (p[0] == '#') *p = '\0';
        str_trim(p);
        if (p[0] == '\0') continue;

        if (strncmp(p, "module", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
            p += 6;
            str_trim(p);
            char *sp = strpbrk(p, " \t");
            if (sp) {
                *sp = '\0';
                snprintf(m->module_name, sizeof(m->module_name), "%s", p);
                p = sp + 1;
                str_trim(p);
                if (p[0]) snprintf(m->module_url, sizeof(m->module_url), "%s", p);
            } else {
                snprintf(m->module_name, sizeof(m->module_name), "%s", p);
            }
            continue;
        }

        if (strncmp(p, "root", 4) == 0 && (p[4] == ' ' || p[4] == '\t' || p[4] == '=' || p[4] == '\0')) {
            p += 4;
            str_trim(p);
            if (p[0] == '=') { p++; str_trim(p); }
            if (p[0] == '\0' || strncmp(p, "true", 4) == 0 || strncmp(p, "1", 1) == 0) {
                m->is_root = 1;
            } else {
                m->is_root = 0;
            }
            continue;
        }

        if (strncmp(p, "is_root", 7) == 0 && (p[7] == ' ' || p[7] == '\t' || p[7] == '=' || p[7] == '\0')) {
            p += 7;
            str_trim(p);
            if (p[0] == '=') { p++; str_trim(p); }
            if (p[0] == '\0' || strncmp(p, "true", 4) == 0 || strncmp(p, "1", 1) == 0) {
                m->is_root = 1;
            } else {
                m->is_root = 0;
            }
            continue;
        }

        if (strncmp(p, "snova", 5) == 0 && (p[5] == ' ' || p[5] == '\t' || p[5] == '"' || p[5] == '=')) {
            p += 5;
            str_trim(p);
            if (p[0] == '=') { p++; str_trim(p); }
            if (p[0] == '"') {
                p++;
                char *q = strchr(p, '"');
                if (q) *q = '\0';
            }
            snprintf(m->snova_version, sizeof(m->snova_version), "%s", p);
            continue;
        }

        if (strstr(p, "indirect") && strchr(p, '=')) {
            in_indirect = 1;
            in_direct = 0;
        } else if (strstr(p, "direct") && strchr(p, '=')) {
            in_direct = 1;
            in_indirect = 0;
        } else if (strstr(p, "require") && strchr(p, '(')) {
            in_direct = 1;
            in_indirect = 0;
            continue;
        }

        /* Go-style require ( github.com/user/pkg v1.0.0 ) without quotes */
        if (in_direct && !strchr(p, '"') && !strchr(p, '[') && !strchr(p, ']') && !strchr(p, '(') && !strchr(p, ')')) {
            char mod_id[256] = {0};
            char mod_ver[64] = {0};
            if (sscanf(p, "%255s %63s", mod_id, mod_ver) >= 1) {
                if (mod_id[0] && mod_id[0] != '/' && mod_id[0] != '#' && strchr(mod_id, '.')) {
                    SnManifestDep *dep = (SnManifestDep *)calloc(1, sizeof(SnManifestDep));
                    snprintf(dep->module, sizeof(dep->module), "%s", mod_id);
                    if (mod_ver[0]) snprintf(dep->version, sizeof(dep->version), "%s", mod_ver);
                    else snprintf(dep->version, sizeof(dep->version), "1.0.0");
                    snprintf(dep->raw, sizeof(dep->raw), "%s@%s", dep->module, dep->version);
                    dep->next = NULL;
                    if (!m->direct) m->direct = dep;
                    else {
                        SnManifestDep *cur = m->direct;
                        while (cur->next) cur = cur->next;
                        cur->next = dep;
                    }
                    m->direct_count++;
                    continue;
                }
            }
        }

        char *quote_start = p;
        while ((quote_start = strchr(quote_start, '"')) != NULL) {
            quote_start++;
            char *quote_end = strchr(quote_start, '"');
            if (!quote_end) break;
            *quote_end = '\0';

            SnManifestDep *dep = (SnManifestDep *)calloc(1, sizeof(SnManifestDep));
            parse_dep_string(quote_start, dep);

            if (in_direct) {
                dep->next = NULL;
                if (!m->direct) {
                    m->direct = dep;
                } else {
                    SnManifestDep *cur = m->direct;
                    while (cur->next) cur = cur->next;
                    cur->next = dep;
                }
                m->direct_count++;
            } else if (in_indirect) {
                dep->next = NULL;
                if (!m->indirect) {
                    m->indirect = dep;
                } else {
                    SnManifestDep *cur = m->indirect;
                    while (cur->next) cur = cur->next;
                    cur->next = dep;
                }
                m->indirect_count++;
            } else {
                free(dep);
            }

            quote_start = quote_end + 1;
        }

        if (strchr(p, ']') || (in_direct && strchr(p, ')'))) {
            if (in_direct) in_direct = 0;
            else if (in_indirect) in_indirect = 0;
        }
    }

    fclose(f);
    return 1;
}

static void manifest_free(SnManifest *m) {
    SnManifestDep *d = m->direct;
    while (d) {
        SnManifestDep *next = d->next;
        free(d);
        d = next;
    }
    m->direct = NULL;
    m->direct_count = 0;

    SnManifestDep *ind = m->indirect;
    while (ind) {
        SnManifestDep *next = ind->next;
        free(ind);
        ind = next;
    }
    m->indirect = NULL;
    m->indirect_count = 0;
}

static int compare_manifest_deps(const void *a, const void *b) {
    const SnManifestDep *const *da = (const SnManifestDep *const *)a;
    const SnManifestDep *const *db = (const SnManifestDep *const *)b;
    return strcmp((*da)->raw, (*db)->raw);
}

static int manifest_write(const char *manifest_path, const SnManifest *m) {
    FILE *out = fopen(manifest_path, "w");
    if (!out) {
        fprintf(stderr, "error: cannot write manifest to %s\n", manifest_path);
        return 0;
    }

    if (m->module_url[0]) {
        fprintf(out, "module %s %s\n\n", m->module_name[0] ? m->module_name : "app", m->module_url);
    } else {
        fprintf(out, "module %s\n\n", m->module_name[0] ? m->module_name : "app");
    }

    if (m->is_root) {
        fprintf(out, "root true\n\n");
    }

    fprintf(out, "snova \"%s\"\n\n", m->snova_version[0] ? m->snova_version : "1.0.0");

    SnManifestDep **direct_arr = NULL;
    if (m->direct_count > 0) {
        direct_arr = (SnManifestDep **)malloc(sizeof(SnManifestDep *) * (size_t)m->direct_count);
        int idx = 0;
        for (SnManifestDep *d = m->direct; d && idx < m->direct_count; d = d->next) {
            direct_arr[idx++] = d;
        }
        qsort(direct_arr, (size_t)m->direct_count, sizeof(SnManifestDep *), compare_manifest_deps);
    }

    SnManifestDep **indirect_arr = NULL;
    if (m->indirect_count > 0) {
        indirect_arr = (SnManifestDep **)malloc(sizeof(SnManifestDep *) * (size_t)m->indirect_count);
        int idx = 0;
        for (SnManifestDep *d = m->indirect; d && idx < m->indirect_count; d = d->next) {
            indirect_arr[idx++] = d;
        }
        qsort(indirect_arr, (size_t)m->indirect_count, sizeof(SnManifestDep *), compare_manifest_deps);
    }

    fprintf(out, "dependencies(\n");
    fprintf(out, "    direct = [\n");
    for (int i = 0; i < m->direct_count; i++) {
        fprintf(out, "        \"%s\"%s\n", direct_arr[i]->raw, (i + 1 < m->direct_count) ? "," : "");
    }
    fprintf(out, "    ],\n");
    fprintf(out, "    indirect = [\n");
    for (int i = 0; i < m->indirect_count; i++) {
        fprintf(out, "        \"%s\"%s\n", indirect_arr[i]->raw, (i + 1 < m->indirect_count) ? "," : "");
    }
    fprintf(out, "    ]\n");
    fprintf(out, ")\n");

    if (direct_arr) free(direct_arr);
    if (indirect_arr) free(indirect_arr);
    fclose(out);
    return 1;
}

/* ── Dependency Identification & Normalization ───────────────────────────── */

static void normalize_module_info(const char *input, const char *version_override,
                                  char *out_id, size_t out_id_sz,
                                  char *out_ver, size_t out_ver_sz,
                                  char *out_url, size_t out_url_sz,
                                  char *out_rel_dir, size_t out_rel_dir_sz) {
    char raw[1024];
    snprintf(raw, sizeof(raw), "%s", input);
    str_trim(raw);

    char ver[64] = {0};
    char *at = strchr(raw, '@');
    if (at) {
        *at = '\0';
        snprintf(ver, sizeof(ver), "%s", at + 1);
        str_trim(ver);
    }

    if (version_override && version_override[0]) {
        snprintf(ver, sizeof(ver), "%s", version_override);
        str_trim(ver);
    }
    if (ver[0] == '\0') {
        snprintf(ver, sizeof(ver), "1.0.0");
    }
    snprintf(out_ver, out_ver_sz, "%s", ver);

    /* Check local path */
    if (strncmp(raw, "file://", 7) == 0 || sn_driver_path_is_dir(raw) || raw[0] == '/' || raw[0] == '.' ||
        raw[0] == '\\' || (isalpha((unsigned char)raw[0]) && raw[1] == ':')) {
        const char *local_path = (strncmp(raw, "file://", 7) == 0) ? raw + 7 : raw;
        snprintf(out_url, out_url_sz, "%s", local_path);

        /* Inspect mod.sns inside local dir for canonical module name/url if available */
        char mod_path[SNOVAC_PATH_MAX + 32];
        if (!find_dir_manifest(local_path, mod_path, sizeof(mod_path))) {
            snprintf(mod_path, sizeof(mod_path), "%s/mod.sns", local_path);
        }
        SnManifest local_m;
        if (sn_driver_path_is_file(mod_path) && manifest_read(mod_path, &local_m)) {
            if (local_m.module_url[0]) {
                snprintf(out_id, out_id_sz, "%s", local_m.module_url);
            } else if (local_m.module_name[0]) {
                snprintf(out_id, out_id_sz, "%s", local_m.module_name);
            } else {
                const char *s1 = strrchr(local_path, '/');
                const char *s2 = strrchr(local_path, '\\');
                const char *slash = (s2 && (!s1 || s2 > s1)) ? s2 : s1;
                snprintf(out_id, out_id_sz, "%s", slash ? slash + 1 : local_path);
            }
            manifest_free(&local_m);
        } else {
            const char *s1 = strrchr(local_path, '/');
            const char *s2 = strrchr(local_path, '\\');
            const char *slash = (s2 && (!s1 || s2 > s1)) ? s2 : s1;
            snprintf(out_id, out_id_sz, "%s", slash ? slash + 1 : local_path);
        }

        const char *s1 = strrchr(local_path, '/');
        const char *s2 = strrchr(local_path, '\\');
        const char *slash = (s2 && (!s1 || s2 > s1)) ? s2 : s1;
        snprintf(out_rel_dir, out_rel_dir_sz, "%s", slash ? slash + 1 : local_path);
        return;
    }

    /* URL normalization */
    char clean_id[512];
    snprintf(clean_id, sizeof(clean_id), "%s", raw);

    if (strncmp(clean_id, "https://", 8) == 0) {
        memmove(clean_id, clean_id + 8, strlen(clean_id + 8) + 1);
    } else if (strncmp(clean_id, "http://", 7) == 0) {
        memmove(clean_id, clean_id + 7, strlen(clean_id + 7) + 1);
    } else if (strncmp(clean_id, "git@", 4) == 0) {
        memmove(clean_id, clean_id + 4, strlen(clean_id + 4) + 1);
        char *colon = strchr(clean_id, ':');
        if (colon) *colon = '/';
    }

    size_t len = strlen(clean_id);
    if (len > 4 && strcmp(clean_id + len - 4, ".git") == 0) {
        clean_id[len - 4] = '\0';
        len -= 4;
    }
    while (len > 0 && clean_id[len - 1] == '/') {
        clean_id[--len] = '\0';
    }

    snprintf(out_id, out_id_sz, "%s", clean_id);
    snprintf(out_rel_dir, out_rel_dir_sz, "%s", clean_id);

    if (strncmp(raw, "http://", 7) == 0 || strncmp(raw, "https://", 8) == 0 ||
        strncmp(raw, "git@", 4) == 0 || strncmp(raw, "ssh://", 6) == 0) {
        snprintf(out_url, out_url_sz, "%s", raw);
    } else {
        snprintf(out_url, out_url_sz, "https://%s", clean_id);
    }
}

/* ── Dependency Graph Resolver & Cycle Detection ────────────────────────── */

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
