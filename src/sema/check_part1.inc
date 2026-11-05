#include "check.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "borrow.h"
#include "builtins.h"
#include "check_ptr.h"

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
    c->current_function = NULL;
    memset(&c->active_catch_types, 0, sizeof(c->active_catch_types));
    c->type_params = NULL;
    c->in_constructor = 0;
    c->in_async_body = 0;
    c->in_pulsar_launch = 0;
    c->loop_depth = 0;
}

static int is_numeric(const SnTypeRep *t) {
    return sn_type_integer_width(t) > 0 || t->tag == SN_T_DOUBLE ||
           t->tag == SN_T_DECIMAL || t->tag == SN_T_FLOAT;
}

static int is_text_type(const SnTypeRep *t) {
    return t && (t->tag == SN_T_STRING || t->tag == SN_T_CHAR);
}

/* Magnitude of an unsuffixed integer literal. Underscores are ignored.
 * `*negative` is set when the expression is a unary minus around that literal. */
static int integer_literal_magnitude(const SnExpr *e, long long *mag, int *negative) {
    const SnExpr *lit = e;
    *negative = 0;
    if (e && e->kind == SN_EXPR_UNARY &&
        (e->op == SN_TOK_MINUS || e->op == SN_TOK_PLUS) && e->lhs &&
        e->lhs->kind == SN_EXPR_INT) {
        *negative = e->op == SN_TOK_MINUS;
        lit = e->lhs;
    }
    if (!lit || lit->kind != SN_EXPR_INT || !lit->text) {
        return 0;
    }
    char buf[128];
    size_t n = 0;
    for (const char *p = lit->text; *p && n + 1 < sizeof(buf); p++) {
        if (*p == '_') {
            continue;
        }
        buf[n++] = *p;
    }
    buf[n] = '\0';
    if (n == 0) {
        return 0;
    }
    errno = 0;
    char *end = NULL;
    long long v = strtoll(buf, &end, 0);
    if (errno != 0 || end == buf) {
        return 0;
    }
    if (v < 0) {
        v = -v;
    }
    *mag = v;
    return 1;
}

static int integer_magnitude_fits(long long mag, int negative, int width, int is_signed) {
    if (width <= 0 || mag < 0) {
        return 0;
    }
    if (!is_signed) {
        if (negative) {
            return 0;
        }
        if (width >= 64) {
            return 1;
        }
        unsigned long long max = (1ULL << (unsigned)width) - 1ULL;
        return (unsigned long long)mag <= max;
    }
    if (width >= 64) {
        return 1;
    }
    if (negative) {
        unsigned long long limit = 1ULL << (unsigned)(width - 1);
        return (unsigned long long)mag <= limit;
    }
    long long max = (1LL << (width - 1)) - 1LL;
    return mag <= max;
}

static int is_error(const SnTypeRep *t) { return t->tag == SN_T_ERROR; }

