#include "check.h"
#include "check_internal.h"

#include <string.h>

#include "builtins.h"

#include "check.h"
#include "check_internal.h"

#include <string.h>

#include "builtins.h"

void sn_checker_init(SnChecker *c, SnArena *a, SnInternTable *it, SnDiagSink *diag,
                      SnResolver *resolver, SnTypeTable *types) {
    c->arena = a;
    c->intern = it;
    c->diag = diag;
    c->resolver = resolver;
    c->types = types;
    c->current_package = NULL;
    c->current_imports = NULL;
    c->enclosing_type = NULL;
    c->current_return_type = NULL;
    c->expected_lambda_type = NULL;
    c->lambda_scope = NULL;
    c->type_params = NULL;
    c->in_constructor = 0;
    c->in_async_body = 0;
    c->in_pulsar_launch = 0;
    c->loop_depth = 0;
}

int is_numeric(const SnTypeRep *t) {
    return t->tag == SN_T_INT || t->tag == SN_T_LONG || t->tag == SN_T_DOUBLE ||
           t->tag == SN_T_DECIMAL || t->tag == SN_T_FLOAT || t->tag == SN_T_BYTE;
}

int sn_check_is_error(const SnTypeRep *t) { return t->tag == SN_T_ERROR; }

/* An UNSUFFIXED numeric literal carries no committed type until context gives
 * it one: `3.0` is an equally valid `decimal` (tests/compile-pass/structs.snova
 * writes `Point(3.0, 4.0)` for two `decimal` fields), `float`
 * (tests/compile-pass/data_class.snova) or `double`.
 *
 * This is NOT the implicit numeric promotion ratified as forbidden (plan.md §7
 * item 1, measured 2026-07-25): that rule is about converting an
 * already-typed VALUE, and it still holds — `long + int` is still an error.
 * A literal has no value-level type to convert from. The lexer already draws
 * the line for us: `1`/`3.0` come through as SN_TOK_INT/SN_TOK_DOUBLE, while
 * the suffixed `42L`/`1.5d` come through as SN_TOK_LONG/SN_TOK_DECIMAL and are
 * committed, so they keep following the exact-match rule.
 *
 * Adopting rewrites the literal's own resolved_type, so the AST records the
 * type the literal actually has rather than the lexer's default. */
SnTypeRep *sn_check_adopt_literal_type(SnChecker *c, SnExpr *e, SnTypeRep *actual,
                                     SnTypeRep *expected) {
    if (!e || !expected || !actual || sn_check_is_error(expected) || expected == actual) {
        return actual;
    }
    if (e->kind == SN_EXPR_UNARY && (e->op == SN_TOK_MINUS || e->op == SN_TOK_PLUS) &&
        e->lhs) {
        /* `-1` is still a literal; the sign doesn't commit it to a type. */
        SnTypeRep *inner = sn_check_adopt_literal_type(c, e->lhs, actual, expected);
        if (inner != actual) {
            e->resolved_type = inner;
        }
        return inner;
    }
    int fits = 0;
    if (e->kind == SN_EXPR_INT) {
        fits = expected->tag == SN_T_INT || expected->tag == SN_T_LONG ||
               expected->tag == SN_T_BYTE;
    } else if (e->kind == SN_EXPR_DOUBLE) {
        fits = expected->tag == SN_T_DOUBLE || expected->tag == SN_T_FLOAT ||
               expected->tag == SN_T_DECIMAL;
    }
    if (!fits) {
        return actual;
    }
    e->resolved_type = expected;
    return expected;
}

SnSymbol *find_type_symbol_anywhere(SnChecker *c, const SnDecl *context_decl, const char *sname) {
    if (!sname) return NULL;

    if (context_decl) {
        for (SnPackageScopeEntry *pe = c->resolver->packages; pe; pe = pe->next) {
            if (pe->scope) {
                for (size_t bi = 0; bi < pe->scope->nbuckets; bi++) {
                    for (SnSymbol *sym = pe->scope->buckets[bi]; sym; sym = sym->next) {
                        if (sym->kind == SN_SYM_TYPE && sym->decl == context_decl) {
                            SnSymbol *s = sn_scope_lookup_local(pe->scope, sname);
                            if (s && s->kind == SN_SYM_TYPE) return s;
                        }
                    }
                }
            }
        }
    }

    if (c->current_package) {
        SnScope *pkg_scope = sn_resolver_package_scope(c->resolver, c->current_package);
        if (pkg_scope) {
            SnSymbol *s = sn_scope_lookup_local(pkg_scope, sname);
            if (s && s->kind == SN_SYM_TYPE) return s;
        }
    }

    if (c->current_imports) {
        for (size_t i = 0; i < c->current_imports->len; i++) {
            const char *imp = SN_LIST_AT(*c->current_imports, const char, i);
            SnScope *imp_scope = sn_resolver_package_scope(c->resolver, imp);
            if (imp_scope) {
                SnSymbol *s = sn_scope_lookup_local(imp_scope, sname);
                if (s && s->kind == SN_SYM_TYPE) return s;
            }
        }
    }

    for (SnPackageScopeEntry *pe = c->resolver->packages; pe; pe = pe->next) {
        if (pe->scope) {
            SnSymbol *s = sn_scope_lookup_local(pe->scope, sname);
            if (s && s->kind == SN_SYM_TYPE) return s;
        }
    }

    if (c->resolver->prelude_scope) {
        SnSymbol *s = sn_scope_lookup_local(c->resolver->prelude_scope, sname);
        if (s && s->kind == SN_SYM_TYPE) return s;
    }

    return NULL;
}

