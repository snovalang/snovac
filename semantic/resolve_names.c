#include "resolve.h"
#include "resolve_internal.h"

#include <string.h>

#include "lex.h"
#include "parse.h"

const char *resolve_import_package_name(const SnResolver *r, const char *imp_pkg) {
    if (sn_resolver_package_scope(r, imp_pkg)) {
        return imp_pkg;
    }
    char prefix[2048];
    size_t n = strlen(imp_pkg);
    if (n >= sizeof(prefix)) {
        return NULL;
    }
    memcpy(prefix, imp_pkg, n + 1u);
    for (;;) {
        char *dot = strrchr(prefix, '.');
        if (!dot) {
            return NULL;
        }
        *dot = '\0';
        const char *candidate = sn_intern_cstr(r->intern, prefix);
        if (sn_resolver_package_scope(r, candidate)) {
            return candidate;
        }
    }
}

SnScope *resolve_import_scope(const SnResolver *r, const char *imp_pkg) {
    const char *real_pkg = resolve_import_package_name(r, imp_pkg);
    return real_pkg ? sn_resolver_package_scope(r, real_pkg) : NULL;
}


/* ── name resolution ──────────────────────────────────────────────────────── */

static SnTypeRep *primitive_by_name(SnResolver *r, const char *iname) {
    SnInternTable *it = r->intern;
    if (iname == sn_intern_cstr(it, "int")) return sn_type_int(r->types);
    if (iname == sn_intern_cstr(it, "string")) return sn_type_string(r->types);
    if (iname == sn_intern_cstr(it, "bool")) return sn_type_bool(r->types);
    if (iname == sn_intern_cstr(it, "unit")) return sn_type_unit(r->types);
    if (iname == sn_intern_cstr(it, "long")) return sn_type_long(r->types);
    if (iname == sn_intern_cstr(it, "double")) return sn_type_double(r->types);
    if (iname == sn_intern_cstr(it, "decimal")) return sn_type_decimal(r->types);
    if (iname == sn_intern_cstr(it, "char")) return sn_type_char(r->types);
    /* Also intrinsic, and also declared by no .snova file anywhere (measured
     * the same way as the seven above): `any`, `float` and `byte` are used in
     * builtin signatures (`func printline(value: any)`) and are on the Rust
     * frontend's builtin-type list too — see
     * crates/snovalang/src/native/selfcheck/static_facts.rs. Without them the
     * checker reported SNOVA0027 for every builtin signature mentioning one. */
    if (iname == sn_intern_cstr(it, "any")) return sn_type_any(r->types);
    if (iname == sn_intern_cstr(it, "float")) return sn_type_float(r->types);
    if (iname == sn_intern_cstr(it, "byte")) return sn_type_byte(r->types);
    return NULL;
}

/* `Int`/`String`/... — accepted, mapped to the primitive, and warned about.
 * Returns NULL when `iname` is not one of these. Tried only after every real
 * lookup has failed, so a package that genuinely declares a type named `Int`
 * still wins.
 *
 * The first six are exactly the pairs
 * crates/snovalang/src/native/selfcheck/mod.rs warns about under SNOVA011.
 * The last four complete the set over the primitives snovac actually has a
 * tag for: `Float` alone accounts for 13 of the errors this gate attributes to
 * tests/compile-pass/data_class.snova, which writes `public let x: Float`.
 * The Rust table stops at six only because it is a hardcoded list, not because
 * the capitalized spelling means something else there — so the same advice is
 * given, rather than SNOVA0027 for a type the shipped `snova check` accepts. */
static SnTypeRep *legacy_primitive_alias(SnResolver *r, const char *iname,
                                          const char *name, SnSpan span) {
    static const char *const ALIASES[][2] = {
        {"String", "string"}, {"Int", "int"},   {"Long", "long"},
        {"Bool", "bool"},     {"Unit", "unit"}, {"Decimal", "decimal"},
        {"Float", "float"},   {"Double", "double"},
        {"Byte", "byte"},     {"Char", "char"},
    };
    for (size_t i = 0; i < sizeof(ALIASES) / sizeof(ALIASES[0]); i++) {
        if (iname != sn_intern_cstr(r->intern, ALIASES[i][0])) {
            continue;
        }
        sn_diag_emit(r->diag, SN_DIAG_WARNING, SNOVA_LEGACY_TYPE_SPELLING, span,
                    "Prefer `%s` over `%s` in native Snovalang.", ALIASES[i][1], name);
        return primitive_by_name(r, sn_intern_cstr(r->intern, ALIASES[i][1]));
    }
    return NULL;
}

