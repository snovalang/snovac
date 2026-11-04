#include "check.h"
#include "check_internal.h"

#include <string.h>

#include "builtins.h"

/* ── symbol -> type, and the linear-scan helpers that back it ────────────── */

SnSymbol *sn_check_find_type_symbol_for_decl(SnResolver *r, const SnDecl *type_decl) {
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

SnSymbol *find_variant_owner_symbol(SnResolver *r, const SnSymbol *variant_sym) {
    const SnDecl *owner_decl = NULL;
    for (SnTypeScopeEntry *e = r->type_scopes; e; e = e->next) {
        SnSymbol *found = sn_scope_lookup_local(e->member_scope, variant_sym->name);
        if (found == variant_sym) {
            owner_decl = e->type_decl;
            break;
        }
    }
    return owner_decl ? sn_check_find_type_symbol_for_decl(r, owner_decl) : NULL;
}

SnTypeRep *func_like_type(SnChecker *c, const SnDecl *decl) {
    SnTypeRep **params = NULL;
    if (decl->params.len) {
        params =
            (SnTypeRep **)sn_arena_alloc(c->arena, decl->params.len * sizeof(SnTypeRep *));
        for (size_t i = 0; i < decl->params.len; i++) {
            SnParam *p = SN_LIST_AT(decl->params, SnParam, i);
            params[i] = sn_check_resolve_type(c, p->type);
        }
    }
    SnTypeRep *ret = sn_check_resolve_type(c, decl->ret);
    return sn_type_func(c->types, params, (uint32_t)decl->params.len, ret);
}

static SnTypeRep *symbol_type_in_scope(SnChecker *c, SnSymbol *sym);
void sn_check_bind_pattern_names(SnChecker *c, SnScope *scope, const SnPattern *pat,
                                 SnTypeRep *subject_type,
                                 int subject_is_borrowed);
int sn_check_match_exhaustiveness(SnChecker *c, const SnTypeRep *target_ty,
                                  const SnList *arms, SnSpan span);

/* True when `sym` was declared in a file other than the one being checked.
 * Resolving such a symbol's declared types re-runs name resolution in the
 * CURRENT context, where the other file's imports and type parameters do not
 * exist — `T` in builtin/Errors.snova's `func try<T, E>` is not in scope at a
 * call site in a fixture. Whatever it reports there would be both wrong and
 * attributed to the wrong file, and the real problem (if any) is reported
 * where that declaration's own body is checked. */
static int symbol_is_foreign(const SnChecker *c, const SnSymbol *sym) {
    return sym && sym->origin && sym->origin->path != c->diag->file.path;
}

/* Resolves a type as written in `sym`'s declaration. Silent when `sym` is
 * foreign: the resolution runs in the current file's scope, which is the
 * wrong one for it, so anything it would report is noise here. */
SnTypeRep *resolve_declared_type(SnChecker *c, const SnSymbol *sym,
                                        const SnType *t) {
    int foreign = symbol_is_foreign(c, sym);
    c->diag->quiet += foreign;
    SnTypeRep *ty = sn_check_resolve_type(c, t);
    c->diag->quiet -= foreign;
    return ty;
}

SnTypeRep *symbol_type(SnChecker *c, SnSymbol *sym) {
    if (!sym) {
        return sn_type_error(c->types);
    }
    int foreign = symbol_is_foreign(c, sym);
    c->diag->quiet += foreign;
    SnTypeRep *ty = symbol_type_in_scope(c, sym);
    c->diag->quiet -= foreign;
    return ty;
}

static SnTypeRep *symbol_type_in_scope(SnChecker *c, SnSymbol *sym) {
    switch (sym->kind) {
    case SN_SYM_LOCAL:
    case SN_SYM_PARAM:
        return sym->value_type ? sym->value_type : sn_type_error(c->types);
    case SN_SYM_FIELD:
    case SN_SYM_CONST:
        return sn_check_resolve_type(c, sym->decl->type);
    case SN_SYM_FUNC:
    case SN_SYM_METHOD:
        return func_like_type(c, sym->decl);
    case SN_SYM_TYPE:
        return sn_type_named(c->types, sym, NULL, 0);
    case SN_SYM_VARIANT:
        /* A bare reference to a variant name without calling it (no corpus
         * evidence this happens — variants are always constructed,
         * `Some(x)`/`None`) has no clean type here; the CALL path handles
         * construction separately via find_variant_owner_symbol. */
        return sn_type_error(c->types);
    case SN_SYM_PACKAGE:
        return sn_type_error(c->types); /* not a value */
    }
    return sn_type_error(c->types);
}

int is_mutable_symbol(const SnChecker *c, const SnSymbol *sym) {
    if (!sym) {
        return 1; /* unresolved: already diagnosed elsewhere, don't cascade */
    }
    switch (sym->kind) {
    case SN_SYM_LOCAL:
        return sym->is_mutable;
    case SN_SYM_FIELD:
        /* A `let` field is immutable everywhere EXCEPT the constructor, which
         * is where it gets its one value — `this.description = description` in
         * tests/compile-pass/p1_syntax_additions.snova. Assigning it twice is
         * a definite-assignment question, and flow analysis is P4. */
        return (sym->decl && sym->decl->is_mutable) || c->in_constructor;
    default:
        return 0;
    }
}

/* ── member lookup with single-inheritance walk (mirrors sn_resolve_ident's,
 * kept local since resolve.c doesn't expose its internal walk as a
 * standalone function) ──────────────────────────────────────────────────── */

SnSymbol *lookup_member_with_inheritance(SnChecker *c, const SnDecl *type_decl,
                                                const char *name) {
    const SnDecl *cur = type_decl;
    int depth = 0;
    while (cur && depth < 64) {
        SnScope *ms = sn_resolver_type_scope(c->resolver, cur);
        if (ms) {
            SnSymbol *sym = sn_scope_lookup_local(ms, name);
            if (sym) {
                return sym;
            }
        }
        if (cur->supertypes.len == 0) {
            break;
        }
        SnSymbol *super_sym = NULL;
        for (size_t i = 0; i < cur->supertypes.len; i++) {
            SnType *super_ty = SN_LIST_AT(cur->supertypes, SnType, i);
            if (super_ty->kind != SN_TYPE_NAME) {
                continue;
            }
            const char *super_name = sn_intern_cstr(c->intern, super_ty->name);
            super_sym = find_type_symbol_anywhere(c, cur, super_name);
            if (super_sym && super_sym->kind == SN_SYM_TYPE) {
                SnScope *sms = sn_resolver_type_scope(c->resolver, super_sym->decl);
                if (sms) {
                    SnSymbol *sym = sn_scope_lookup_local(sms, name);
                    if (sym) {
                        return sym;
                    }
                }
                break;
            }
        }
        cur = (super_sym && super_sym->kind == SN_SYM_TYPE) ? super_sym->decl : NULL;
        depth++;
    }
    return NULL;
}
