/* borrow_flow.c — bindings, branches, and loops. */
#include "borrow_int.h"

#include <string.h>

void sn_borrow_bind(SnBorrowCx *cx, const char *name, int mut, SnExpr *init) {
    SnTypeRep *ty = init ? init->resolved_type : NULL;
    int vi = sn_borrow_define(cx, name, ty, mut, init);
    if (vi < 0) {
        return;
    }
    if (init && init->kind == SN_EXPR_UNARY && init->op == SN_TOK_AMP &&
        init->lhs && init->lhs->kind == SN_EXPR_IDENT) {
        sn_borrow_borrow_place(cx, sn_borrow_lookup(cx, init->lhs->text), vi, init->span);
        return;
    }
    if (init && init->kind == SN_EXPR_NULL) {
        return;
    }
    int mode = (mut && init && init->resolved_type && !sn_borrow_is_copy(init->resolved_type))
                   ? SN_BORROW_USE_MOVE
                   : SN_BORROW_USE_READ;
    sn_borrow_expr(cx, init, mode);
}

static int null_test(SnExpr *e, const char **name, int *ne) {
    if (!e || e->kind != SN_EXPR_BINARY) {
        return 0;
    }
    SnExpr *id = NULL;
    if (e->lhs && e->lhs->kind == SN_EXPR_IDENT && e->rhs && e->rhs->kind == SN_EXPR_NULL) {
        id = e->lhs;
    } else if (e->rhs && e->rhs->kind == SN_EXPR_IDENT && e->lhs &&
               e->lhs->kind == SN_EXPR_NULL) {
        id = e->rhs;
    }
    if (!id || (e->op != SN_TOK_EQ && e->op != SN_TOK_NE)) {
        return 0;
    }
    *name = id->text;
    *ne = e->op == SN_TOK_NE;
    return 1;
}

void sn_borrow_if(SnBorrowCx *cx, SnStmt *s) {
    const char *nm = NULL;
    int ne = 0;
    int narrowed = null_test(s->expr, &nm, &ne);
    sn_borrow_expr(cx, s->expr, SN_BORROW_USE_READ);
    int n = cx->nv;
    SnBorrowVar before[SN_BORROW_MAX];
    SnBorrowVar then_state[SN_BORROW_MAX];
    memcpy(before, cx->vars, (size_t)n * sizeof(*before));
    int hold_watermark = cx->nh;
    SnBorrowVar *v = narrowed ? sn_borrow_lookup(cx, nm) : NULL;
    int prev = v ? v->nonnull : 0;
    if (v && ne) {
        v->nonnull = 1;
    }
    sn_borrow_stmt(cx, s->then_br);
    sn_borrow_pop_holds(cx, hold_watermark);
    if (v) {
        v->nonnull = prev;
    }
    for (int i = 0; i < n; i++) {
        then_state[i] = cx->vars[i];
        cx->vars[i] = before[i];
    }
    if (v && !ne) {
        v->nonnull = 1;
    }
    sn_borrow_stmt(cx, s->else_br);
    sn_borrow_pop_holds(cx, hold_watermark);
    if (v) {
        v->nonnull = prev;
    }
    for (int i = 0; i < n; i++) {
        SnBorrowVar else_state = cx->vars[i];
        SnBorrowVar merged = else_state;
        merged.moved = then_state[i].moved || else_state.moved;
        merged.escapes = then_state[i].escapes || else_state.escapes;
        merged.defer_hold = then_state[i].defer_hold || else_state.defer_hold;
        merged.shared = then_state[i].shared > else_state.shared
                            ? then_state[i].shared : else_state.shared;
        merged.exclusive = then_state[i].exclusive || else_state.exclusive;
        if (then_state[i].points != else_state.points ||
            then_state[i].hold_excl != else_state.hold_excl) {
            sn_borrow_fail(cx, SNOVA_BORROW_BRANCH,
                           s->span,
                           "reference is rebound to different places across branches");
            merged.points = -1;
            merged.hold_excl = 0;
        }
        merged.nonnull = before[i].nonnull;
        cx->vars[i] = merged;
    }
}

void sn_borrow_loop(SnBorrowCx *cx, SnStmt *body) {
    int n = cx->nv;
    SnBorrowVar before[SN_BORROW_MAX];
    memcpy(before, cx->vars, (size_t)n * sizeof(*before));
    sn_borrow_stmt(cx, body);
    for (int i = 0; i < n; i++) {
        SnBorrowVar after = cx->vars[i];
        if (!before[i].moved && after.moved) {
            sn_borrow_fail(cx, SNOVA_USE_AFTER_MOVE,
                     cx->vars[i].init ? cx->vars[i].init->span : (SnSpan){0},
                     "value moved in a loop");
        }
        if (before[i].points != after.points ||
            before[i].hold_excl != after.hold_excl) {
            sn_borrow_fail(cx, SNOVA_BORROW_LOOP,
                           body ? body->span : (SnSpan){0},
                           "reference is rebound across a loop iteration; loop-carried borrows are not supported");
        }
        cx->vars[i] = before[i];
        cx->vars[i].escapes = before[i].escapes || after.escapes;
        cx->vars[i].defer_hold = before[i].defer_hold || after.defer_hold;
    }
}
