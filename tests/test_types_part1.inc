/* test_types.c — hash-consing assertions for types.c.
 *
 * specs/20260719/snovac-p2-resolver-typechecker/plan.md §8 step 5
 * verification ("List<int> == List<int> por ponteiro"). Standalone C binary,
 * same rationale as test_symbol.c / test_package.c: types.c has no CLI
 * surface yet.
 */
#include <stdio.h>

#include "arena.h"
#include "builtins.h"
#include "intern.h"
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

static SnSymbol *fake_decl(SnScope *scope, SnInternTable *it, const char *name) {
    SnSpan span;
    span.offset = 0;
    span.len = 0;
    span.line = 1;
    span.col = 1;
    return sn_scope_define(scope, sn_intern_cstr(it, name), SN_SYM_TYPE, NULL,
                            span);
}

static void test_primitives(SnTypeTable *t) {
    CHECK("primitives: int is stable across calls",
          sn_type_int(t) == sn_type_int(t));
    CHECK("primitives: int and long are distinct",
          sn_type_int(t) != sn_type_long(t));
    CHECK("primitives: error is its own tag",
          sn_type_error(t)->tag == SN_T_ERROR);
    CHECK("primitives: string and char are distinct",
          sn_type_string(t) != sn_type_char(t));
    CHECK("primitives: all nine are pairwise distinct",
          sn_type_error(t) != sn_type_unit(t) &&
              sn_type_unit(t) != sn_type_bool(t) &&
              sn_type_bool(t) != sn_type_int(t) &&
              sn_type_int(t) != sn_type_long(t) &&
              sn_type_long(t) != sn_type_double(t) &&
              sn_type_double(t) != sn_type_decimal(t) &&
              sn_type_decimal(t) != sn_type_string(t) &&
              sn_type_string(t) != sn_type_char(t));
    CHECK("primitives: integer ladder is distinct from int",
          sn_type_int8(t) != sn_type_int(t) &&
              sn_type_int16(t) != sn_type_int8(t) &&
              sn_type_int32(t) != sn_type_int16(t) &&
              sn_type_int64(t) != sn_type_int(t) &&
              sn_type_int64(t) != sn_type_long(t) &&
              sn_type_int128(t) != sn_type_int64(t) &&
              sn_type_byte(t) != sn_type_int8(t) &&
              sn_type_integer_width(sn_type_int(t)) == 64 &&
              sn_type_integer_width(sn_type_int8(t)) == 8 &&
              sn_type_integer_width(sn_type_byte(t)) == 8 &&
              sn_type_integer_signed(sn_type_int(t)) &&
              !sn_type_integer_signed(sn_type_byte(t)) &&
              sn_type_integer_width(sn_type_char(t)) == 0);
    CHECK("catalog: every primitive name resolves to its singleton",
          sn_builtin_primitive_count() >= 16 &&
              sn_builtin_primitive_type(t, "int") == sn_type_int(t) &&
              sn_builtin_primitive_type(t, "string") == sn_type_string(t) &&
              sn_builtin_primitive_type(t, "bool") == sn_type_bool(t) &&
              sn_builtin_primitive_type(t, "float") == sn_type_float(t) &&
              sn_builtin_primitive_type(t, "any") == sn_type_any(t) &&
              sn_builtin_primitive_type(t, "int8") == sn_type_int8(t) &&
              sn_builtin_primitive_type(t, "int16") == sn_type_int16(t) &&
              sn_builtin_primitive_type(t, "int32") == sn_type_int32(t) &&
              sn_builtin_primitive_type(t, "int64") == sn_type_int64(t) &&
              sn_builtin_primitive_type(t, "int128") == sn_type_int128(t) &&
              sn_builtin_primitive_type(t, "byte") == sn_type_byte(t) &&
              sn_builtin_primitive_type(t, "char") == sn_type_char(t) &&
              sn_builtin_primitive_type(t, "unit") == sn_type_unit(t) &&
              sn_builtin_primitive_type(t, "long") == sn_type_long(t) &&
              sn_builtin_primitive_type(t, "double") == sn_type_double(t) &&
              sn_builtin_primitive_type(t, "decimal") == sn_type_decimal(t));
    CHECK("catalog: a type name is not a primitive",
          !sn_builtin_is_primitive_name("Message") &&
              sn_builtin_primitive_type(t, "Int") == NULL &&
              sn_builtin_primitive_type(t, "Nope") == NULL);
}

