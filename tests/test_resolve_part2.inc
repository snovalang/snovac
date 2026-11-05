static void test_multi_section_file_is_skipped(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/multisection", tmp);
    mkdir(dir, 0755);
    write_file(dir, "e.snl",
              "package pkg.multi1\n\nfunc providerFn(): int {\n    return 1\n}\n\n"
              "package pkg.multi2\n\nfunc consumerFn(): int {\n    return 2\n}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    size_t skipped = sn_resolver_collect(&w.resolver);

    /* One physical file, two `package` sections -> two SnPackageFile
     * entries in the graph (package.c splits by section) -> the skip
     * counter, which counts entries, is 2 here, not 1. */
    CHECK("multi-section: collect reports both sections as skipped (2 entries)",
          skipped == 2);

    SnScope *s1 = sn_resolver_package_scope(&w.resolver, sn_intern_cstr(&w.intern, "pkg.multi1"));
    SnScope *s2 = sn_resolver_package_scope(&w.resolver, sn_intern_cstr(&w.intern, "pkg.multi2"));
    int has_provider = s1 && sn_scope_lookup_local(s1, sn_intern_cstr(&w.intern, "providerFn"));
    int has_consumer = s2 && sn_scope_lookup_local(s2, sn_intern_cstr(&w.intern, "consumerFn"));
    CHECK("multi-section: neither section's declarations were collected "
          "(documented limitation, not silently wrong)",
          !has_provider && !has_consumer);

    sn_arena_free(&w.arena);
}

static void test_member_path_package_prefix(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/memberpath", tmp);
    mkdir(dir, 0755);
    char sub[1024];
    snprintf(sub, sizeof(sub), "%s/deep", dir);
    mkdir(sub, 0755);
    write_file(sub, "c.snl", "package a.b\n\nclass C {\n    method m(): unit\n}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);

    SnList segs;
    memset(&segs, 0, sizeof(segs));
    sn_list_push(&w.arena, &segs, (void *)sn_intern_cstr(&w.intern, "a"));
    sn_list_push(&w.arena, &segs, (void *)sn_intern_cstr(&w.intern, "b"));
    sn_list_push(&w.arena, &segs, (void *)sn_intern_cstr(&w.intern, "C"));

    SnSymbol *found = sn_resolve_member_path(&w.resolver,
                                             sn_intern_cstr(&w.intern, "somewhere.else"),
                                             NULL, NULL, NULL, &segs, zero_span());
    CHECK("member-path: a.b.C resolves via longest package-prefix match",
          found != NULL && found->kind == SN_SYM_TYPE);
    CHECK("member-path: emits no diagnostic on success", w.diag.error_count == 0);

    SnList segs_pkg_only;
    memset(&segs_pkg_only, 0, sizeof(segs_pkg_only));
    sn_list_push(&w.arena, &segs_pkg_only, (void *)sn_intern_cstr(&w.intern, "a"));
    sn_list_push(&w.arena, &segs_pkg_only, (void *)sn_intern_cstr(&w.intern, "b"));
    SnSymbol *bare_pkg = sn_resolve_member_path(&w.resolver,
                                                sn_intern_cstr(&w.intern, "somewhere.else"),
                                                NULL, NULL, NULL, &segs_pkg_only, zero_span());
    CHECK("member-path: a bare package name (nothing left to resolve) returns "
          "NULL without a diagnostic",
          bare_pkg == NULL && w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static int scratch_dir(char *buf, size_t n, const char *leaf) {
#if defined(_WIN32)
    const char *root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') root = getenv("TMP");
    if (root == NULL || root[0] == '\0') root = ".";
    int wrote = snprintf(buf, n, "%s\\%s", root, leaf);
#else
    int wrote = snprintf(buf, n, "/tmp/%s", leaf);
#endif
    if (wrote < 0 || (size_t)wrote >= n) return 0;
    return mkdtemp(buf) != NULL;
}

int main(void) {
    char tmp[1024];
    if (!scratch_dir(tmp, sizeof(tmp), "snovac_resolve_test_XXXXXX")) {
        printf("FAIL setup: mkdtemp failed\n");
        return 1;
    }

    test_basic_collection_and_package_level_lookup(tmp);
    test_local_shadows_package(tmp);
    test_inheritance_walk(tmp);
    test_import_and_not_imported(tmp);
    test_truly_unknown_type(tmp);
    test_duplicate_declaration(tmp);
    test_field_method_same_name_is_not_a_collision(tmp);
    test_extension_merging_and_prelude(tmp);
    test_primitives_bypass_everything(tmp);
    test_multi_section_file_is_skipped(tmp);
    test_member_path_package_prefix(tmp);

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
