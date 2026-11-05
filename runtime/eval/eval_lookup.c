/* eval_lookup.c — declaration and member lookup over a borrowed AST unit. */
#include "eval_internal.h"

const SnDecl *find_member(const SnDecl *cls, const char *name) {
    if (!cls) return NULL;
    for (size_t i = 0; i < cls->members.len; ++i) {
        const SnDecl *member = (const SnDecl *)cls->members.items[i];
        if (member->name && strcmp(member->name, name) == 0) return member;
    }
    return NULL;
}

const SnDecl *find_member_inherited(const Interp *in, const SnDecl *cls,
                                    const char *name) {
    if (!cls) return NULL;
    const SnDecl *member = find_member(cls, name);
    if (member) return member;
    for (size_t i = 0; i < cls->supertypes.len; ++i) {
        const SnType *supertype = (const SnType *)cls->supertypes.items[i];
        if (!supertype || !supertype->name) continue;
        const SnDecl *parent = find_type(in, supertype->name);
        if (!parent) continue;
        member = find_member_inherited(in, parent, name);
        if (member) return member;
    }
    return NULL;
}

const SnDecl *find_top(const Interp *in, const char *name, SnDeclKind kind) {
    for (size_t i = 0; i < in->unit->decls.len; ++i) {
        const SnDecl *decl = (const SnDecl *)in->unit->decls.items[i];
        if (decl->kind == kind && decl->name && strcmp(decl->name, name) == 0)
            return decl;
    }
    return NULL;
}

const SnDecl *find_type(const Interp *in, const char *name) {
    for (size_t i = 0; i < in->unit->decls.len; ++i) {
        const SnDecl *decl = (const SnDecl *)in->unit->decls.items[i];
        if ((decl->kind == SN_DECL_CLASS || decl->kind == SN_DECL_STRUCT) &&
            decl->name && strcmp(decl->name, name) == 0)
            return decl;
    }
    return NULL;
}
