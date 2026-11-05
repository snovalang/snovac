static void test_subst(SnTypeTable *t, SnInternTable *it, SnArena *arena) {
    SnScope scope;
    sn_scope_init(&scope, arena, NULL);

    SnSymbol *tp = fake_decl(&scope, it, "SubT");
    SnSymbol *up = fake_decl(&scope, it, "SubU");
    SnSymbol *zp = fake_decl(&scope, it, "SubZ");
    SnSymbol *list = fake_decl(&scope, it, "SubList");
    SnSymbol *triple = fake_decl(&scope, it, "SubTriple");
    /* A zero-arg named type that reuses the typevar's declaration, so its
     * name matches a substitution parameter. */
    SnTypeRep *bare_t = sn_type_named(t, tp, NULL, 0);
    SnSymbol *box = fake_decl(&scope, it, "SubBox");
    SnTypeRep *bare_box = sn_type_named(t, box, NULL, 0);

    SnTypeRep *tvar = sn_type_typevar(t, tp);
    SnTypeRep *uvar = sn_type_typevar(t, up);
    SnTypeRep *zvar = sn_type_typevar(t, zp);

    const char *names[] = {"SubT", "SubU", "SubZ"};
    SnTypeRep *args[] = {sn_type_int(t), sn_type_string(t), sn_type_bool(t)};

    CHECK("subst: null type stays null",
          sn_type_subst_names(t, NULL, names, args, 1) == NULL);
    CHECK("subst: a zero count leaves the type alone",
          sn_type_subst_names(t, tvar, names, args, 0) == tvar);
    CHECK("subst: null parameter names leave the type alone",
          sn_type_subst_names(t, tvar, NULL, args, 1) == tvar);
    CHECK("subst: null argument types leave the type alone",
          sn_type_subst_names(t, tvar, names, NULL, 1) == tvar);
    CHECK("subst: a primitive is unchanged",
          sn_type_subst_names(t, sn_type_int(t), names, args, 3) ==
              sn_type_int(t));

    CHECK("subst: typevar SubT becomes int",
          sn_type_subst_names(t, tvar, names, args, 2) == sn_type_int(t));
    /* Match on the second name only, so a loop that touches just names[0]
     * (or never enters) cannot pass. */
    CHECK("subst: typevar SubU matches the second parameter",
          sn_type_subst_names(t, uvar, names, args, 2) == sn_type_string(t));
    CHECK("subst: an unmatched typevar is unchanged",
          sn_type_subst_names(t, zvar, names, args, 2) == zvar);
    /* count excludes names[1], which would otherwise match SubU. */
    CHECK("subst: the count stops before a later matching name",
          sn_type_subst_names(t, uvar, names, args, 1) == uvar);

    CHECK("subst: a zero-arg named type whose name matches is replaced",
          sn_type_subst_names(t, bare_t, names, args, 1) == sn_type_int(t));
    CHECK("subst: a zero-arg named type with another name stays",
          sn_type_subst_names(t, bare_box, names, args, 3) == bare_box);

    SnTypeRep *list_args[1];
    list_args[0] = tvar;
    SnTypeRep *list_t = sn_type_named(t, list, list_args, 1);
    SnTypeRep *list_int_args[1];
    list_int_args[0] = sn_type_int(t);
    SnTypeRep *list_int = sn_type_named(t, list, list_int_args, 1);
    CHECK("subst: List<SubT> becomes List<int>",
          sn_type_subst_names(t, list_t, names, args, 3) == list_int);
    CHECK("subst: List<int> is the same pointer when nothing changes",
          sn_type_subst_names(t, list_int, names, args, 3) == list_int);

    SnTypeRep *tri_args[3];
    tri_args[0] = tvar;
    tri_args[1] = uvar;
    tri_args[2] = zvar;
    SnTypeRep *tri = sn_type_named(t, triple, tri_args, 3);
    SnTypeRep *tri_got = sn_type_subst_names(t, tri, names, args, 3);
    CHECK("subst: all three type arguments are replaced",
          tri_got == sn_type_named(t, triple, args, 3) &&
              tri_got->args[2] == sn_type_bool(t));

    SnTypeRep *arr_t = sn_type_array(t, tvar);
    CHECK("subst: Array<SubT> becomes Array<int>",
          sn_type_subst_names(t, arr_t, names, args, 3) ==
              sn_type_array(t, sn_type_int(t)));
    SnTypeRep *arr_int = sn_type_array(t, sn_type_int(t));
    CHECK("subst: Array<int> is unchanged",
          sn_type_subst_names(t, arr_int, names, args, 3) == arr_int);

    SnTypeRep *nested_args[1];
    nested_args[0] = arr_t;
    SnTypeRep *nested = sn_type_named(t, list, nested_args, 1);
    SnTypeRep *nested_exp_args[1];
    nested_exp_args[0] = sn_type_array(t, sn_type_int(t));
    CHECK("subst: List<Array<SubT>> replaces the inner typevar",
          sn_type_subst_names(t, nested, names, args, 3) ==
              sn_type_named(t, list, nested_exp_args, 1));

    SnTypeRep *ref_t = sn_type_ref(t, tvar, 0);
    CHECK("subst: ref SubT becomes ref int",
          sn_type_subst_names(t, ref_t, names, args, 3) ==
              sn_type_ref(t, sn_type_int(t), 0));
    SnTypeRep *nref_t = sn_type_ref(t, tvar, 1);
    CHECK("subst: a nullable ref keeps nullability",
          sn_type_subst_names(t, nref_t, names, args, 3) ==
              sn_type_ref(t, sn_type_int(t), 1));
    SnTypeRep *ref_int = sn_type_ref(t, sn_type_int(t), 0);
    CHECK("subst: ref int is unchanged",
          sn_type_subst_names(t, ref_int, names, args, 3) == ref_int);

    SnTypeRep *both[2];
    both[0] = tvar;
    both[1] = uvar;
    SnTypeRep *fn_both = sn_type_func(t, both, 2, tvar);
    SnTypeRep *both_exp[2];
    both_exp[0] = sn_type_int(t);
    both_exp[1] = sn_type_string(t);
    CHECK("subst: (SubT, SubU) -> SubT becomes (int, string) -> int",
          sn_type_subst_names(t, fn_both, names, args, 3) ==
              sn_type_func(t, both_exp, 2, sn_type_int(t)));

    SnTypeRep *fn_ret = sn_type_func(t, NULL, 0, tvar);
    CHECK("subst: () -> SubT becomes () -> int",
          sn_type_subst_names(t, fn_ret, names, args, 3) ==
              sn_type_func(t, NULL, 0, sn_type_int(t)));

    SnTypeRep *only_param[1];
    only_param[0] = tvar;
    SnTypeRep *fn_param = sn_type_func(t, only_param, 1, sn_type_bool(t));
    SnTypeRep *param_exp[1];
    param_exp[0] = sn_type_int(t);
    CHECK("subst: (SubT) -> bool becomes (int) -> bool",
          sn_type_subst_names(t, fn_param, names, args, 3) ==
              sn_type_func(t, param_exp, 1, sn_type_bool(t)));

    SnTypeRep *unchanged_fn = sn_type_func(t, param_exp, 1, sn_type_bool(t));
    CHECK("subst: a function with no typevar is the same pointer",
          sn_type_subst_names(t, unchanged_fn, names, args, 3) == unchanged_fn);

    SnTypeRep *fn3 = sn_type_func(t, tri_args, 3, sn_type_unit(t));
    SnTypeRep *fn3_got = sn_type_subst_names(t, fn3, names, args, 3);
    CHECK("subst: a three-parameter function replaces every parameter",
          fn3_got == sn_type_func(t, args, 3, sn_type_unit(t)) &&
              fn3_got->args[2] == sn_type_bool(t));
}

