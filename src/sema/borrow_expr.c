/* borrow_expr.c — uses, moves, and borrows inside expressions. */
#include "borrow_int.h"

#include <stdlib.h>
#include <string.h>

static void sn_borrow_place_use(SnBorrowCx *cx, SnBorrowVar *v, int mode, SnSpan span) {
    if (!v) {
        return;
    }
    if (!v->alive) {
        sn_borrow_fail(cx, SNOVA_USE_AFTER_DROP, span, "use of a dropped value");
        return;
    }
    if (v->moved) {
        sn_borrow_fail(cx, SNOVA_USE_AFTER_MOVE, span, "use of a moved value");
        return;
    }
    if (mode == SN_BORROW_USE_MUT && (v->shared || v->exclusive || v->defer_hold)) {
        sn_borrow_fail(cx, v->defer_hold ? SNOVA_DEFER_UNSOUND : SNOVA_MUT_WHILE_SHARED, span,
                 v->defer_hold ? "defer still uses this value on function exit"
                               : "mutation while the value is borrowed");
        return;
    }
    if (mode == SN_BORROW_USE_MOVE && !v->copyv) {
        if (v->shared || v->exclusive || v->defer_hold) {
            sn_borrow_fail(cx, v->defer_hold ? SNOVA_DEFER_UNSOUND : SNOVA_MUT_WHILE_SHARED, span,
                     "moving a value that is still borrowed");
            return;
        }
        v->moved = 1;
    }
}

static int through_holder(SnBorrowCx *cx, SnExpr *e, int place) {
    if (!e) {
        return 0;
    }
    if (e->kind == SN_EXPR_UNARY && e->op == SN_TOK_STAR && e->lhs &&
        e->lhs->kind == SN_EXPR_IDENT) {
        SnBorrowVar *h = sn_borrow_lookup(cx, e->lhs->text);
        return h && h->points == place && h->hold_excl;
    }
    if (e->kind == SN_EXPR_INDEX || e->kind == SN_EXPR_MEMBER) {
        return through_holder(cx, e->lhs, place);
    }
    return 0;
}

static void sn_borrow_mutate(SnBorrowCx *cx, SnExpr *lhs) {
    if (!lhs) {
        return;
    }
    if (lhs->kind == SN_EXPR_IDENT) {
        SnBorrowVar *v = sn_borrow_lookup(cx, lhs->text);
        sn_borrow_place_use(cx, v, SN_BORROW_USE_MUT, lhs->span);
        return;
    }
    if ((lhs->kind == SN_EXPR_INDEX || lhs->kind == SN_EXPR_MEMBER) && lhs->lhs &&
        lhs->lhs->kind == SN_EXPR_IDENT) {
        SnBorrowVar *v = sn_borrow_lookup(cx, lhs->lhs->text);
        if (v && v->points >= 0 && through_holder(cx, lhs, v->points)) {
            return;
        }
        if (v && v->points >= 0) {
            sn_borrow_place_use(cx, &cx->vars[v->points], SN_BORROW_USE_MUT, lhs->span);
            return;
        }
        sn_borrow_place_use(cx, v, SN_BORROW_USE_MUT, lhs->span);
        return;
    }
    sn_borrow_expr(cx, lhs, SN_BORROW_USE_READ);
}

static void sn_borrow_bounds(SnBorrowCx *cx, SnExpr *e) {
    if (!e || e->kind != SN_EXPR_INDEX || !e->rhs || e->rhs->kind != SN_EXPR_INT ||
        !e->lhs || e->lhs->kind != SN_EXPR_IDENT || !e->rhs->text) {
        return;
    }
    SnBorrowVar *v = sn_borrow_lookup(cx, e->lhs->text);
    int len = -1;
    if (v && v->points >= 0) {
        len = cx->vars[v->points].len;
    } else if (v) {
        len = v->len;
    }
    if (len < 0) {
        return;
    }
    long long idx = atoll(e->rhs->text);
    if (idx < 0 || idx >= len) {
        sn_borrow_fail(cx, SNOVA_INDEX_OOB, e->span, "index is outside the fat pointer length");
        return;
    }
    e->bounds_proven = 1;
}

static int task_name(const char *name) {
    if (!name) {
        return 0;
    }
    return strcmp(name, "spawn") == 0 || strcmp(name, "go") == 0 ||
           strcmp(name, "submit") == 0 || strcmp(name, "forEach") == 0 ||
           strcmp(name, "map") == 0;
}

static int mutating_method(const char *name) {
    if (!name) {
        return 0;
    }
    return strcmp(name, "set") == 0 || strcmp(name, "push") == 0 ||
           strcmp(name, "pop") == 0 || strcmp(name, "clear") == 0 ||
           strcmp(name, "add") == 0 || strcmp(name, "insert") == 0 ||
           strcmp(name, "remove") == 0 || strcmp(name, "send") == 0 ||
           strcmp(name, "close") == 0 || strcmp(name, "increment") == 0;
}

