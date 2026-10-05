/* emit_scope.c — break, continue, null, bodyless routines, class, enum,
 * trait, match, async, await, pulsar, for, and defer. */
#include "emit_internal.h"

#include <stdio.h>
#include <string.h>

void eb_loop_push(Compiler *c, int known, size_t pos) {
    if (c->loop_top + 1 >= EB_LOOPS) {
        eb_emit_unsupported(c, (SnSpan){0}, "loop");
        return;
    }
    c->loop_top++;
    c->cont_known[c->loop_top] = known;
    c->cont_pos[c->loop_top] = pos;
    c->break_n[c->loop_top] = 0;
    c->cont_n[c->loop_top] = 0;
}

static void remember(Compiler *c, size_t *slot, int *n, size_t at) {
    if (*n >= EB_PATCH) {
        eb_emit_unsupported(c, (SnSpan){0}, "loop");
        return;
    }
    slot[(*n)++] = at;
}

void eb_loop_continue(Compiler *c, uint32_t line) {
    if (c->loop_top < 0) {
        eb_emit_unsupported(c, (SnSpan){0}, "continue");
        return;
    }
    if (c->cont_known[c->loop_top]) {
        size_t target = c->cont_pos[c->loop_top];
        eb_emit_byte(c, OP_JUMP, line);
        size_t at = c->current_fn->chunk.count;
        int32_t off = (int32_t)(target - (at + 4));
        eb_emit_u32(c, (uint32_t)off, line);
        return;
    }
    size_t at = eb_emit_jump(c, OP_JUMP, line);
    remember(c, c->cont_at[c->loop_top], &c->cont_n[c->loop_top], at);
}

void eb_loop_break(Compiler *c, uint32_t line) {
    if (c->loop_top < 0) {
        eb_emit_unsupported(c, (SnSpan){0}, "break");
        return;
    }
    size_t at = eb_emit_jump(c, OP_JUMP, line);
    remember(c, c->break_at[c->loop_top], &c->break_n[c->loop_top], at);
}

void eb_loop_patch_continues(Compiler *c) {
    if (c->loop_top < 0) {
        return;
    }
    for (int i = 0; i < c->cont_n[c->loop_top]; i++) {
        eb_patch_jump(c, c->cont_at[c->loop_top][i]);
    }
    c->cont_n[c->loop_top] = 0;
}

void eb_loop_patch_breaks(Compiler *c) {
    if (c->loop_top < 0) {
        return;
    }
    for (int i = 0; i < c->break_n[c->loop_top]; i++) {
        eb_patch_jump(c, c->break_at[c->loop_top][i]);
    }
    c->break_n[c->loop_top] = 0;
}

void eb_loop_pop(Compiler *c) {
    if (c->loop_top >= 0) {
        c->loop_top--;
    }
}

