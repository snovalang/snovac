static void emit_pattern_test(Compiler *c, uint32_t subj, const SnPattern *pat, uint32_t line) {
    const char *tag = NULL;
    eb_emit_byte(c, OP_GET_LOCAL, line);
    eb_emit_u32(c, subj, line);
    if (!pat || pat->kind == SN_PAT_WILDCARD) {
        eb_emit_byte(c, OP_POP, line);
        eb_emit_byte(c, OP_CONST_BOOL, line);
        eb_emit_byte(c, 1, line);
        return;
    }
    if (pattern_variant_tag(c, pat, &tag)) {
        uint32_t si = sn_bcunit_add_string(c->bc, tag);
        eb_emit_byte(c, OP_IS_VARIANT, line);
        eb_emit_u32(c, si, line);
        return;
    }
    if (pat->kind == SN_PAT_LITERAL && pat->literal) {
        eb_compile_expr(c, pat->literal);
        eb_emit_byte(c, OP_EQ, line);
        return;
    }
    eb_emit_byte(c, OP_POP, line);
    eb_emit_byte(c, OP_CONST_BOOL, line);
    eb_emit_byte(c, 1, line);
}

static void emit_match(Compiler *c, const SnExpr *subject, const SnList *arms, int as_value, uint32_t line) {
    uint32_t saved = c->local_count;
    eb_compile_expr(c, subject);
    uint32_t subj = eb_add_local(c, ".subj", line ? (SnSpan){0} : (SnSpan){0});
    (void)line;
    eb_emit_byte(c, OP_SET_LOCAL, subject ? subject->span.line : 0);
    eb_emit_u32(c, subj, subject ? subject->span.line : 0);
    eb_emit_byte(c, OP_POP, subject ? subject->span.line : 0);
    size_t ends[64];
    int nends = 0;
    uint32_t arm_line = subject ? subject->span.line : 0;
    for (size_t i = 0; i < arms->len; i++) {
        const SnMatchArm *arm = (const SnMatchArm *)arms->items[i];
        arm_line = arm->span.line;
        emit_pattern_test(c, subj, arm->pattern, arm_line);
        size_t next = eb_emit_jump(c, OP_JUMP_IF_FALSE, arm_line);
        if (arm->guard) {
            eb_compile_expr(c, arm->guard);
            size_t guard_fail = eb_emit_jump(c, OP_JUMP_IF_FALSE, arm_line);
            if (arm->value) {
                eb_compile_expr(c, arm->value);
                if (!as_value) {
                    eb_emit_byte(c, OP_POP, arm_line);
                }
            } else if (arm->body) {
                eb_compile_stmt(c, arm->body);
                if (as_value) {
                    eb_emit_byte(c, OP_CONST_UNIT, arm_line);
                }
            } else if (as_value) {
                eb_emit_byte(c, OP_CONST_UNIT, arm_line);
            }
            if (nends < 64) {
                ends[nends++] = eb_emit_jump(c, OP_JUMP, arm_line);
            }
            eb_patch_jump(c, guard_fail);
            eb_patch_jump(c, next);
            continue;
        }
        if (arm->pattern && arm->pattern->kind == SN_PAT_BINDING && arm->pattern->name &&
            !variant_named(c, arm->pattern->name)) {
            uint32_t slot = eb_add_local(c, arm->pattern->name, arm->pattern->span);
            eb_emit_byte(c, OP_GET_LOCAL, arm_line);
            eb_emit_u32(c, subj, arm_line);
            eb_emit_byte(c, OP_SET_LOCAL, arm_line);
            eb_emit_u32(c, slot, arm_line);
            eb_emit_byte(c, OP_POP, arm_line);
        }
        if (arm->value) {
            eb_compile_expr(c, arm->value);
            if (!as_value) {
                eb_emit_byte(c, OP_POP, arm_line);
            }
        } else if (arm->body) {
            eb_compile_stmt(c, arm->body);
            if (as_value) {
                eb_emit_byte(c, OP_CONST_UNIT, arm_line);
            }
        } else if (as_value) {
            eb_emit_byte(c, OP_CONST_UNIT, arm_line);
        }
        if (nends < 64) {
            ends[nends++] = eb_emit_jump(c, OP_JUMP, arm_line);
        }
        eb_patch_jump(c, next);
    }
    if (as_value) {
        eb_emit_byte(c, OP_TRAP, arm_line);
        eb_emit_byte(c, OP_CONST_UNIT, arm_line);
    }
    for (int i = 0; i < nends; i++) {
        eb_patch_jump(c, ends[i]);
    }
    c->local_count = saved;
    c->expr_type_name = NULL;
}

