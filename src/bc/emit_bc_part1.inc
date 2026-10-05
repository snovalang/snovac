/* emit_bc.c — AST to SnBC bytecode code generation.
 * A node this file does not lower is SNOVA_EMIT_UNSUPPORTED. The implicit
 * OP_CONST_UNIT / OP_RETURN epilogue is the fall-off return of a body that
 * was lowered. It is not a substitute for a dropped node. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "emit_bc.h"
#include "emit_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define emit_unsupported eb_emit_unsupported
#define add_local eb_add_local
#define resolve_local eb_resolve_local
#define resolve_func eb_resolve_func
#define emit_byte eb_emit_byte
#define emit_u32 eb_emit_u32
#define emit_i64 eb_emit_i64
#define emit_jump eb_emit_jump
#define patch_jump eb_patch_jump
#define compile_expr eb_compile_expr
#define compile_stmt eb_compile_stmt
#define emit_default_for_type eb_emit_default_for_type
#define reject_unlowered_decl eb_reject_decl

void emit_unsupported(Compiler *c, SnSpan span, const char *what) {
    c->failed = 1;
    if (c->diag) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_EMIT_UNSUPPORTED, span,
                     "cannot lower %s to SnBC\n", what);
    }
}

uint32_t add_local(Compiler *c, const char *name, SnSpan span) {
    if (c->local_count >= 256) {
        emit_unsupported(c, span, "local");
        return 0;
    }
    uint32_t idx = c->local_count++;
    c->locals[idx].name = name;
    c->locals[idx].type_name = NULL;
    c->locals[idx].index = idx;
    if (c->local_count > c->current_fn->local_count) {
        c->current_fn->local_count = c->local_count;
    }
    return idx;
}

int resolve_local(Compiler *c, const char *name, uint32_t *out_idx) {
    if (!name) return 0;
    for (int i = (int)c->local_count - 1; i >= 0; i--) {
        if (c->locals[i].name && strcmp(c->locals[i].name, name) == 0) {
            *out_idx = c->locals[i].index;
            return 1;
        }
    }
    return 0;
}

int resolve_func(Compiler *c, const char *name, uint32_t *out_idx) {
    for (size_t i = 0; i < c->bc->function_count; i++) {
        if (c->bc->functions[i]->name && strcmp(c->bc->functions[i]->name, name) == 0) {
            *out_idx = (uint32_t)i;
            return 1;
        }
    }
    return 0;
}

void emit_byte(Compiler *c, uint8_t b, uint32_t line) {
    sn_chunk_write(&c->current_fn->chunk, b, line);
}

void emit_u32(Compiler *c, uint32_t val, uint32_t line) {
    sn_chunk_write_u32(&c->current_fn->chunk, val, line);
}

void emit_i64(Compiler *c, int64_t val, uint32_t line) {
    sn_chunk_write_i64(&c->current_fn->chunk, val, line);
}

static void emit_double(Compiler *c, double val, uint32_t line) {
    sn_chunk_write_double(&c->current_fn->chunk, val, line);
}

size_t emit_jump(Compiler *c, SnOpcode op, uint32_t line) {
    emit_byte(c, (uint8_t)op, line);
    size_t pos = c->current_fn->chunk.count;
    emit_u32(c, 0, line); /* placeholder */
    return pos;
}

void patch_jump(Compiler *c, size_t jump_offset_pos) {
    int32_t offset = (int32_t)(c->current_fn->chunk.count - (jump_offset_pos + 4));
    uint8_t *p = &c->current_fn->chunk.code[jump_offset_pos];
    p[0] = (uint8_t)(offset & 0xFF);
    p[1] = (uint8_t)((offset >> 8) & 0xFF);
    p[2] = (uint8_t)((offset >> 16) & 0xFF);
    p[3] = (uint8_t)((offset >> 24) & 0xFF);
}

/* compile_expr and compile_stmt are eb_compile_expr / eb_compile_stmt. */

/* `.len` / `.length`, `.push`, and `.get` are the array methods the checker
 * already types. `.get` is the same load as `xs[i]`. */
static int emit_array_method(Compiler *c, const SnExpr *e, uint32_t line,
                             const char **produced) {
    if (!e->lhs || e->lhs->kind != SN_EXPR_MEMBER || !e->lhs->text || !e->lhs->lhs) {
        return 0;
    }
    const char *m = e->lhs->text;
    if ((strcmp(m, "len") == 0 || strcmp(m, "length") == 0) && e->args.len == 0) {
        compile_expr(c, e->lhs->lhs);
        emit_byte(c, OP_ARRAY_LEN, line);
        *produced = "int";
        return 1;
    }
    if (strcmp(m, "push") == 0 && e->args.len == 1) {
        compile_expr(c, e->lhs->lhs);
        compile_expr(c, SN_LIST_AT(e->args, SnExpr, 0));
        emit_byte(c, OP_ARRAY_PUSH, line);
        *produced = "int";
        return 1;
    }
    if (strcmp(m, "get") == 0 && e->args.len == 1) {
        compile_expr(c, e->lhs->lhs);
        compile_expr(c, SN_LIST_AT(e->args, SnExpr, 0));
        emit_byte(c, OP_GET_INDEX, line);
        return 1;
    }
    return 0;
}

