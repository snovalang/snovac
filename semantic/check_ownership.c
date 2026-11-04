#include "check.h"
#include "check_internal.h"

#include <string.h>

int sn_check_type_is_owned(const SnTypeRep *type) {
    return type && (type->tag == SN_T_STRING || type->tag == SN_T_ARRAY ||
                    type->tag == SN_T_NAMED);
}

static SnExpr *sn_check_stmt_tail_expression(SnStmt *stmt) {
    if (!stmt) return NULL;
    if (stmt->kind == SN_STMT_EXPR) return stmt->expr;
    if (stmt->kind == SN_STMT_BLOCK && stmt->stmts.len) {
        return sn_check_stmt_tail_expression(
            SN_LIST_AT(stmt->stmts, SnStmt, stmt->stmts.len - 1u));
    }
    return NULL;
}

static int sn_check_stmt_expr_has_borrowed_origin(const SnStmt *stmt) {
    if (!stmt) return 0;
    if (stmt->kind == SN_STMT_EXPR) return sn_check_expr_has_borrowed_origin(stmt->expr);
    if (stmt->kind == SN_STMT_BLOCK && stmt->stmts.len) {
        return sn_check_stmt_expr_has_borrowed_origin(
            SN_LIST_AT(stmt->stmts, SnStmt, stmt->stmts.len - 1u));
    }
    if (stmt->kind == SN_STMT_RETURN) return sn_check_expr_has_borrowed_origin(stmt->expr);
    return 0;
}

int sn_check_expr_has_borrowed_origin(const SnExpr *expr) {
    if (!expr) return 0;
    if (expr->kind == SN_EXPR_IDENT) {
        return expr->resolved_symbol && expr->resolved_symbol->is_borrowed;
    }
    switch (expr->kind) {
    case SN_EXPR_ASSIGN:
    case SN_EXPR_CAST:
    case SN_EXPR_UNARY:
    case SN_EXPR_MOVE:
        return sn_check_expr_has_borrowed_origin(expr->rhs ? expr->rhs : expr->lhs);
    case SN_EXPR_IF:
        return sn_check_expr_has_borrowed_origin(expr->rhs) ||
               sn_check_expr_has_borrowed_origin(expr->value) ||
               sn_check_stmt_expr_has_borrowed_origin(expr->body) ||
               sn_check_stmt_expr_has_borrowed_origin(expr->else_body);
    case SN_EXPR_MATCH:
        for (size_t i = 0; i < expr->arms.len; ++i) {
            const SnMatchArm *arm = SN_LIST_AT(expr->arms, SnMatchArm, i);
            if (sn_check_expr_has_borrowed_origin(arm->value) ||
                sn_check_stmt_expr_has_borrowed_origin(arm->body)) return 1;
        }
        return 0;
    case SN_EXPR_BINARY:
        if (expr->op == SN_TOK_QUESTION && expr->rhs &&
            expr->rhs->kind == SN_EXPR_BINARY && expr->rhs->op == SN_TOK_COLON) {
            return sn_check_expr_has_borrowed_origin(expr->rhs->lhs) ||
                   sn_check_expr_has_borrowed_origin(expr->rhs->rhs);
        }
        /* Other binary forms produce scalars or a fresh concatenated string. */
        return 0;
    case SN_EXPR_CALL:
        if (expr->lhs && expr->lhs->kind == SN_EXPR_MEMBER &&
            sn_check_expr_has_borrowed_origin(expr->lhs->lhs)) return 1;
        for (size_t i = 0; i < expr->args.len; ++i) {
            if (sn_check_expr_has_borrowed_origin(SN_LIST_AT(expr->args, SnExpr, i))) return 1;
        }
        return 0;
    case SN_EXPR_STRUCT_LIT:
    case SN_EXPR_ARRAY:
        for (size_t i = 0; i < expr->args.len; ++i) {
            if (sn_check_expr_has_borrowed_origin(SN_LIST_AT(expr->args, SnExpr, i))) return 1;
        }
        return 0;
    case SN_EXPR_MEMBER:
    case SN_EXPR_INDEX:
        return sn_check_expr_has_borrowed_origin(expr->lhs);
    default:
        /* Literals, scalar operations and lambda bodies do not alias a borrow. */
        return 0;
    }
}

void sn_check_mark_expr_consumed(SnChecker *c, SnExpr *expr, SnTypeRep *type) {
    if (!expr || !sn_check_type_is_owned(type)) return;
    if (sn_check_expr_has_borrowed_origin(expr)) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW, expr->span,
                     "borrowed value cannot escape into an owning value");
        return;
    }
    if (expr->kind == SN_EXPR_IF) {
        SnExpr *then_value = sn_check_stmt_tail_expression(expr->body);
        if (then_value)
            sn_check_mark_expr_consumed(c, then_value,
                                       then_value->resolved_type ? then_value->resolved_type : type);
        if (expr->rhs) {
            sn_check_mark_expr_consumed(c, expr->rhs,
                                       expr->rhs->resolved_type ? expr->rhs->resolved_type : type);
        } else {
            SnExpr *else_value = sn_check_stmt_tail_expression(expr->else_body);
            if (else_value)
                sn_check_mark_expr_consumed(c, else_value,
                                           else_value->resolved_type ? else_value->resolved_type : type);
        }
        return;
    }
    if (expr->kind == SN_EXPR_MATCH) {
        for (size_t i = 0; i < expr->arms.len; ++i) {
            SnMatchArm *arm = SN_LIST_AT(expr->arms, SnMatchArm, i);
            SnExpr *arm_value = arm->value ? arm->value :
                                sn_check_stmt_tail_expression(arm->body);
            if (arm_value)
                sn_check_mark_expr_consumed(c, arm_value,
                                           arm_value->resolved_type ? arm_value->resolved_type : type);
        }
        return;
    }
    if (expr->kind == SN_EXPR_BINARY && expr->op == SN_TOK_QUESTION &&
        expr->rhs && expr->rhs->kind == SN_EXPR_BINARY &&
        expr->rhs->op == SN_TOK_COLON) {
        SnExpr *then_value = expr->rhs->lhs;
        SnExpr *else_value = expr->rhs->rhs;
        sn_check_mark_expr_consumed(c, then_value,
                                   then_value && then_value->resolved_type ? then_value->resolved_type : type);
        sn_check_mark_expr_consumed(c, else_value,
                                   else_value && else_value->resolved_type ? else_value->resolved_type : type);
        return;
    }
    if (expr->kind != SN_EXPR_IDENT) return;
    SnSymbol *source = expr->resolved_symbol;
    if (!source || (source->kind != SN_SYM_LOCAL && source->kind != SN_SYM_PARAM) ||
        source->is_moved) return;
    if (source->is_captured) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_INVALID_MOVE, expr->span,
                     "`%s` is captured by a closure and cannot be moved into an owned value",
                     source->name);
        return;
    }
    source->is_moved = 1;
    expr->implicit_move = 1;
}

SnTypeRep *sn_check_expr_consume(SnChecker *c, SnScope *local, SnExpr *expr) {
    SnTypeRep *type = sn_check_expr(c, local, expr);
    sn_check_mark_expr_consumed(c, expr, type);
    return type;
}
