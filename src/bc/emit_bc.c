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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    uint32_t index;
} LocalVar;

typedef struct {
    SnArena *arena;
    SnDiagSink *diag;
    const SnUnit *unit;
    SnBCUnit *bc;

    SnFunctionChunk *current_fn;
    LocalVar locals[256];
    uint32_t local_count;
    int failed;
} Compiler;

static void emit_unsupported(Compiler *c, SnSpan span, const char *what) {
    c->failed = 1;
    if (c->diag) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_EMIT_UNSUPPORTED, span,
                     "cannot lower %s to SnBC\n", what);
    }
}

static uint32_t add_local(Compiler *c, const char *name, SnSpan span) {
    if (c->local_count >= 256) {
        emit_unsupported(c, span, "local");
        return 0;
    }
    uint32_t idx = c->local_count++;
    c->locals[idx].name = name;
    c->locals[idx].index = idx;
    if (c->local_count > c->current_fn->local_count) {
        c->current_fn->local_count = c->local_count;
    }
    return idx;
}

static int resolve_local(Compiler *c, const char *name, uint32_t *out_idx) {
    if (!name) return 0;
    for (int i = (int)c->local_count - 1; i >= 0; i--) {
        if (c->locals[i].name && strcmp(c->locals[i].name, name) == 0) {
            *out_idx = c->locals[i].index;
            return 1;
        }
    }
    return 0;
}

static int resolve_func(Compiler *c, const char *name, uint32_t *out_idx) {
    for (size_t i = 0; i < c->bc->function_count; i++) {
        if (c->bc->functions[i]->name && strcmp(c->bc->functions[i]->name, name) == 0) {
            *out_idx = (uint32_t)i;
            return 1;
        }
    }
    return 0;
}

static void emit_byte(Compiler *c, uint8_t b, uint32_t line) {
    sn_chunk_write(&c->current_fn->chunk, b, line);
}

static void emit_u32(Compiler *c, uint32_t val, uint32_t line) {
    sn_chunk_write_u32(&c->current_fn->chunk, val, line);
}

static void emit_i64(Compiler *c, int64_t val, uint32_t line) {
    sn_chunk_write_i64(&c->current_fn->chunk, val, line);
}

static void emit_double(Compiler *c, double val, uint32_t line) {
    sn_chunk_write_double(&c->current_fn->chunk, val, line);
}

static size_t emit_jump(Compiler *c, SnOpcode op, uint32_t line) {
    emit_byte(c, (uint8_t)op, line);
    size_t pos = c->current_fn->chunk.count;
    emit_u32(c, 0, line); /* placeholder */
    return pos;
}

static void patch_jump(Compiler *c, size_t jump_offset_pos) {
    int32_t offset = (int32_t)(c->current_fn->chunk.count - (jump_offset_pos + 4));
    uint8_t *p = &c->current_fn->chunk.code[jump_offset_pos];
    p[0] = (uint8_t)(offset & 0xFF);
    p[1] = (uint8_t)((offset >> 8) & 0xFF);
    p[2] = (uint8_t)((offset >> 16) & 0xFF);
    p[3] = (uint8_t)((offset >> 24) & 0xFF);
}

static void compile_expr(Compiler *c, const SnExpr *e);
static void compile_stmt(Compiler *c, const SnStmt *s);

/* A type declaration has no opcode. It is a dropped program only when it
 * carries code this pass does not lower: a nested routine, an initializer,
 * or an accessor projection. A field-only struct is not one of those. */
static void reject_unlowered_decl(Compiler *c, const SnDecl *d) {
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

static void compile_expr(Compiler *c, const SnExpr *e) {
    if (!e) {
        emit_unsupported(c, (SnSpan){0}, "missing expression");
        return;
    }
    uint32_t line = e->span.line;

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
            return;
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
        compile_expr(c, e->rhs);
        int stored = 0;
        if (e->op == SN_TOK_ASSIGN && e->lhs && e->lhs->kind == SN_EXPR_IDENT) {
            uint32_t loc_idx;
            if (resolve_local(c, e->lhs->text, &loc_idx)) {
                emit_byte(c, OP_SET_LOCAL, line);
                emit_u32(c, loc_idx, line);
                stored = 1;
            }
        }
        if (!stored) {
            emit_unsupported(c, e->span, "assignment");
        }
        break;
    }
    case SN_EXPR_BINARY:
        compile_expr(c, e->lhs);
        compile_expr(c, e->rhs);
        switch (e->op) {
        case SN_TOK_PLUS:    emit_byte(c, OP_ADD, line); break;
        case SN_TOK_MINUS:   emit_byte(c, OP_SUB, line); break;
        case SN_TOK_STAR:    emit_byte(c, OP_MUL, line); break;
        case SN_TOK_SLASH:   emit_byte(c, OP_DIV, line); break;
        case SN_TOK_PERCENT: emit_byte(c, OP_MOD, line); break;
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
    default:
        emit_unsupported(c, e->span, "expression");
        break;
    }
}

static void compile_stmt(Compiler *c, const SnStmt *s) {
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
        compile_expr(c, s->expr);
        size_t exit_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
        compile_stmt(c, s->then_br);
        emit_byte(c, OP_JUMP, line);
        int32_t loop_offset = (int32_t)(loop_start - (c->current_fn->chunk.count + 4));
        emit_u32(c, (uint32_t)loop_offset, line);
        patch_jump(c, exit_jump);
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
    c.failed = 0;

    /* First pass: register function prototypes. Any other declaration is
     * still in the program, so dropping it would publish a partial image. */
    for (size_t i = 0; i < unit->decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(unit->decls, SnDecl, i);
        if (d->kind == SN_DECL_FUNC || d->kind == SN_DECL_METHOD) {
            uint32_t fn_idx = sn_bcunit_add_function(out, d->name, (uint32_t)d->params.len);
            if (d->name && strcmp(d->name, "main") == 0) {
                out->main_func_idx = fn_idx;
            }
        } else {
            reject_unlowered_decl(&c, d);
        }
    }

    /* Second pass: compile function bodies */
    for (size_t i = 0; i < unit->decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(unit->decls, SnDecl, i);
        if (d->kind == SN_DECL_FUNC || d->kind == SN_DECL_METHOD) {
            uint32_t fn_idx = 0;
            resolve_func(&c, d->name, &fn_idx);
            c.current_fn = out->functions[fn_idx];
            c.local_count = 0;

            for (size_t pi = 0; pi < d->params.len; pi++) {
                const SnParam *p = SN_LIST_AT(d->params, SnParam, pi);
                add_local(&c, p->name, p->span);
            }

            if (d->body) {
                compile_stmt(&c, d->body);
            } else {
                emit_unsupported(&c, d->span, "function without a body");
            }
            emit_byte(&c, OP_CONST_UNIT, d->span.line);
            emit_byte(&c, OP_RETURN, d->span.line);
        }
    }

    return c.failed ? 0 : 1;
}
