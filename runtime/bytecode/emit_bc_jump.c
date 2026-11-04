#include "emit_bc_internal.h"
void patch_jump(Compiler *c, size_t jump_offset_pos) {
    int32_t offset = (int32_t)(c->current_fn->chunk.count - (jump_offset_pos + 4));
    uint8_t *p = &c->current_fn->chunk.code[jump_offset_pos];
    p[0] = (uint8_t)(offset & 0xFF);
    p[1] = (uint8_t)((offset >> 8) & 0xFF);
    p[2] = (uint8_t)((offset >> 16) & 0xFF);
    p[3] = (uint8_t)((offset >> 24) & 0xFF);
}

