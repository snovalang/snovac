/* test_symbol.c — arena, intern-table, and nested-scope assertions.
 *
 * specs/20260719/snovac-p2-resolver-typechecker/plan.md §8 step 3 asks for
 * "unit tests de escopo aninhado". intern.c and symbol.c have no CLI surface
 * of their own yet (that arrives with resolve.c, several steps later), so —
 * unlike tests/run.sh, which drives the lexer/parser through the snovac
 * binary — this is a standalone C program that links arena.c, intern.c, and
 * symbol.c directly and asserts on their behavior.
 */
#include <stdio.h>
#include <string.h>

#include "../arena.h"
#include "../intern.h"
#include "../symbol.h"

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

static void test_intern(SnInternTable *it) {
    const char *a1 = sn_intern_cstr(it, "hello");
    const char *a2 = sn_intern_cstr(it, "hello");
    const char *b = sn_intern_cstr(it, "world");
    CHECK("intern: equal strings intern to the same pointer", a1 == a2);
    CHECK("intern: different strings intern to different pointers", a1 != b);
    CHECK("intern: content preserved", strcmp(a1, "hello") == 0);

    /* len-bounded variant must agree with the NUL-terminated one */
    const char *sliced = sn_intern(it, "helloXXXX", 5);
    CHECK("intern: sn_intern respects len, not the NUL", sliced == a1);

    /* two distinct strings that happen to share a prefix must not collide */
    const char *he = sn_intern_cstr(it, "he");
    const char *hell = sn_intern_cstr(it, "hell");
    CHECK("intern: prefix of an interned string is a distinct entry",
          he != a1 && hell != a1 && he != hell);

    const char *empty_n = sn_intern(it, "unused", 0);
    const char *empty_z = sn_intern_cstr(it, "");
    CHECK("intern: empty strings share one pointer",
          empty_n == empty_z && empty_n[0] == '\0');

    /* The length is the key. A NUL inside the slice must not end it. */
    char raw[3];
    raw[0] = 'a';
    raw[1] = '\0';
    raw[2] = 'b';
    const char *bin = sn_intern(it, raw, 3);
    CHECK("intern: embedded NUL is part of the key",
          bin != sn_intern_cstr(it, "a") && memcmp(bin, raw, 3) == 0 &&
              bin[3] == '\0');

    /* force several rehashes and confirm every entry is still found */
    char buf[32];
    const char *many[500];
    for (int i = 0; i < 500; i++) {
        snprintf(buf, sizeof(buf), "sym_%d", i);
        many[i] = sn_intern_cstr(it, buf);
    }
    int all = 1;
    for (int i = 0; i < 500; i++) {
        snprintf(buf, sizeof(buf), "sym_%d", i);
        if (sn_intern_cstr(it, buf) != many[i]) {
            all = 0;
        }
    }
    CHECK("intern: every entry survives rehash", all);
}

/* Identity checks still pass if the allocator opens a new block too early
 * or drops the byte reserved for a NUL. These assertions watch capacity. */
static void test_arena(void) {
    SnArena small;
    sn_arena_init(&small, 64);
    unsigned char *p = sn_arena_alloc(&small, 16);
    unsigned char *q = sn_arena_alloc(&small, 16);
    memset(p, 0x11, 16);
    memset(q, 0x22, 16);
    CHECK("arena: a small request keeps the configured block",
          small.total_bytes == 64 && q == p + 16 && p[0] == 0x11);

    SnArena fit;
    sn_arena_init(&fit, 32);
    unsigned char *f1 = sn_arena_alloc(&fit, 16);
    unsigned char *f2 = sn_arena_alloc(&fit, 16);
    CHECK("arena: an exact fit stays in the block",
          fit.total_bytes == 32 && f2 == f1 + 16);

    SnArena grown;
    sn_arena_init(&grown, 32);
    unsigned char *big = sn_arena_alloc(&grown, 40);
    memset(big, 0x33, 40);
    CHECK("arena: a request past the block size rounds the block up",
          grown.total_bytes == 48 && big[39] == 0x33);

    SnArena z;
    sn_arena_init(&z, 64);
    void *z1 = sn_arena_alloc(&z, 0);
    void *z2 = sn_arena_alloc(&z, 0);
    CHECK("arena: zero-size allocs are distinct", z1 != NULL && z2 != NULL && z1 != z2);

    char raw[16];
    memset(raw, 'A', sizeof(raw));
    SnArena texts;
    sn_arena_init(&texts, 256);
    char *dup = sn_arena_strndup(&texts, raw, 16);
    char *after = sn_arena_alloc(&texts, 16);
    memset(after, 'B', 16);
    char *empty = sn_arena_strndup(&texts, raw, 0);
    CHECK("arena: strndup keeps n bytes and a terminator",
          memcmp(dup, raw, 16) == 0 && dup[16] == '\0' && strlen(dup) == 16);
    CHECK("arena: strndup of zero bytes is an empty string", empty[0] == '\0');

    sn_arena_free(&texts);
    CHECK("arena: free drops the blocks and keeps the block size",
          texts.head == NULL && texts.total_bytes == 0 && texts.block_size == 256);
    sn_arena_alloc(&texts, 8);
    CHECK("arena: the block size still applies after free", texts.total_bytes == 256);

    SnArena def;
    sn_arena_init(&def, 0);
    sn_arena_alloc(&def, 1);
    CHECK("arena: a zero block size selects 64KiB",
          def.block_size == (size_t)64 * 1024 && def.total_bytes == (size_t)64 * 1024);

    sn_arena_free(&small);
    sn_arena_free(&fit);
    sn_arena_free(&grown);
    sn_arena_free(&z);
    sn_arena_free(&texts);
    sn_arena_free(&def);
}

