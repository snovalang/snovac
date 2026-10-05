void compile_stmt(Compiler *c, const SnStmt *s) {
    if (!s) return;
    uint32_t line = s->span.line;
    if (eb_try_stmt(c, s)) {
        return;
    }

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
            c->expr_type_name = NULL;
        }
        const char *ty = c->expr_type_name;
        uint32_t idx = add_local(c, s->name, s->span);
        c->locals[idx].type_name = ty;
        emit_byte(c, OP_SET_LOCAL, line);
        emit_u32(c, idx, line);
        emit_byte(c, OP_POP, line);
        break;
    }
    case SN_STMT_RETURN: {
        if (s->expr) {
            compile_expr(c, s->expr);
        } else {
            emit_byte(c, OP_CONST_UNIT, line);
        }
        emit_byte(c, OP_RETURN, line);
        break;
    }
    case SN_STMT_IF: {
        compile_expr(c, s->expr);
        size_t else_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
        compile_stmt(c, s->then_br);
        if (s->else_br) {
            size_t end_jump = emit_jump(c, OP_JUMP, line);
            patch_jump(c, else_jump);
            compile_stmt(c, s->else_br);
            patch_jump(c, end_jump);
        } else {
            patch_jump(c, else_jump);
        }
        break;
    }
    case SN_STMT_WHILE: {
        size_t loop_start = c->current_fn->chunk.count;
        eb_loop_push(c, 1, loop_start);
        compile_expr(c, s->expr);
        size_t exit_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
        compile_stmt(c, s->then_br);
        emit_byte(c, OP_JUMP, line);
        int32_t loop_offset = (int32_t)(loop_start - (c->current_fn->chunk.count + 4));
        emit_u32(c, (uint32_t)loop_offset, line);
        patch_jump(c, exit_jump);
        eb_loop_patch_breaks(c);
        eb_loop_pop(c);
        break;
    }
    case SN_STMT_BLOCK: {
        uint32_t saved_locals = c->local_count;
        for (size_t i = 0; i < s->stmts.len; i++) {
            compile_stmt(c, SN_LIST_AT(s->stmts, SnStmt, i));
        }
        c->local_count = saved_locals;
        break;
    }
    default:
        emit_unsupported(c, s->span, "statement");
        break;
    }
}

int sn_emit_bytecode(SnArena *arena, SnDiagSink *diag, const SnUnit *unit, SnBCUnit *out) {
    sn_bcunit_init(out);

    Compiler c;
    c.arena = arena;
    c.diag = diag;
    c.unit = unit;
    c.bc = out;
    c.current_fn = NULL;
    c.local_count = 0;
    c.expr_type_name = NULL;
    c.failed = 0;
    c.owner = NULL;
    c.loop_top = -1;
    memset(c.fn_async, 0, sizeof(c.fn_async));
    memset(c.fn_pulsar, 0, sizeof(c.fn_pulsar));

    /* Register every routine, then lower its body. A type with no routine
     * is not a dropped program. A routine with no body is a declaration. */
    eb_register_unit(&c);
    eb_compile_unit(&c);

    return c.failed ? 0 : 1;
}
