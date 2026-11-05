static void test_array_literal_element_types(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/array_element_types", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.array_element_types\n\n"
              "class Animal {}\n"
              "class Dog : Animal {}\n"
              "func good(): int {\n"
              "    let values = [1, 2, 3]\n"
              "    return 0\n"
              "}\n"
              "func bad(): int {\n"
              "    let values = [1, \"two\"]\n"
              "    return 0\n"
              "}\n"
              "func subtype_ok(): int {\n"
              "    let values = [Animal(), Dog()]\n"
              "    return 0\n"
              "}\n"
              "func subtype_ordered_bad(): int {\n"
              "    let values = [Dog(), Animal()]\n"
              "    return 0\n"
              "}\n");

    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.array_element_types", "good");
        CHECK("array literal: elements matching the inferred type pass",
              w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.array_element_types", "bad");
        CHECK("array literal: incompatible element is one error",
              w.diag.error_count == 1);
        CHECK("array literal: incompatible element emits SNOVA0149",
              diag_has_code(&w, "SNOVA0149"));
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.array_element_types", "subtype_ok");
        CHECK("array literal: a subtype fits the first element's base type",
              w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.array_element_types", "subtype_ordered_bad");
        CHECK("array literal: a base type does not fit a first-element subtype",
              w.diag.error_count == 1);
        sn_arena_free(&w.arena);
    }
}

static void test_for_binding_types(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/for_binding_types", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.for_binding_types\n\n"
              "class Channel<T> {}\n"
              "func ranges(): int {\n"
              "    for (let index in 4) { let checked: int = index }\n"
              "    return 0\n"
              "}\n"
              "func arrays(values: string[]): int {\n"
              "    for (let item in values) { let checked: string = item }\n"
              "    return 0\n"
              "}\n"
              "func channels(channel: Channel<byte>): int {\n"
              "    for (let item <~ channel) { let checked: byte = item }\n"
              "    return 0\n"
              "}\n");

    const char *functions[] = {"ranges", "arrays", "channels"};
    for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); i++) {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.for_binding_types", functions[i]);
        CHECK("for binding: range, array and channel element types resolve",
              w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
}

static void test_for_binding_annotation_mismatch(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/for_binding_annotation", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.for_binding_annotation\n\n"
              "func bad(values: int[]): int {\n"
              "    for (let item: string in values) {}\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.for_binding_annotation", "bad");
    CHECK("for binding: an incompatible annotation is one error",
          w.diag.error_count == 1);
    CHECK("for binding: incompatible annotation emits SNOVA0150",
          diag_has_code(&w, "SNOVA0150"));
    sn_arena_free(&w.arena);
}

static void test_builtin_member_signatures(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/builtin_member_signatures", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.builtin_member_signatures\n\n"
              "func good(values: int[]): int {\n"
              "    values.push(3)\n"
              "    let first: int = values.get(0)\n"
              "    values.set(0, first)\n"
              "    return first\n"
              "}\n"
              "func bad_arity(values: int[]): int { return values.get() }\n"
              "func bad_type(values: int[]): int { values.push(\"bad\"); return 0 }\n"
              "func bad_index(values: int[]): int { return values.get(\"bad\") }\n"
              "func bad_string_arity(): string { return \"abc\".substring(1) }\n");

    const char *valid[] = {"good"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.builtin_member_signatures", valid[i]);
        CHECK("builtin method: valid signatures pass", w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }

    const char *invalid[] = {"bad_arity", "bad_type", "bad_index", "bad_string_arity"};
    const char *codes[] = {"SNOVA0131", "SNOVA0132", "SNOVA0132", "SNOVA0131"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.builtin_member_signatures", invalid[i]);
        CHECK("builtin method: invalid arguments are rejected", w.diag.error_count == 1);
        CHECK("builtin method: diagnostic identifies arity or type", diag_has_code(&w, codes[i]));
        sn_arena_free(&w.arena);
    }
}
