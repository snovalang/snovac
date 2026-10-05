static void test_this_in_method(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/thistest", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.thistest\n\n"
              "class Counter {\n"
              "    method self(): Counter {\n"
              "        return this\n"
              "    }\n"
              "}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);
    sn_resolver_build_prelude(&w.resolver);

    const char *pkg = sn_intern_cstr(&w.intern, "pkg.thistest");
    SnScope *pkg_scope = sn_resolver_package_scope(&w.resolver, pkg);
    SnSymbol *counter_sym =
        pkg_scope ? sn_scope_lookup_local(pkg_scope, sn_intern_cstr(&w.intern, "Counter"))
                  : NULL;
    CHECK("this: Counter collected", counter_sym != NULL);
    SnScope *member_scope =
        counter_sym ? sn_resolver_type_scope(&w.resolver, counter_sym->decl) : NULL;
    SnSymbol *method_sym =
        member_scope ? sn_scope_lookup_local(member_scope, sn_intern_cstr(&w.intern, "self"))
                     : NULL;
    CHECK("this: self() method collected", method_sym != NULL);

    if (method_sym) {
        w.checker.current_package = pkg;
        w.checker.current_imports = NULL;
        w.checker.enclosing_type = counter_sym->decl;
        sn_check_decl_body(&w.checker, (SnDecl *)method_sym->decl);
    }
    world_finish_diag(&w);
    CHECK("this: `return this` inside a method matching its own return type — no errors",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_nested_optional_rejected(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/opt2", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.opt2\n\n"
              "func bad(x: string" "?" "?): int {\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.opt2", "bad");
    CHECK("nested optional: T?? emits error", w.diag.error_count == 1);
    CHECK("nested optional: emits SNOVA0204", diag_has_code(&w, "SNOVA0204"));

    sn_arena_free(&w.arena);
}

static void test_null_coalescing_semantics(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/nullcoalesce", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.nullcoalesce\n\n"
              "func bad_fallback(opt: Option<int>): int {\n"
              "    return opt ?? \"not an int\"\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.nullcoalesce", "bad_fallback");
    CHECK("null coalescing: mismatched fallback emits error", w.diag.error_count == 1);
    CHECK("null coalescing: emits SNOVA0202", diag_has_code(&w, "SNOVA0202"));

    sn_arena_free(&w.arena);
}

static void test_implicit_any_rejected(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/anyinf", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.anyinf\n\n"
              "func bad(x: any): int {\n"
              "    let y = x\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.anyinf", "bad");
    CHECK("implicit any: unannotated inference emits SNOVA0211", diag_has_code(&w, "SNOVA0211"));

    sn_arena_free(&w.arena);
}

static void test_public_any_in_stdlib_rejected(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/pubany", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package stdlib.pubany\n\n"
              "public func bad(x: any): int {\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "stdlib.pubany", "bad");
    CHECK("public any: public stdlib signature with any emits SNOVA0210", diag_has_code(&w, "SNOVA0210"));

    sn_arena_free(&w.arena);
}

static void test_subsumption_non_null(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/subsumption", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.subsumption\n\n"
              "func accept_opt(opt: Option<int>): int {\n"
              "    return 0\n"
              "}\n"
              "func test(): int {\n"
              "    let x: int = 42\n"
              "    return accept_opt(x)\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.subsumption", "test");
    CHECK("subsumption: passing T to Option<T> parameter succeeds with 0 errors", w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_optional_type_required(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/optreq", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.optreq\n\n"
              "func accept_concrete(val: int): int {\n"
              "    return val\n"
              "}\n"
              "func test(opt: Option<int>): int {\n"
              "    return accept_concrete(opt)\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.optreq", "test");
    CHECK("optional type required: passing Option<T> where T is expected emits error", w.diag.error_count == 1);

    sn_arena_free(&w.arena);
}

