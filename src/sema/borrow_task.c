/* borrow_task.c — defer/task capture: Send, mutable share, borrowed sends. */
#include "borrow_int.h"

#include <string.h>

static int shadowed(SnBorrowCx *cx, const char *name) {
    for (int i = 0; i < cx->nscan; i++) {
        if (cx->scan_locals[i] && name && strcmp(cx->scan_locals[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void sn_borrow_hit(SnBorrowCx *cx, const char *name, SnSpan span, int writing) {
    if (!name || shadowed(cx, name)) {
        return;
    }
    SnBorrowVar *v = sn_borrow_lookup(cx, name);
    if (!v || !v->alive) {
        return;
    }
    v->escapes = 1;
    if (v->exclusive || v->shared || v->defer_hold) {
        sn_diag_emit(cx->ck->diag, SN_DIAG_ERROR, SNOVA_SEND_WHILE_BORROWED, span,
                     "cannot send `%s` to a task while it is borrowed", name);
        return;
    }
    if (!sn_borrow_is_send(v->ty)) {
        const char *tn = (v->ty && v->ty->decl && v->ty->decl->name) ? v->ty->decl->name
                                                                     : "value";
        sn_diag_emit(cx->ck->diag, SN_DIAG_ERROR, SNOVA_PULSAR_NON_SEND_CAPTURE, span,
                     "Type `%s` is not `Send` and cannot cross a pulsar boundary.", tn);
        return;
    }
    if (writing && v->mut) {
        if (cx->task_kind == 1) {
            sn_diag_emit(cx->ck->diag, SN_DIAG_ERROR, SNOVA_PULSAR_MUTABLE_CAPTURE, span,
                         "Mutable variable `%s` cannot be captured by a pulsar without a Sync primitive.",
                         name);
        } else {
            sn_diag_emit(cx->ck->diag, SN_DIAG_ERROR, SNOVA_SEND_WHILE_BORROWED, span,
                         "mutable value `%s` shared with another task", name);
        }
        return;
    }
    if (v->mut && !v->copyv) {
        sn_diag_emit(cx->ck->diag, SN_DIAG_ERROR, SNOVA_SEND_WHILE_BORROWED, span,
                     "mutable value `%s` shared with another task", name);
    }
}

static void sn_borrow_scan_expr(SnBorrowCx *cx, SnExpr *e);

static void sn_borrow_add_local(SnBorrowCx *cx, const char *name) {
    if (name && cx->nscan < SN_BORROW_MAX) {
        cx->scan_locals[cx->nscan++] = name;
    }
}

static void sn_borrow_scan_expr(SnBorrowCx *cx, SnExpr *e) {
    if (!e) {
        return;
    }
    if (e->kind == SN_EXPR_IDENT) {
        sn_borrow_hit(cx, e->text, e->span, 0);
        return;
    }
    if (e->kind == SN_EXPR_ASSIGN) {
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT) {
            sn_borrow_hit(cx, e->lhs->text, e->span, 1);
        }
        sn_borrow_scan_expr(cx, e->rhs);
        return;
    }
    if (e->kind == SN_EXPR_LAMBDA) {
        int saved = cx->nscan;
        for (size_t i = 0; i < e->params.len; i++) {
            SnParam *p = (SnParam *)e->params.items[i];
            sn_borrow_add_local(cx, p->name);
        }
        sn_borrow_scan_expr(cx, e->value);
        sn_borrow_scan_stmt(cx, e->body);
        cx->nscan = saved;
        return;
    }
    sn_borrow_scan_expr(cx, e->lhs);
    sn_borrow_scan_expr(cx, e->rhs);
    sn_borrow_scan_expr(cx, e->value);
    for (size_t i = 0; i < e->args.len; i++) {
        sn_borrow_scan_expr(cx, (SnExpr *)e->args.items[i]);
    }
}

void sn_borrow_scan_stmt(SnBorrowCx *cx, SnStmt *s) {
    if (!s) {
        return;
    }
    switch (s->kind) {
    case SN_STMT_BLOCK: {
        int saved = cx->nscan;
        for (size_t i = 0; i < s->stmts.len; i++) {
            sn_borrow_scan_stmt(cx, (SnStmt *)s->stmts.items[i]);
        }
        cx->nscan = saved;
        break;
    }
    case SN_STMT_LET:
    case SN_STMT_VAR:
        sn_borrow_scan_expr(cx, s->expr);
        sn_borrow_add_local(cx, s->name);
        break;
    case SN_STMT_EXPR:
    case SN_STMT_RETURN:
        sn_borrow_scan_expr(cx, s->expr);
        break;
    case SN_STMT_IF:
        sn_borrow_scan_expr(cx, s->expr);
        sn_borrow_scan_stmt(cx, s->then_br);
        sn_borrow_scan_stmt(cx, s->else_br);
        break;
    case SN_STMT_WHILE:
    case SN_STMT_FOR:
        sn_borrow_scan_expr(cx, s->expr);
        sn_borrow_scan_stmt(cx, s->then_br);
        break;
    default:
        sn_borrow_scan_expr(cx, s->expr);
        sn_borrow_scan_stmt(cx, s->then_br);
        sn_borrow_scan_stmt(cx, s->else_br);
        break;
    }
}

static int is_task_call(const SnExpr *e) {
    if (!e || e->kind != SN_EXPR_CALL || !e->lhs) {
        return 0;
    }
    const char *nm = NULL;
    if (e->lhs->kind == SN_EXPR_IDENT || e->lhs->kind == SN_EXPR_MEMBER) {
        nm = e->lhs->text;
    }
    return nm && (strcmp(nm, "spawn") == 0 || strcmp(nm, "go") == 0 ||
                  strcmp(nm, "submit") == 0 || strcmp(nm, "forEach") == 0 ||
                  strcmp(nm, "map") == 0);
}

void sn_borrow_note_task_expr(SnBorrowCx *cx, SnExpr *e) {
    if (!is_task_call(e)) {
        return;
    }
    int prev = cx->task_kind;
    if (!prev) {
        cx->task_kind = 2;
    }
    for (size_t i = 0; i < e->args.len; i++) {
        SnExpr *arg = (SnExpr *)e->args.items[i];
        if (arg && arg->kind == SN_EXPR_LAMBDA) {
            sn_borrow_scan_expr(cx, arg);
        }
    }
    cx->task_kind = prev;
}