/* An UNSUFFIXED numeric literal carries no committed type until context gives
 * it one: `3.0` is an equally valid `decimal` (tests/compile-pass/structs.snl
 * writes `Point{3.0, 4.0}` for two `decimal` fields), `float`
 * (tests/compile-pass/data_class.snl) or `double`.
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
static SnTypeRep *adopt_literal_type(SnChecker *c, SnExpr *e, SnTypeRep *actual,
                                     SnTypeRep *expected) {
    if (!e || !expected || !actual || is_error(expected) || expected == actual) {
        return actual;
    }
    if (e->kind == SN_EXPR_UNARY && (e->op == SN_TOK_MINUS || e->op == SN_TOK_PLUS) &&
        e->lhs && sn_type_integer_width(expected) == 0) {
        /* `-1` is still a literal; the sign doesn't commit it to a type.
         * Integer targets are handled below so the sign participates in the
         * range check (a negative value does not fit `byte`). */
        SnTypeRep *inner = adopt_literal_type(c, e->lhs, actual, expected);
        if (inner != actual) {
            e->resolved_type = inner;
        }
        return inner;
    }
    int fits = 0;
    int lit_negative = 0;
    long long lit_mag = 0;
    if (sn_type_integer_width(expected) > 0 &&
        integer_literal_magnitude(e, &lit_mag, &lit_negative)) {
        fits = integer_magnitude_fits(lit_mag, lit_negative,
                                      sn_type_integer_width(expected),
                                      sn_type_integer_signed(expected));
        if (fits && e->kind == SN_EXPR_UNARY && e->lhs) {
            e->lhs->resolved_type = expected;
            e->resolved_type = expected;
            return expected;
        }
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

static SnSymbol *find_type_symbol_anywhere(SnChecker *c, const SnDecl *context_decl, const char *sname) {
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
    /* Every declared object type inherits the builtin root unless it is the
     * root itself or explicitly declares another supertype. */
    if (sub->name && strcmp(sub->name, "Object") != 0) {
        SnSymbol *object_sym = find_type_symbol_anywhere(
            c, sub, sn_intern_cstr(c->intern, "Object"));
        if (object_sym && object_sym->kind == SN_SYM_TYPE && object_sym->decl &&
            decl_derives_from(c, object_sym->decl, super, depth + 1)) {
            return 1;
        }
    }
    return 0;
}

static int is_checked_error_type(SnChecker *c, const SnTypeRep *ty) {
    if (!ty || ty->tag != SN_T_NAMED || !ty->decl || !ty->decl->decl) return 0;
    SnSymbol *root = find_type_symbol_anywhere(
        c, c->enclosing_type, sn_intern_cstr(c->intern, "Error"));
    return root && root->decl && decl_derives_from(c, ty->decl->decl, root->decl, 0);
}

static int error_is_covered(SnChecker *c, const SnTypeRep *error_ty) {
    for (size_t i = 0; i < c->active_catch_types.len; i++) {
        SnTypeRep *caught = SN_LIST_AT(c->active_catch_types, SnTypeRep, i);
        if (!caught) return 1;
        if (caught->tag == SN_T_NAMED && caught->decl && error_ty->decl &&
            decl_derives_from(c, error_ty->decl->decl, caught->decl->decl, 0)) return 1;
    }
    if (!c->current_function) return 0;
    for (size_t i = 0; i < c->current_function->throws_types.len; i++) {
        SnTypeRep *declared = sn_check_resolve_type(
            c, SN_LIST_AT(c->current_function->throws_types, SnType, i));
        if (declared && declared->tag == SN_T_NAMED && declared->decl &&
            error_ty->decl && decl_derives_from(c, error_ty->decl->decl,
                                                 declared->decl->decl, 0)) return 1;
    }
    return 0;
}

static void require_error_handled(SnChecker *c, const SnTypeRep *error_ty, SnSpan span) {
    if (is_checked_error_type(c, error_ty) && !error_is_covered(c, error_ty)) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNHANDLED_ERROR, span,
                     "checked error `%s` must be caught or declared in this function's throws clause",
                     error_ty->decl->name);
    }
}

static void check_call_throws(SnChecker *c, const SnDecl *callee, SnSpan span) {
    if (!callee) return;
    for (size_t i = 0; i < callee->throws_types.len; i++) {
        SnTypeRep *error_ty = sn_check_resolve_type(
            c, SN_LIST_AT(callee->throws_types, SnType, i));
        require_error_handled(c, error_ty, span);
    }
}

/* True when a value of type `value` cannot stand where `expected` is wanted.
 *
 * The order is (value, expected) at every call site and matters: assignability
 * is directional. A `Dog` may initialize an `Animal` binding
 * (tests/compile-pass/inheritance_polymorphism.snl) — the reverse may not.
 *
 * SN_T_ERROR means a failure was already reported upstream, and SN_T_ANY is
 * the corpus's escape hatch (`func printline(value: any)`); neither should
 * produce a second, derived diagnostic. */