/* Same three-parameter substitution on a table that has not rehashed.
 * The parameter array is sized with nargs * sizeof. If that product is
 * truncated, the following SnTypeRep allocation overwrites the last
 * parameter before it is copied. A crowded table hides that: the next
 * allocation is a rehash, which lands in a new arena block. */
static void test_func_subst_on_fresh_table(void) {
    SnArena arena;
    sn_arena_init(&arena, 0);
    SnInternTable it;
    sn_intern_init(&it, &arena);
    SnTypeTable t;
    sn_types_init(&t, &arena);
    SnScope scope;
    sn_scope_init(&scope, &arena, NULL);

    SnSymbol *a = fake_decl(&scope, &it, "A");
    SnSymbol *b = fake_decl(&scope, &it, "B");
    SnSymbol *c = fake_decl(&scope, &it, "C");
    SnTypeRep *params[3];
    params[0] = sn_type_typevar(&t, a);
    params[1] = sn_type_typevar(&t, b);
    params[2] = sn_type_typevar(&t, c);
    SnTypeRep *fn = sn_type_func(&t, params, 3, sn_type_unit(&t));

    const char *names[] = {"A", "B", "C"};
    SnTypeRep *args[] = {sn_type_int(&t), sn_type_string(&t), sn_type_bool(&t)};
    SnTypeRep *got = sn_type_subst_names(&t, fn, names, args, 3);
    CHECK("subst: fresh table keeps the third substituted parameter",
          got == sn_type_func(&t, args, 3, sn_type_unit(&t)) && got->nargs == 3 &&
              got->args[2] == sn_type_bool(&t));

    sn_arena_free(&arena);
}

int main(void) {
    SnArena arena;
    sn_arena_init(&arena, 0);
    SnInternTable it;
    sn_intern_init(&it, &arena);
    SnTypeTable t;
    sn_types_init(&t, &arena);
    SnScope scope;
    sn_scope_init(&scope, &arena, NULL);

    test_primitives(&t);
    test_named_generics(&t, &scope, &it);
    test_typevar(&t, &scope, &it);
    test_func(&t);
    test_array(&t);
    test_tag_isolation(&t, &scope, &it);
    test_later_argument(&t, &scope, &it);
    test_queries(&t);
    test_ref(&t);
    test_subst(&t, &it, &arena);
    test_func_subst_on_fresh_table();
    test_rehash_stress(&t, &scope, &it);

    SnArena load_arena;
    sn_arena_init(&load_arena, 0);
    test_load_factor(&load_arena);
    sn_arena_free(&load_arena);

    sn_arena_free(&arena);

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
