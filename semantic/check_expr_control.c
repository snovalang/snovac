#include "check.h"
#include "check_internal.h"

#include <string.h>

#include "builtins.h"

SnTypeRep *check_expr_control(SnChecker *c, SnScope *local, SnExpr *e) {
    SnTypeRep *result = sn_type_error(c->types);
    switch (e->kind) {
    case SN_EXPR_IF: {
        if (e->lhs) {
            SnTypeRep *condition_ty = sn_check_expr(c, local, e->lhs);
            if (!sn_check_is_error(condition_ty) && !sn_type_is_any(condition_ty) &&
                condition_ty->tag != SN_T_BOOL)
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_CONDITION_NOT_BOOL,
                             e->lhs->span, "condition must be `bool`");
        }
        SnMoveSnapshot *moves = sn_check_move_snapshot_new(c, local);
        SnTypeRep *then_ty = sn_type_unit(c->types);
        if (e->body) {
            sn_check_stmt(c, local, e->body);
            if (e->body->kind == SN_STMT_BLOCK && e->body->stmts.len) {
                SnStmt *last = SN_LIST_AT(e->body->stmts, SnStmt, e->body->stmts.len - 1);
                if (last->kind == SN_STMT_EXPR && last->expr)
                    then_ty = last->expr->resolved_type ? last->expr->resolved_type
                                                       : sn_check_expr(c, local, last->expr);
            }
        }
        if (!sn_check_stmt_always_returns(e->body))
            sn_check_move_snapshot_accumulate(moves);
        sn_check_move_snapshot_restore(moves);
        if (e->rhs) then_ty = sn_check_expr(c, local, e->rhs);
        SnTypeRep *else_ty = sn_type_unit(c->types);
        if (e->else_body) {
            sn_check_stmt(c, local, e->else_body);
            if (e->else_body->kind == SN_STMT_BLOCK && e->else_body->stmts.len) {
                SnStmt *last = SN_LIST_AT(e->else_body->stmts, SnStmt, e->else_body->stmts.len - 1);
                if (last->kind == SN_STMT_EXPR && last->expr)
                    else_ty = last->expr->resolved_type ? last->expr->resolved_type
                                                       : sn_check_expr(c, local, last->expr);
            }
        }
        if (e->value) else_ty = sn_check_expr(c, local, e->value);
        else if (e->rhs && !e->else_body) else_ty = then_ty;
        if (!e->else_body || !sn_check_stmt_always_returns(e->else_body))
            sn_check_move_snapshot_accumulate(moves);
        sn_check_move_snapshot_apply(moves);
        if (sn_check_is_error(then_ty)) result = else_ty;
        else if (sn_check_is_error(else_ty)) result = then_ty;
        else if (!sn_check_types_clash(c, then_ty, else_ty)) result = then_ty;
        else if (!sn_check_types_clash(c, else_ty, then_ty)) result = else_ty;
        else {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, e->span,
                         "`if` expression branches have incompatible types");
            result = sn_type_error(c->types);
        }
        break;
    }
    case SN_EXPR_MATCH: {
        SnTypeRep *target_ty = e->lhs ? sn_check_expr(c, local, e->lhs) : NULL;
        SnMoveSnapshot *moves = sn_check_move_snapshot_new(c, local);
        sn_check_match_exhaustiveness(c, target_ty, &e->arms, e->span);
        SnTypeRep *inferred = NULL;
        for (size_t i = 0; i < e->arms.len; i++) {
            sn_check_move_snapshot_restore(moves);
            SnMatchArm *arm = SN_LIST_AT(e->arms, SnMatchArm, i);
            SnScope arm_scope;
            sn_scope_init(&arm_scope, c->arena, local);
            sn_check_bind_pattern_names(c, &arm_scope, arm->pattern, target_ty,
                                        sn_check_expr_has_borrowed_origin(e->lhs));
            if (arm->guard) {
                sn_check_expr(c, &arm_scope, arm->guard);
            }
            SnTypeRep *arm_ty = NULL;
            if (arm->value) {
                arm_ty = sn_check_expr(c, &arm_scope, arm->value);
            }
            if (arm->body) {
                sn_check_stmt(c, &arm_scope, arm->body);
                if (arm->body->kind == SN_STMT_RETURN && arm->body->expr)
                    arm_ty = arm->body->expr->resolved_type ? arm->body->expr->resolved_type
                                                           : sn_check_expr(c, &arm_scope, arm->body->expr);
                else if (arm->body->kind == SN_STMT_BLOCK && arm->body->stmts.len) {
                    SnStmt *last = SN_LIST_AT(arm->body->stmts, SnStmt,
                                              arm->body->stmts.len - 1);
                    if (last->kind == SN_STMT_RETURN && last->expr)
                        arm_ty = last->expr->resolved_type ? last->expr->resolved_type
                                                          : sn_check_expr(c, &arm_scope, last->expr);
                    else if (last->kind == SN_STMT_EXPR && last->expr)
                        arm_ty = last->expr->resolved_type ? last->expr->resolved_type
                                                          : sn_check_expr(c, &arm_scope, last->expr);
                }
            }
            if (!arm->body || !sn_check_stmt_always_returns(arm->body))
                sn_check_move_snapshot_accumulate(moves);
            if (arm_ty && !sn_check_is_error(arm_ty)) {
                if (!inferred) inferred = arm_ty;
                else if (sn_check_types_clash(c, arm_ty, inferred) &&
                         sn_check_types_clash(c, inferred, arm_ty)) {
                    sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH,
                                 arm->span,
                                 "match arms have incompatible result types");
                    inferred = sn_type_error(c->types);
                } else if (!sn_check_types_clash(c, arm_ty, inferred)) {
                    inferred = arm_ty;
                }
            }
        }
        sn_check_move_snapshot_apply(moves);
        result = inferred ? inferred : sn_type_unit(c->types);
        break;
    }
    case SN_EXPR_STRUCT_LIT: {
        SnSymbol *sym = NULL;
        if (e->lhs && e->lhs->text) {
            sym = find_type_symbol_anywhere(c, c->enclosing_type, e->lhs->text);
        }
        if (sym && sym->kind == SN_SYM_TYPE) {
            result = sn_type_named(c->types, sym, NULL, 0);
            const SnDecl *td = sym->decl;
            if (td) {
                for (size_t i = 0; i < e->field_names.len; i++) {
                    const char *fname = SN_LIST_AT(e->field_names, const char, i);
                    SnExpr *arg = SN_LIST_AT(e->args, SnExpr, i);
                    SnTypeRep *arg_ty = sn_check_expr_consume(c, local, arg);
                    SnSymbol *fsym = lookup_member_with_inheritance(c, td, sn_intern_cstr(c->intern, fname));
                    if (!fsym) {
                        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNKNOWN_MEMBER, arg->span,
                                     "type `%s` has no field named `%s`", td->name, fname);
                    } else {
                        SnTypeRep *decl_ty = symbol_type(c, fsym);
                        if (decl_ty && sn_check_types_clash(c, decl_ty, arg_ty)) {
                            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, arg->span,
                                         "cannot assign incompatible value to field `%s`", fname);
                        }
                    }
                }
            }
        } else {
            result = sn_type_error(c->types);
        }
        break;
    }

    default:
        break;
    }
    return result;
}