static void test_intern_capacity(void) {
    SnArena arena;
    sn_arena_init(&arena, 4096);
    SnInternTable it;
    sn_intern_init(&it, &arena);

    /* 8 bytes puts the terminator on the far side of the 16-byte alignment
     * padding. The following allocation is then filled with non-zero bytes:
     * if the entry was sized one short, that fill overwrites the NUL. */
    const char *bounded = sn_intern(&it, "01234567", 8);
    unsigned char *tail = sn_arena_alloc(&arena, 16);
    memset(tail, 0x5A, 16);
    CHECK("intern: stored text keeps its terminator",
          memcmp(bounded, "01234567", 8) == 0 && bounded[8] == '\0');

    /* Fresh table: 64 buckets, rehash when the 49th distinct string is
     * inserted (load above 3/4). 48 must not have doubled yet. */
    SnArena fresh_arena;
    sn_arena_init(&fresh_arena, 0);
    SnInternTable fresh;
    sn_intern_init(&fresh, &fresh_arena);
    CHECK("intern: table starts at 64 buckets", fresh.nbuckets == 64 && fresh.count == 0);

    const char *seen[64];
    char buf[32];
    for (int i = 0; i < 48; i++) {
        snprintf(buf, sizeof(buf), "cap_%d", i);
        seen[i] = sn_intern_cstr(&fresh, buf);
    }
    CHECK("intern: 48 strings stay in the initial buckets",
          fresh.nbuckets == 64 && fresh.count == 48);

    size_t count_at_48 = fresh.count;
    const char *again = sn_intern_cstr(&fresh, "cap_0");
    CHECK("intern: a duplicate does not grow the table",
          again == seen[0] && fresh.count == count_at_48 && fresh.nbuckets == 64);

    snprintf(buf, sizeof(buf), "cap_%d", 48);
    seen[48] = sn_intern_cstr(&fresh, buf);
    CHECK("intern: the 49th string doubles the buckets",
          fresh.nbuckets == 128 && fresh.count == 49 && seen[48] != NULL);

    int all = 1;
    for (int i = 0; i < 49; i++) {
        snprintf(buf, sizeof(buf), "cap_%d", i);
        if (sn_intern_cstr(&fresh, buf) != seen[i]) {
            all = 0;
        }
    }
    CHECK("intern: keys inserted across the rehash still match", all);

    sn_arena_free(&arena);
    sn_arena_free(&fresh_arena);
}

static void test_scope_load(void) {
    SnArena arena;
    sn_arena_init(&arena, 0);
    SnInternTable it;
    sn_intern_init(&it, &arena);

    SnSpan span;
    memset(&span, 0, sizeof(span));
    span.offset = 11;
    span.len = 3;
    span.line = 4;
    span.col = 7;

    SnScope scope;
    sn_scope_init(&scope, &arena, NULL);
    CHECK("scope: a new scope starts at 8 buckets", scope.nbuckets == 8 && scope.count == 0);

    SnSymbol *syms[8];
    char buf[32];
    for (int i = 0; i < 6; i++) {
        snprintf(buf, sizeof(buf), "slot_%d", i);
        const char *name = sn_intern_cstr(&it, buf);
        syms[i] = sn_scope_define(&scope, name, SN_SYM_LOCAL, NULL, span);
    }
    CHECK("scope: six definitions stay under the 3/4 ceiling",
          scope.nbuckets == 8 && scope.count == 6 && syms[0] != NULL);
    CHECK("scope: define records the span and leaves checker fields clear",
          syms[0]->span.offset == 11 && syms[0]->span.len == 3 &&
              syms[0]->span.line == 4 && syms[0]->span.col == 7 &&
              syms[0]->decl == NULL && syms[0]->value_type == NULL &&
              syms[0]->is_mutable == 0 && syms[0]->origin == NULL);

    snprintf(buf, sizeof(buf), "slot_%d", 6);
    const char *last_name = sn_intern_cstr(&it, buf);
    SnSymbol *extra = sn_scope_define(&scope, last_name, SN_SYM_FUNC, NULL, span);
    CHECK("scope: the seventh definition doubles the buckets",
          extra != NULL && extra->kind == SN_SYM_FUNC && scope.nbuckets == 16 &&
              scope.count == 7);

    int all = 1;
    for (int i = 0; i < 6; i++) {
        snprintf(buf, sizeof(buf), "slot_%d", i);
        const char *name = sn_intern_cstr(&it, buf);
        if (sn_scope_lookup_local(&scope, name) != syms[i]) {
            all = 0;
        }
    }
    CHECK("scope: earlier definitions survive the rehash", all);
    CHECK("scope: lookup walks to the definition that triggered rehash",
          sn_scope_lookup(&scope, last_name) == extra);

    SnSymbol *dup = sn_scope_define(&scope, last_name, SN_SYM_FIELD, NULL, span);
    CHECK("scope: a second kind for the same name is still a duplicate",
          dup == NULL && scope.count == 7 &&
              sn_scope_lookup_local(&scope, last_name) == extra);

    sn_arena_free(&arena);
}