static void test_named_generics(SnTypeTable *t, SnScope *scope, SnInternTable *it) {
    SnSymbol *list_decl = fake_decl(scope, it, "List");
    SnSymbol *map_decl = fake_decl(scope, it, "Map");

    SnTypeRep *int_args[1];
    int_args[0] = sn_type_int(t);
    SnTypeRep *list_int_a = sn_type_named(t, list_decl, int_args, 1);

    SnTypeRep *int_args2[1];
    int_args2[0] = sn_type_int(t); /* fresh local array, same content */
    SnTypeRep *list_int_b = sn_type_named(t, list_decl, int_args2, 1);

    CHECK("named: List<int> built twice is the same pointer",
          list_int_a == list_int_b);

    SnTypeRep *string_args[1];
    string_args[0] = sn_type_string(t);
    SnTypeRep *list_string = sn_type_named(t, list_decl, string_args, 1);
    CHECK("named: List<int> != List<string>", list_int_a != list_string);

    SnTypeRep *map_int = sn_type_named(t, map_decl, int_args, 1);
    CHECK("named: List<int> != Map<int> (different decl, same args)",
          list_int_a != map_int);

    SnTypeRep *list_no_args = sn_type_named(t, list_decl, NULL, 0);
    CHECK("named: List<int> != raw List (arity differs)",
          list_int_a != list_no_args);

    /* plan.md's own example, nested: List<List<int>> == List<List<int>> */
    SnTypeRep *inner_a[1];
    inner_a[0] = list_int_a;
    SnTypeRep *nested_a = sn_type_named(t, list_decl, inner_a, 1);

    SnTypeRep *inner_b[1];
    inner_b[0] = list_int_b; /* == list_int_a, but constructed independently */
    SnTypeRep *nested_b = sn_type_named(t, list_decl, inner_b, 1);

    CHECK("named: List<List<int>> == List<List<int>> (nested hash-consing)",
          nested_a == nested_b);
}

static void test_typevar(SnTypeTable *t, SnScope *scope, SnInternTable *it) {
    SnSymbol *tp = fake_decl(scope, it, "T");
    SnSymbol *up = fake_decl(scope, it, "U");

    CHECK("typevar: same decl is the same pointer",
          sn_type_typevar(t, tp) == sn_type_typevar(t, tp));
    CHECK("typevar: different decls are distinct",
          sn_type_typevar(t, tp) != sn_type_typevar(t, up));
    CHECK("typevar: not confused with a NAMED type of the same decl",
          sn_type_typevar(t, tp)->tag == SN_T_TYPEVAR);
}

static void test_func(SnTypeTable *t) {
    SnTypeRep *params_a[2];
    params_a[0] = sn_type_int(t);
    params_a[1] = sn_type_string(t);
    SnTypeRep *fn_a = sn_type_func(t, params_a, 2, sn_type_bool(t));

    SnTypeRep *params_b[2];
    params_b[0] = sn_type_int(t);
    params_b[1] = sn_type_string(t);
    SnTypeRep *fn_b = sn_type_func(t, params_b, 2, sn_type_bool(t));
    CHECK("func: (int, string) -> bool built twice is the same pointer",
          fn_a == fn_b);

    SnTypeRep *fn_diff_ret = sn_type_func(t, params_a, 2, sn_type_int(t));
    CHECK("func: differing return type is a different pointer",
          fn_a != fn_diff_ret);

    SnTypeRep *params_c[2];
    params_c[0] = sn_type_string(t); /* order swapped */
    params_c[1] = sn_type_int(t);
    SnTypeRep *fn_swapped = sn_type_func(t, params_c, 2, sn_type_bool(t));
    CHECK("func: parameter order matters", fn_a != fn_swapped);

    SnTypeRep *fn_zero_params = sn_type_func(t, NULL, 0, sn_type_unit(t));
    CHECK("func: zero-parameter function type works",
          fn_zero_params->tag == SN_T_FUNC && fn_zero_params->nargs == 0);
}

static void test_array(SnTypeTable *t) {
    SnTypeRep *arr_int_a = sn_type_array(t, sn_type_int(t));
    SnTypeRep *arr_int_b = sn_type_array(t, sn_type_int(t));
    CHECK("array: Array<int> built twice is the same pointer",
          arr_int_a == arr_int_b);

    SnTypeRep *arr_string = sn_type_array(t, sn_type_string(t));
    CHECK("array: Array<int> != Array<string>", arr_int_a != arr_string);

    SnTypeRep *arr_arr_int = sn_type_array(t, arr_int_a);
    CHECK("array: Array<Array<int>> != Array<int>", arr_arr_int != arr_int_a);
}

