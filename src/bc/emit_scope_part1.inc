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

