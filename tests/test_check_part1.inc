/* test_check.c — expression/statement type checking.
 *
 * specs/20260719/snovac-p2-resolver-typechecker/plan.md §8 steps 7-8
 * verification, scoped to what check.c actually covers (see check.h's file
 * header for the explicit list of what's solid vs. documented-gap).
 * Standalone C binary, same rationale as the other test_*.c files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define mkdir(dir, mode) _mkdir(dir)
#endif

#include "arena.h"
#include "check.h"
#include "diag.h"
#include "intern.h"
#include "package.h"
#include "resolve.h"
#include "symbol.h"
#include "types.h"

static int pass = 0;
static int fail = 0;

#define CHECK(name, cond)                                                    \
    do {                                                                     \
        if (cond) {                                                          \
            pass++;                                                         \
        } else {                                                             \
            fail++;                                                         \
            printf("FAIL %s\n", name);                                      \
        }                                                                    \
    } while (0)

static void write_file(const char *dir, const char *name, const char *content) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("FAIL setup: could not create %s\n", path);
        return;
    }
    fputs(content, f);
    fclose(f);
}

typedef struct {
    SnArena arena;
    SnInternTable intern;
    char *diag_buf;
    size_t diag_buf_len;
    SnDiagSink diag;
    SnPackageGraph graph;
    SnTypeTable types;
    SnResolver resolver;
    SnChecker checker;
} World;

static void world_init(World *w) {
    sn_arena_init(&w->arena, 0);
    sn_intern_init(&w->intern, &w->arena);
    sn_diag_init(&w->diag, "<test>", "", 0);
#if defined(_WIN32)
    w->diag_buf = NULL;
    w->diag_buf_len = 0;
    w->diag.out = tmpfile();
#else
    w->diag.out = open_memstream(&w->diag_buf, &w->diag_buf_len);
#endif
    sn_pkggraph_init(&w->graph, &w->arena, &w->intern, &w->diag);
    sn_types_init(&w->types, &w->arena);
    sn_resolver_init(&w->resolver, &w->arena, &w->intern, &w->diag, &w->graph, &w->types);
    sn_checker_init(&w->checker, &w->arena, &w->intern, &w->diag, &w->resolver, &w->types);
}

static void world_finish_diag(World *w) {
#if defined(_WIN32)
    if (w->diag.out) {
        fflush(w->diag.out);
        long sz = ftell(w->diag.out);
        if (sz >= 0) {
            rewind(w->diag.out);
            w->diag_buf = (char *)malloc((size_t)sz + 1);
            if (w->diag_buf) {
                size_t read_bytes = fread(w->diag_buf, 1, (size_t)sz, w->diag.out);
                w->diag_buf[read_bytes] = '\0';
                w->diag_buf_len = read_bytes;
            }
        }
        fclose(w->diag.out);
        w->diag.out = NULL;
    }
#else
    fclose(w->diag.out);
#endif
}

static int diag_has_code(World *w, const char *code /* e.g. "SNOVA0131" */) {
    return w->diag_buf && strstr(w->diag_buf, code) != NULL;
}

/* Scans `dir`, collects, builds the prelude, then checks the body of
 * top-level func `func_name` in `package_name`. Leaves diag output flushed
 * into w->diag_buf so callers can inspect error_count and/or diag_has_code. */
static void run_check_on_func(World *w, const char *dir, const char *package_name,
                              const char *func_name) {
    sn_pkggraph_scan_root(&w->graph, dir);
    sn_resolver_collect(&w->resolver);
    sn_resolver_build_prelude(&w->resolver);

    const char *pkg = sn_intern_cstr(&w->intern, package_name);
    SnScope *pkg_scope = sn_resolver_package_scope(&w->resolver, pkg);
    SnSymbol *fn_sym =
        pkg_scope ? sn_scope_lookup_local(pkg_scope, sn_intern_cstr(&w->intern, func_name))
                  : NULL;
    if (!fn_sym) {
        world_finish_diag(w);
        return;
    }

    w->checker.current_package = pkg;
    w->checker.current_imports = NULL;
    w->checker.enclosing_type = NULL;
    sn_check_decl_body(&w->checker, fn_sym->decl);
    world_finish_diag(w);
}

/* ── scenarios ────────────────────────────────────────────────────────────── */

static void test_literals_and_arithmetic(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/lit", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.lit\n\n"
              "func good(): int {\n"
              "    let a = 1\n"
              "    let b = 2\n"
              "    let c = a + b\n"
              "    return c\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.lit", "good");
    CHECK("literals: int + int, matching return type — no errors", w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_no_implicit_promotion(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/promo", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.promo\n\n"
              "func bad(): int {\n"
              "    let a: int = 1\n"
              "    let b: long = 2L\n"
              "    let c = a + b\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.promo", "bad");
    CHECK("no promotion: int + long is exactly one error", w.diag.error_count == 1);
    CHECK("no promotion: emits SNOVA0133 (binary type mismatch)",
          diag_has_code(&w, "SNOVA0133"));

    sn_arena_free(&w.arena);
}