SnSymbol *sn_resolve_ident(SnResolver *r, const char *current_package,
                            const SnScope *local, const SnDecl *enclosing_type,
                            const SnList *imports, const char *name, SnSpan span) {
    const char *iname = sn_intern_cstr(r->intern, name);

    if (local) {
        SnSymbol *sym = sn_scope_lookup(local, iname);
        if (sym) {
            return sym;
        }
    }

    if (enclosing_type) {
        /* Single-inheritance walk (plan.md's ratified ambiguity 5). The
         * supertype name is looked up in the current package's scope only
         * — no corpus evidence of cross-package inheritance to test against;
         * a `depth` guard stands in for cycle detection since supertypes
         * aren't validated acyclic anywhere before this runs. */
        const SnDecl *cur = enclosing_type;
        int depth = 0;
        while (cur && depth < 64) {
            SnScope *ms = sn_resolver_type_scope(r, cur);
            if (ms) {
                SnSymbol *sym = sn_scope_lookup_local(ms, iname);
                if (sym) {
                    return sym;
                }
            }
            if (cur->supertypes.len == 0) {
                break;
            }
            SnType *super_ty = SN_LIST_AT(cur->supertypes, SnType, 0);
            if (super_ty->kind != SN_TYPE_NAME) {
                break;
            }
            const char *super_name = sn_intern_cstr(r->intern, super_ty->name);
            SnScope *pkg_scope = sn_resolver_package_scope(r, current_package);
            SnSymbol *super_sym =
                pkg_scope ? sn_scope_lookup_local(pkg_scope, super_name) : NULL;
            cur = (super_sym && super_sym->kind == SN_SYM_TYPE) ? super_sym->decl : NULL;
            depth++;
        }
    }

    SnScope *pkg_scope = sn_resolver_package_scope(r, current_package);
    if (pkg_scope) {
        SnSymbol *sym = sn_scope_lookup_local(pkg_scope, iname);
        if (sym) {
            return sym;
        }
    }

    if (imports) {
        for (size_t i = 0; i < imports->len; i++) {
            const char *imp_pkg = SN_LIST_AT(*imports, const char, i);
            SnScope *imp_scope = resolve_import_scope(r, imp_pkg);
            if (!imp_scope) {
                continue;
            }
            SnSymbol *sym = sn_scope_lookup_local(imp_scope, iname);
            if (sym) {
                return sym;
            }
        }
    }

    if (r->prelude_scope) {
        SnSymbol *sym = sn_scope_lookup_local(r->prelude_scope, iname);
        if (sym) {
            return sym;
        }
    }

    sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_UNDECLARED_NAME, span,
                "`%s` is not defined in this scope", name);
    return NULL;
}

SnTypeRep *sn_resolve_type_name(SnResolver *r, const char *current_package,
                                 const SnList *imports, const char *name,
                                 SnSpan span) {
    const char *iname = sn_intern_cstr(r->intern, name);

    SnTypeRep *prim = primitive_by_name(r, iname);
    if (prim) {
        return prim;
    }

    SnScope *pkg_scope = sn_resolver_package_scope(r, current_package);
    SnSymbol *sym = pkg_scope ? sn_scope_lookup_local(pkg_scope, iname) : NULL;

    if (!sym && imports) {
        for (size_t i = 0; i < imports->len && !sym; i++) {
            const char *imp_pkg = SN_LIST_AT(*imports, const char, i);
            SnScope *imp_scope = resolve_import_scope(r, imp_pkg);
            if (imp_scope) {
                sym = sn_scope_lookup_local(imp_scope, iname);
            }
        }
    }

    if (!sym && r->prelude_scope) {
        sym = sn_scope_lookup_local(r->prelude_scope, iname);
    }

    if (sym && sym->kind == SN_SYM_TYPE) {
        /* Bare reference, no generic args threaded through — this is name
         * resolution, not full construction with argument checking (P2.5's
         * job). Callers build `List<int>` themselves via sn_type_named()
         * once `List` and `int` have each resolved independently. */
        return sn_type_named(r->types, sym, NULL, 0);
    }

    /* Not found locally/via import/via prelude: search the whole graph
     * before giving up, to tell "doesn't exist" (SNOVA_UNKNOWN_TYPE) apart
     * from "exists, just not imported" (SNOVA_TYPE_NOT_IMPORTED). */
    for (SnPackageScopeEntry *e = r->packages; e; e = e->next) {
        SnSymbol *found = sn_scope_lookup_local(e->scope, iname);
        if (found && found->kind == SN_SYM_TYPE) {
            sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_TYPE_NOT_IMPORTED, span,
                        "type `%s` is not imported", name);
            return NULL;
        }
    }

    /* Legacy capitalized spellings resolve to the primitive with a warning,
     * not an error — the same six pairs, and the same SNOVA011 advice, that
     * crates/snovalang/src/native/selfcheck/mod.rs applies. Without this,
     * `func main(): Int` (tests/compile-pass/hello.snova) failed here while
     * the shipped `snova check` accepted it. */
    SnTypeRep *legacy = legacy_primitive_alias(r, iname, name, span);
    if (legacy) {
        return legacy;
    }

    sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_UNKNOWN_TYPE, span,
                "Unknown type `%s`. Declare it or import the package that provides it.",
                name);
    return NULL;
}
