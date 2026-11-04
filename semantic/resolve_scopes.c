#include "resolve.h"
#include "resolve_internal.h"

#include <string.h>

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
SnScope *sn_resolver_type_scope(const SnResolver *r, const SnDecl *type_decl) {
    for (SnTypeScopeEntry *e = r->type_scopes; e; e = e->next) {
        if (e->type_decl == type_decl) {
            return e->member_scope;
        }
    }
    return NULL;
}

SnScope *package_scope_find_or_create(SnResolver *r, const char *name) {
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

SnScope *type_scope_create(SnResolver *r, const SnDecl *decl) {
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

