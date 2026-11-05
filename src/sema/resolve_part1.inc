#include "resolve.h"

#include <string.h>

#include "lex.h"
#include "parse.h"

/* Option/Result's real package (see resolve.h note 3). */
#define SN_PRELUDE_PACKAGE "builtin.types.Types"

void sn_resolver_init(SnResolver *r, SnArena *a, SnInternTable *it,
                       SnDiagSink *diag, SnPackageGraph *graph,
                       SnTypeTable *types) {
    r->arena = a;
    r->intern = it;
    r->diag = diag;
    r->graph = graph;
    r->types = types;
    r->prelude_scope = NULL;
    r->packages = NULL;
    r->type_scopes = NULL;
    r->current_origin = NULL;
}

SnScope *sn_resolver_package_scope(const SnResolver *r, const char *package_name) {
    for (SnPackageScopeEntry *e = r->packages; e; e = e->next) {
        if (e->package_name == package_name) {
            return e->scope;
        }
    }
    return NULL;
}

/* An import names either a whole package or PACKAGE.SYMBOL — package.c's
 * resolve_import_target comment documents the same ambiguity for cycle
 * detection/SNOVA050 ("the symbol spelling is the common one in real
 * projects"). Every import-scope lookup in this file needs the same
 * longest-match behavior: try the import string outright as a package name,
 * then drop trailing segments until one names a real, collected package.
 * Without this, `import a.b.RealSymbol` (a.b is the real package) never
 * actually brought RealSymbol into scope — only the bare `import a.b` form
 * worked, silently, even though package.c's own linking phase already
 * accepts both spellings as valid. Returns NULL (an uninterned string, not
 * a valid scope key) when no prefix is a collected package at all. */
