#ifndef SNOVAC_RESOLVE_INTERNAL_H
#define SNOVAC_RESOLVE_INTERNAL_H

#include "resolve.h"

SnScope *package_scope_find_or_create(SnResolver *resolver, const char *name);
SnScope *type_scope_create(SnResolver *resolver, const SnDecl *decl);
int top_level_symbol_kind(SnDeclKind kind, SnSymbolKind *out);
int member_symbol_kind(SnDeclKind kind, SnSymbolKind *out);
int decl_kind_has_member_scope(SnDeclKind kind);
const char *resolve_import_package_name(const SnResolver *resolver,
                                       const char *import_name);
SnScope *resolve_import_scope(const SnResolver *resolver,
                              const char *import_name);

#endif