/* `sub` is `super`, or inherits from it. Follows EVERY supertype listed after
 * `:`, not just the first: the parser deliberately does not tell a base class
 * from an implemented interface (ast.h, `supertypes`), so both count for
 * assignability. */
static int decl_derives_from(SnChecker *c, const SnDecl *sub, const SnDecl *super,
                             int depth) {
    if (!sub || !super || depth > 8) {
        return 0;
    }
    if (sub == super) {
        return 1;
    }
    for (size_t i = 0; i < sub->supertypes.len; i++) {
        const SnType *st = SN_LIST_AT(sub->supertypes, SnType, i);
        if (st->kind != SN_TYPE_NAME) {
            continue;
        }
        const char *sname = sn_intern_cstr(c->intern, st->name);
        SnSymbol *ssym = find_type_symbol_anywhere(c, sub, sname);
        if (ssym && ssym->kind == SN_SYM_TYPE &&
            decl_derives_from(c, ssym->decl, super, depth + 1)) {
            return 1;
        }
    }
    return 0;
}

/* True when a value of type `value` cannot stand where `expected` is wanted.
 *
 * The order is (value, expected) at every call site and matters: assignability
 * is directional. A `Dog` may initialize an `Animal` binding
 * (tests/compile-pass/inheritance_polymorphism.snova) — the reverse may not.
 *
 * SN_T_ERROR means a failure was already reported upstream, and SN_T_ANY is
 * the corpus's escape hatch (`func printline(value: any)`); neither should
 * produce a second, derived diagnostic. */
int sn_check_types_clash(SnChecker *c, const SnTypeRep *value, const SnTypeRep *expected) {
    if (!value || !expected || sn_check_is_error(value) || sn_check_is_error(expected)) {
        return 0;
    }
    if (sn_type_is_any(value) || sn_type_is_any(expected)) {
        return 0;
    }
    if (value == expected) {
        return 0;
    }
    if (expected->tag == SN_T_NAMED && expected->decl &&
        strcmp(expected->decl->name, "Option") == 0 && expected->nargs == 1) {
        if (!sn_check_types_clash(c, value, expected->args[0])) {
            return 0; /* Subsumption: T <: T? */
        }
    }
    if (value->tag == SN_T_NAMED && expected->tag == SN_T_NAMED && value->decl &&
        expected->decl &&
        decl_derives_from(c, value->decl->decl, expected->decl->decl, 0)) {
        return 0;
    }
    return 1;
}

/* The first borrow ABI is deliberately limited to immutable scalar/string
 * parameters. They are safe to pass through the existing value stack without
 * exposing mutable aliases or allowing an address to escape. */
SnTypeRep *sn_check_supported_borrow_target(const SnTypeRep *type) {
    if (!type || type->tag != SN_T_BORROW || type->nargs != 1 || !type->args[0])
        return NULL;
    SnTypeRep *target = type->args[0];
    switch (target->tag) {
    case SN_T_BOOL: case SN_T_INT: case SN_T_LONG: case SN_T_DOUBLE:
    case SN_T_DECIMAL: case SN_T_STRING: case SN_T_CHAR: case SN_T_FLOAT:
    case SN_T_BYTE:
        return target;
    default:
        return NULL;
    }
}

int sn_check_type_contains_reference(const SnTypeRep *type) {
    if (!type) return 0;
    if (type->tag == SN_T_BORROW || type->tag == SN_T_BORROW_MUT ||
        type->tag == SN_T_RAW_PTR_CONST || type->tag == SN_T_RAW_PTR_MUT)
        return 1;
    for (uint32_t i = 0; i < type->nargs; ++i) {
        if (sn_check_type_contains_reference(type->args[i])) return 1;
    }
    return sn_check_type_contains_reference(type->ret);
}

int sn_check_argument_type_clash(SnChecker *c, const SnTypeRep *value,
                                 const SnTypeRep *expected) {
    if (expected && (expected->tag == SN_T_BORROW ||
                     expected->tag == SN_T_BORROW_MUT ||
                     expected->tag == SN_T_RAW_PTR_CONST ||
                     expected->tag == SN_T_RAW_PTR_MUT)) {
        if (expected->nargs == 1 && sn_check_is_error(expected->args[0])) return 0;
        SnTypeRep *target = sn_check_supported_borrow_target(expected);
        return target ? sn_check_types_clash(c, value, target) : 1;
    }
    return sn_check_types_clash(c, value, expected);
}