static const char *resolve_import_package_name(const SnResolver *r, const char *imp_pkg) {
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

static SnScope *resolve_import_scope(const SnResolver *r, const char *imp_pkg) {
    const char *real_pkg = resolve_import_package_name(r, imp_pkg);
    return real_pkg ? sn_resolver_package_scope(r, real_pkg) : NULL;
}

SnScope *sn_resolver_type_scope(const SnResolver *r, const SnDecl *type_decl) {
    for (SnTypeScopeEntry *e = r->type_scopes; e; e = e->next) {
        if (e->type_decl == type_decl) {
            return e->member_scope;
        }
    }
    return NULL;
}

static SnScope *package_scope_find_or_create(SnResolver *r, const char *name) {
    SnScope *s = sn_resolver_package_scope(r, name);
    if (s) {
        return s;
    }
    s = (SnScope *)sn_arena_alloc(r->arena, sizeof(SnScope));
    sn_scope_init(s, r->arena, NULL);

    SnPackageScopeEntry *e =
        (SnPackageScopeEntry *)sn_arena_alloc(r->arena, sizeof(SnPackageScopeEntry));
    e->package_name = name;
    e->scope = s;
    e->next = r->packages;
    r->packages = e;
    return s;
}

static SnScope *type_scope_create(SnResolver *r, const SnDecl *decl) {
    SnScope *s = (SnScope *)sn_arena_alloc(r->arena, sizeof(SnScope));
    sn_scope_init(s, r->arena, NULL);

    SnTypeScopeEntry *e =
        (SnTypeScopeEntry *)sn_arena_alloc(r->arena, sizeof(SnTypeScopeEntry));
    e->type_decl = decl;
    e->member_scope = s;
    e->next = r->type_scopes;
    r->type_scopes = e;
    return s;
}

/* ── declaration collection ───────────────────────────────────────────────── */

static int top_level_symbol_kind(SnDeclKind k, SnSymbolKind *out) {
    switch (k) {
    case SN_DECL_CLASS:
    case SN_DECL_STRUCT:
    case SN_DECL_ENUM:
    case SN_DECL_INTERFACE:
    case SN_DECL_TYPEALIAS:
        *out = SN_SYM_TYPE;
        return 1;
    case SN_DECL_FUNC:
        *out = SN_SYM_FUNC;
        return 1;
    case SN_DECL_CONST:
        *out = SN_SYM_CONST;
        return 1;
    default:
        return 0; /* METHOD/FIELD/VARIANT never appear at top level;
                   * EXTENSION is handled by the caller separately */
    }
}

static int member_symbol_kind(SnDeclKind k, SnSymbolKind *out) {
    switch (k) {
    case SN_DECL_METHOD:
    /* Extension members (both `extension X { func f() }` and the
     * single-declaration `extension X.f()` sugar) are parsed as
     * SN_DECL_FUNC, never SN_DECL_METHOD — parse_extension() always
     * builds a plain func decl for the member. Without this case,
     * collect_member() silently dropped every extension method (the
     * `!via_extension` FUNC-in-type-body diagnostic above is the only
     * other place SN_DECL_FUNC meets a member scope, and it already
     * reports its own error), so no `extension` block or single-decl
     * method — including Option/Result's isSome()/unwrap()/map() —
     * was ever actually resolvable as a member. */
    case SN_DECL_FUNC:
        *out = SN_SYM_METHOD;
        return 1;
    case SN_DECL_FIELD:
        *out = SN_SYM_FIELD;
        return 1;
    case SN_DECL_CONST:
        *out = SN_SYM_CONST;
        return 1;
    default:
        return 0;
    }
}

static int decl_kind_has_member_scope(SnDeclKind k) {
    return k == SN_DECL_CLASS || k == SN_DECL_STRUCT || k == SN_DECL_INTERFACE ||
           k == SN_DECL_ENUM;
}

static char *read_whole_file(SnArena *a, const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)sn_arena_alloc(a, (size_t)n + 1u);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

/* sn_scope_define() plus the provenance stamp — see SnSymbol.origin. Every
 * definition made while walking a file must go through here, or a diagnostic
 * raised for that symbol later has no file to be measured against. */
static SnSymbol *define_with_origin(SnResolver *r, SnScope *scope, const char *name,
                                     SnSymbolKind kind, const SnDecl *decl,
                                     SnSpan span) {
    SnSymbol *sym = sn_scope_define(scope, name, kind, decl, span);
    if (sym) {
        sym->origin = r->current_origin;
    }
    return sym;
}

static int ast_type_eq(const SnType *a, const SnType *b) {
    if (a == b) {
        return 1;
    }
    if (!a || !b || a->kind != b->kind) {
        return 0;
    }
    switch (a->kind) {
    case SN_TYPE_NAME: {
        if (!a->name || !b->name || strcmp(a->name, b->name) != 0) {
            return 0;
        }
        if (a->args.len != b->args.len || a->is_optional != b->is_optional) {
            return 0;
        }
        for (size_t i = 0; i < a->args.len; i++) {
            if (!ast_type_eq(SN_LIST_AT(a->args, SnType, i),
                             SN_LIST_AT(b->args, SnType, i))) {
                return 0;
            }
        }
        return 1;
    }
    case SN_TYPE_FUNC:
        if (a->params.len != b->params.len || !ast_type_eq(a->ret, b->ret)) {
            return 0;
        }
        for (size_t i = 0; i < a->params.len; i++) {
            if (!ast_type_eq(SN_LIST_AT(a->params, SnType, i),
                             SN_LIST_AT(b->params, SnType, i))) {
                return 0;
            }
        }
        return 1;
    case SN_TYPE_TUPLE:
        if (a->params.len != b->params.len) {
            return 0;
        }
        for (size_t i = 0; i < a->params.len; i++) {
            if (!ast_type_eq(SN_LIST_AT(a->params, SnType, i),
                             SN_LIST_AT(b->params, SnType, i))) {
                return 0;
            }
        }
        return 1;
    case SN_TYPE_REF:
        return a->is_nullable == b->is_nullable && ast_type_eq(a->pointee, b->pointee);
    default:
        return 0;
    }
}

static int decl_param_types_eq(const SnDecl *a, const SnDecl *b) {
    if (!a || !b || a->params.len != b->params.len) {
        return 0;
    }
    for (size_t i = 0; i < a->params.len; i++) {
        const SnParam *pa = SN_LIST_AT(a->params, SnParam, i);
        const SnParam *pb = SN_LIST_AT(b->params, SnParam, i);
        if (!ast_type_eq(pa->type, pb->type)) {
            return 0;
        }
    }
    return 1;
}

static int overload_chain_has_sig(const SnSymbol *primary, const SnDecl *decl) {
    for (const SnSymbol *s = primary; s; s = s->overloads) {
        if (decl_param_types_eq(s->decl, decl)) {
            return 1;
        }
    }
    return 0;
}

static void chain_overload(SnResolver *r, SnSymbol *primary, const char *name,
                           SnSymbolKind kind, const SnDecl *decl, SnSpan span) {
    SnSymbol *extra = (SnSymbol *)sn_arena_calloc(r->arena, sizeof(SnSymbol));
    extra->name = name;
    extra->kind = kind;
    extra->decl = decl;
    extra->span = span;
    extra->origin = r->current_origin;
    sn_symbol_chain_overload(primary, extra);
}

static void collect_member(SnResolver *r, SnScope *member_scope, const SnDecl *owner,
                            const SnDecl *m, const char *via_extension) {
    /* `func` belongs at the top level or in an `extension` body; inside a
     * class/struct/interface it must be `method`. Checked before
     * member_symbol_kind so it is reported whether or not a top-level-shaped
     * declaration has a member symbol kind at all. */
    if (!via_extension && m->kind == SN_DECL_FUNC) {
        sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_FUNC_IN_TYPE_BODY, m->span,
                     "`func` is not valid inside a class body -- use `method`.");
    }

    SnSymbolKind msk;
    if (!member_symbol_kind(m->kind, &msk)) {
        return;
    }
    const char *mname = sn_intern_cstr(r->intern, m->name);

    /* A private backing field and its same-named public accessor method
     * (`private let path: Path` + `method path(): Path { return path }`,
     * both real, in builtin/FileSystem.snl's `File`) is a legitimate,
     * idiomatic pattern — not a collision. Found while running this
     * collector against the real corpus (2026-07-25); not one of plan.md
     * §7's ratified ambiguities, so treated as its own narrow rule: a
     * FIELD/METHOD pair sharing a name is allowed, and whichever was
     * collected first wins the name in the member scope (the practical
     * effect: within-class references to the bare name still resolve to
     * *something* real; disambiguating "the field" from "a call to the
     * method" needs call-site syntax, which is check.c's job, not this
     * scope table's). Any other same-kind collision remains a real error. */
    SnSymbol *existing = sn_scope_lookup_local(member_scope, mname);
    if (existing && ((existing->kind == SN_SYM_FIELD && msk == SN_SYM_METHOD) ||
                      (existing->kind == SN_SYM_METHOD && msk == SN_SYM_FIELD))) {
        return;
    }
    if (existing && existing->kind == msk &&
        (msk == SN_SYM_METHOD || msk == SN_SYM_FUNC)) {
        if (overload_chain_has_sig(existing, m)) {
            sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_DUPLICATE_DECL, m->span,
                         "`%s` is already declared in `%s`", m->name, owner->name);
            return;
        }
        chain_overload(r, existing, mname, msk, m, m->span);
        return;
    }

    SnSymbol *msym = define_with_origin(r, member_scope, mname, msk, m, m->span);
    if (!msym) {
        SnSymbol *existing_m = sn_scope_lookup_local(member_scope, mname);
        if (existing_m && existing_m->kind == msk &&
            existing_m->origin && r->current_origin &&
            existing_m->origin->path && r->current_origin->path &&
            strcmp(existing_m->origin->path, r->current_origin->path) != 0) {
            return;
        }
        if (via_extension) {
            sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_DUPLICATE_DECL, m->span,
                         "`%s` is already declared in `%s` (via extension)",
                         m->name, owner->name);
        } else {
            sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_DUPLICATE_DECL, m->span,
                         "`%s` is already declared in `%s`", m->name, owner->name);
        }
    }
}

