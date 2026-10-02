/* borrow_task.c — defer/task capture: Send, mutable share, borrowed sends. */
#include "borrow_int.h"

#include <string.h>

static int shadowed(OwnCx *cx, const char *name) {
    for (int i = 0; i < cx->nscan; i++) {
        if (cx->scan_locals[i] && name && strcmp(cx->scan_locals[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void own_hit(OwnCx *cx, const char *name, SnSpan span, int writing) {
    if (!name || shadowed(cx, name)) {
        return;
    }
    OwnVar *v = own_lookup(cx, name);
    if (!v || !v->alive) {
        return;
    }
    v->escapes = 1;
    if (v->exclusive || v->shared || v->defer_hold) {
        sn_diag_emit(cx->ck->diag, SN_DIAG_ERROR, SNOVA_SEND_WHILE_BORROWED, span,
                     "cannot send `%s` to a task while it is borrowed", name);
        return;
    }
    if (!own_is_send(v->ty)) {
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

static void own_scan_expr(OwnCx *cx, SnExpr *e);

static void own_add_local(OwnCx *cx, const char *name) {
    if (name && cx->nscan < OWN_MAX) {
        cx->scan_locals[cx->nscan++] = name;
    }
}

static void own_scan_expr(OwnCx *cx, SnExpr *e) {
    if (!e) {
        return;
    }
    if (e->kind == SN_EXPR_IDENT) {
        own_hit(cx, e->text, e->span, 0);
        return;
    }
    if (e->kind == SN_EXPR_ASSIGN) {
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT) {
            own_hit(cx, e->lhs->text, e->span, 1);
        }
        own_scan_expr(cx, e->rhs);
        return;
    }
    if (e->kind == SN_EXPR_LAMBDA) {
        int saved = cx->nscan;
        for (size_t i = 0; i < e->params.len; i++) {
            SnParam *p = (SnParam *)e->params.items[i];
            own_add_local(cx, p->name);
        }
        own_scan_expr(cx, e->value);
        own_scan_stmt(cx, e->body);
        cx->nscan = saved;
        return;
    }
    own_scan_expr(cx, e->lhs);
    own_scan_expr(cx, e->rhs);
    own_scan_expr(cx, e->value);
    for (size_t i = 0; i < e->args.len; i++) {
        own_scan_expr(cx, (SnExpr *)e->args.items[i]);
    }
}

void own_scan_stmt(OwnCx *cx, SnStmt *s) {
    if (!s) {
        return;
    }
    switch (s->kind) {
    case SN_STMT_BLOCK: {
        int saved = cx->nscan;
        for (size_t i = 0; i < s->stmts.len; i++) {
            own_scan_stmt(cx, (SnStmt *)s->stmts.items[i]);
        }
        cx->nscan = saved;
        break;
    }
    case SN_STMT_LET:
    case SN_STMT_VAR:
        own_scan_expr(cx, s->expr);
        own_add_local(cx, s->name);
        break;
    case SN_STMT_EXPR:
    case SN_STMT_RETURN:
        own_scan_expr(cx, s->expr);
        break;
    case SN_STMT_IF:
        own_scan_expr(cx, s->expr);
        own_scan_stmt(cx, s->then_br);
        own_scan_stmt(cx, s->else_br);
        break;
    case SN_STMT_WHILE:
    case SN_STMT_FOR:
        own_scan_expr(cx, s->expr);
        own_scan_stmt(cx, s->then_br);
        break;
    default:
        own_scan_expr(cx, s->expr);
        own_scan_stmt(cx, s->then_br);
        own_scan_stmt(cx, s->else_br);
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

void own_note_task_expr(OwnCx *cx, SnExpr *e) {
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
            own_scan_expr(cx, arg);
        }
    }
    cx->task_kind = prev;
}
