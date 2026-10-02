/* parse_ptr.c — `&expr`, `*expr`, and the `null` literal. */
#include "parse_internal.h"

SnExpr *parse_ptr_atom(P *p, SnSpan span) {
    if (at(p, SN_TOK_NULL)) {
        advance_p(p);
        return new_expr(p, SN_EXPR_NULL, span);
    }
    SnTokKind op = kind(p);
    advance_p(p);
    SnExpr *e = new_expr(p, SN_EXPR_UNARY, span);
    e->op = op;
    e->lhs = parse_primary(p);
    return e;
}
