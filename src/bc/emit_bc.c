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

void compile_expr(Compiler *c, const SnExpr *e) {
    const char *produced = NULL;
    if (!e) {
        emit_unsupported(c, (SnSpan){0}, "missing expression");
        c->expr_type_name = NULL;
        return;
    }
    uint32_t line = e->span.line;
    if (eb_try_expr(c, e)) {
        return;
    }

    switch (e->kind) {
    case SN_EXPR_INT:
    case SN_EXPR_LONG: {
        int64_t val = e->text ? (int64_t)strtoll(e->text, NULL, 0) : 0;
        emit_byte(c, OP_CONST_INT, line);
        emit_i64(c, val, line);
        break;
    }
    case SN_EXPR_DOUBLE:
    case SN_EXPR_DECIMAL: {
        double val = e->text ? strtod(e->text, NULL) : 0.0;
        emit_byte(c, OP_CONST_DOUBLE, line);
        emit_double(c, val, line);
        break;
    }
    case SN_EXPR_STRING:
    case SN_EXPR_CHAR: {
        if (e->interpolated) {
            emit_unsupported(c, e->span, "interpolated string");
            break;
        }
        const char *raw = e->text ? e->text : "";
        char buf[8192];
        size_t n = strlen(raw);
        if (n >= 2 && raw[0] == '"' && raw[n - 1] == '"') {
            raw++;
            n -= 2;
        }
        size_t out_len = 0;
        for (size_t i = 0; i < n && out_len + 1 < sizeof(buf); i++) {
            char ch = raw[i];
            if (ch == '\\' && i + 1 < n) {
                char esc = raw[++i];
                switch (esc) {
                case 'n':  buf[out_len++] = '\n'; break;
                case 't':  buf[out_len++] = '\t'; break;
                case 'r':  buf[out_len++] = '\r'; break;
                case '0':  buf[out_len++] = '\0'; break;
                case '\\': buf[out_len++] = '\\'; break;
                case '"':  buf[out_len++] = '"'; break;
                default:   buf[out_len++] = esc; break;
                }
            } else {
                buf[out_len++] = ch;
            }
        }
        buf[out_len] = '\0';
        uint32_t s_idx = sn_bcunit_add_string(c->bc, buf);
        emit_byte(c, OP_CONST_STRING, line);
        emit_u32(c, s_idx, line);
        break;
    }
    case SN_EXPR_BOOL: {
        uint8_t b = (e->text && strcmp(e->text, "true") == 0) ? 1 : 0;
        emit_byte(c, OP_CONST_BOOL, line);
        emit_byte(c, b, line);
        break;
    }
    case SN_EXPR_IDENT: {
        uint32_t loc_idx;
        if (resolve_local(c, e->text, &loc_idx)) {
            emit_byte(c, OP_GET_LOCAL, line);
            emit_u32(c, loc_idx, line);
            produced = c->locals[loc_idx].type_name;
        } else {
            uint32_t fn_idx;
            if (resolve_func(c, e->text, &fn_idx)) {
                emit_byte(c, OP_CONST_INT, line);
                emit_i64(c, (int64_t)fn_idx, line);
            } else {
                emit_unsupported(c, e->span, "name");
            }
        }
        break;
    }
    case SN_EXPR_ASSIGN: {
        if (e->op != SN_TOK_ASSIGN) {
            emit_unsupported(c, e->span, "assignment");
            break;
        }
        compile_expr(c, e->rhs);
        const char *rhs_ty = c->expr_type_name;
        int stored = 0;
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT) {
            uint32_t loc_idx;
            if (resolve_local(c, e->lhs->text, &loc_idx)) {
                emit_byte(c, OP_SET_LOCAL, line);
                emit_u32(c, loc_idx, line);
                c->locals[loc_idx].type_name = rhs_ty;
                produced = rhs_ty;
                stored = 1;
            }
        } else if (e->lhs && e->lhs->kind == SN_EXPR_MEMBER && e->lhs->lhs) {
            compile_expr(c, e->lhs->lhs);
            const char *recv_ty = c->expr_type_name;
            uint32_t field_idx = 0;
            const SnDecl *field = NULL;
            if (struct_field_at(find_struct(c, recv_ty), e->lhs->text, &field_idx, &field)) {
                emit_byte(c, OP_SET_FIELD, line);
                emit_u32(c, field_idx, line);
                produced = field_type_name(field);
                stored = 1;
            }
        }
        if (!stored) {
            emit_unsupported(c, e->span, "assignment");
        }
        break;
    }
    case SN_EXPR_BINARY:
        if (e->op == SN_TOK_ANDAND) {
            emit_andand(c, e, line);
            break;
        }
        if (e->op == SN_TOK_OROR) {
            emit_oror(c, e, line);
            break;
        }
        compile_expr(c, e->lhs);
        compile_expr(c, e->rhs);
        switch (e->op) {
        case SN_TOK_PLUS:    emit_byte(c, OP_ADD, line); break;
        case SN_TOK_MINUS:   emit_byte(c, OP_SUB, line); break;
        case SN_TOK_STAR:    emit_byte(c, OP_MUL, line); break;
        case SN_TOK_SLASH:   emit_byte(c, OP_DIV, line); break;
        case SN_TOK_PERCENT: emit_byte(c, OP_MOD, line); break;
        case SN_TOK_AMP:     emit_byte(c, OP_BIT_AND, line); break;
        case SN_TOK_PIPE:    emit_byte(c, OP_BIT_OR, line); break;
        case SN_TOK_CARET:   emit_byte(c, OP_BIT_XOR, line); break;
        case SN_TOK_SHL:     emit_byte(c, OP_SHL, line); break;
        case SN_TOK_SHR:     emit_byte(c, OP_SHR, line); break;
        case SN_TOK_EQ:      emit_byte(c, OP_EQ, line); break;
        case SN_TOK_NE:      emit_byte(c, OP_NE, line); break;
        case SN_TOK_LT:      emit_byte(c, OP_LT, line); break;
        case SN_TOK_LE:      emit_byte(c, OP_LE, line); break;
        case SN_TOK_GT:      emit_byte(c, OP_GT, line); break;
        case SN_TOK_GE:      emit_byte(c, OP_GE, line); break;
        default:
            emit_unsupported(c, e->span, "operator");
            break;
        }
        break;
    case SN_EXPR_UNARY:
        compile_expr(c, e->lhs);
        if (e->op == SN_TOK_MINUS) {
            emit_byte(c, OP_NEG, line);
        } else if (e->op == SN_TOK_BANG) {
            emit_byte(c, OP_NOT, line);
        } else {
            emit_unsupported(c, e->span, "operator");
        }
        break;
    case SN_EXPR_CALL: {
        if (e->lhs && e->lhs->kind == SN_EXPR_MEMBER && e->lhs->text &&
            (strcmp(e->lhs->text, "printline") == 0 || strcmp(e->lhs->text, "println") == 0 || strcmp(e->lhs->text, "print") == 0)) {
            int is_nl = strcmp(e->lhs->text, "print") != 0;
            if (e->args.len > 0) {
                compile_expr(c, SN_LIST_AT(e->args, SnExpr, 0));
            } else {
                uint32_t s_idx = sn_bcunit_add_string(c->bc, "");
                emit_byte(c, OP_CONST_STRING, line);
                emit_u32(c, s_idx, line);
            }
            emit_byte(c, OP_PRINT, line);
            emit_byte(c, (uint8_t)is_nl, line);
            emit_byte(c, OP_CONST_UNIT, line);
            break;
        }
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT && e->lhs->text &&
            (strcmp(e->lhs->text, "printline") == 0 || strcmp(e->lhs->text, "println") == 0 || strcmp(e->lhs->text, "print") == 0)) {
            int is_nl = strcmp(e->lhs->text, "print") != 0;
            if (e->args.len > 0) {
                compile_expr(c, SN_LIST_AT(e->args, SnExpr, 0));
            } else {
                uint32_t s_idx = sn_bcunit_add_string(c->bc, "");
                emit_byte(c, OP_CONST_STRING, line);
                emit_u32(c, s_idx, line);
            }
            emit_byte(c, OP_PRINT, line);
            emit_byte(c, (uint8_t)is_nl, line);
            emit_byte(c, OP_CONST_UNIT, line);
            break;
        }

        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT && e->lhs->text &&
            strcmp(e->lhs->text, "read_bytes") == 0 && e->args.len == 1) {
            compile_expr(c, SN_LIST_AT(e->args, SnExpr, 0));
            emit_byte(c, OP_READ_BYTES, line);
            break;
        }
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT && e->lhs->text &&
            strcmp(e->lhs->text, "write_bytes") == 0 && e->args.len == 2) {
            compile_expr(c, SN_LIST_AT(e->args, SnExpr, 0));
            compile_expr(c, SN_LIST_AT(e->args, SnExpr, 1));
            emit_byte(c, OP_WRITE_BYTES, line);
            break;
        }
        if (emit_array_method(c, e, line, &produced)) {
            break;
        }

        uint32_t fn_idx = 0;
        int found = 0;
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT) {
            found = resolve_func(c, e->lhs->text, &fn_idx);
        }
        for (size_t i = 0; i < e->args.len; i++) {
            compile_expr(c, SN_LIST_AT(e->args, SnExpr, i));
        }
        if (found) {
            emit_byte(c, OP_CALL, line);
            emit_u32(c, fn_idx, line);
            emit_u32(c, (uint32_t)e->args.len, line);
        } else {
            emit_unsupported(c, e->span, "call");
        }
        break;
    }
    case SN_EXPR_ARRAY: {
        for (size_t i = 0; i < e->args.len; i++) {
            compile_expr(c, SN_LIST_AT(e->args, SnExpr, i));
        }
        emit_byte(c, OP_NEW_ARRAY, line);
        emit_u32(c, (uint32_t)e->args.len, line);
        break;
    }
    case SN_EXPR_INDEX: {
        compile_expr(c, e->lhs);
        compile_expr(c, e->rhs);
        emit_byte(c, OP_GET_INDEX, line);
        break;
    }
    case SN_EXPR_STRUCT_LIT: {
        const char *tname = (e->lhs && e->lhs->kind == SN_EXPR_IDENT) ? e->lhs->text : NULL;
        const SnDecl *st = find_struct(c, tname);
        if (!st) {
            emit_unsupported(c, e->span, "struct literal");
            break;
        }
        for (size_t j = 0; j < e->field_names.len; j++) {
            const char *fname = (const char *)e->field_names.items[j];
            if (!struct_field_at(st, fname, NULL, NULL)) {
                emit_unsupported(c, e->span, "struct field");
                break;
            }
        }
        if (c->failed) {
            break;
        }
        uint32_t nfields = 0;
        for (size_t i = 0; i < st->members.len; i++) {
            const SnDecl *m = SN_LIST_AT(st->members, SnDecl, i);
            if (!m || m->kind != SN_DECL_FIELD) {
                continue;
            }
            const SnExpr *val = NULL;
            for (size_t j = 0; j < e->field_names.len; j++) {
                const char *fname = (const char *)e->field_names.items[j];
                if (m->name && fname && strcmp(fname, m->name) == 0) {
                    val = SN_LIST_AT(e->args, SnExpr, j);
                    break;
                }
            }
            if (val) {
                compile_expr(c, val);
            } else {
                emit_default_for_type(c, m->type, line);
            }
            if (nfields == UINT32_MAX) {
                emit_unsupported(c, e->span, "struct literal");
                break;
            }
            nfields++;
        }
        if (c->failed) {
            break;
        }
        uint32_t class_idx = sn_bcunit_add_string(c->bc, tname ? tname : "");
        emit_byte(c, OP_NEW_OBJ, line);
        emit_u32(c, class_idx, line);
        emit_u32(c, nfields, line);
        produced = tname;
        break;
    }
    case SN_EXPR_MEMBER: {
        if (!e->lhs) {
            emit_unsupported(c, e->span, "field");
            break;
        }
        compile_expr(c, e->lhs);
        const char *recv_ty = c->expr_type_name;
        uint32_t field_idx = 0;
        const SnDecl *field = NULL;
        if (!struct_field_at(find_struct(c, recv_ty), e->text, &field_idx, &field)) {
            emit_unsupported(c, e->span, "field");
            break;
        }
        emit_byte(c, OP_GET_FIELD, line);
        emit_u32(c, field_idx, line);
        produced = field_type_name(field);
        break;
    }
    default:
        emit_unsupported(c, e->span, "expression");
        break;
    }
    c->expr_type_name = produced;
}

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
