
static void test_return_type_mismatch(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/ret", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.ret\n\n"
              "func bad(): int {\n"
              "    return \"not an int\"\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.ret", "bad");
    CHECK("return: mismatched return type is exactly one error", w.diag.error_count == 1);
    CHECK("return: emits SNOVA0136", diag_has_code(&w, "SNOVA0136"));

    sn_arena_free(&w.arena);
}

static void test_let_type_mismatch(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/lettype", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.lettype\n\n"
              "func bad(): int {\n"
              "    let x: int = \"nope\"\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.lettype", "bad");
    CHECK("let type: declared type disagreeing with initializer is exactly one error",
          w.diag.error_count == 1);
    CHECK("let type: emits SNOVA0138", diag_has_code(&w, "SNOVA0138"));

    sn_arena_free(&w.arena);
}

static void test_empty_array_literal(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/arr", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.arr\n\n"
              "func bad(): int {\n"
              "    var xs = []\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.arr", "bad");
    CHECK("array: empty untyped literal is exactly one error", w.diag.error_count == 1);
    CHECK("array: emits SNOVA0100 (reused, matches the real fixture's meaning exactly)",
          diag_has_code(&w, "SNOVA0100"));

    sn_arena_free(&w.arena);
}

/* ── intrinsics, constructors and literal typing (2026-07-25) ─────────────── */

static void test_array_intrinsic(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/arrint", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.arrint\n\n"
              "func good(xs: Array<int>): int {\n"
              "    let n = xs.len()\n"
              "    let first = xs.get(0)\n"
              "    let joined = xs.join(\", \")\n"
              "    let alt: int[] = xs\n"
              "    return n + first\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.arrint", "good");
    CHECK("array intrinsic: Array<int> resolves and carries len/get/join — no errors",
          w.diag.error_count == 0);
    CHECK("array intrinsic: `int[]` is the same hash-consed type as `Array<int>`",
          !diag_has_code(&w, "SNOVA0138"));

    sn_arena_free(&w.arena);
}

static void test_array_intrinsic_unknown_member(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/arrbad", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.arrbad\n\n"
              "func bad(xs: Array<int>): int {\n"
              "    return xs.notAMember()\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.arrbad", "bad");
    CHECK("array intrinsic: a member outside the table is still SNOVA0028",
          diag_has_code(&w, "SNOVA0028"));

    sn_arena_free(&w.arena);
}

static void test_array_literal_with_declared_type(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/arrann", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.arrann\n\n"
              "func good(): int {\n"
              "    var ids: Array<string> = []\n"
              "    var xs: string[] = []\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.arrann", "good");
    CHECK("array: an annotated binding supplies the element type SNOVA0100 asks for",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_scalar_and_string_members(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/prim", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.prim\n\n"
              "func good(): int {\n"
              "    let n = 7\n"
              "    let s = n.toString()\n"
              "    let up = s.trim()\n"
              "    let len = up.length()\n"
              "    let empty = s.isEmpty()\n"
              "    return len\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.prim", "good");
    CHECK("primitives: toString/trim/length/isEmpty resolve with their modeled results",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_constructor_call(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/ctor", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.ctor\n\n"
              "struct Point {\n"
              "    public let x: decimal\n"
              "    public let y: decimal\n"
              "}\n\n"
              "func good(): int {\n"
              "    let p = Point(3.0, 4.0)\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.ctor", "good");
    CHECK("constructor: the field list is the signature, and `3.0` adopts `decimal`",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_constructor_arity(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/ctorbad", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.ctorbad\n\n"
              "struct Point {\n"
              "    public let x: int\n"
              "    public let y: int\n"
              "}\n\n"
              "func bad(): int {\n"
              "    let p = Point(1)\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.ctorbad", "bad");
    CHECK("constructor: too few arguments is exactly one error", w.diag.error_count == 1);
    CHECK("constructor: emits SNOVA0131 (arity)", diag_has_code(&w, "SNOVA0131"));

    sn_arena_free(&w.arena);
}

static void test_lambda_value_call(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/lamcall", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.lamcall\n\n"
              "func good(n: int): int {\n"
              "    let double = (x: int) -> x * 2\n"
              "    return double(n)\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.lamcall", "good");
    CHECK("call: a function-typed local is callable, not SNOVA0137",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_lambda_value_call_arity(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/lamarity", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.lamarity\n\n"
              "func bad(n: int): int {\n"
              "    let double = (x: int) -> x * 2\n"
              "    return double(n, n)\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.lamarity", "bad");
    CHECK("call: a function-typed local still has its arity checked",
          diag_has_code(&w, "SNOVA0131"));

    sn_arena_free(&w.arena);
}

static void test_subtype_assignability(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/sub", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.sub\n\n"
              "class Animal {\n"
              "    method speak(): string = \"...\"\n"
              "}\n\n"
              "class Dog : Animal {\n"
              "    override method speak(): string = \"woof\"\n"
              "}\n\n"
              "func good(): int {\n"
              "    let pet: Animal = Dog()\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.sub", "good");
    CHECK("subtyping: a Dog initializes an Animal binding", w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_subtype_not_symmetric(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/subrev", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.subrev\n\n"
              "class Animal {\n"
              "    method speak(): string = \"...\"\n"
              "}\n\n"
              "class Dog : Animal {\n"
              "    override method speak(): string = \"woof\"\n"
              "}\n\n"
              "func bad(a: Animal): int {\n"
              "    let pet: Dog = a\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.subrev", "bad");
    CHECK("subtyping: assignability is directional — an Animal is not a Dog",
          diag_has_code(&w, "SNOVA0138"));

    sn_arena_free(&w.arena);
}

static void test_literal_suffix_still_strict(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/suffix", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.suffix\n\n"
              "func bad(): int {\n"
              "    let a: int = 2L\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.suffix", "bad");
    CHECK("literals: a SUFFIXED literal is committed and still mismatches",
          diag_has_code(&w, "SNOVA0138"));

    sn_arena_free(&w.arena);
}

static void test_func_in_class_body(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/shape", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.shape\n\n"
              "class Greeter {\n"
              "    func greet(): string {\n"
              "        return \"hello\"\n"
              "    }\n"
              "}\n\n"
              "method badTopLevel(): int {\n"
              "    return 0\n"
              "}\n\n"
              "func main(): int {\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);
    world_finish_diag(&w);
    CHECK("shape: `func` inside a class body emits SNOVA0030",
          diag_has_code(&w, "SNOVA0030"));
    CHECK("shape: `method` at the top level emits SNOVA0031",
          diag_has_code(&w, "SNOVA0031"));

    sn_arena_free(&w.arena);
}

static void test_extension_func_is_legal(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/extfn", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.extfn\n\n"
              "class Counter {\n"
              "    private var value: int\n"
              "}\n\n"
              "extension Counter {\n"
              "    func isZero(): bool = value == 0\n"
              "}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);
    world_finish_diag(&w);
    CHECK("shape: `func` inside an `extension` body is legal — no SNOVA0030",
          !diag_has_code(&w, "SNOVA0030"));

    sn_arena_free(&w.arena);
}

static void test_cast_and_is(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/castis", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.castis\n\n"
              "func good(): long {\n"
              "    let x = 5\n"
              "    let y = x as long\n"
              "    let flag = x is int\n"
              "    return y\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.castis", "good");
    CHECK("cast/is: `as` trusts the annotation, `is` returns bool — no errors",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