void sn_borrow_expr(SnBorrowCx *cx, SnExpr *e, int mode) {
    if (!e) {
        return;
    }
    if (e->adjust == 2 && e->kind == SN_EXPR_IDENT) {
        sn_borrow_borrow_place(cx, sn_borrow_lookup(cx, e->text), -1, e->span);
        return;
    }
    switch (e->kind) {
    case SN_EXPR_IDENT:
        sn_borrow_place_use(cx, sn_borrow_lookup(cx, e->text), mode, e->span);
        break;
    case SN_EXPR_UNARY:
        if (e->op == SN_TOK_AMP && e->lhs && e->lhs->kind == SN_EXPR_IDENT) {
            sn_borrow_borrow_place(cx, sn_borrow_lookup(cx, e->lhs->text), -1, e->span);
            break;
        }
        if (e->op == SN_TOK_STAR && e->lhs && e->lhs->kind == SN_EXPR_IDENT) {
            SnBorrowVar *v = sn_borrow_lookup(cx, e->lhs->text);
            sn_borrow_place_use(cx, v, SN_BORROW_USE_READ, e->span);
            if (v && v->nullable && !v->nonnull) {
                sn_borrow_fail(cx, SNOVA_NULL_DEREF, e->span,
                         "dereference of a nullable pointer without a null check");
            }
            break;
        }
        sn_borrow_expr(cx, e->lhs, SN_BORROW_USE_READ);
        break;
    case SN_EXPR_INDEX:
        sn_borrow_expr(cx, e->lhs, SN_BORROW_USE_READ);
        sn_borrow_expr(cx, e->rhs, SN_BORROW_USE_READ);
        sn_borrow_bounds(cx, e);
        break;
    case SN_EXPR_ASSIGN:
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT && e->rhs &&
            e->rhs->kind == SN_EXPR_UNARY && e->rhs->op == SN_TOK_AMP &&
            e->rhs->lhs && e->rhs->lhs->kind == SN_EXPR_IDENT) {
            sn_borrow_rebind_ref(cx, e->lhs->text, e->rhs->lhs->text, e->span);
            break;
        }
        if (e->rhs && e->rhs->resolved_type && !sn_borrow_is_copy(e->rhs->resolved_type) &&
            e->rhs->kind == SN_EXPR_IDENT) {
            sn_borrow_expr(cx, e->rhs, SN_BORROW_USE_MOVE);
        } else {
            sn_borrow_expr(cx, e->rhs, SN_BORROW_USE_READ);
        }
        sn_borrow_mutate(cx, e->lhs);
        break;
    case SN_EXPR_CALL: {
        int wm = cx->nh;
        const char *nm = NULL;
        if (e->lhs && (e->lhs->kind == SN_EXPR_IDENT || e->lhs->kind == SN_EXPR_MEMBER)) {
            nm = e->lhs->text;
        }
        if (e->lhs && e->lhs->kind == SN_EXPR_MEMBER && mutating_method(nm) &&
            e->lhs->lhs && e->lhs->lhs->kind == SN_EXPR_IDENT) {
            sn_borrow_place_use(cx, sn_borrow_lookup(cx, e->lhs->lhs->text), SN_BORROW_USE_MUT, e->span);
        } else {
            sn_borrow_expr(cx, e->lhs, SN_BORROW_USE_READ);
        }
        int is_task = task_name(nm);
        for (size_t i = 0; i < e->args.len; i++) {
            SnExpr *arg = (SnExpr *)e->args.items[i];
            if (!(arg && arg->kind == SN_EXPR_LAMBDA && is_task)) {
                sn_borrow_expr(cx, arg, SN_BORROW_USE_READ);
            }
        }
        if (is_task) {
            sn_borrow_note_task_expr(cx, e);
        }
        if (!cx->defer_arm) {
            sn_borrow_pop_holds(cx, wm);
        }
        break;
    }
    case SN_EXPR_MEMBER:
        sn_borrow_expr(cx, e->lhs, SN_BORROW_USE_READ);
        break;
    case SN_EXPR_BINARY:
        sn_borrow_expr(cx, e->lhs, SN_BORROW_USE_READ);
        sn_borrow_expr(cx, e->rhs, SN_BORROW_USE_READ);
        break;
    case SN_EXPR_ARRAY:
        for (size_t i = 0; i < e->args.len; i++) {
            sn_borrow_expr(cx, (SnExpr *)e->args.items[i], SN_BORROW_USE_READ);
        }
        break;
    default:
        sn_borrow_expr(cx, e->lhs, SN_BORROW_USE_READ);
        sn_borrow_expr(cx, e->rhs, SN_BORROW_USE_READ);
        sn_borrow_expr(cx, e->value, SN_BORROW_USE_READ);
        break;
    }
}
