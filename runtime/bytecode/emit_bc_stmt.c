#include "emit_bc_internal.h"
#include <string.h>

static void emit_scope_cleanup(Compiler *c, uint32_t local_depth, uint32_t line) {
    for (uint32_t i = c->local_count; i > local_depth; --i) {
        emit_byte(c, OP_CLEAR_LOCAL, line);
        emit_u32(c, c->locals[i - 1u].index, line);
    }
}

static void compile_scoped_stmt(Compiler *c, const SnStmt *stmt, uint32_t line) {
    uint32_t saved_locals = c->local_count;
    sn_compile_stmt(c, stmt);
    emit_scope_cleanup(c, saved_locals, line);
    c->local_count = saved_locals;
}

static void emit_loop_jump(Compiler *c, size_t target, uint32_t line) {
    emit_byte(c, OP_JUMP, line);
    int32_t offset = (int32_t)(target - (c->current_fn->chunk.count + 4u));
    emit_u32(c, (uint32_t)offset, line);
}

void sn_compile_stmt(Compiler *c, const SnStmt *s) {
    if (!s) return;
    uint32_t line = s->span.line;

    switch (s->kind) {
    case SN_STMT_EXPR:
        compile_expr(c, s->expr);
        emit_byte(c, OP_POP, line);
        break;
    case SN_STMT_LET:
    case SN_STMT_VAR: {
        if (s->expr) {
            compile_expr(c, s->expr);
        } else {
            emit_byte(c, OP_CONST_UNIT, line);
        }
        uint32_t idx = add_local(c, s->name, s->span);
        emit_byte(c, OP_SET_LOCAL, line);
        emit_u32(c, idx, line);
        emit_byte(c, OP_POP, line);
        break;
    }
    case SN_STMT_RETURN: {
        if (s->expr) {
            compile_return_expr(c, s->expr);
        } else {
            emit_byte(c, OP_CONST_UNIT, line);
        }
        emit_byte(c, OP_RETURN, line);
        break;
    }
    case SN_STMT_BREAK:
        if (c->loop) {
            LoopContext *loop = c->loop;
            emit_scope_cleanup(c, loop->local_depth, line);
            BreakJump *jump = (BreakJump *)sn_arena_alloc(c->arena, sizeof(*jump));
            jump->offset = emit_jump(c, OP_JUMP, line);
            jump->next = loop->breaks;
            loop->breaks = jump;
        }
        break;
    case SN_STMT_CONTINUE:
        if (c->loop) {
            LoopContext *loop = c->loop;
            emit_scope_cleanup(c, loop->local_depth, line);
            emit_loop_jump(c, loop->start, line);
        }
        break;
    case SN_STMT_IF: {
        compile_expr(c, s->expr);
        size_t else_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
        compile_scoped_stmt(c, s->then_br, line);
        if (s->else_br) {
            size_t end_jump = emit_jump(c, OP_JUMP, line);
            patch_jump(c, else_jump);
            compile_scoped_stmt(c, s->else_br, line);
            patch_jump(c, end_jump);
        } else {
            patch_jump(c, else_jump);
        }
        break;
    }
    case SN_STMT_WHILE: {
        size_t loop_start = c->current_fn->chunk.count;
        compile_expr(c, s->expr);
        size_t exit_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
        LoopContext loop = {0};
        loop.start = loop_start;
        loop.local_depth = c->local_count;
        loop.parent = c->loop;
        c->loop = &loop;
        compile_scoped_stmt(c, s->then_br, line);
        emit_byte(c, OP_JUMP, line);
        int32_t loop_offset = (int32_t)(loop_start - (c->current_fn->chunk.count + 4));
        emit_u32(c, (uint32_t)loop_offset, line);
        patch_jump(c, exit_jump);
        for (BreakJump *jump = loop.breaks; jump; jump = jump->next) patch_jump(c, jump->offset);
        c->loop = loop.parent;
        break;
    }
    case SN_STMT_BLOCK: {
        uint32_t saved_locals = c->local_count;
        for (size_t i = 0; i < s->stmts.len; i++) {
            sn_compile_stmt(c, SN_LIST_AT(s->stmts, SnStmt, i));
        }
        emit_scope_cleanup(c, saved_locals, line);
        c->local_count = saved_locals;
        break;
    }
    default:
        break;
    }
}
