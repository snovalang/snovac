static void test_inner_func_call(const char *tmp) {
    char dir[1024];
    char miss_dir[1024];
    snprintf(dir, sizeof(dir), "%s/iife", tmp);
    snprintf(miss_dir, sizeof(miss_dir), "%s/iife_miss", tmp);
    mkdir(dir, 0755);
    mkdir(miss_dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.iife\n\n"
              "func top(): int {\n"
              "    return 1\n"
              "}\n"
              "func plain(): int {\n"
              "    func test() {\n"
              "        Console.println(\"inner\")\n"
              "    }()\n"
              "    return 0\n"
              "}\n"
              "func pulsed(): int {\n"
              "    pulsar func test(x: int) {\n"
              "        Console.println(x)\n"
              "    }(1)\n"
              "    return 0\n"
              "}\n"
              "func anon(): int {\n"
              "    func() {\n"
              "        Console.println(\"anon\")\n"
              "    }()\n"
              "    return 0\n"
              "}\n");
    write_file(miss_dir, "miss.snl",
              "package pkg.iife.miss\n\n"
              "func missing(): int {\n"
              "    func test() {\n"
              "        Console.println(\"no\")\n"
              "    }\n"
              "    return 0\n"
              "}\n");
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.iife", "top");
        CHECK("top-level func is not an immediate call", w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.iife", "plain");
        CHECK("plain inner func call is accepted", w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.iife", "pulsed");
        CHECK("pulsar inner func call is accepted", w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.iife", "anon");
        CHECK("anonymous func call is accepted", w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, miss_dir, "pkg.iife.miss", "missing");
        CHECK("missing inner call is an error", w.diag.error_count >= 1);
        CHECK("missing inner call emits SNOVA0148", diag_has_code(&w, "SNOVA0148"));
        sn_arena_free(&w.arena);
    }
}

static void test_narrowing_cast(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/narrow", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.narrow\n\n"
              "func bad(): int8 {\n"
              "    let x: int = 5\n"
              "    let y = x as int8\n"
              "    return y\n"
              "}\n"
              "func wide(): int16 {\n"
              "    let x: int8 = 20\n"
              "    return x as int16\n"
              "}\n"
              "func literal_ok(): int8 {\n"
              "    let x: int8 = 20\n"
              "    return x\n"
              "}\n"
              "func literal_big(): int8 {\n"
              "    let x: int8 = 300\n"
              "    return x\n"
              "}\n");
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.narrow", "bad");
        CHECK("narrowing: int as int8 is an error", w.diag.error_count >= 1);
        CHECK("narrowing: emits SNOVA0145", diag_has_code(&w, "SNOVA0145"));
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.narrow", "wide");
        CHECK("widening: int8 as int16 is allowed", w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.narrow", "literal_ok");
        CHECK("literal: 20 fits int8", w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.narrow", "literal_big");
        CHECK("literal: 300 does not fit int8", w.diag.error_count >= 1);
        sn_arena_free(&w.arena);
    }
}

static void test_string_is_chars(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/chars", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.chars\n\n"
              "func idx(): char {\n"
              "    let s = \"ab\"\n"
              "    return s[0]\n"
              "}\n"
              "func cat(): string {\n"
              "    let a: char = 'a'\n"
              "    return a + a\n"
              "}\n"
              "func bad(): string {\n"
              "    return \"n\" + 1\n"
              "}\n");
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.chars", "idx");
        CHECK("string index yields char", w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.chars", "cat");
        CHECK("char + char is string", w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.chars", "bad");
        CHECK("string + int is an error", w.diag.error_count >= 1);
        CHECK("string + int emits SNOVA0133", diag_has_code(&w, "SNOVA0133"));
        sn_arena_free(&w.arena);
    }
}

static void test_duplicate_fields(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/dupf", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.dupf\n\n"
              "struct Pair {\n"
              "    public let field: int\n"
              "    public let field: string\n"
              "}\n"
              "func main(): int { return 0 }\n");
    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);
    world_finish_diag(&w);
    CHECK("duplicate fields emit SNOVA0120", diag_has_code(&w, "SNOVA0120"));
    sn_arena_free(&w.arena);
}

