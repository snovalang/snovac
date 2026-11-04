#include "check_internal.h"

int sn_check_stmt_always_returns(const SnStmt *s) {
    if (!s) return 0;
    switch (s->kind) {
    case SN_STMT_RETURN:
    case SN_STMT_THROW:
        return 1;
    case SN_STMT_BLOCK:
        return s->stmts.len > 0 && sn_check_stmt_always_returns(
            SN_LIST_AT(s->stmts, SnStmt, s->stmts.len - 1u));
    case SN_STMT_IF:
        return s->else_br && sn_check_stmt_always_returns(s->then_br) &&
               sn_check_stmt_always_returns(s->else_br);
    case SN_STMT_MATCH:
        if (!s->arms.len) return 0;
        for (size_t i = 0; i < s->arms.len; ++i) {
            const SnMatchArm *arm = SN_LIST_AT(s->arms, SnMatchArm, i);
            if (!arm->body || !sn_check_stmt_always_returns(arm->body)) return 0;
        }
        return 1;
    default:
        return 0;
    }
}

SnMoveSnapshot *sn_check_move_snapshot_new(SnChecker *c, SnScope *scope) {
    SnMoveSnapshot *snap = (SnMoveSnapshot *)sn_arena_calloc(c->arena, sizeof(*snap));
    for (SnScope *s = scope; s; s = s->parent) snap->count += s->count;
    if (!snap->count) return snap;
    snap->symbols = (SnSymbol **)sn_arena_alloc(c->arena,
                                                snap->count * sizeof(*snap->symbols));
    snap->before = (uint8_t *)sn_arena_alloc(c->arena, snap->count);
    snap->any_moved = (uint8_t *)sn_arena_alloc(c->arena, snap->count);
    size_t at = 0;
    for (SnScope *s = scope; s; s = s->parent) {
        for (size_t i = 0; i < s->nbuckets; ++i) {
            for (SnSymbol *sym = s->buckets[i]; sym; sym = sym->next) {
                snap->symbols[at] = sym;
                snap->before[at] = sym->is_moved;
                snap->any_moved[at] = sym->is_moved;
                at++;
            }
        }
    }
    snap->count = at;
    return snap;
}

void sn_check_move_snapshot_restore(const SnMoveSnapshot *snap) {
    for (size_t i = 0; i < snap->count; ++i)
        snap->symbols[i]->is_moved = snap->before[i];
}

void sn_check_move_snapshot_accumulate(SnMoveSnapshot *snap) {
    for (size_t i = 0; i < snap->count; ++i)
        snap->any_moved[i] |= snap->symbols[i]->is_moved;
}

void sn_check_move_snapshot_apply(const SnMoveSnapshot *snap) {
    for (size_t i = 0; i < snap->count; ++i)
        snap->symbols[i]->is_moved = snap->any_moved[i];
}

/* ── loop analysis ───────────────────────────────────────────────────────── */

int sn_check_stmt_can_exit_loop(const SnStmt *s) {
    if (!s) return 0;
    switch (s->kind) {
    case SN_STMT_BREAK:
    case SN_STMT_RETURN:
    case SN_STMT_THROW:
        return 1;
    case SN_STMT_BLOCK:
        for (size_t i = 0; i < s->stmts.len; i++) {
            if (sn_check_stmt_can_exit_loop(SN_LIST_AT(s->stmts, SnStmt, i))) return 1;
        }
        return 0;
    case SN_STMT_IF:
        if (sn_check_stmt_can_exit_loop(s->then_br)) return 1;
        if (s->else_br && sn_check_stmt_can_exit_loop(s->else_br)) return 1;
        return 0;
    case SN_STMT_TRY:
        if (sn_check_stmt_can_exit_loop(s->then_br)) return 1;
        for (size_t i = 0; i < s->catches.len; i++) {
            if (sn_check_stmt_can_exit_loop(SN_LIST_AT(s->catches, SnStmt, i))) return 1;
        }
        return 0;
    case SN_STMT_MATCH:
        for (size_t i = 0; i < s->arms.len; i++) {
            const SnMatchArm *arm = SN_LIST_AT(s->arms, SnMatchArm, i);
            if (arm->body && sn_check_stmt_can_exit_loop(arm->body)) return 1;
        }
        return 0;
    default:
        return 0;
    }
}

int sn_check_stmt_is_empty(const SnStmt *s) {
    if (!s) return 1;
    if (s->kind == SN_STMT_BLOCK) {
        return s->stmts.len == 0;
    }
    return 0;
}

