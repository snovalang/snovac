/* borrow.c — scope, moves, and statement-level borrow rules. */
#include "borrow_int.h"

#include <stdlib.h>
#include <string.h>

int sn_borrow_is_copy(const SnTypeRep *t) {
    if (!t) {
        return 1;
    }
    return t->tag != SN_T_ARRAY && t->tag != SN_T_FUNC && t->tag != SN_T_NAMED &&
           t->tag != SN_T_REF;
}

int sn_borrow_is_send(const SnTypeRep *t) {
    if (!t || t->tag != SN_T_NAMED || !t->decl || !t->decl->decl) {
        return 1;
    }
    SnDeclKind k = t->decl->decl->kind;
    return k != SN_DECL_CLASS && k != SN_DECL_STRUCT;
}

SnBorrowVar *sn_borrow_lookup(SnBorrowCx *cx, const char *name) {
    if (!name) {
        return NULL;
    }
    for (int i = cx->nv - 1; i >= 0; i--) {
        if (cx->vars[i].alive && cx->vars[i].name &&
            strcmp(cx->vars[i].name, name) == 0) {
            return &cx->vars[i];
        }
    }
    return NULL;
}

static void sn_borrow_release(SnBorrowCx *cx, int vi);

static int var_index(SnBorrowCx *cx, SnBorrowVar *v) {
    return v ? (int)(v - cx->vars) : -1;
}

int sn_borrow_define(SnBorrowCx *cx, const char *name, SnTypeRep *ty, int mut, SnExpr *init) {
    if (cx->nv >= SN_BORROW_MAX) {
        return -1;
    }
    SnBorrowVar *v = &cx->vars[cx->nv];
    memset(v, 0, sizeof(*v));
    v->name = name;
    v->ty = ty ? ty : (init ? init->resolved_type : NULL);
    v->init = init;
    v->depth = cx->depth;
    v->mut = mut;
    v->copyv = sn_borrow_is_copy(v->ty);
    v->len = -1;
    v->alive = 1;
    v->points = -1;
    v->nullable = sn_type_ref_nullable(v->ty);
    if (init && init->kind == SN_EXPR_ARRAY) {
        v->len = (int)init->args.len;
    }
    return cx->nv++;
}

void sn_borrow_pop_holds(SnBorrowCx *cx, int watermark) {
    while (cx->nh > watermark) {
        SnBorrowHold h = cx->holds[--cx->nh];
        if (h.vi < 0 || h.vi >= cx->nv) {
            continue;
        }
        SnBorrowVar *p = &cx->vars[h.vi];
        if (h.excl) {
            p->exclusive = 0;
        } else if (p->shared > 0) {
            p->shared--;
        }
    }
}

void sn_borrow_rebind_ref(SnBorrowCx *cx, const char *dst_name, const char *src_name, SnSpan span) {
    SnBorrowVar *dst = sn_borrow_lookup(cx, dst_name);
    if (dst && dst->points >= 0) {
        sn_borrow_release(cx, var_index(cx, dst));
    }
    int holder = dst ? var_index(cx, dst) : -1;
    sn_borrow_borrow_place(cx, sn_borrow_lookup(cx, src_name), holder, span);
}

void sn_borrow_fail(SnBorrowCx *cx, uint32_t code, SnSpan span, const char *msg) {
    sn_diag_emit(cx->ck->diag, SN_DIAG_ERROR, code, span, "%s", msg);
}

void sn_borrow_borrow_place(SnBorrowCx *cx, SnBorrowVar *place, int holder, SnSpan span) {
    if (!place || !place->alive) {
        sn_borrow_fail(cx, SNOVA_USE_AFTER_DROP, span, "use of a value that has already been dropped");
        return;
    }
    if (place->moved) {
        sn_borrow_fail(cx, SNOVA_USE_AFTER_MOVE, span, "use of a moved value");
        return;
    }
    if (place->exclusive || place->defer_hold) {
        sn_borrow_fail(cx, place->defer_hold ? SNOVA_DEFER_UNSOUND : SNOVA_EXCLUSIVE_BORROW, span,
                 place->defer_hold ? "defer still uses this value on function exit"
                                   : "second exclusive borrow of a mutable place");
        return;
    }
    int excl = place->mut && place->shared == 0;
    if (excl) {
        place->exclusive = 1;
    } else {
        place->shared++;
    }
    if (cx->defer_arm) {
        place->defer_hold = 1;
        return;
    }
    if (holder >= 0) {
        cx->vars[holder].points = var_index(cx, place);
        cx->vars[holder].hold_excl = excl;
        return;
    }
    if (cx->nh < SN_BORROW_MAX) {
        cx->holds[cx->nh].vi = var_index(cx, place);
        cx->holds[cx->nh].excl = excl;
        cx->nh++;
    }
}

static void sn_borrow_release(SnBorrowCx *cx, int vi) {
    SnBorrowVar *v = &cx->vars[vi];
    if (v->points < 0) {
        return;
    }
    SnBorrowVar *p = &cx->vars[v->points];
    if (v->hold_excl) {
        p->exclusive = 0;
    } else if (p->shared > 0) {
        p->shared--;
    }
    v->points = -1;
}

