#include "check_internal.h"

#include <string.h>

int sn_check_control_stmt(SnChecker *c, SnScope *local, SnStmt *s) {
    switch (s->kind) {
    case SN_STMT_IF: {
        if (s->expr) {
            SnTypeRep *cond = sn_check_expr(c, local, s->expr);
            if (!sn_check_is_error(cond) && !sn_type_is_any(cond) && cond->tag != SN_T_BOOL)
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_CONDITION_NOT_BOOL, s->expr->span,
                             "condition must be `bool`");
        }
        SnMoveSnapshot *moves = sn_check_move_snapshot_new(c, local);
        if (s->then_br) sn_check_stmt(c, local, s->then_br);
        if (!sn_check_stmt_always_returns(s->then_br))
            sn_check_move_snapshot_accumulate(moves);
        sn_check_move_snapshot_restore(moves);
        if (s->else_br) {
            sn_check_stmt(c, local, s->else_br);
            if (!sn_check_stmt_always_returns(s->else_br))
                sn_check_move_snapshot_accumulate(moves);
        } else {
            sn_check_move_snapshot_accumulate(moves);
        }
        sn_check_move_snapshot_apply(moves);
        return 1;
    }
    case SN_STMT_WHILE: {
        int is_always_true = 0;
        if (s->expr) {
            SnTypeRep *cond = sn_check_expr(c, local, s->expr);
            if (!sn_check_is_error(cond) && !sn_type_is_any(cond) && cond->tag != SN_T_BOOL)
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_CONDITION_NOT_BOOL, s->expr->span,
                             "condition must be `bool`");
            if (s->expr->kind == SN_EXPR_BOOL && s->expr->text &&
                strcmp(s->expr->text, "true") == 0) is_always_true = 1;
        }
        if (sn_check_stmt_is_empty(s->then_br)) {
            sn_diag_emit(c->diag, SN_DIAG_WARNING, SNOVA_EMPTY_LOOP, s->span,
                         "empty `while` loop body detected (potential busy-wait/dead code)");
        } else if (is_always_true && !sn_check_stmt_can_exit_loop(s->then_br)) {
            sn_diag_emit(c->diag, SN_DIAG_WARNING, SNOVA_INFINITE_LOOP, s->span,
                         "infinite `while (true)` loop has no `break`, `return`, or `throw` -- potential unintended busy hang");
        }
        SnMoveSnapshot *moves = sn_check_move_snapshot_new(c, local);
        if (s->then_br) {
            c->loop_depth++;
            sn_check_stmt(c, local, s->then_br);
            c->loop_depth--;
        }
        sn_check_move_snapshot_accumulate(moves);
        sn_check_move_snapshot_apply(moves);
        return 1;
    }
    case SN_STMT_FOR: {
        SnTypeRep *iter_ty = s->expr ? sn_check_expr(c, local, s->expr) : NULL;
        SnTypeRep *elem_ty = NULL;
        if (iter_ty && iter_ty->tag == SN_T_ARRAY && iter_ty->nargs == 1)
            elem_ty = iter_ty->args[0];
        else if (iter_ty && iter_ty->tag == SN_T_NAMED && iter_ty->nargs > 0 &&
                 iter_ty->decl && iter_ty->decl->name &&
                 (strcmp(iter_ty->decl->name, "Iterable") == 0 ||
                  strcmp(iter_ty->decl->name, "Array") == 0 ||
                  strcmp(iter_ty->decl->name, "List") == 0 ||
                  strcmp(iter_ty->decl->name, "Channel") == 0))
            elem_ty = iter_ty->args[iter_ty->nargs - 1];
        if (s->type) {
            SnTypeRep *declared_elem = sn_check_resolve_type(c, s->type);
            if (!sn_check_is_error(declared_elem) && sn_check_type_contains_reference(declared_elem))
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW, s->span,
                             "borrow and raw pointer types cannot be stored in loop bindings");
            if (elem_ty && sn_check_types_clash(c, elem_ty, declared_elem))
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_LET_TYPE_MISMATCH, s->span,
                             "loop binding type does not match iterable element type");
            elem_ty = declared_elem;
        }
        SnScope body_scope;
        sn_scope_init(&body_scope, c->arena, local);
        if (s->name) {
            SnSymbol *sym = sn_scope_define(&body_scope, sn_intern_cstr(c->intern, s->name),
                                            SN_SYM_LOCAL, NULL, s->span);
            if (sym) sym->value_type = elem_ty ? elem_ty : sn_type_error(c->types);
        }
        SnMoveSnapshot *moves = sn_check_move_snapshot_new(c, local);
        if (s->then_br) {
            c->loop_depth++;
            sn_check_stmt(c, &body_scope, s->then_br);
            c->loop_depth--;
        }
        sn_check_move_snapshot_accumulate(moves);
        sn_check_move_snapshot_apply(moves);
        return 1;
    }
    case SN_STMT_TRY: {
        SnMoveSnapshot *moves = sn_check_move_snapshot_new(c, local);
        if (s->then_br) sn_check_stmt(c, local, s->then_br);
        if (!sn_check_stmt_always_returns(s->then_br))
            sn_check_move_snapshot_accumulate(moves);
        uint8_t *try_state = NULL;
        if (moves->count) {
            try_state = (uint8_t *)sn_arena_alloc(c->arena, moves->count);
            for (size_t i = 0; i < moves->count; ++i)
                try_state[i] = moves->symbols[i]->is_moved;
        }
        for (size_t i = 0; i < s->catches.len; ++i) {
            for (size_t j = 0; j < moves->count; ++j)
                moves->symbols[j]->is_moved = try_state[j];
            SnStmt *catch_body = SN_LIST_AT(s->catches, SnStmt, i);
            sn_check_stmt(c, local, catch_body);
            if (!sn_check_stmt_always_returns(catch_body))
                sn_check_move_snapshot_accumulate(moves);
        }
        sn_check_move_snapshot_apply(moves);
        if (s->finally_br) sn_check_stmt(c, local, s->finally_br);
        return 1;
    }
    case SN_STMT_MATCH: {
        SnTypeRep *target_ty = s->expr ? sn_check_expr(c, local, s->expr) : NULL;
        SnMoveSnapshot *moves = sn_check_move_snapshot_new(c, local);
        int exhaustive = sn_check_match_exhaustiveness(c, target_ty, &s->arms, s->span);
        for (size_t i = 0; i < s->arms.len; ++i) {
            sn_check_move_snapshot_restore(moves);
            SnMatchArm *arm = SN_LIST_AT(s->arms, SnMatchArm, i);
            SnScope arm_scope;
            sn_scope_init(&arm_scope, c->arena, local);
            sn_check_bind_pattern_names(c, &arm_scope, arm->pattern, target_ty,
                                        sn_check_expr_has_borrowed_origin(s->expr));
            if (arm->guard) sn_check_expr(c, &arm_scope, arm->guard);
            if (arm->value) sn_check_expr(c, &arm_scope, arm->value);
            if (arm->body) sn_check_stmt(c, &arm_scope, arm->body);
            if (!arm->body || !sn_check_stmt_always_returns(arm->body))
                sn_check_move_snapshot_accumulate(moves);
        }
        if (!exhaustive) {
            sn_check_move_snapshot_restore(moves);
            sn_check_move_snapshot_accumulate(moves);
        }
        sn_check_move_snapshot_apply(moves);
        return 1;
    }
    case SN_STMT_PULSAR:
        if (c->in_async_body)
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PULSAR_IN_ASYNC, s->span,
                         "pulsar statements and functions cannot be used inside an async function");
        if (s->expr) {
            c->in_pulsar_launch = 1;
            sn_check_expr(c, local, s->expr);
            c->in_pulsar_launch = 0;
        }
        return 1;
    default:
        return 0;
    }
}