static void test_method_overload(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/ov", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.ov\n\n"
              "class Box {\n"
              "    method show(value: int): string { return \"int\" }\n"
              "    method show(value: string): string { return \"string\" }\n"
              "    method both(value: int): string { return \"i\" }\n"
              "    method both(value: int64): string { return \"i64\" }\n"
              "}\n"
              "func good(): string {\n"
              "    let b = Box()\n"
              "    return b.show(\"hi\")\n"
              "}\n"
              "func amb(): string {\n"
              "    let b = Box()\n"
              "    return b.both(1)\n"
              "}\n");
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.ov", "good");
        CHECK("overload: string argument selects the string method", w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.ov", "amb");
        CHECK("overload: int literal matching int and int64 is ambiguous",
              w.diag.error_count >= 1);
        CHECK("overload: emits SNOVA0146", diag_has_code(&w, "SNOVA0146"));
        sn_arena_free(&w.arena);
    }
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

static void test_empty_array_struct_field(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/arrlit", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.arrlit\n\n"
              "public struct Bags {\n"
              "    public var fn_ns: int[]\n"
              "}\n"
              "func good(): int {\n"
              "    let bg = Bags{ fn_ns: [] }\n"
              "    return 0\n"
              "}\n"
              "func bad(): int {\n"
              "    let bg = Bags{ fn_ns: \"afad\" }\n"
              "    return 0\n"
              "}\n");
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.arrlit", "good");
        CHECK("struct literal: empty array adopts the field array type",
              w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.arrlit", "bad");
        CHECK("struct literal: string is not int[]", w.diag.error_count >= 1);
        sn_arena_free(&w.arena);
    }
}

int main(void) {
    char tmp[1024];
    if (!scratch_dir(tmp, sizeof(tmp), "snovac_check_test_XXXXXX")) {
        printf("FAIL setup: mkdtemp failed\n");
        return 1;
    }

    test_literals_and_arithmetic(tmp);
    test_no_implicit_promotion(tmp);
    test_string_concat(tmp);
    test_unary_operators(tmp);
    test_assignment_mutability(tmp);
    test_member_access_with_inheritance(tmp);
    test_call_arity_and_arg_types(tmp);
    test_variant_construction(tmp);
    test_condition_must_be_bool(tmp);
    test_return_type_mismatch(tmp);
    test_let_type_mismatch(tmp);
    test_empty_array_literal(tmp);
    test_cast_and_is(tmp);
    test_this_in_method(tmp);
    test_array_intrinsic(tmp);
    test_array_intrinsic_unknown_member(tmp);
    test_array_literal_with_declared_type(tmp);
    test_scalar_and_string_members(tmp);
    test_constructor_call(tmp);
    test_constructor_arity(tmp);
    test_lambda_value_call(tmp);
    test_lambda_value_call_arity(tmp);
    test_subtype_assignability(tmp);
    test_subtype_not_symmetric(tmp);
    test_literal_suffix_still_strict(tmp);
    test_func_in_class_body(tmp);
    test_extension_func_is_legal(tmp);
    test_nested_optional_rejected(tmp);
    test_null_coalescing_semantics(tmp);
    test_implicit_any_rejected(tmp);
    test_public_any_in_stdlib_rejected(tmp);
    test_subsumption_non_null(tmp);
    test_optional_type_required(tmp);
    test_pulsar_and_async_rules(tmp);
    test_struct_field_rejects_string(tmp);
    test_optional_struct_literal(tmp);
    test_empty_array_struct_field(tmp);
    test_inner_func_call(tmp);
    test_narrowing_cast(tmp);
    test_string_is_chars(tmp);
    test_duplicate_fields(tmp);
    test_method_overload(tmp);

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