static void test_tag_isolation(SnTypeTable *t, SnScope *scope, SnInternTable *it) {
    /* An ARRAY and a FUNC can end up with coincidentally identical
     * args/nargs/ret if built carelessly (both default decl=NULL); the tag
     * must still keep them apart. Array<int> vs a zero-arg func returning
     * int share nargs=1 vs nargs=0 naturally, so construct a case that
     * actually collides on every field except tag: a NAMED type with
     * decl=NULL is not constructible through the public API, so instead
     * verify FUNC and ARRAY (whose "shape" both use args/nargs, decl=NULL)
     * never alias each other even at the same effective arity. */
    SnTypeRep *fn_params[1];
    fn_params[0] = sn_type_int(t);
    SnTypeRep *arr_int = sn_type_array(t, sn_type_int(t));
    SnTypeRep *fn_one_param = sn_type_func(t, fn_params, 1, sn_type_unit(t));
    CHECK("tag isolation: Array<int> (nargs=1, ret=NULL) != a 1-arg func "
          "(nargs=1, ret=unit) even though args[0] matches",
          arr_int != fn_one_param);
    (void)scope;
    (void)it;
}

static void test_rehash_stress(SnTypeTable *t, SnScope *scope, SnInternTable *it) {
    char buf[32];
    SnTypeRep *first = NULL;
    SnSymbol *decl = fake_decl(scope, it, "Stress");

    for (int i = 0; i < 300; i++) {
        snprintf(buf, sizeof(buf), "Elem%d", i);
        SnSymbol *elem_decl = fake_decl(scope, it, buf);
        SnTypeRep *args[1];
        args[0] = sn_type_typevar(t, elem_decl);
        SnTypeRep *ty = sn_type_named(t, decl, args, 1);
        if (i == 0) {
            first = ty;
        }
    }

    /* Rebuild the exact same shape as iteration 0 (same decl, same typevar
     * symbol looked back up by its interned name) and confirm it still
     * resolves to the same pointer after ~300 insertions forced at least a
     * couple of rehashes along the way. */
    SnSymbol *elem0_sym = sn_scope_lookup_local(scope, sn_intern_cstr(it, "Elem0"));
    SnTypeRep *rebuilt_args[1];
    rebuilt_args[0] = sn_type_typevar(t, elem0_sym);
    SnTypeRep *rebuilt = sn_type_named(t, decl, rebuilt_args, 1);
    CHECK("named: survives rehash across ~300 distinct generic instantiations",
          rebuilt == first);
}

/* Two shapes that share every field except a later argument. A comparison
 * loop that stops after args[0] would hash-cons them together. */
static void test_later_argument(SnTypeTable *t, SnScope *scope, SnInternTable *it) {
    SnSymbol *pair = fake_decl(scope, it, "Pair");
    SnTypeRep *left[2];
    left[0] = sn_type_int(t);
    left[1] = sn_type_string(t);
    SnTypeRep *right[2];
    right[0] = sn_type_int(t);
    right[1] = sn_type_bool(t);

    SnTypeRep *fn_left = sn_type_func(t, left, 2, sn_type_unit(t));
    SnTypeRep *fn_right = sn_type_func(t, right, 2, sn_type_unit(t));
    CHECK("func: a later parameter is part of the shape", fn_left != fn_right);

    SnTypeRep *left_again[2];
    left_again[0] = sn_type_int(t);
    left_again[1] = sn_type_string(t);
    CHECK("func: matching later parameter still hash-conses",
          fn_left == sn_type_func(t, left_again, 2, sn_type_unit(t)));

    CHECK("named: a later type argument is part of the shape",
          sn_type_named(t, pair, left, 2) != sn_type_named(t, pair, right, 2));

    /* Three parameters: the argument array is sized with nargs * sizeof.
     * Reading the last slot after intern fails if that product is truncated
     * and the following allocation overwrites the tail. */
    SnTypeRep *three[3];
    three[0] = sn_type_int(t);
    three[1] = sn_type_string(t);
    three[2] = sn_type_bool(t);
    SnTypeRep *fn3 = sn_type_func(t, three, 3, sn_type_unit(t));
    SnTypeRep *three_again[3];
    three_again[0] = sn_type_int(t);
    three_again[1] = sn_type_string(t);
    three_again[2] = sn_type_bool(t);
    CHECK("func: three parameters hash-cons and keep the last one",
          fn3 == sn_type_func(t, three_again, 3, sn_type_unit(t)) &&
              fn3->nargs == 3 && fn3->args[2] == sn_type_bool(t));
}

/* sn_types_init caches 17 primitive singletons in a 64-bucket table.
 * Rehash runs when (count + 1) * 4 > nbuckets * 3, i.e. the insert that
 * starts at count == 48 doubles 64 to 128. 31 further typevars land exactly
 * on that boundary. */
