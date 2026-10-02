/* borrow_int.h — shared state for the borrow checker modules. */
#ifndef SNOVAC_BORROW_INT_H
#define SNOVAC_BORROW_INT_H

#include "borrow.h"

#define OWN_MAX 96

enum { USE_READ = 0, USE_MOVE = 1, USE_MUT = 2, USE_BORROW = 3 };

typedef struct {
    const char *name;
    SnTypeRep *ty;
    SnExpr *init;
    int depth;
    int mut;
    int copyv;
    int moved;
    int shared;
    int exclusive;
    int len;
    int alive;
    int points;
    int hold_excl;
    int nullable;
    int nonnull;
    int escapes;
    int defer_hold;
} OwnVar;

typedef struct {
    int vi;
    int excl;
} OwnHold;

typedef struct {
    SnChecker *ck;
    OwnVar vars[OWN_MAX];
    int nv;
    int depth;
    int fn_depth;
    int defer_arm;
    int task_kind; /* 1 = pulsar, 2 = other task */
    OwnHold holds[OWN_MAX];
    int nh;
    const char *scan_locals[OWN_MAX];
    int nscan;
} OwnCx;

int own_is_copy(const SnTypeRep *t);
int own_is_send(const SnTypeRep *t);
OwnVar *own_lookup(OwnCx *cx, const char *name);
int own_define(OwnCx *cx, const char *name, SnTypeRep *ty, int mut, SnExpr *init);
void own_pop_holds(OwnCx *cx, int watermark);
void own_borrow_place(OwnCx *cx, OwnVar *place, int holder, SnSpan span);
void own_fail(OwnCx *cx, uint32_t code, SnSpan span, const char *msg);
void own_rebind_ref(OwnCx *cx, const char *dst_name, const char *src_name, SnSpan span);
void own_bind(OwnCx *cx, const char *name, int mut, SnExpr *init);
void own_if(OwnCx *cx, SnStmt *s);
void own_loop(OwnCx *cx, SnStmt *body);
void own_expr(OwnCx *cx, SnExpr *e, int mode);
void own_stmt(OwnCx *cx, SnStmt *s);
void own_scan_stmt(OwnCx *cx, SnStmt *s);
void own_note_task_expr(OwnCx *cx, SnExpr *e);

#endif