typedef struct ExtEntry ExtEntry;
struct ExtEntry {
    const SnDecl *decl; /* SN_DECL_EXTENSION */
    const char *package;
    const SnDiagFile *origin; /* file the extension block was parsed from */
    ExtEntry *next;
};

/* Merges one `extension` block's members into the same-package type it
 * targets. Silently does nothing when the target isn't a type of that package
 * — there is no corpus evidence of cross-package extension, so the whole
 * graph is not searched for one. */
static void collect_extension_members(SnResolver *r, const ExtEntry *ee) {
    const char *target_name = sn_intern_cstr(r->intern, ee->decl->name);
    SnScope *pkg_scope = sn_resolver_package_scope(r, ee->package);
    SnSymbol *target_sym =
        pkg_scope ? sn_scope_lookup_local(pkg_scope, target_name) : NULL;
    if (!target_sym || target_sym->kind != SN_SYM_TYPE) {
        return;
    }
    SnScope *member_scope = sn_resolver_type_scope(r, target_sym->decl);
    if (!member_scope) {
        return;
    }
    for (size_t j = 0; j < ee->decl->members.len; j++) {
        SnDecl *m = SN_LIST_AT(ee->decl->members, SnDecl, j);
        collect_member(r, member_scope, ee->decl, m, ee->decl->name);
    }
}

