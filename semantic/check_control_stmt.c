#include "check_internal.h"

#include <string.h>

typedef struct {
    int found_move_error;
    int code;
    SnSpan span;
    char message[1024];
} LoopMoveProbe;

typedef struct {
    int falls_through;
    int reaches_next_iteration;
} LoopBodyFlow;

static LoopBodyFlow loop_body_flow(const SnStmt *stmt) {
    if (!stmt) return (LoopBodyFlow){1, 1};
    switch (stmt->kind) {
    case SN_STMT_BREAK:
    case SN_STMT_RETURN:
    case SN_STMT_THROW:
        return (LoopBodyFlow){0, 0};
    case SN_STMT_CONTINUE:
        return (LoopBodyFlow){0, 1};
    case SN_STMT_BLOCK: {
        int falls_through = 1;
        int reaches_next_iteration = 0;
        for (size_t i = 0; i < stmt->stmts.len && falls_through; ++i) {
            LoopBodyFlow next = loop_body_flow(SN_LIST_AT(stmt->stmts, SnStmt, i));
            reaches_next_iteration |= next.reaches_next_iteration;
            falls_through = next.falls_through;
        }
        return (LoopBodyFlow){falls_through,
                              reaches_next_iteration || falls_through};
    }
    case SN_STMT_IF: {
        LoopBodyFlow then_flow = loop_body_flow(stmt->then_br);
        LoopBodyFlow else_flow = stmt->else_br
                                     ? loop_body_flow(stmt->else_br)
                                     : (LoopBodyFlow){1, 1};
        return (LoopBodyFlow){then_flow.falls_through || else_flow.falls_through,
                              then_flow.reaches_next_iteration ||
                                  else_flow.reaches_next_iteration};
    }
    case SN_STMT_MATCH: {
        /* Without a proof of exhaustiveness, preserve the no-arm path. */
        int falls_through = 1;
        int reaches_next_iteration = 1;
        for (size_t i = 0; i < stmt->arms.len; ++i) {
            const SnMatchArm *arm = SN_LIST_AT(stmt->arms, SnMatchArm, i);
            LoopBodyFlow arm_flow = arm->body
                                        ? loop_body_flow(arm->body)
                                        : (LoopBodyFlow){1, 1};
            falls_through |= arm_flow.falls_through;
            reaches_next_iteration |= arm_flow.reaches_next_iteration;
        }
        return (LoopBodyFlow){falls_through, reaches_next_iteration};
    }
    case SN_STMT_TRY:
        /* Conservatively account for normal flow through try/catch/finally. */
        return (LoopBodyFlow){1, 1};
    default:
        return (LoopBodyFlow){1, 1};
    }
}

static void capture_loop_move_diagnostic(void *ctx, SnDiagLevel level, int code,
                                         SnSpan span, const char *message,
                                         const char *file_path) {
    (void)file_path;
    LoopMoveProbe *probe = (LoopMoveProbe *)ctx;
    if (level != SN_DIAG_ERROR ||
        (code != SNOVA_USE_AFTER_MOVE && code != SNOVA_INVALID_MOVE) ||
        probe->found_move_error) return;
    probe->found_move_error = 1;
    probe->code = code;
    probe->span = span;
    snprintf(probe->message, sizeof(probe->message), "%s", message);
}

/* Analyze one possible loop backedge using the move state produced by the
 * first pass. The normal pass remains responsible for diagnostics; this probe
 * only reports a new move error reachable on a later iteration. */
static void check_loop_backedge(SnChecker *c, SnScope *scope, SnExpr *condition,
                                SnStmt *body, SnSpan span,
                                SnMoveSnapshot *moves) {
    if (!loop_body_flow(body).reaches_next_iteration) return;
    int changed = 0;
    for (size_t i = 0; i < moves->count; ++i) {
        if (moves->symbols[i]->is_moved != moves->before[i]) {
            changed = 1;
            break;
        }
    }
    if (!changed) return;

    uint8_t *backedge_state = (uint8_t *)sn_arena_alloc(c->arena, moves->count);
    for (size_t i = 0; i < moves->count; ++i)
        backedge_state[i] = moves->symbols[i]->is_moved;

    FILE *discard = tmpfile();
    if (!discard) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_INVALID_MOVE, span,
                     "cannot verify ownership safety across loop iterations");
        return;
    }

    SnDiagSink *outer_diag = c->diag;
    SnDiagSink probe_diag;
    sn_diag_init(&probe_diag, outer_diag->file.path, outer_diag->file.src,
                 outer_diag->file.src_len);
    probe_diag.file = outer_diag->file;
    probe_diag.out = discard;
    LoopMoveProbe probe = {0};
    sn_diag_set_callback(&probe_diag, capture_loop_move_diagnostic, &probe);

    for (size_t i = 0; i < moves->count; ++i)
        moves->symbols[i]->is_moved = backedge_state[i] != 0;
    c->diag = &probe_diag;
    if (condition) (void)sn_check_expr(c, scope, condition);
    c->loop_depth++;
    if (body) sn_check_stmt(c, scope, body);
    c->loop_depth--;
    c->diag = outer_diag;

    /* A failed probe cannot make an unsafe loop acceptable. */
    if (probe.found_move_error) {
        sn_diag_emit(outer_diag, SN_DIAG_ERROR, probe.code, probe.span, "%s",
                     probe.message);
    } else if (probe_diag.error_count > 0) {
        sn_diag_emit(outer_diag, SN_DIAG_ERROR, SNOVA_INVALID_MOVE, span,
                     "cannot prove ownership safety across loop iterations");
    }

    sn_check_move_snapshot_accumulate(moves);
    sn_check_move_snapshot_apply(moves);
    fclose(discard);
}

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
        SnMoveSnapshot *moves = sn_check_move_snapshot_new(c, local);
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
        sn_check_move_snapshot_accumulate(moves);
        if (s->then_br) {
            c->loop_depth++;
            sn_check_stmt(c, local, s->then_br);
            c->loop_depth--;
        }
        sn_check_move_snapshot_accumulate(moves);
        check_loop_backedge(c, local, s->expr, s->then_br, s->span, moves);
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
        SnSymbol *loop_symbol = NULL;
        if (s->name) {
            loop_symbol = sn_scope_define(&body_scope,
                                          sn_intern_cstr(c->intern, s->name),
                                          SN_SYM_LOCAL, NULL, s->span);
            if (loop_symbol)
                loop_symbol->value_type = elem_ty ? elem_ty : sn_type_error(c->types);
        }
        SnMoveSnapshot *moves = sn_check_move_snapshot_new(c, local);
        if (s->then_br) {
            c->loop_depth++;
            sn_check_stmt(c, &body_scope, s->then_br);
            c->loop_depth--;
        }
        sn_check_move_snapshot_accumulate(moves);
        /* The loop binding receives a fresh element on every iteration. */
        if (loop_symbol) loop_symbol->is_moved = 0;
        check_loop_backedge(c, &body_scope, NULL, s->then_br, s->span, moves);
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
