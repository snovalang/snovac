/* emit_internal.h — shared state for SnBC lowering. */
#ifndef SNOVAC_EMIT_INTERNAL_H
#define SNOVAC_EMIT_INTERNAL_H

#include "emit_bc.h"

#include <stddef.h>
#include <stdint.h>

#define EB_LOOPS 8
#define EB_PATCH 24
#define EB_FUNCS 512

typedef struct {
    const char *name;
    const char *type_name;
    uint32_t index;
} EbLocal;

typedef struct Compiler {
    SnArena *arena;
    SnDiagSink *diag;
    const SnUnit *unit;
    SnBCUnit *bc;

    SnFunctionChunk *current_fn;
    EbLocal locals[256];
    uint32_t local_count;
    const char *expr_type_name;
    int failed;

    const SnDecl *owner;
    int loop_top;
    int cont_known[EB_LOOPS];
    size_t cont_pos[EB_LOOPS];
    size_t break_at[EB_LOOPS][EB_PATCH];
    int break_n[EB_LOOPS];
    size_t cont_at[EB_LOOPS][EB_PATCH];
    int cont_n[EB_LOOPS];
    uint8_t fn_async[EB_FUNCS];
    uint8_t fn_pulsar[EB_FUNCS];
} Compiler;

void eb_emit_unsupported(Compiler *c, SnSpan span, const char *what);
uint32_t eb_add_local(Compiler *c, const char *name, SnSpan span);
int eb_resolve_local(Compiler *c, const char *name, uint32_t *out_idx);
int eb_resolve_func(Compiler *c, const char *name, uint32_t *out_idx);
void eb_emit_byte(Compiler *c, uint8_t b, uint32_t line);
void eb_emit_u32(Compiler *c, uint32_t val, uint32_t line);
void eb_emit_i64(Compiler *c, int64_t val, uint32_t line);
size_t eb_emit_jump(Compiler *c, SnOpcode op, uint32_t line);
void eb_patch_jump(Compiler *c, size_t jump_offset_pos);
void eb_emit_default_for_type(Compiler *c, const SnType *type, uint32_t line);
void eb_compile_expr(Compiler *c, const SnExpr *e);
void eb_compile_stmt(Compiler *c, const SnStmt *s);

void eb_loop_push(Compiler *c, int known, size_t pos);
void eb_loop_continue(Compiler *c, uint32_t line);
void eb_loop_break(Compiler *c, uint32_t line);
void eb_loop_patch_continues(Compiler *c);
void eb_loop_patch_breaks(Compiler *c);
void eb_loop_pop(Compiler *c);

int eb_try_expr(Compiler *c, const SnExpr *e);
int eb_try_stmt(Compiler *c, const SnStmt *s);
void eb_register_unit(Compiler *c);
void eb_compile_unit(Compiler *c);
void eb_reject_decl(Compiler *c, const SnDecl *d);

#endif /* SNOVAC_EMIT_INTERNAL_H */