static void test_pulsar_and_async_rules(const char *root) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/pulsar_async", root);
    mkdir(dir, 0777);

    write_file(dir, "p.snl",
              "package pkg.pulsar_async\n"
              "\n"
              "pulsar func worker(): unit {\n"
              "    return\n"
              "}\n"
              "\n"
              "func caller(): unit {\n"
              "    pulsar worker()\n"
              "    return\n"
              "}\n"
              "\n"
              "async func async_with_pulsar_stmt(): unit {\n"
              "    pulsar worker()\n"
              "    return\n"
              "}\n"
              "\n"
              "async func async_with_direct_pulsar_call(): unit {\n"
              "    worker()\n"
              "    return\n"
              "}\n"
              "\n"
              "async pulsar func invalid_combined(): unit {\n"
              "    return\n"
              "}\n");

    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.pulsar_async", "caller");
        CHECK("pulsar: legal pulsar launch in synchronous func passes", w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.pulsar_async", "async_with_pulsar_stmt");
        CHECK("pulsar in async: pulsar statement in async func rejected with SNOVA0125",
              w.diag.error_count > 0 && diag_has_code(&w, "SNOVA0125"));
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.pulsar_async", "async_with_direct_pulsar_call");
        CHECK("pulsar in async: pulsar call in async func rejected with SNOVA0125",
              w.diag.error_count > 0 && diag_has_code(&w, "SNOVA0125"));
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.pulsar_async", "invalid_combined");
        CHECK("pulsar in async: combined async pulsar func rejected with SNOVA0125",
              w.diag.error_count > 0 && diag_has_code(&w, "SNOVA0125"));
        sn_arena_free(&w.arena);
    }
}

static void test_struct_field_rejects_string(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/fieldty", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.fieldty\n\n"
              "public struct Snovalang {\n"
              "    public let field: int\n"
              "}\n"
              "func main(): unit {\n"
              "    let snova = Snovalang{field: \"Oi!\"}\n"
              "    return\n"
              "}\n");
    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.fieldty", "main");
    CHECK("struct field: string is not int", w.diag.error_count >= 1);
    CHECK("struct field: emits SNOVA0132", diag_has_code(&w, "SNOVA0132"));
    sn_arena_free(&w.arena);
}

/* Brace literals, copies, and `?.` must keep the field's type. `int?` accepts
 * `int` and rejects string, int64, long, and char. No implicit conversion. */
static void test_optional_struct_literal(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/optlit", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.optlit\n\n"
              "public struct Snovalang {\n"
              "    public let field: int?\n"
              "}\n"
              "func bad_string(): int {\n"
              "    let snova = Snovalang{\n"
              "        field: \"afad\"\n"
              "    }\n"
              "    let snova2 = snova\n"
              "    Console.println(snova?.field)\n"
              "    return 0\n"
              "}\n"
              "func bad_int64(): int {\n"
              "    let a: int64 = 1\n"
              "    let snova = Snovalang{ field: a }\n"
              "    return 0\n"
              "}\n"
              "func bad_long(): int {\n"
              "    let a: long = 1L\n"
              "    let snova = Snovalang{ field: a }\n"
              "    return 0\n"
              "}\n"
              "func bad_char(): int {\n"
              "    let a: char = 'a'\n"
              "    let snova = Snovalang{ field: a }\n"
              "    return 0\n"
              "}\n"
              "func bad_copy(): int {\n"
              "    let snova = Snovalang{ field: 1 }\n"
              "    let snova2: string = snova\n"
              "    return 0\n"
              "}\n"
              "func bad_qdot(): int {\n"
              "    let snova = Snovalang{ field: 1 }\n"
              "    let snova2 = snova\n"
              "    let text: string = snova2?.field\n"
              "    return 0\n"
              "}\n"
              "func good(): int {\n"
              "    let snova = Snovalang{ field: 1 }\n"
              "    let snova2 = snova\n"
              "    let n: int? = snova2?.field\n"
              "    return 0\n"
              "}\n");
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.optlit", "bad_string");
        CHECK("struct literal: string is not int?", w.diag.error_count >= 1);
        CHECK("struct literal: string into int? emits SNOVA0132",
              diag_has_code(&w, "SNOVA0132"));
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.optlit", "bad_int64");
        CHECK("struct literal: int64 is not int?", w.diag.error_count >= 1);
        CHECK("struct literal: int64 into int? emits SNOVA0132",
              diag_has_code(&w, "SNOVA0132"));
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.optlit", "bad_long");
        CHECK("struct literal: long is not int?", w.diag.error_count >= 1);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.optlit", "bad_char");
        CHECK("struct literal: char is not int?", w.diag.error_count >= 1);
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.optlit", "bad_copy");
        CHECK("struct copy: struct is not string", w.diag.error_count >= 1);
        CHECK("struct copy: emits SNOVA0138", diag_has_code(&w, "SNOVA0138"));
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.optlit", "bad_qdot");
        CHECK("optional field: ?. access is not string", w.diag.error_count >= 1);
        CHECK("optional field: ?. access emits SNOVA0138",
              diag_has_code(&w, "SNOVA0138"));
        sn_arena_free(&w.arena);
    }
    {
        World w;
        world_init(&w);
        run_check_on_func(&w, dir, "pkg.optlit", "good");
        CHECK("struct literal: int fits int? through a copy and ?.",
              w.diag.error_count == 0);
        sn_arena_free(&w.arena);
    }
}

