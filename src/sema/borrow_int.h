/* borrow_int.h — shared state for the borrow checker modules. */
#ifndef SNOVAC_BORROW_INT_H
#define SNOVAC_BORROW_INT_H

#include "borrow.h"

#define SN_BORROW_MAX 96

enum { SN_BORROW_USE_READ = 0, SN_BORROW_USE_MOVE = 1, SN_BORROW_USE_MUT = 2, SN_BORROW_USE_BORROW = 3 };

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
} SnBorrowVar;

typedef struct {
    int vi;
    int excl;
} SnBorrowHold;

typedef struct {
    SnChecker *ck;
    SnBorrowVar vars[SN_BORROW_MAX];
    int nv;
    int depth;
    int fn_depth;
    int defer_arm;
    int task_kind; /* 1 = pulsar, 2 = other task */
    SnBorrowHold holds[SN_BORROW_MAX];
    int nh;
    const char *scan_locals[SN_BORROW_MAX];
    int nscan;
} SnBorrowCx;

int sn_borrow_is_copy(const SnTypeRep *t);
int sn_borrow_is_send(const SnTypeRep *t);
SnBorrowVar *sn_borrow_lookup(SnBorrowCx *cx, const char *name);
int sn_borrow_define(SnBorrowCx *cx, const char *name, SnTypeRep *ty, int mut, SnExpr *init);
void sn_borrow_pop_holds(SnBorrowCx *cx, int watermark);
void sn_borrow_borrow_place(SnBorrowCx *cx, SnBorrowVar *place, int holder, SnSpan span);
void sn_borrow_fail(SnBorrowCx *cx, uint32_t code, SnSpan span, const char *msg);
void sn_borrow_rebind_ref(SnBorrowCx *cx, const char *dst_name, const char *src_name, SnSpan span);
void sn_borrow_bind(SnBorrowCx *cx, const char *name, int mut, SnExpr *init);
void sn_borrow_if(SnBorrowCx *cx, SnStmt *s);
void sn_borrow_loop(SnBorrowCx *cx, SnStmt *body);
void sn_borrow_expr(SnBorrowCx *cx, SnExpr *e, int mode);
void sn_borrow_stmt(SnBorrowCx *cx, SnStmt *s);
void sn_borrow_scan_stmt(SnBorrowCx *cx, SnStmt *s);
void sn_borrow_note_task_expr(SnBorrowCx *cx, SnExpr *e);

#endif