static void test_load_factor(SnArena *scratch) {
    SnInternTable it;
    sn_intern_init(&it, scratch);
    SnTypeTable t;
    sn_types_init(&t, scratch);
    SnScope scope;
    sn_scope_init(&scope, scratch, NULL);

    CHECK("load: seventeen primitive singletons", t.count == 17u);
    CHECK("load: initial capacity is 64", t.nbuckets == 64u);

    char buf[32];
    SnTypeRep *first = NULL;
    SnSymbol *first_sym = NULL;
    for (int i = 0; i < 31; i++) {
        snprintf(buf, sizeof(buf), "L%d", i);
        SnSymbol *sym = fake_decl(&scope, &it, buf);
        SnTypeRep *ty = sn_type_typevar(&t, sym);
        if (i == 0) {
            first = ty;
            first_sym = sym;
        }
    }
    CHECK("load: full at 3/4 does not rehash yet",
          t.count == 48u && t.nbuckets == 64u);

    SnSymbol *boundary_sym = fake_decl(&scope, &it, "L36");
    SnTypeRep *boundary = sn_type_typevar(&t, boundary_sym);
    CHECK("load: the next insert doubles the table",
          t.count == 49u && t.nbuckets == 128u);
    CHECK("load: an early typevar is still interned after rehash",
          sn_type_typevar(&t, first_sym) == first);
    CHECK("load: the typevar that triggered rehash is interned",
          sn_type_typevar(&t, boundary_sym) == boundary);
}

static void test_queries(SnTypeTable *t) {
    CHECK("primitives: float, byte, and any are distinct",
          sn_type_float(t) != sn_type_byte(t) &&
              sn_type_byte(t) != sn_type_any(t) &&
              sn_type_any(t) != sn_type_int(t) &&
              sn_type_float(t) != sn_type_double(t));
    CHECK("any: predicate matches only the any singleton",
          sn_type_is_any(sn_type_any(t)) && !sn_type_is_any(sn_type_int(t)) &&
              !sn_type_is_any(NULL));
    CHECK("equals: identical pointers are equal",
          sn_type_equals(sn_type_int(t), sn_type_int(t)));
    CHECK("equals: distinct types are not equal",
          !sn_type_equals(sn_type_int(t), sn_type_long(t)));
    CHECK("equals: two nulls are equal", sn_type_equals(NULL, NULL));
}

static void test_ref(SnTypeTable *t) {
    SnTypeRep *ref_int = sn_type_ref(t, sn_type_int(t), 0);
    SnTypeRep *ref_int_b = sn_type_ref(t, sn_type_int(t), 0);
    SnTypeRep *ref_str = sn_type_ref(t, sn_type_string(t), 0);
    SnTypeRep *null_int = sn_type_ref(t, sn_type_int(t), 1);
    SnTypeRep *null_int_b = sn_type_ref(t, sn_type_int(t), 1);

    CHECK("ref: non-null ref<int> hash-conses", ref_int == ref_int_b);
    CHECK("ref: pointee is part of the shape", ref_int != ref_str);
    CHECK("ref: nullability is part of the shape", ref_int != null_int);
    CHECK("ref: nullable ref<int> hash-conses", null_int == null_int_b);
    CHECK("ref: predicate",
          sn_type_is_ref(ref_int) && sn_type_is_ref(null_int) &&
              !sn_type_is_ref(sn_type_int(t)) && !sn_type_is_ref(NULL));
    CHECK("ref: pointee accessor",
          sn_type_pointee(ref_int) == sn_type_int(t) &&
              sn_type_pointee(null_int) == sn_type_int(t) &&
              sn_type_pointee(sn_type_int(t)) == NULL);
    CHECK("ref: nullable flag",
          !sn_type_ref_nullable(ref_int) && sn_type_ref_nullable(null_int) &&
              !sn_type_ref_nullable(sn_type_int(t)) &&
              !sn_type_ref_nullable(NULL));

    /* Nullable ref stores the pointee in `ret` and a unit flag in args[0],
     * which is the same field layout as func(unit) -> int. The tag has to
     * keep them apart. */
    SnTypeRep *fn_params[1];
    fn_params[0] = sn_type_unit(t);
    SnTypeRep *fn = sn_type_func(t, fn_params, 1, sn_type_int(t));
    CHECK("ref: nullable ref<int> is not func(unit) -> int", null_int != fn);
    CHECK("ref: non-null ref<int> is not func() -> int",
          ref_int != sn_type_func(t, NULL, 0, sn_type_int(t)));
}