static void emit_for(Compiler *c, const SnStmt *s) {
    uint32_t line = s->span.line;
    uint32_t saved = c->local_count;
    uint32_t is_arr = eb_add_local(c, ".forarr", s->span);
    uint32_t iter = eb_add_local(c, ".foriter", s->span);
    uint32_t idx = eb_add_local(c, ".foridx", s->span);
    uint32_t user = eb_add_local(c, s->name ? s->name : ".forv", s->span);

    eb_compile_expr(c, s->expr);
    eb_emit_byte(c, OP_SET_LOCAL, line);
    eb_emit_u32(c, iter, line);
    eb_emit_byte(c, OP_POP, line);
    eb_emit_byte(c, OP_GET_LOCAL, line);
    eb_emit_u32(c, iter, line);
    eb_emit_byte(c, OP_IS_ARRAY, line);
    eb_emit_byte(c, OP_SET_LOCAL, line);
    eb_emit_u32(c, is_arr, line);
    eb_emit_byte(c, OP_POP, line);
    eb_emit_byte(c, OP_CONST_INT, line);
    eb_emit_i64(c, 0, line);
    eb_emit_byte(c, OP_SET_LOCAL, line);
    eb_emit_u32(c, idx, line);
    eb_emit_byte(c, OP_POP, line);

    size_t head = c->current_fn->chunk.count;
    eb_emit_byte(c, OP_GET_LOCAL, line);
    eb_emit_u32(c, is_arr, line);
    size_t to_int = eb_emit_jump(c, OP_JUMP_IF_FALSE, line);

    eb_emit_byte(c, OP_GET_LOCAL, line);
    eb_emit_u32(c, idx, line);
    eb_emit_byte(c, OP_GET_LOCAL, line);
    eb_emit_u32(c, iter, line);
    eb_emit_byte(c, OP_ARRAY_LEN, line);
    eb_emit_byte(c, OP_LT, line);
    size_t arr_exit = eb_emit_jump(c, OP_JUMP_IF_FALSE, line);
    eb_emit_byte(c, OP_GET_LOCAL, line);
    eb_emit_u32(c, iter, line);
    eb_emit_byte(c, OP_GET_LOCAL, line);
    eb_emit_u32(c, idx, line);
    eb_emit_byte(c, OP_GET_INDEX, line);
    eb_emit_byte(c, OP_SET_LOCAL, line);
    eb_emit_u32(c, user, line);
    eb_emit_byte(c, OP_POP, line);
    size_t to_body = eb_emit_jump(c, OP_JUMP, line);

    eb_patch_jump(c, to_int);
    eb_emit_byte(c, OP_GET_LOCAL, line);
    eb_emit_u32(c, idx, line);
    eb_emit_byte(c, OP_GET_LOCAL, line);
    eb_emit_u32(c, iter, line);
    eb_emit_byte(c, OP_LT, line);
    size_t int_exit = eb_emit_jump(c, OP_JUMP_IF_FALSE, line);
    eb_emit_byte(c, OP_GET_LOCAL, line);
    eb_emit_u32(c, idx, line);
    eb_emit_byte(c, OP_SET_LOCAL, line);
    eb_emit_u32(c, user, line);
    eb_emit_byte(c, OP_POP, line);

    eb_patch_jump(c, to_body);
    eb_loop_push(c, 0, 0);
    if (s->then_br) {
        eb_compile_stmt(c, s->then_br);
    }
    eb_loop_patch_continues(c);
    eb_emit_byte(c, OP_GET_LOCAL, line);
    eb_emit_u32(c, idx, line);
    eb_emit_byte(c, OP_CONST_INT, line);
    eb_emit_i64(c, 1, line);
    eb_emit_byte(c, OP_ADD, line);
    eb_emit_byte(c, OP_SET_LOCAL, line);
    eb_emit_u32(c, idx, line);
    eb_emit_byte(c, OP_POP, line);
    eb_emit_byte(c, OP_JUMP, line);
    size_t back_at = c->current_fn->chunk.count;
    int32_t back = (int32_t)(head - (back_at + 4));
    eb_emit_u32(c, (uint32_t)back, line);
    eb_patch_jump(c, arr_exit);
    eb_patch_jump(c, int_exit);
    eb_loop_patch_breaks(c);
    eb_loop_pop(c);
    c->local_count = saved;
}

