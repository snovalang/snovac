#include "check.h"
#include "check_internal.h"

#include <string.h>

#include "builtins.h"

/* ── operators ────────────────────────────────────────────────────────────── */

static int is_comparison_op(SnTokKind op) {
    switch (op) {
    case SN_TOK_LT: case SN_TOK_GT: case SN_TOK_LE: case SN_TOK_GE:
    case SN_TOK_EQ: case SN_TOK_NE:
        return 1;
    default:
        return 0;
    }
}

SnTypeRep *check_binary(SnChecker *c, SnExpr *e, SnTypeRep *lt, SnTypeRep *rt) {
    if (sn_check_is_error(lt) || sn_check_is_error(rt)) {
        return sn_type_error(c->types); /* plan.md §5.1: silent propagation */
    }
    if (sn_type_is_any(lt) || sn_type_is_any(rt)) {
        /* types.h: `any` never takes part in a mismatch, in either direction.
         * A comparison still yields bool; everything else stays `any` rather
         * than guessing which operand's type wins. */
        return is_comparison_op(e->op) ? sn_type_bool(c->types) : sn_type_any(c->types);
    }
    switch (e->op) {
    case SN_TOK_PLUS:
        if (lt->tag == SN_T_STRING && rt->tag == SN_T_STRING) {
            return sn_type_string(c->types);
        }
        /* fallthrough */
    case SN_TOK_MINUS:
    case SN_TOK_STAR:
    case SN_TOK_SLASH:
    case SN_TOK_PERCENT:
    case SN_TOK_AMP:
    case SN_TOK_PIPE:
    case SN_TOK_CARET:
    case SN_TOK_SHL:
    case SN_TOK_SHR:
        if (is_numeric(lt) && lt == rt) {
            return lt;
        }
        break;
    case SN_TOK_LT:
    case SN_TOK_GT:
    case SN_TOK_LE:
    case SN_TOK_GE:
        if (is_numeric(lt) && lt == rt) {
            return sn_type_bool(c->types);
        }
        break;
    case SN_TOK_EQ:
    case SN_TOK_NE:
        if (lt == rt) {
            return sn_type_bool(c->types);
        }
        break;
    case SN_TOK_ANDAND:
    case SN_TOK_OROR:
        if (lt->tag == SN_T_BOOL && rt->tag == SN_T_BOOL) {
            return sn_type_bool(c->types);
        }
        break;
    case SN_TOK_QQ: {
        int is_opt = (lt->tag == SN_T_NAMED && lt->decl &&
                      strcmp(lt->decl->name, "Option") == 0 && lt->nargs == 1);
        if (!is_opt) {
            sn_diag_emit(c->diag, SN_DIAG_WARNING, SNOVA_REDUNDANT_NULL_COALESCING, e->span,
                         "redundant null-coalescing operator `??` applied to non-optional type");
            return lt;
        }
        SnTypeRep *inner = lt->args[0];
        if (rt != inner) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_MISMATCHED_FALLBACK, e->span,
                         "null-coalescing fallback type mismatch: expected inner type of Option");
            return sn_type_error(c->types);
        }
        return inner;
    }
    default:
        break;
    }
    sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_BINARY_TYPE_MISMATCH, e->span,
                "operator `%s` cannot be applied to these operand types "
                "(no implicit numeric promotion -- operands must match exactly)",
                sn_tok_name(e->op));
    return sn_type_error(c->types);
}

SnTypeRep *check_unary(SnChecker *c, SnExpr *e, SnTypeRep *operand) {
    if (sn_check_is_error(operand)) {
        return sn_type_error(c->types);
    }
    if (sn_type_is_any(operand)) {
        return operand; /* types.h: `any` never takes part in a mismatch */
    }

    if (e->op == SN_TOK_QUESTION) {
        /* PROVISIONAL — see check.h file header. plan.md §7 item 6 was left
         * unratified by resolve.c; real corpus usage is essentially absent. */
        if (operand->tag == SN_T_NAMED && operand->decl && operand->nargs >= 1 &&
            (operand->decl->name == sn_intern_cstr(c->intern, "Option") ||
             operand->decl->name == sn_intern_cstr(c->intern, "Result"))) {
            return operand->args[0];
        }
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNARY_TYPE_MISMATCH, e->span,
                    "`?` requires an Option or Result operand");
        return sn_type_error(c->types);
    }

    switch (e->op) {
    case SN_TOK_BANG:
        if (operand->tag == SN_T_BOOL) {
            return operand;
        }
        break;
    case SN_TOK_MINUS:
    case SN_TOK_PLUS:
        if (is_numeric(operand)) {
            return operand;
        }
        break;
    case SN_TOK_TILDE:
        if (operand->tag == SN_T_INT || operand->tag == SN_T_LONG) {
            return operand;
        }
        break;
    default:
        break;
    }
    sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNARY_TYPE_MISMATCH, e->span,
                "invalid operand type for unary `%s`", sn_tok_name(e->op));
    return sn_type_error(c->types);
}