static const SnDecl *find_struct(const Compiler *c, const char *name) {
    if (!c || !c->unit || !name) {
        return NULL;
    }
    for (size_t i = 0; i < c->unit->decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(c->unit->decls, SnDecl, i);
        if (d && (d->kind == SN_DECL_STRUCT || d->kind == SN_DECL_CLASS) &&
            d->name && strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

static int struct_field_at(const SnDecl *st, const char *name, uint32_t *out_idx,
                           const SnDecl **out_field) {
    if (!st || !name) {
        return 0;
    }
    uint32_t idx = 0;
    for (size_t i = 0; i < st->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(st->members, SnDecl, i);
        if (!m || m->kind != SN_DECL_FIELD) {
            continue;
        }
        if (m->name && strcmp(m->name, name) == 0) {
            if (out_idx) {
                *out_idx = idx;
            }
            if (out_field) {
                *out_field = m;
            }
            return 1;
        }
        if (idx == UINT32_MAX) {
            return 0;
        }
        idx++;
    }
    return 0;
}

static const char *field_type_name(const SnDecl *field) {
    if (field && field->type && field->type->kind == SN_TYPE_NAME && field->type->name) {
        return field->type->name;
    }
    return NULL;
}

void emit_default_for_type(Compiler *c, const SnType *type, uint32_t line) {
    const char *n = (type && type->kind == SN_TYPE_NAME && type->name) ? type->name : "";
    if (strcmp(n, "bool") == 0) {
        emit_byte(c, OP_CONST_BOOL, line);
        emit_byte(c, 0, line);
        return;
    }
    if (strcmp(n, "string") == 0) {
        uint32_t s_idx = sn_bcunit_add_string(c->bc, "");
        emit_byte(c, OP_CONST_STRING, line);
        emit_u32(c, s_idx, line);
        return;
    }
    if (strcmp(n, "double") == 0 || strcmp(n, "decimal") == 0) {
        emit_byte(c, OP_CONST_DOUBLE, line);
        emit_double(c, 0.0, line);
        return;
    }
    emit_byte(c, OP_CONST_INT, line);
    emit_i64(c, 0, line);
}

static void emit_bool_const(Compiler *c, int value, uint32_t line) {
    emit_byte(c, OP_CONST_BOOL, line);
    emit_byte(c, value ? 1 : 0, line);
}

static void emit_andand(Compiler *c, const SnExpr *e, uint32_t line) {
    compile_expr(c, e->lhs);
    size_t to_false = emit_jump(c, OP_JUMP_IF_FALSE, line);
    compile_expr(c, e->rhs);
    size_t rhs_false = emit_jump(c, OP_JUMP_IF_FALSE, line);
    emit_bool_const(c, 1, line);
    size_t to_end = emit_jump(c, OP_JUMP, line);
    patch_jump(c, to_false);
    patch_jump(c, rhs_false);
    emit_bool_const(c, 0, line);
    patch_jump(c, to_end);
}

static void emit_oror(Compiler *c, const SnExpr *e, uint32_t line) {
    compile_expr(c, e->lhs);
    size_t to_true = emit_jump(c, OP_JUMP_IF_TRUE, line);
    compile_expr(c, e->rhs);
    size_t rhs_true = emit_jump(c, OP_JUMP_IF_TRUE, line);
    emit_bool_const(c, 0, line);
    size_t to_end = emit_jump(c, OP_JUMP, line);
    patch_jump(c, to_true);
    patch_jump(c, rhs_true);
    emit_bool_const(c, 1, line);
    patch_jump(c, to_end);
}

/* A type declaration has no opcode. It is a dropped program only when it
 * carries code this pass does not lower: a nested routine, an initializer,
 * or an accessor projection. A field-only struct is not one of those. */
void reject_unlowered_decl(Compiler *c, const SnDecl *d) {
    if (!d) {
        return;
    }
    if (d->kind == SN_DECL_FUNC || d->kind == SN_DECL_METHOD) {
        emit_unsupported(c, d->span, "declaration");
        return;
    }
    if (d->init) {
        emit_unsupported(c, d->init->span, "declaration");
    }
    if (d->has_accessors) {
        if ((d->getter && d->getter->proj) || (d->setter && d->setter->proj)) {
            emit_unsupported(c, d->span, "declaration");
        }
    }
    for (size_t i = 0; i < d->members.len; i++) {
        reject_unlowered_decl(c, SN_LIST_AT(d->members, SnDecl, i));
    }
    for (size_t i = 0; i < d->variants.len; i++) {
        reject_unlowered_decl(c, SN_LIST_AT(d->variants, SnDecl, i));
    }
}