int eb_try_expr(Compiler *c, const SnExpr *e) {
    if (!e) {
        return 0;
    }
    if (e->kind == SN_EXPR_NULL) {
        eb_emit_byte(c, OP_CONST_NULL, e->span.line);
        c->expr_type_name = NULL;
        return 1;
    }
    if (e->kind == SN_EXPR_AWAIT) {
        eb_compile_expr(c, e->lhs);
        return 1;
    }
    if (e->kind == SN_EXPR_MATCH) {
        emit_match(c, e->lhs, &e->arms, 1, e->span.line);
        return 1;
    }
    if (e->kind == SN_EXPR_MEMBER && emit_enum_member(c, e)) {
        return 1;
    }
    if (e->kind == SN_EXPR_IDENT && c->owner && e->text) {
        uint32_t slot = 0;
        if (!eb_resolve_local(c, e->text, &slot)) {
            uint32_t fi = 0;
            if (field_of(c->owner, e->text, &fi)) {
                load_this_field(c, fi, e->span.line);
                c->expr_type_name = NULL;
                return 1;
            }
        }
    }
    if (e->kind == SN_EXPR_ASSIGN && e->op == SN_TOK_ASSIGN && e->lhs &&
        e->lhs->kind == SN_EXPR_IDENT && c->owner && e->lhs->text) {
        uint32_t slot = 0;
        if (!eb_resolve_local(c, e->lhs->text, &slot)) {
            uint32_t fi = 0;
            if (field_of(c->owner, e->lhs->text, &fi)) {
                eb_compile_expr(c, e->rhs);
                eb_emit_byte(c, OP_GET_LOCAL, e->span.line);
                eb_emit_u32(c, 0, e->span.line);
                eb_emit_byte(c, OP_SET_FIELD, e->span.line);
                eb_emit_u32(c, fi, e->span.line);
                return 1;
            }
        }
    }
    if (e->kind == SN_EXPR_CALL) {
        if (emit_method_call(c, e)) {
            return 1;
        }
        if (emit_named_call(c, e)) {
            return 1;
        }
    }
    return 0;
}

