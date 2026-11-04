/* emit_bc_expr.c — expression lowering to SnBC. */
#include "emit_bc_internal.h"
#include <stdlib.h>
#include <string.h>

void compile_expr(Compiler *c, const SnExpr *e) {
    if (c->failed) return;
    if (!e) {
        emit_unsupported(c, (SnSpan){0}, "a missing expression"); return;
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
            emit_byte(c, e->implicit_move ? OP_MOVE_LOCAL : OP_GET_LOCAL, line);
            emit_u32(c, loc_idx, line);
        } else {
            uint32_t fn_idx;
            if (resolve_func(c, e->text, &fn_idx)) {
                emit_byte(c, OP_CONST_INT, line);
                emit_i64(c, (int64_t)fn_idx, line);
            } else {
                emit_unsupported(c, e->span, "global or unresolved identifiers");
            }
        }
        break;
    }
    case SN_EXPR_ASSIGN: {
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT) {
            compile_expr(c, e->rhs);
            uint32_t loc_idx;
            if (resolve_local(c, e->lhs->text, &loc_idx)) {
                emit_byte(c, OP_SET_LOCAL, line);
                emit_u32(c, loc_idx, line);
            } else {
                emit_unsupported(c, e->span, "assignment to globals");
            }
        } else if (e->lhs && e->lhs->kind == SN_EXPR_INDEX) {
            compile_expr(c, e->lhs->lhs);
            compile_expr(c, e->lhs->rhs);
            compile_expr(c, e->rhs);
            emit_byte(c, OP_SET_INDEX, line);
        } else {
            emit_unsupported(c, e->span, "this assignment target");
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
        default: emit_unsupported(c, e->span, "this binary operator"); break;
        }
        break;
    case SN_EXPR_UNARY:
        compile_expr(c, e->lhs);
        if (e->op == SN_TOK_MINUS) {
            emit_byte(c, OP_NEG, line);
        } else if (e->op == SN_TOK_BANG) {
            emit_byte(c, OP_NOT, line);
        } else {
            emit_unsupported(c, e->span, "this unary operator");
        }
        break;
    case SN_EXPR_MOVE: {
        uint32_t slot;
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT &&
            resolve_local(c, e->lhs->text, &slot)) {
            emit_byte(c, OP_MOVE_LOCAL, line);
            emit_u32(c, slot, line);
        } else {
            emit_unsupported(c, e->span, "this move expression");
        }
        break;
    }
    case SN_EXPR_CALL: {
        if (e->lhs && e->lhs->kind == SN_EXPR_MEMBER && e->lhs->lhs &&
            e->lhs->text) {
            const char *method = e->lhs->text;
            if (strcmp(method, "push") == 0 && e->args.len == 1) {
                compile_expr(c, e->lhs->lhs);
                compile_expr(c, SN_LIST_AT(e->args, SnExpr, 0));
                emit_byte(c, OP_ARRAY_PUSH, line);
                break;
            }
            if (strcmp(method, "pop") == 0 && e->args.len == 0) {
                compile_expr(c, e->lhs->lhs);
                emit_byte(c, OP_ARRAY_POP, line);
                break;
            }
            if (strcmp(method, "clear") == 0 && e->args.len == 0) {
                compile_expr(c, e->lhs->lhs);
                emit_byte(c, OP_ARRAY_CLEAR, line);
                break;
            }
        }
        if (e->lhs && e->lhs->kind == SN_EXPR_MEMBER && e->lhs->lhs &&
            e->lhs->lhs->kind == SN_EXPR_IDENT && e->lhs->lhs->text &&
            strcmp(e->lhs->lhs->text, "Console") == 0 && e->lhs->text &&
            (strcmp(e->lhs->text, "printline") == 0 || strcmp(e->lhs->text, "print") == 0)) {
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
            emit_unsupported(c, e->span, "this call (only direct known functions and supported intrinsics are available)");
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
        emit_unsupported(c, e->span, "this expression");
        break;
    }
}
