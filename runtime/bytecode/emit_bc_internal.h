#ifndef SNOVAC_EMIT_BC_INTERNAL_H
#define SNOVAC_EMIT_BC_INTERNAL_H
#include "emit_bc.h"
typedef struct { const char *name; uint32_t index; } LocalVar;
typedef struct BreakJump {
    size_t offset;
    struct BreakJump *next;
} BreakJump;
typedef struct LoopContext {
    size_t start;
    uint32_t local_depth;
    BreakJump *breaks;
    struct LoopContext *parent;
} LoopContext;
typedef struct {
    SnArena *arena;
    SnDiagSink *diag;
    const SnUnit *unit;
    SnBCUnit *bc;
    SnFunctionChunk *current_fn;
    LocalVar *locals;
    size_t locals_capacity;
    uint32_t local_count;
    LoopContext *loop;
    int failed;
} Compiler;
void sn_compile_stmt(Compiler *, const SnStmt *);
void compile_expr(Compiler *, const SnExpr *);
void compile_return_expr(Compiler *, const SnExpr *);
uint32_t add_local(Compiler *, const char *, SnSpan);
void emit_byte(Compiler *, uint8_t, uint32_t);
void emit_u32(Compiler *, uint32_t, uint32_t);
void emit_i64(Compiler *, int64_t, uint32_t);
void emit_double(Compiler *, double, uint32_t);
size_t emit_jump(Compiler *, SnOpcode, uint32_t);
void patch_jump(Compiler *, size_t);
void emit_unsupported(Compiler *, SnSpan, const char *);
#endif