static void test_string_concat(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/strcat", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.strcat\n\n"
              "func greet(): string {\n"
              "    let name = \"world\"\n"
              "    return \"hello \" + name\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.strcat", "greet");
    CHECK("string concat: string + string returns string, matches return type — no errors",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_unary_operators(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/unary", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.unary\n\n"
              "func good(): bool {\n"
              "    let x = 5\n"
              "    let y = -x\n"
              "    let flag = true\n"
              "    return !flag\n"
              "}\n\n"
              "func bad(): int {\n"
              "    let flag = true\n"
              "    let broken = -flag\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.unary", "good");
    CHECK("unary: -int and !bool are both fine", w.diag.error_count == 0);
    sn_arena_free(&w.arena);

    World w2;
    world_init(&w2);
    run_check_on_func(&w2, dir, "pkg.unary", "bad");
    CHECK("unary: -bool is exactly one error", w2.diag.error_count == 1);
    CHECK("unary: emits SNOVA0134", diag_has_code(&w2, "SNOVA0134"));
    sn_arena_free(&w2.arena);
}

static void test_assignment_mutability(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/mut", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.mut\n\n"
              "func reassignVar(): int {\n"
              "    var total = 0\n"
              "    total = 5\n"
              "    return total\n"
              "}\n\n"
              "func reassignLet(): int {\n"
              "    let total = 0\n"
              "    total = 5\n"
              "    return total\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.mut", "reassignVar");
    CHECK("mutability: reassigning a `var` is fine", w.diag.error_count == 0);
    sn_arena_free(&w.arena);

    World w2;
    world_init(&w2);
    run_check_on_func(&w2, dir, "pkg.mut", "reassignLet");
    CHECK("mutability: reassigning a `let` is exactly one error", w2.diag.error_count == 1);
    CHECK("mutability: emits SNOVA0047 (already fixture-backed code, reused)",
          diag_has_code(&w2, "SNOVA0047"));
    sn_arena_free(&w2.arena);
}

static void test_member_access_with_inheritance(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/member", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.member\n\n"
              "class Animal {\n"
              "    method speak(): string {\n"
              "        return \"...\"\n"
              "    }\n"
              "}\n\n"
              "class Dog extends Animal {\n"
              "    method bark(): string {\n"
              "        return \"woof\"\n"
              "    }\n"
              "}\n\n"
              "func makeNoise(d: Dog): string {\n"
              "    return d.speak()\n"
              "}\n\n"
              "func badMember(d: Dog): string {\n"
              "    return d.nonexistent()\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.member", "makeNoise");
    CHECK("member access: inherited method call resolves — no errors",
          w.diag.error_count == 0);
    sn_arena_free(&w.arena);

    World w2;
    world_init(&w2);
    run_check_on_func(&w2, dir, "pkg.member", "badMember");
    CHECK("member access: unknown member is exactly one error", w2.diag.error_count == 1);
    CHECK("member access: emits SNOVA0028 (already fixture-backed code, reused)",
          diag_has_code(&w2, "SNOVA0028"));
    sn_arena_free(&w2.arena);
}

static void test_call_arity_and_arg_types(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/call", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.call\n\n"
              "func add(a: int, b: int): int {\n"
              "    return a + b\n"
              "}\n\n"
              "func good(): int {\n"
              "    return add(1, 2)\n"
              "}\n\n"
              "func wrongArity(): int {\n"
              "    return add(1)\n"
              "}\n\n"
              "func wrongArgType(): int {\n"
              "    return add(1, \"two\")\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.call", "good");
    CHECK("call: correct arity and types — no errors", w.diag.error_count == 0);
    sn_arena_free(&w.arena);

    World w2;
    world_init(&w2);
    run_check_on_func(&w2, dir, "pkg.call", "wrongArity");
    CHECK("call: arity mismatch is exactly one error", w2.diag.error_count == 1);
    CHECK("call: emits SNOVA0131", diag_has_code(&w2, "SNOVA0131"));
    sn_arena_free(&w2.arena);

    World w3;
    world_init(&w3);
    run_check_on_func(&w3, dir, "pkg.call", "wrongArgType");
    CHECK("call: arg type mismatch is exactly one error", w3.diag.error_count == 1);
    CHECK("call: emits SNOVA0132", diag_has_code(&w3, "SNOVA0132"));
    sn_arena_free(&w3.arena);
}

static void test_variant_construction(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/variant", tmp);
    mkdir(dir, 0755);
    write_file(dir, "types.snl",
              "package builtin.types.Types\n\n"
              "enum Option {\n"
              "    Some(value: int),\n"
              "    None,\n"
              "}\n");
    write_file(dir, "user.snl",
              "package pkg.user\n\n"
              "func makeSome(): int {\n"
              "    let x = Some(1)\n"
              "    return 1\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.user", "makeSome");
    CHECK("variant construction: `Some(1)` resolves through the prelude with no errors",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_condition_must_be_bool(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/cond", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snl",
              "package pkg.cond\n\n"
              "func bad(): int {\n"
              "    let n = 5\n"
              "    if n {\n"
              "        return 1\n"
              "    }\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.cond", "bad");
    CHECK("condition: non-bool `if` condition is exactly one error", w.diag.error_count == 1);
    CHECK("condition: emits SNOVA0135", diag_has_code(&w, "SNOVA0135"));

    sn_arena_free(&w.arena);
}