static int types_clash(SnChecker *c, const SnTypeRep *value, const SnTypeRep *expected) {
    if (!value || !expected || is_error(value) || is_error(expected)) {
        return 0;
    }
    if (sn_type_is_any(value) || sn_type_is_any(expected)) {
        return 0;
    }
    if (value == expected) {
        return 0;
    }
    if (value->tag == SN_T_FUNC && expected->tag == SN_T_FUNC) {
        if (value->nargs != expected->nargs) return 1;
        for (uint32_t i = 0; i < value->nargs; i++) {
            if (types_clash(c, value->args[i], expected->args[i])) return 1;
        }
        if (types_clash(c, value->ret, expected->ret)) return 1;
        /* The target signature must promise to handle every checked effect
         * the assigned function can produce. Exception subtypes are covered
         * by a declared supertype effect. */
        for (uint32_t i = 0; i < value->nthrows; i++) {
            const SnTypeRep *actual_effect = value->throws_types[i];
            int covered = 0;
            for (uint32_t j = 0; j < expected->nthrows; j++) {
                const SnTypeRep *accepted_effect = expected->throws_types[j];
                if (actual_effect == accepted_effect ||
                    sn_type_is_any(accepted_effect)) {
                    covered = 1;
                    break;
                }
                if (actual_effect && accepted_effect &&
                    actual_effect->tag == SN_T_NAMED && accepted_effect->tag == SN_T_NAMED &&
                    actual_effect->decl && accepted_effect->decl &&
                    decl_derives_from(c, actual_effect->decl->decl,
                                      accepted_effect->decl->decl, 0)) {
                    covered = 1;
                    break;
                }
            }
            if (!covered) return 1;
        }
        return 0;
    }
    if (value->tag == expected->tag &&
        (value->tag == SN_T_ARRAY || value->tag == SN_T_NAMED) &&
        value->decl == expected->decl && value->nargs == expected->nargs) {
        for (uint32_t i = 0; i < value->nargs; i++) {
            if (types_clash(c, value->args[i], expected->args[i])) {
                return 1;
            }
        }
        return 0;
    }
    if (expected->tag == SN_T_NAMED && expected->decl &&
        strcmp(expected->decl->name, "Option") == 0 && expected->nargs == 1) {
        if (!types_clash(c, value, expected->args[0])) {
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
        /* Intrinsic runtime representation for the source-declared Array<T>,
         * plus the source alias Partial<T>. Arguments are resolved first
         * because the mapped representation needs them; an ordinary unknown
         * type still reports once before its generic arguments are visited. */
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
        SnTypeRep **throws_types = NULL;
        if (t->throws_types.len) {
            throws_types = (SnTypeRep **)sn_arena_alloc(
                c->arena, t->throws_types.len * sizeof(SnTypeRep *));
            for (size_t i = 0; i < t->throws_types.len; i++) {
                throws_types[i] = sn_check_resolve_type(
                    c, SN_LIST_AT(t->throws_types, SnType, i));
            }
        }
        return sn_type_func_effect(c->types, params, (uint32_t)t->params.len,
                                   ret, throws_types,
                                   (uint32_t)t->throws_types.len);
    }
    case SN_TYPE_REF:
        return sn_check_resolve_ref(c, t);
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
        SnSymbol *option = find_type_symbol_anywhere(
            c, c->enclosing_type, sn_intern_cstr(c->intern, "Option"));
        if (option && option->kind == SN_SYM_TYPE && option->decl &&
            option->decl->kind == SN_DECL_ENUM && option->decl->generics.len == 1) {
            res = sn_type_named(c->types, option, &res, 1);
        } else {
            res = sn_builtin_generic(c->types, c->intern,
                                      sn_intern_cstr(c->intern, "Option"), &res, 1);
        }
    }
    return res;
}

/* ── symbol -> type, and the linear-scan helpers that back it ────────────── */

static SnSymbol *find_type_symbol_for_decl(SnResolver *r, const SnDecl *type_decl) {
    for (SnPackageScopeEntry *e = r->packages; e; e = e->next) {
        for (size_t bi = 0; bi < e->scope->nbuckets; bi++) {
            for (SnSymbol *s = e->scope->buckets[bi]; s; s = s->next) {
                if (s->kind == SN_SYM_TYPE && s->decl == type_decl) {
                    return s;
                }
            }
        }
    }
    return NULL;
}