int eb_try_stmt(Compiler *c, const SnStmt *s) {
    if (!s) {
        return 0;
    }
    uint32_t line = s->span.line;
    if (s->kind == SN_STMT_BREAK) {
        eb_loop_break(c, line);
        return 1;
    }
    if (s->kind == SN_STMT_CONTINUE) {
        eb_loop_continue(c, line);
        return 1;
    }
    if (s->kind == SN_STMT_FOR) {
        emit_for(c, s);
        return 1;
    }
    if (s->kind == SN_STMT_MATCH) {
        emit_match(c, s->expr, &s->arms, 0, line);
        return 1;
    }
    if (s->kind == SN_STMT_DEFER) {
        const SnExpr *call = s->expr;
        if (!call || call->kind != SN_EXPR_CALL || !call->lhs || call->lhs->kind != SN_EXPR_IDENT ||
            !call->lhs->text) {
            eb_emit_unsupported(c, s->span, "defer");
            return 1;
        }
        uint32_t fn = 0;
        if (!eb_resolve_func(c, call->lhs->text, &fn)) {
            eb_emit_unsupported(c, s->span, "defer");
            return 1;
        }
        for (size_t i = 0; i < call->args.len; i++) {
            eb_compile_expr(c, SN_LIST_AT(call->args, SnExpr, i));
        }
        eb_emit_byte(c, OP_DEFER, line);
        eb_emit_u32(c, fn, line);
        eb_emit_u32(c, (uint32_t)call->args.len, line);
        return 1;
    }
    if (s->kind == SN_STMT_PULSAR) {
        const SnExpr *call = s->expr;
        if (call && call->kind == SN_EXPR_CALL && call->lhs && call->lhs->kind == SN_EXPR_IDENT &&
            call->lhs->text) {
            uint32_t fn = 0;
            if (eb_resolve_func(c, call->lhs->text, &fn)) {
                for (size_t i = 0; i < call->args.len; i++) {
                    eb_compile_expr(c, SN_LIST_AT(call->args, SnExpr, i));
                }
                eb_emit_byte(c, OP_SPAWN, line);
                eb_emit_u32(c, fn, line);
                eb_emit_u32(c, (uint32_t)call->args.len, line);
                return 1;
            }
        }
        if (s->expr) {
            eb_compile_expr(c, s->expr);
            eb_emit_byte(c, OP_POP, line);
        }
        return 1;
    }
    return 0;
}

static void register_one(Compiler *c, const SnDecl *d, const SnDecl *owner) {
    if (!d) {
        return;
    }
    if (d->kind == SN_DECL_FUNC || d->kind == SN_DECL_METHOD) {
        char buf[256];
        routine_name(owner, d, buf, sizeof(buf));
        uint32_t arity = (uint32_t)d->params.len + (owner ? 1u : 0u);
        uint32_t idx = sn_bcunit_add_function(c->bc, buf, arity);
        if (!owner && d->name && strcmp(d->name, "main") == 0) {
            c->bc->main_func_idx = idx;
        }
        if (idx < EB_FUNCS) {
            c->fn_async[idx] = d->is_async ? 1 : 0;
            c->fn_pulsar[idx] = d->is_pulsar ? 1 : 0;
        }
        return;
    }
    if (d->kind == SN_DECL_STRUCT) {
        eb_reject_decl(c, d);
        return;
    }
    if (d->kind == SN_DECL_CLASS || d->kind == SN_DECL_INTERFACE) {
        if (d->has_accessors && ((d->getter && d->getter->proj) || (d->setter && d->setter->proj))) {
            eb_emit_unsupported(c, d->span, "declaration");
        }
        for (size_t i = 0; i < d->members.len; i++) {
            const SnDecl *m = SN_LIST_AT(d->members, SnDecl, i);
            if (m && m->kind == SN_DECL_FIELD && m->has_accessors &&
                ((m->getter && m->getter->proj) || (m->setter && m->setter->proj))) {
                eb_emit_unsupported(c, m->span, "declaration");
            }
            if (m && (m->kind == SN_DECL_FUNC || m->kind == SN_DECL_METHOD)) {
                register_one(c, m, d);
            }
        }
        return;
    }
    if (d->kind == SN_DECL_ENUM) {
        return;
    }
    eb_reject_decl(c, d);
}

void eb_register_unit(Compiler *c) {
    for (size_t i = 0; i < c->unit->decls.len; i++) {
        register_one(c, SN_LIST_AT(c->unit->decls, SnDecl, i), NULL);
    }
}

