/* borrow_flow.c — bindings, branches, and loops. */
#include "borrow_int.h"

void own_bind(OwnCx *cx, const char *name, int mut, SnExpr *init) {
    SnTypeRep *ty = init ? init->resolved_type : NULL;
    int vi = own_define(cx, name, ty, mut, init);
    if (vi < 0) {
        return;
    }
    if (init && init->kind == SN_EXPR_UNARY && init->op == SN_TOK_AMP &&
        init->lhs && init->lhs->kind == SN_EXPR_IDENT) {
        own_borrow_place(cx, own_lookup(cx, init->lhs->text), vi, init->span);
        return;
    }
    if (init && init->kind == SN_EXPR_NULL) {
        return;
    }
    int mode = (mut && init && init->resolved_type && !own_is_copy(init->resolved_type))
                   ? USE_MOVE
                   : USE_READ;
    own_expr(cx, init, mode);
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

void own_if(OwnCx *cx, SnStmt *s) {
    const char *nm = NULL;
    int ne = 0;
    int narrowed = null_test(s->expr, &nm, &ne);
    own_expr(cx, s->expr, USE_READ);
    int moved[OWN_MAX];
    int n = cx->nv;
    for (int i = 0; i < n; i++) {
        moved[i] = cx->vars[i].moved;
    }
    OwnVar *v = narrowed ? own_lookup(cx, nm) : NULL;
    int prev = v ? v->nonnull : 0;
    if (v && ne) {
        v->nonnull = 1;
    }
    own_stmt(cx, s->then_br);
    if (v) {
        v->nonnull = prev;
    }
    int then_moved[OWN_MAX];
    for (int i = 0; i < n; i++) {
        then_moved[i] = cx->vars[i].moved;
        cx->vars[i].moved = moved[i];
    }
    if (v && !ne) {
        v->nonnull = 1;
    }
    own_stmt(cx, s->else_br);
    if (v) {
        v->nonnull = prev;
    }
    for (int i = 0; i < n; i++) {
        cx->vars[i].moved = then_moved[i] || cx->vars[i].moved;
    }
}

void own_loop(OwnCx *cx, SnStmt *body) {
    int moved[OWN_MAX];
    int shared[OWN_MAX];
    int excl[OWN_MAX];
    int n = cx->nv;
    for (int i = 0; i < n; i++) {
        moved[i] = cx->vars[i].moved;
        shared[i] = cx->vars[i].shared;
        excl[i] = cx->vars[i].exclusive;
    }
    own_stmt(cx, body);
    for (int i = 0; i < n; i++) {
        if (!moved[i] && cx->vars[i].moved) {
            own_fail(cx, SNOVA_USE_AFTER_MOVE,
                     cx->vars[i].init ? cx->vars[i].init->span : (SnSpan){0},
                     "value moved in a loop");
        }
        cx->vars[i].moved = moved[i];
        cx->vars[i].shared = shared[i];
        cx->vars[i].exclusive = excl[i];
    }
}