static void sn_borrow_end_scope(SnBorrowCx *cx, int depth, int strict) {
    for (int i = 0; i < cx->nv; i++) {
        SnBorrowVar *v = &cx->vars[i];
        if (!v->alive || v->depth != depth) {
            continue;
        }
        if (strict && v->defer_hold) {
            sn_borrow_fail(cx, SNOVA_DEFER_UNSOUND, v->init ? v->init->span : (SnSpan){0},
                     "defer uses a value that dies before function exit");
        }
        for (int j = 0; j < cx->nv; j++) {
            if (j != i && cx->vars[j].alive && cx->vars[j].points == i &&
                cx->vars[j].depth < depth) {
                sn_borrow_fail(cx, SNOVA_USE_AFTER_DROP, v->init ? v->init->span : (SnSpan){0},
                         "pointer outlives the value it borrows");
                cx->vars[j].points = -1;
            }
        }
        sn_borrow_release(cx, i);
        v->alive = 0;
    }
}

static void sn_borrow_finish_storage(SnBorrowCx *cx) {
    for (int i = 0; i < cx->nv; i++) {
        SnBorrowVar *v = &cx->vars[i];
        if (v->init) {
            v->init->storage = v->escapes ? 2 : 1;
        }
    }
}

void sn_borrow_stmt(SnBorrowCx *cx, SnStmt *s) {
    if (!s) {
        return;
    }
    int wm = cx->nh;
    switch (s->kind) {
    case SN_STMT_BLOCK:
        cx->depth++;
        for (size_t i = 0; i < s->stmts.len; i++) {
            sn_borrow_stmt(cx, (SnStmt *)s->stmts.items[i]);
        }
        sn_borrow_end_scope(cx, cx->depth, cx->depth > cx->fn_depth);
        cx->depth--;
        break;
    case SN_STMT_LET:
        sn_borrow_bind(cx, s->name, 0, s->expr);
        break;
    case SN_STMT_VAR:
        sn_borrow_bind(cx, s->name, 1, s->expr);
        break;
    case SN_STMT_RETURN:
        if (s->expr && s->expr->kind == SN_EXPR_UNARY && s->expr->op == SN_TOK_AMP &&
            s->expr->lhs && s->expr->lhs->kind == SN_EXPR_IDENT) {
            SnBorrowVar *p = sn_borrow_lookup(cx, s->expr->lhs->text);
            if (p) {
                p->escapes = 1;
            }
        } else if (s->expr && s->expr->kind == SN_EXPR_IDENT) {
            SnBorrowVar *p = sn_borrow_lookup(cx, s->expr->text);
            if (p && p->points >= 0) {
                cx->vars[p->points].escapes = 1;
            }
            sn_borrow_expr(cx, s->expr, (p && !p->copyv) ? SN_BORROW_USE_MOVE : SN_BORROW_USE_READ);
        } else {
            sn_borrow_expr(cx, s->expr, SN_BORROW_USE_READ);
        }
        break;
    case SN_STMT_IF:
        sn_borrow_if(cx, s);
        break;
    case SN_STMT_WHILE:
        sn_borrow_expr(cx, s->expr, SN_BORROW_USE_READ);
        sn_borrow_loop(cx, s->then_br);
        break;
    case SN_STMT_FOR:
        sn_borrow_expr(cx, s->expr, SN_BORROW_USE_READ);
        sn_borrow_loop(cx, s->then_br);
        break;
    case SN_STMT_DEFER:
        cx->defer_arm = 1;
        if (!s->expr || s->expr->kind != SN_EXPR_CALL) {
            sn_borrow_fail(cx, SNOVA_DEFER_UNSOUND, s->span, "defer requires a call");
        } else {
            sn_borrow_expr(cx, s->expr, SN_BORROW_USE_READ);
        }
        cx->defer_arm = 0;
        break;
    case SN_STMT_PULSAR:
        cx->task_kind = 1;
        sn_borrow_expr(cx, s->expr, SN_BORROW_USE_READ);
        sn_borrow_scan_stmt(cx, s->then_br);
        cx->task_kind = 0;
        break;
    case SN_STMT_EXPR:
        sn_borrow_expr(cx, s->expr, SN_BORROW_USE_READ);
        break;
    case SN_STMT_TRY:
        sn_borrow_stmt(cx, s->then_br);
        for (size_t i = 0; i < s->catches.len; i++) {
            sn_borrow_stmt(cx, (SnStmt *)s->catches.items[i]);
        }
        sn_borrow_stmt(cx, s->finally_br);
        break;
    default:
        sn_borrow_expr(cx, s->expr, SN_BORROW_USE_READ);
        sn_borrow_stmt(cx, s->then_br);
        sn_borrow_stmt(cx, s->else_br);
        break;
    }
    sn_borrow_pop_holds(cx, wm);
}

void sn_borrow_func(SnChecker *c, const SnDecl *decl) {
    if (!c || !decl || !decl->body) {
        return;
    }
    SnBorrowCx cx;
    memset(&cx, 0, sizeof(cx));
    cx.ck = c;
    for (size_t i = 0; i < decl->params.len; i++) {
        SnParam *p = (SnParam *)decl->params.items[i];
        SnTypeRep *pty = p->type ? sn_check_resolve_type(c, p->type) : NULL;
        sn_borrow_define(&cx, p->name, pty, 0, NULL);
    }
    if (decl->body->kind == SN_STMT_BLOCK) {
        cx.fn_depth = 1;
    }
    sn_borrow_stmt(&cx, decl->body);
    sn_borrow_finish_storage(&cx);
}