/* ── AST type -> resolved type ────────────────────────────────────────────── */

static SnTypeRep *sn_check_resolve_type_base(SnChecker *c, const SnType *t) {
    if (!t) {
        return sn_type_error(c->types);
    }
    switch (t->kind) {
    case SN_TYPE_NAME: {
        /* A type parameter shadows everything — `T` inside `func f<T>()` is
         * that parameter, never some package's type named T. Checked before
         * sn_resolve_type_name so it never gets a chance to report SNOVA0027. */
        if (c->type_params) {
            SnSymbol *tp =
                sn_scope_lookup_local(c->type_params, sn_intern_cstr(c->intern, t->name));
            if (tp) {
                return sn_type_typevar(c->types, tp);
            }
        }
        /* Intrinsic generic constructors — `Array<T>`, `Partial<T>`. No
         * .snova file declares them, so they short-circuit before any scope
         * lookup, exactly as the primitives do inside sn_resolve_type_name.
         * Their arguments are resolved first because the constructed type
         * needs them; a non-intrinsic name still resolves its own name first,
         * so an unknown type keeps reporting one diagnostic and not also one
         * per unresolved argument. */
        if (sn_builtin_is_generic_name(c->intern, sn_intern_cstr(c->intern, t->name))) {
            SnTypeRep **iargs = NULL;
            if (t->args.len) {
                iargs = (SnTypeRep **)sn_arena_alloc(c->arena,
                                                     t->args.len * sizeof(SnTypeRep *));
                for (size_t i = 0; i < t->args.len; i++) {
                    iargs[i] = sn_check_resolve_type(c, SN_LIST_AT(t->args, SnType, i));
                }
            }
            return sn_builtin_generic(c->types, c->intern,
                                      sn_intern_cstr(c->intern, t->name), iargs,
                                      (uint32_t)t->args.len);
        }

        SnTypeRep *base = sn_resolve_type_name(c->resolver, c->current_package,
                                               c->current_imports, t->name, t->span);
        if (!base) {
            if (c->diag->quiet > 0 && t->name) {
                SnSymbol *sym = (SnSymbol *)sn_arena_calloc(c->arena, sizeof(SnSymbol));
                sym->kind = SN_SYM_TYPE;
                sym->name = sn_intern_cstr(c->intern, t->name);
                return sn_type_named(c->types, sym, NULL, 0);
            }
            return sn_type_error(c->types);
        }
        if (t->args.len == 0 || !base->decl) {
            /* No generic args, or a primitive (which never legitimately
             * takes args in this corpus) — degrade gracefully rather than
             * fabricate a shape types.c can't represent. */
            return base;
        }
        SnTypeRep **args =
            (SnTypeRep **)sn_arena_alloc(c->arena, t->args.len * sizeof(SnTypeRep *));
        for (size_t i = 0; i < t->args.len; i++) {
            SnType *arg_t = SN_LIST_AT(t->args, SnType, i);
            args[i] = sn_check_resolve_type(c, arg_t);
        }
        return sn_type_named(c->types, base->decl, args, (uint32_t)t->args.len);
    }
    case SN_TYPE_FUNC: {
        SnTypeRep **params = NULL;
        if (t->params.len) {
            params =
                (SnTypeRep **)sn_arena_alloc(c->arena, t->params.len * sizeof(SnTypeRep *));
            for (size_t i = 0; i < t->params.len; i++) {
                SnType *pt = SN_LIST_AT(t->params, SnType, i);
                params[i] = sn_check_resolve_type(c, pt);
            }
        }
        SnTypeRep *ret = sn_check_resolve_type(c, t->ret);
        return sn_type_func(c->types, params, (uint32_t)t->params.len, ret);
    }
    case SN_TYPE_BORROW:
    case SN_TYPE_RAW_PTR: {
        if (t->args.len != 1) return sn_type_error(c->types);
        SnTypeRep *target = sn_check_resolve_type(c, SN_LIST_AT(t->args, SnType, 0));
        return t->kind == SN_TYPE_BORROW
                   ? sn_type_borrow(c->types, target, t->is_mutable_ref)
                   : sn_type_raw_ptr(c->types, target, t->is_mutable_ref);
    }
    case SN_TYPE_TUPLE:
        /* types.c's SnTypeTag (plan.md §5) has no tuple tag. Documented gap
         * — degrade rather than mis-tag a tuple as something it isn't. */
        return sn_type_error(c->types);
    }
    return sn_type_error(c->types);
}

SnTypeRep *sn_check_resolve_type(SnChecker *c, const SnType *t) {
    SnTypeRep *res = sn_check_resolve_type_base(c, t);
    if (t && t->is_optional && res != sn_type_error(c->types)) {
        res = sn_builtin_generic(c->types, c->intern,
                                  sn_intern_cstr(c->intern, "Option"), &res, 1);
    }
    return res;
}