static void test_scope_single(SnInternTable *it, SnArena *a) {
    SnScope root;
    sn_scope_init(&root, a, NULL);

    const char *foo = sn_intern_cstr(it, "foo");
    SnSpan span0;
    memset(&span0, 0, sizeof(span0));

    SnSymbol *def1 = sn_scope_define(&root, foo, SN_SYM_LOCAL, NULL, span0);
    CHECK("scope: first define succeeds", def1 != NULL);
    CHECK("scope: defined symbol keeps its name/kind", def1 != NULL &&
                                                            def1->name == foo &&
                                                            def1->kind == SN_SYM_LOCAL);

    SnSymbol *dup = sn_scope_define(&root, foo, SN_SYM_LOCAL, NULL, span0);
    CHECK("scope: duplicate define in the same scope is rejected", dup == NULL);
    CHECK("scope: rejected duplicate did not replace the original",
          sn_scope_lookup_local(&root, foo) == def1);

    CHECK("scope: local lookup finds it", sn_scope_lookup_local(&root, foo) == def1);

    const char *bar = sn_intern_cstr(it, "bar");
    CHECK("scope: undefined name is not found",
          sn_scope_lookup_local(&root, bar) == NULL);
    CHECK("scope: lookup (not just lookup_local) also fails for undefined names",
          sn_scope_lookup(&root, bar) == NULL);
}

static void test_scope_nesting(SnInternTable *it, SnArena *a) {
    SnSpan span0;
    memset(&span0, 0, sizeof(span0));

    SnScope root;
    sn_scope_init(&root, a, NULL);
    const char *foo = sn_intern_cstr(it, "foo");
    SnSymbol *def1 = sn_scope_define(&root, foo, SN_SYM_LOCAL, NULL, span0);

    SnScope child;
    sn_scope_init(&child, a, &root);
    CHECK("scope: child sees parent's symbol via lookup",
          sn_scope_lookup(&child, foo) == def1);
    CHECK("scope: child lookup_local does NOT see the parent's symbol",
          sn_scope_lookup_local(&child, foo) == NULL);

    SnSymbol *shadow = sn_scope_define(&child, foo, SN_SYM_LOCAL, NULL, span0);
    CHECK("scope: child can shadow the parent's name",
          shadow != NULL && shadow != def1);
    CHECK("scope: lookup from the child now finds the shadow",
          sn_scope_lookup(&child, foo) == shadow);
    CHECK("scope: the parent itself is unaffected by the child's shadow",
          sn_scope_lookup_local(&root, foo) == def1);

    SnScope grandchild;
    sn_scope_init(&grandchild, a, &child);
    const char *baz = sn_intern_cstr(it, "baz_not_yet_defined");
    CHECK("scope: grandchild walks multiple levels and still fails cleanly",
          sn_scope_lookup(&grandchild, baz) == NULL);

    SnSymbol *def_baz = sn_scope_define(&root, baz, SN_SYM_TYPE, NULL, span0);
    CHECK("scope: grandchild sees a symbol added later to a distant ancestor",
          sn_scope_lookup(&grandchild, baz) == def_baz);
}

static void test_scope_rehash(SnInternTable *it, SnArena *a) {
    SnSpan span0;
    memset(&span0, 0, sizeof(span0));

    SnScope big;
    sn_scope_init(&big, a, NULL);

    char buf[32];
    SnSymbol *first_sym = NULL;
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof(buf), "member_%d", i);
        const char *name = sn_intern_cstr(it, buf);
        SnSymbol *sym = sn_scope_define(&big, name, SN_SYM_METHOD, NULL, span0);
        if (i == 0) {
            first_sym = sym;
        }
    }
    const char *member0 = sn_intern_cstr(it, "member_0");
    const char *member199 = sn_intern_cstr(it, "member_199");
    CHECK("scope: survives rehash — earliest entry still found",
          sn_scope_lookup_local(&big, member0) == first_sym);
    CHECK("scope: survives rehash — latest entry also found",
          sn_scope_lookup_local(&big, member199) != NULL);
}

int main(void) {
    test_arena();
    test_intern_capacity();
    test_scope_load();

    SnArena arena;
    sn_arena_init(&arena, 0);

    SnInternTable it;
    sn_intern_init(&it, &arena);

    test_intern(&it);
    test_scope_single(&it, &arena);
    test_scope_nesting(&it, &arena);
    test_scope_rehash(&it, &arena);

    sn_arena_free(&arena);

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
