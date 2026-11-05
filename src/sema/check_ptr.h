/* check_ptr.h — pointer types, auto-deref, and auto-reborrow. */
#ifndef SNOVAC_CHECK_PTR_H
#define SNOVAC_CHECK_PTR_H

#include "check.h"

SnTypeRep *sn_check_resolve_ref(SnChecker *c, const SnType *t);

/* Returns 1 when `e` is `&` or `*`. `*out` is the type of that operator. */
int sn_check_ptr_unary(SnChecker *c, SnExpr *e, SnTypeRep *operand,
                       SnTypeRep **out);

/* Non-null `&T` peels to T and marks `e` for auto-deref. Nullable refs stay. */
SnTypeRep *sn_ptr_peel(SnExpr *e, SnTypeRep *ty);

/* Makes `actual` line up with `expected` by deref or reborrow when that is
 * the obvious pointer conversion. Records the choice on `arg->adjust`. */
SnTypeRep *sn_ptr_adapt_arg(SnChecker *c, SnExpr *arg, SnTypeRep *actual,
                            SnTypeRep *expected);

int sn_rt_probe_name(const char *name);
int sn_ptr_null_cmp(const SnTypeRep *lt, const SnTypeRep *rt);

#endif
