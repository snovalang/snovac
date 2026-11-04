#include "emit_bc_internal.h"
void emit_i64(Compiler *c, int64_t val, uint32_t line) {
    sn_chunk_write_i64(&c->current_fn->chunk, val, line);
}

void emit_double(Compiler *c, double val, uint32_t line) {
    sn_chunk_write_double(&c->current_fn->chunk, val, line);
}

