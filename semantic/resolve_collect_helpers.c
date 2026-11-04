#include "resolve.h"
#include "resolve_internal.h"

#include <string.h>

int top_level_symbol_kind(SnDeclKind k, SnSymbolKind *out) {
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

int member_symbol_kind(SnDeclKind k, SnSymbolKind *out) {
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

int decl_kind_has_member_scope(SnDeclKind k) {
    return k == SN_DECL_CLASS || k == SN_DECL_STRUCT || k == SN_DECL_INTERFACE ||
           k == SN_DECL_ENUM;
}

