/* parse_expr.c — the Pratt engine: postfix chains, infix binding powers and
 * the ternary/error-propagation split. Primaries live in parse_primary.c. */
#include "parse_internal.h"

typedef struct {
    int lbp; /* left binding power; 0 means "not an infix operator" */
    int right_assoc;
} BindPower;

static BindPower infix_power(SnTokKind k) {
    BindPower b = {0, 0};
    switch (k) {
    case SN_TOK_ASSIGN:
    case SN_TOK_PLUS_EQ: case SN_TOK_MINUS_EQ:
    case SN_TOK_STAR_EQ: case SN_TOK_SLASH_EQ:
        b.lbp = 1; b.right_assoc = 1; break;
    case SN_TOK_OROR:    b.lbp = 2; break;
    case SN_TOK_QQ:      b.lbp = 2; b.right_assoc = 1; break; /* `a ?? b` right-associative null-coalescing */
    case SN_TOK_ANDAND:  b.lbp = 3; break;
    case SN_TOK_PIPE:    b.lbp = 4; break;
    case SN_TOK_CARET:   b.lbp = 5; break;
    case SN_TOK_AMP:     b.lbp = 6; break;
    case SN_TOK_EQ: case SN_TOK_NE: b.lbp = 7; break;
    case SN_TOK_LT: case SN_TOK_GT:
    case SN_TOK_LE: case SN_TOK_GE: b.lbp = 8; break;
    /* 9 is reserved for shift — see at_shift(). */
    case SN_TOK_PLUS: case SN_TOK_MINUS: b.lbp = 10; break;
    case SN_TOK_STAR: case SN_TOK_SLASH: case SN_TOK_PERCENT: b.lbp = 11; break;
    case SN_TOK_AS: case SN_TOK_IS: b.lbp = 12; break;
    default: break;
    }
    return b;
}

#define SN_SHIFT_BP 9

/* Shift operators exist, but the lexer never emits `>>` because that sequence
 * also closes nested generics (`Task<Result<unit, E>>`). Both readings are real:
 * libs/ uses `>>` only for generics, compiler/src has `(hi >> 16) & 0xFF`.
 *
 * They are told apart by position, not by spelling: only in infix position,
 * with the two angle brackets physically adjacent, is this a shift. Type
 * argument lists consume one `>` at a time and never reach here. */
static int at_shift(P *p, SnTokKind *op_out) {
    SnTokKind k = kind(p);
    if (k != SN_TOK_GT && k != SN_TOK_LT) {
        return 0;
    }
    const SnToken *a = cur(p);
    const SnToken *b = peek_at(p, 1);
    if (b->kind != k) {
        return 0;
    }
    if (a->span.offset + a->span.len != b->span.offset) {
        return 0; /* `a > > b` is not a shift */
    }
    /* Not SN_TOK_LT/SN_TOK_GT: those already mean "less/greater than" to
     * every consumer of e->op. A dedicated op keeps shift from being
     * silently misread as a comparison. */
    *op_out = (k == SN_TOK_LT) ? SN_TOK_SHL : SN_TOK_SHR;
    return 1;
}

static SnExpr *parse_binary(P *p, int min_bp) {
    SnExpr *lhs = parse_postfix(p, parse_primary(p));

    for (;;) {
        /* In a match-arm body, an infix operator that opens a new line is not a
         * continuation: `0 -> return 1` followed by `-1 -> ...` must end the
         * body at the newline so `-1` reads as the next arm's pattern. */
        if (p->nl_stops_infix && p->pos > 0 &&
            cur(p)->span.line > p->toks->data[p->pos - 1].span.line) {
            break;
        }
        SnTokKind shift_op;
        if (at_shift(p, &shift_op) && SN_SHIFT_BP >= min_bp) {
            SnSpan span = cur(p)->span;
            advance_p(p);
            advance_p(p);
            SnExpr *e = new_expr(p, SN_EXPR_BINARY, span);
            e->op = shift_op;
            e->lhs = lhs;
            e->rhs = parse_binary(p, SN_SHIFT_BP + 1);
            lhs = e;
            continue;
        }

        SnTokKind op = kind(p);
        BindPower bp = infix_power(op);
        if (bp.lbp == 0 || bp.lbp < min_bp) {
            break;
        }
        SnSpan span = cur(p)->span;
        advance_p(p);

        if (op == SN_TOK_AS || op == SN_TOK_IS) {
            SnExpr *e = new_expr(p, op == SN_TOK_AS ? SN_EXPR_CAST : SN_EXPR_IS,
                                 span);
            e->lhs = lhs;
            e->type = parse_type(p);
            lhs = e;
            continue;
        }

        int next_bp = bp.right_assoc ? bp.lbp : bp.lbp + 1;
        SnExpr *rhs = parse_binary(p, next_bp);

        SnExprKind ek = (bp.lbp == 1) ? SN_EXPR_ASSIGN : SN_EXPR_BINARY;
        SnExpr *e = new_expr(p, ek, span);
        e->op = op;
        e->lhs = lhs;
        e->rhs = rhs;
        lhs = e;
    }
    return lhs;
}

SnExpr *parse_expr(P *p) {
    SnExpr *e = parse_binary(p, 1);

    if (at(p, SN_TOK_QUESTION) && is_ternary_question(p)) {
        SnSpan span = cur(p)->span;
        /* Ternary: `parseInt(e) >= 0 ? intToString(...) : "-"`. Modelled as
         * a one-armed match-free conditional using the binary slots. */
        advance_p(p);
        SnExpr *t = new_expr(p, SN_EXPR_BINARY, span);
        t->op = SN_TOK_QUESTION;
        t->lhs = e;
        SnExpr *then_e = parse_expr(p);
        expect(p, SN_TOK_COLON);
        SnExpr *else_e = parse_expr(p);
        SnExpr *pair = new_expr(p, SN_EXPR_BINARY, span);
        pair->op = SN_TOK_COLON;
        pair->lhs = then_e;
        pair->rhs = else_e;
        t->rhs = pair;
        return t;
    }
    return e;
}
