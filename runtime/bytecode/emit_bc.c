/* emit_bc.c — AST to SnBC bytecode code generation. */
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

#include "emit_bc_internal.h"

void emit_unsupported(Compiler *c, SnSpan span, const char *feature) {
    if (!c || c->failed) return;
    sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_BYTECODE_UNSUPPORTED, span,
                 "bytecode backend does not support %s yet", feature);
    c->failed = 1;
}

uint32_t add_local(Compiler *c, const char *name, SnSpan span) {
    if (c->failed) return 0;
    if (c->local_count >= SN_BC_VM_STACK_CAPACITY) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_TOO_MANY_LOCALS, span,
                     "function exceeds the VM limit of %u simultaneous local slots",
                     (unsigned)SN_BC_VM_STACK_CAPACITY);
        c->failed = 1;
        return 0;
    }
    if ((size_t)c->local_count == c->locals_capacity) {
        size_t capacity = c->locals_capacity ? c->locals_capacity * 2u : 16u;
        if (capacity > SN_BC_VM_STACK_CAPACITY) {
            capacity = SN_BC_VM_STACK_CAPACITY;
        }
        LocalVar *locals = (LocalVar *)sn_arena_alloc(
            c->arena, capacity * sizeof(*locals));
        if (c->local_count) {
            memcpy(locals, c->locals, (size_t)c->local_count * sizeof(*locals));
        }
        c->locals = locals;
        c->locals_capacity = capacity;
    }
    uint32_t idx = c->local_count++;
    c->locals[idx].name = name;
    c->locals[idx].index = idx;
    if (c->local_count > c->current_fn->local_count) {
        c->current_fn->local_count = c->local_count;
    }
    return idx;
}

int resolve_local(Compiler *c, const char *name, uint32_t *out_idx) {
    if (!name) return 0;
    for (size_t i = c->local_count; i > 0; --i) {
        const LocalVar *local = &c->locals[i - 1u];
        if (local->name && strcmp(local->name, name) == 0) {
            *out_idx = local->index;
            return 1;
        }
    }
    return 0;
}

void compile_return_expr(Compiler *c, const SnExpr *expr) {
    uint32_t slot;
    if (expr && expr->kind == SN_EXPR_IDENT &&
        resolve_local(c, expr->text, &slot)) {
        emit_byte(c, OP_MOVE_LOCAL, expr->span.line);
        emit_u32(c, slot, expr->span.line);
        return;
    }
    compile_expr(c, expr);
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

size_t emit_jump(Compiler *c, SnOpcode op, uint32_t line) {
    emit_byte(c, (uint8_t)op, line);
    size_t pos = c->current_fn->chunk.count;
    emit_u32(c, 0, line); /* placeholder */
    return pos;
}


int sn_emit_bytecode(SnArena *arena, SnDiagSink *diag, const SnUnit *unit, SnBCUnit *out) {
    if (!arena || !diag || !unit || !out) return 0;
    sn_bcunit_init(out);

    Compiler c;
    memset(&c, 0, sizeof(c));
    c.arena = arena;
    c.diag = diag;
    c.unit = unit;
    c.bc = out;

    /* First pass: register function prototypes */
    for (size_t i = 0; i < unit->decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(unit->decls, SnDecl, i);
        if (d && d->kind == SN_DECL_FUNC) {
            uint32_t fn_idx = sn_bcunit_add_function(out, d->name, (uint32_t)d->params.len);
            if (strcmp(d->name, "main") == 0) {
                out->main_func_idx = fn_idx;
            }
        } else if (d) {
            emit_unsupported(&c, d->span, "this top-level declaration");
        }
    }

    /* Second pass: compile function bodies */
    for (size_t i = 0; i < unit->decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(unit->decls, SnDecl, i);
        if (!d || d->kind != SN_DECL_FUNC) continue;
        {
            uint32_t fn_idx = 0;
            resolve_func(&c, d->name, &fn_idx);
            c.current_fn = out->functions[fn_idx];
            c.local_count = 0;
            c.loop = NULL;

            for (size_t pi = 0; pi < d->params.len; pi++) {
                const SnParam *p = SN_LIST_AT(d->params, SnParam, pi);
                add_local(&c, p->name, p->span);
            }

            if (d->body) {
                sn_compile_stmt(&c, d->body);
            }
            emit_byte(&c, OP_CONST_UNIT, d->span.line);
            emit_byte(&c, OP_RETURN, d->span.line);
        }
    }

    if (c.failed) {
        sn_bcunit_free(out);
        return 0;
    }
    return 1;
}