static const SnDecl *decl_named(const Compiler *c, const char *name, SnDeclKind kind) {
    if (!c->unit || !name) {
        return NULL;
    }
    for (size_t i = 0; i < c->unit->decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(c->unit->decls, SnDecl, i);
        if (d && d->kind == kind && d->name && strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

static const SnDecl *type_named(const Compiler *c, const char *name) {
    const SnDecl *d = decl_named(c, name, SN_DECL_CLASS);
    if (d) {
        return d;
    }
    d = decl_named(c, name, SN_DECL_STRUCT);
    if (d) {
        return d;
    }
    d = decl_named(c, name, SN_DECL_INTERFACE);
    if (d) {
        return d;
    }
    return decl_named(c, name, SN_DECL_ENUM);
}

static const SnDecl *member_named(const SnDecl *owner, const char *name, SnDeclKind kind) {
    if (!owner || !name) {
        return NULL;
    }
    for (size_t i = 0; i < owner->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(owner->members, SnDecl, i);
        if (m && m->kind == kind && m->name && strcmp(m->name, name) == 0) {
            return m;
        }
    }
    return NULL;
}

static int variant_named(const Compiler *c, const char *name) {
    if (!c->unit || !name) {
        return 0;
    }
    for (size_t i = 0; i < c->unit->decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(c->unit->decls, SnDecl, i);
        if (!d || d->kind != SN_DECL_ENUM) {
            continue;
        }
        for (size_t v = 0; v < d->variants.len; v++) {
            const SnDecl *var = SN_LIST_AT(d->variants, SnDecl, v);
            if (var && var->name && strcmp(var->name, name) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static const char *ret_name(const SnDecl *fn) {
    if (fn && fn->ret && fn->ret->kind == SN_TYPE_NAME) {
        return fn->ret->name;
    }
    return NULL;
}

static void routine_name(const SnDecl *owner, const SnDecl *fn, char *buf, size_t n) {
    if (owner && owner->name && fn->name) {
        snprintf(buf, n, "%s.%s", owner->name, fn->name);
        return;
    }
    snprintf(buf, n, "%s", fn->name ? fn->name : "");
}

static void emit_call_op(Compiler *c, uint32_t fn_idx, uint32_t argc, int async_fn, uint32_t line) {
    eb_emit_byte(c, async_fn ? OP_CALL_ASYNC : OP_CALL, line);
    eb_emit_u32(c, fn_idx, line);
    eb_emit_u32(c, argc, line);
}

static int fn_is_async(const Compiler *c, uint32_t idx) {
    return idx < EB_FUNCS && c->fn_async[idx];
}

static void emit_variant(Compiler *c, const char *name, uint32_t line) {
    uint32_t tag = sn_bcunit_add_string(c->bc, name ? name : "");
    eb_emit_byte(c, OP_VARIANT, line);
    eb_emit_u32(c, tag, line);
    eb_emit_u32(c, 0, line);
}

static int emit_enum_member(Compiler *c, const SnExpr *e) {
    if (!e || e->kind != SN_EXPR_MEMBER || !e->lhs || e->lhs->kind != SN_EXPR_IDENT || !e->text) {
        return 0;
    }
    uint32_t dummy = 0;
    if (eb_resolve_local(c, e->lhs->text, &dummy)) {
        return 0;
    }
    const SnDecl *en = decl_named(c, e->lhs->text, SN_DECL_ENUM);
    if (!en) {
        return 0;
    }
    emit_variant(c, e->text, e->span.line);
    c->expr_type_name = en->name;
    return 1;
}

static int field_of(const SnDecl *owner, const char *name, uint32_t *idx) {
    if (!owner || !name) {
        return 0;
    }
    uint32_t n = 0;
    for (size_t i = 0; i < owner->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(owner->members, SnDecl, i);
        if (!m || m->kind != SN_DECL_FIELD) {
            continue;
        }
        if (m->name && strcmp(m->name, name) == 0) {
            if (idx) {
                *idx = n;
            }
            return 1;
        }
        n++;
    }
    return 0;
}

static void load_this_field(Compiler *c, uint32_t idx, uint32_t line) {
    eb_emit_byte(c, OP_GET_LOCAL, line);
    eb_emit_u32(c, 0, line);
    eb_emit_byte(c, OP_GET_FIELD, line);
    eb_emit_u32(c, idx, line);
}

static int emit_ctor(Compiler *c, const SnExpr *e, const SnDecl *cls) {
    uint32_t line = e->span.line;
    uint32_t nfields = 0;
    uint32_t argi = 0;
    for (size_t i = 0; i < cls->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(cls->members, SnDecl, i);
        if (!m || m->kind != SN_DECL_FIELD) {
            continue;
        }
        if (argi < e->args.len) {
            eb_compile_expr(c, SN_LIST_AT(e->args, SnExpr, argi));
            argi++;
        } else if (m->init) {
            eb_compile_expr(c, m->init);
        } else {
            eb_emit_default_for_type(c, m->type, line);
        }
        nfields++;
    }
    uint32_t si = sn_bcunit_add_string(c->bc, cls->name ? cls->name : "");
    eb_emit_byte(c, OP_NEW_OBJ, line);
    eb_emit_u32(c, si, line);
    eb_emit_u32(c, nfields, line);
    c->expr_type_name = cls->name;
    return 1;
}

static int emit_method_call(Compiler *c, const SnExpr *e) {
    const SnExpr *callee = e->lhs;
    if (!callee || callee->kind != SN_EXPR_MEMBER || !callee->text || !callee->lhs) {
        return 0;
    }
    if (strcmp(callee->text, "printline") == 0 || strcmp(callee->text, "println") == 0 ||
        strcmp(callee->text, "print") == 0 || strcmp(callee->text, "len") == 0 ||
        strcmp(callee->text, "length") == 0 || strcmp(callee->text, "push") == 0 ||
        strcmp(callee->text, "get") == 0) {
        return 0;
    }
    const char *ty = NULL;
    if (callee->lhs->kind == SN_EXPR_IDENT) {
        uint32_t slot = 0;
        if (eb_resolve_local(c, callee->lhs->text, &slot)) {
            ty = c->locals[slot].type_name;
        } else {
            const SnDecl *as_type = type_named(c, callee->lhs->text);
            if (as_type && as_type->kind != SN_DECL_ENUM) {
                const SnDecl *m = member_named(as_type, callee->text, SN_DECL_METHOD);
                if (!m) {
                    m = member_named(as_type, callee->text, SN_DECL_FUNC);
                }
                if (m) {
                    char buf[256];
                    routine_name(as_type, m, buf, sizeof(buf));
                    uint32_t fn = 0;
                    if (!eb_resolve_func(c, buf, &fn)) {
                        return 0;
                    }
                    for (size_t i = 0; i < e->args.len; i++) {
                        eb_compile_expr(c, SN_LIST_AT(e->args, SnExpr, i));
                    }
                    emit_call_op(c, fn, (uint32_t)e->args.len, fn_is_async(c, fn), e->span.line);
                    c->expr_type_name = ret_name(m);
                    return 1;
                }
            }
        }
    }
    if (!ty) {
        return 0;
    }
    const SnDecl *owner = type_named(c, ty);
    const SnDecl *m = owner ? member_named(owner, callee->text, SN_DECL_METHOD) : NULL;
    if (!m) {
        m = owner ? member_named(owner, callee->text, SN_DECL_FUNC) : NULL;
    }
    if (!m) {
        return 0;
    }
    char buf[256];
    routine_name(owner, m, buf, sizeof(buf));
    uint32_t fn = 0;
    if (!eb_resolve_func(c, buf, &fn)) {
        return 0;
    }
    eb_compile_expr(c, callee->lhs);
    for (size_t i = 0; i < e->args.len; i++) {
        eb_compile_expr(c, SN_LIST_AT(e->args, SnExpr, i));
    }
    emit_call_op(c, fn, (uint32_t)e->args.len + 1u, fn_is_async(c, fn), e->span.line);
    c->expr_type_name = ret_name(m);
    return 1;
}

static int emit_named_call(Compiler *c, const SnExpr *e) {
    const SnExpr *callee = e->lhs;
    if (!callee || callee->kind != SN_EXPR_IDENT || !callee->text) {
        return 0;
    }
    if (strcmp(callee->text, "read_bytes") == 0 || strcmp(callee->text, "write_bytes") == 0 ||
        strcmp(callee->text, "printline") == 0 || strcmp(callee->text, "println") == 0 ||
        strcmp(callee->text, "print") == 0) {
        return 0;
    }
    uint32_t fn = 0;
    if (eb_resolve_func(c, callee->text, &fn)) {
        if (!fn_is_async(c, fn)) {
            return 0;
        }
        for (size_t i = 0; i < e->args.len; i++) {
            eb_compile_expr(c, SN_LIST_AT(e->args, SnExpr, i));
        }
        emit_call_op(c, fn, (uint32_t)e->args.len, 1, e->span.line);
        c->expr_type_name = NULL;
        return 1;
    }
    const SnDecl *cls = type_named(c, callee->text);
    if (cls && cls->kind != SN_DECL_ENUM) {
        return emit_ctor(c, e, cls);
    }
    if (variant_named(c, callee->text) && e->args.len == 0) {
        emit_variant(c, callee->text, e->span.line);
        c->expr_type_name = NULL;
        return 1;
    }
    if (c->owner) {
        const SnDecl *m = member_named(c->owner, callee->text, SN_DECL_METHOD);
        if (!m) {
            m = member_named(c->owner, callee->text, SN_DECL_FUNC);
        }
        if (m) {
            char buf[256];
            routine_name(c->owner, m, buf, sizeof(buf));
            if (!eb_resolve_func(c, buf, &fn)) {
                return 0;
            }
            eb_emit_byte(c, OP_GET_LOCAL, e->span.line);
            eb_emit_u32(c, 0, e->span.line);
            for (size_t i = 0; i < e->args.len; i++) {
                eb_compile_expr(c, SN_LIST_AT(e->args, SnExpr, i));
            }
            emit_call_op(c, fn, (uint32_t)e->args.len + 1u, fn_is_async(c, fn), e->span.line);
            c->expr_type_name = ret_name(m);
            return 1;
        }
    }
    return 0;
}

static int pattern_variant_tag(const Compiler *c, const SnPattern *pat, const char **tag) {
    if (!pat) {
        return 0;
    }
    if (pat->kind == SN_PAT_VARIANT && pat->name) {
        const char *dot = strrchr(pat->name, '.');
        *tag = dot ? dot + 1 : pat->name;
        return 1;
    }
    if (pat->kind == SN_PAT_BINDING && pat->name && variant_named(c, pat->name) && pat->subs.len == 0) {
        *tag = pat->name;
        return 1;
    }
    return 0;
}

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

static void compile_routine(Compiler *c, const SnDecl *d, const SnDecl *owner) {
    char buf[256];
    routine_name(owner, d, buf, sizeof(buf));
    uint32_t fn_idx = 0;
    if (!eb_resolve_func(c, buf, &fn_idx)) {
        eb_emit_unsupported(c, d->span, "function");
        return;
    }
    c->current_fn = c->bc->functions[fn_idx];
    c->local_count = 0;
    c->owner = owner;
    c->loop_top = -1;
    if (owner) {
        uint32_t slot = eb_add_local(c, "this", d->span);
        c->locals[slot].type_name = owner->name;
    }
    for (size_t pi = 0; pi < d->params.len; pi++) {
        const SnParam *p = SN_LIST_AT(d->params, SnParam, pi);
        uint32_t idx = eb_add_local(c, p->name, p->span);
        if (p->type && p->type->kind == SN_TYPE_NAME) {
            c->locals[idx].type_name = p->type->name;
        }
    }
    if (d->body) {
        eb_compile_stmt(c, d->body);
    } else {
        eb_emit_byte(c, OP_TRAP, d->span.line);
    }
    eb_emit_byte(c, OP_CONST_UNIT, d->span.line);
    eb_emit_byte(c, OP_RETURN, d->span.line);
    c->owner = NULL;
}

static void compile_nested(Compiler *c, const SnDecl *d, const SnDecl *owner) {
    if (!d) {
        return;
    }
    if (d->kind == SN_DECL_FUNC || d->kind == SN_DECL_METHOD) {
        compile_routine(c, d, owner);
        return;
    }
    if (d->kind == SN_DECL_CLASS || d->kind == SN_DECL_INTERFACE) {
        for (size_t i = 0; i < d->members.len; i++) {
            const SnDecl *m = SN_LIST_AT(d->members, SnDecl, i);
            if (m && (m->kind == SN_DECL_FUNC || m->kind == SN_DECL_METHOD)) {
                compile_nested(c, m, d);
            }
        }
    }
}

void eb_compile_unit(Compiler *c) {
    for (size_t i = 0; i < c->unit->decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(c->unit->decls, SnDecl, i);
        if (!d) {
            continue;
        }
        if (d->kind == SN_DECL_FUNC || d->kind == SN_DECL_METHOD) {
            compile_routine(c, d, NULL);
        } else if (d->kind == SN_DECL_CLASS || d->kind == SN_DECL_INTERFACE) {
            compile_nested(c, d, NULL);
        }
    }
}
