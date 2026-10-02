/* check_ptr.c — `&T` / `*expr` typing and call-site pointer adjustments. */
#include "check_ptr.h"

#include <string.h>

SnTypeRep *sn_check_resolve_ref(SnChecker *c, const SnType *t) {
    SnTypeRep *inner = sn_check_resolve_type(c, t->pointee);
    return sn_type_ref(c->types, inner, t->is_nullable);
}

int sn_check_ptr_unary(SnChecker *c, SnExpr *e, SnTypeRep *operand,
                       SnTypeRep **out) {
    if (!e || (e->op != SN_TOK_AMP && e->op != SN_TOK_STAR)) {
        return 0;
    }
    if (!operand || operand->tag == SN_T_ERROR) {
        *out = sn_type_error(c->types);
        return 1;
    }
    if (e->op == SN_TOK_AMP) {
        *out = sn_type_ref(c->types, operand, 0);
        return 1;
    }
    if (!sn_type_is_ref(operand) || !sn_type_pointee(operand)) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNARY_TYPE_MISMATCH, e->span,
                     "cannot dereference a non-pointer");
        *out = sn_type_error(c->types);
        return 1;
    }
    /* Nullable `&T?` still has a pointee type. The borrow pass rejects `*`
     * unless a null check has already narrowed it. */
    *out = sn_type_pointee(operand);
    return 1;
}

SnTypeRep *sn_ptr_peel(SnExpr *e, SnTypeRep *ty) {
    if (sn_type_is_ref(ty) && !sn_type_ref_nullable(ty) && sn_type_pointee(ty)) {
        if (e) {
            e->adjust = 1;
        }
        return sn_type_pointee(ty);
    }
    return ty;
}

static int null_lit(const SnTypeRep *t) {
    return sn_type_is_ref(t) && sn_type_ref_nullable(t) && !t->ret;
}

SnTypeRep *sn_ptr_adapt_arg(SnChecker *c, SnExpr *arg, SnTypeRep *actual,
                            SnTypeRep *expected) {
    (void)c;
    if (!actual || !expected || actual->tag == SN_T_ERROR ||
        expected->tag == SN_T_ERROR || sn_type_is_any(actual) ||
        sn_type_is_any(expected) || actual == expected) {
        return actual;
    }
    if (null_lit(actual) && sn_type_ref_nullable(expected)) {
        if (arg) {
            arg->resolved_type = expected;
        }
        return expected;
    }
    if (sn_type_is_ref(expected) && sn_type_pointee(expected) == actual) {
        if (arg) {
            arg->adjust = 2;
        }
        return expected;
    }
    if (sn_type_is_ref(actual) && !sn_type_ref_nullable(actual) &&
        sn_type_pointee(actual) == expected) {
        if (arg) {
            arg->adjust = 1;
        }
        return expected;
    }
    /* A non-null `&T` inhabits `&T?`. */
    if (sn_type_is_ref(expected) && sn_type_is_ref(actual) &&
        sn_type_ref_nullable(expected) && sn_type_pointee(expected) &&
        sn_type_pointee(expected) == sn_type_pointee(actual)) {
        return expected;
    }
    return actual;
}

int sn_ptr_null_cmp(const SnTypeRep *lt, const SnTypeRep *rt) {
    if (null_lit(lt) && sn_type_ref_nullable(rt)) {
        return 1;
    }
    if (null_lit(rt) && sn_type_ref_nullable(lt)) {
        return 1;
    }
    return 0;
}

int sn_rt_probe_name(const char *name) {
    if (!name) {
        return 0;
    }
    return strcmp(name, "heapAllocs") == 0 || strcmp(name, "heapLive") == 0 ||
           strcmp(name, "stackAllocs") == 0 || strcmp(name, "boundsChecks") == 0;
}
