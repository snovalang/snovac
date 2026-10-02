/* borrow.h — ownership and borrow checking over a checked function. */
#ifndef SNOVAC_BORROW_H
#define SNOVAC_BORROW_H

#include "check.h"

#define SNOVA_PULSAR_MUTABLE_CAPTURE 125
#define SNOVA_PULSAR_NON_SEND_CAPTURE 126
#define SNOVA_USE_AFTER_MOVE      240
#define SNOVA_USE_AFTER_DROP      241
#define SNOVA_EXCLUSIVE_BORROW    242
#define SNOVA_MUT_WHILE_SHARED    243
#define SNOVA_SEND_WHILE_BORROWED 244
#define SNOVA_NULL_DEREF          245
#define SNOVA_INDEX_OOB           246
#define SNOVA_DEFER_UNSOUND       247

void sn_borrow_func(SnChecker *c, const SnDecl *decl);

#endif
