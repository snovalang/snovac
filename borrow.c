/* borrow.c — scope, moves, and statement-level borrow rules. */
#include "borrow_int.h"

#include <stdlib.h>
#include <string.h>

int own_is_copy(const SnTypeRep *t) {
    if (!t) {
        return 1;
    }
    return t->tag != SN_T_ARRAY && t->tag != SN_T_FUNC && t->tag != SN_T_NAMED &&
           t->tag != SN_T_REF;
}

int own_is_send(const SnTypeRep *t) {
    if (!t || t->tag != SN_T_NAMED || !t->decl || !t->decl->decl) {
        return 1;
    }
    SnDeclKind k = t->decl->decl->kind;
    return k != SN_DECL_CLASS && k != SN_DECL_STRUCT;
}

OwnVar *own_lookup(OwnCx *cx, const char *name) {
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

static void own_release(OwnCx *cx, int vi);

static int var_index(OwnCx *cx, OwnVar *v) {
    return v ? (int)(v - cx->vars) : -1;
}

int own_define(OwnCx *cx, const char *name, SnTypeRep *ty, int mut, SnExpr *init) {
    if (cx->nv >= OWN_MAX) {
        return -1;
    }
    OwnVar *v = &cx->vars[cx->nv];
    memset(v, 0, sizeof(*v));
    v->name = name;
    v->ty = ty ? ty : (init ? init->resolved_type : NULL);
    v->init = init;
    v->depth = cx->depth;
    v->mut = mut;
    v->copyv = own_is_copy(v->ty);
    v->len = -1;
    v->alive = 1;
    v->points = -1;
    v->nullable = sn_type_ref_nullable(v->ty);
    if (init && init->kind == SN_EXPR_ARRAY) {
        v->len = (int)init->args.len;
    }
    return cx->nv++;
}

void own_pop_holds(OwnCx *cx, int watermark) {
    while (cx->nh > watermark) {
        OwnHold h = cx->holds[--cx->nh];
        if (h.vi < 0 || h.vi >= cx->nv) {
            continue;
        }
        OwnVar *p = &cx->vars[h.vi];
        if (h.excl) {
            p->exclusive = 0;
        } else if (p->shared > 0) {
            p->shared--;
        }
    }
}

void own_rebind_ref(OwnCx *cx, const char *dst_name, const char *src_name, SnSpan span) {
    OwnVar *dst = own_lookup(cx, dst_name);
    if (dst && dst->points >= 0) {
        own_release(cx, var_index(cx, dst));
    }
    int holder = dst ? var_index(cx, dst) : -1;
    own_borrow_place(cx, own_lookup(cx, src_name), holder, span);
}

void own_fail(OwnCx *cx, uint32_t code, SnSpan span, const char *msg) {
    sn_diag_emit(cx->ck->diag, SN_DIAG_ERROR, code, span, "%s", msg);
}

void own_borrow_place(OwnCx *cx, OwnVar *place, int holder, SnSpan span) {
    if (!place || !place->alive) {
        own_fail(cx, SNOVA_USE_AFTER_DROP, span, "use of a value that has already been dropped");
        return;
    }
    if (place->moved) {
        own_fail(cx, SNOVA_USE_AFTER_MOVE, span, "use of a moved value");
        return;
    }
    if (place->exclusive || place->defer_hold) {
        own_fail(cx, place->defer_hold ? SNOVA_DEFER_UNSOUND : SNOVA_EXCLUSIVE_BORROW, span,
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
    if (cx->nh < OWN_MAX) {
        cx->holds[cx->nh].vi = var_index(cx, place);
        cx->holds[cx->nh].excl = excl;
        cx->nh++;
    }
}

static void own_release(OwnCx *cx, int vi) {
    OwnVar *v = &cx->vars[vi];
    if (v->points < 0) {
        return;
    }
    OwnVar *p = &cx->vars[v->points];
    if (v->hold_excl) {
        p->exclusive = 0;
    } else if (p->shared > 0) {
        p->shared--;
    }
    v->points = -1;
}

static void own_end_scope(OwnCx *cx, int depth, int strict) {
    for (int i = 0; i < cx->nv; i++) {
        OwnVar *v = &cx->vars[i];
        if (!v->alive || v->depth != depth) {
            continue;
        }
        if (strict && v->defer_hold) {
            own_fail(cx, SNOVA_DEFER_UNSOUND, v->init ? v->init->span : (SnSpan){0},
                     "defer uses a value that dies before function exit");
        }
        for (int j = 0; j < cx->nv; j++) {
            if (j != i && cx->vars[j].alive && cx->vars[j].points == i &&
                cx->vars[j].depth < depth) {
                own_fail(cx, SNOVA_USE_AFTER_DROP, v->init ? v->init->span : (SnSpan){0},
                         "pointer outlives the value it borrows");
                cx->vars[j].points = -1;
            }
        }
        own_release(cx, i);
        v->alive = 0;
    }
}

static void own_finish_storage(OwnCx *cx) {
    for (int i = 0; i < cx->nv; i++) {
        OwnVar *v = &cx->vars[i];
        if (v->init) {
            v->init->storage = v->escapes ? 2 : 1;
        }
    }
}

void own_stmt(OwnCx *cx, SnStmt *s) {
    if (!s) {
        return;
    }
    int wm = cx->nh;
    switch (s->kind) {
    case SN_STMT_BLOCK:
        cx->depth++;
        for (size_t i = 0; i < s->stmts.len; i++) {
            own_stmt(cx, (SnStmt *)s->stmts.items[i]);
        }
        own_end_scope(cx, cx->depth, cx->depth > cx->fn_depth);
        cx->depth--;
        break;
    case SN_STMT_LET:
        own_bind(cx, s->name, 0, s->expr);
        break;
    case SN_STMT_VAR:
        own_bind(cx, s->name, 1, s->expr);
        break;
    case SN_STMT_RETURN:
        if (s->expr && s->expr->kind == SN_EXPR_UNARY && s->expr->op == SN_TOK_AMP &&
            s->expr->lhs && s->expr->lhs->kind == SN_EXPR_IDENT) {
            OwnVar *p = own_lookup(cx, s->expr->lhs->text);
            if (p) {
                p->escapes = 1;
            }
        } else if (s->expr && s->expr->kind == SN_EXPR_IDENT) {
            OwnVar *p = own_lookup(cx, s->expr->text);
            if (p && p->points >= 0) {
                cx->vars[p->points].escapes = 1;
            }
            own_expr(cx, s->expr, (p && !p->copyv) ? USE_MOVE : USE_READ);
        } else {
            own_expr(cx, s->expr, USE_READ);
        }
        break;
    case SN_STMT_IF:
        own_if(cx, s);
        break;
    case SN_STMT_WHILE:
        own_expr(cx, s->expr, USE_READ);
        own_loop(cx, s->then_br);
        break;
    case SN_STMT_FOR:
        own_expr(cx, s->expr, USE_READ);
        own_loop(cx, s->then_br);
        break;
    case SN_STMT_DEFER:
        cx->defer_arm = 1;
        if (!s->expr || s->expr->kind != SN_EXPR_CALL) {
            own_fail(cx, SNOVA_DEFER_UNSOUND, s->span, "defer requires a call");
        } else {
            own_expr(cx, s->expr, USE_READ);
        }
        cx->defer_arm = 0;
        break;
    case SN_STMT_PULSAR:
        cx->task_kind = 1;
        own_expr(cx, s->expr, USE_READ);
        own_scan_stmt(cx, s->then_br);
        cx->task_kind = 0;
        break;
    case SN_STMT_EXPR:
        own_expr(cx, s->expr, USE_READ);
        break;
    case SN_STMT_TRY:
        own_stmt(cx, s->then_br);
        for (size_t i = 0; i < s->catches.len; i++) {
            own_stmt(cx, (SnStmt *)s->catches.items[i]);
        }
        own_stmt(cx, s->finally_br);
        break;
    default:
        own_expr(cx, s->expr, USE_READ);
        own_stmt(cx, s->then_br);
        own_stmt(cx, s->else_br);
        break;
    }
    own_pop_holds(cx, wm);
}

void sn_borrow_func(SnChecker *c, const SnDecl *decl) {
    if (!c || !decl || !decl->body) {
        return;
    }
    OwnCx cx;
    memset(&cx, 0, sizeof(cx));
    cx.ck = c;
    for (size_t i = 0; i < decl->params.len; i++) {
        SnParam *p = (SnParam *)decl->params.items[i];
        SnTypeRep *pty = p->type ? sn_check_resolve_type(c, p->type) : NULL;
        own_define(&cx, p->name, pty, 0, NULL);
    }
    if (decl->body->kind == SN_STMT_BLOCK) {
        cx.fn_depth = 1;
    }
    own_stmt(&cx, decl->body);
    own_finish_storage(&cx);
}
