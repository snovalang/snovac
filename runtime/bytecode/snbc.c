#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "snbc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sn_bc_oom(void) {
    fputs("snovac bytecode: out of memory\n", stderr);
    abort();
}

static void *sn_bc_malloc(size_t size) {
    void *ptr = malloc(size ? size : 1u);
    if (!ptr) sn_bc_oom();
    return ptr;
}

static void *sn_bc_realloc(void *old, size_t size) {
    void *ptr = realloc(old, size ? size : 1u);
    if (!ptr) sn_bc_oom();
    return ptr;
}

static size_t sn_bc_next_capacity(size_t current, size_t minimum, size_t item_size) {
    size_t next = current < 8u ? 8u : current;
    while (next < minimum) {
        if (next > SIZE_MAX / 2u) {
            next = minimum;
            break;
        }
        next *= 2u;
    }
    if (next > SIZE_MAX / item_size) {
        fputs("snovac bytecode: capacity overflow\n", stderr);
        abort();
    }
    return next;
}

void sn_chunk_init(SnChunk *chunk) {
    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->count = 0;
    chunk->capacity = 0;
}

void sn_chunk_free(SnChunk *chunk) {
    free(chunk->code);
    free(chunk->lines);
    sn_chunk_init(chunk);
}

void sn_chunk_write(SnChunk *chunk, uint8_t byte, uint32_t line) {
    if (chunk->count == SIZE_MAX) {
        fputs("snovac bytecode: chunk size overflow\n", stderr);
        abort();
    }
    if (chunk->capacity < chunk->count + 1u) {
        size_t capacity = sn_bc_next_capacity(chunk->capacity, chunk->count + 1u,
                                              sizeof(uint32_t));
        uint8_t *code = (uint8_t *)sn_bc_realloc(chunk->code, capacity);
        uint32_t *lines = (uint32_t *)sn_bc_realloc(chunk->lines,
                                                    capacity * sizeof(uint32_t));
        chunk->code = code;
        chunk->lines = lines;
        chunk->capacity = capacity;
    }
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

void sn_chunk_write_u32(SnChunk *chunk, uint32_t val, uint32_t line) {
    for (int i = 0; i < 4; i++) {
        sn_chunk_write(chunk, (uint8_t)((val >> (i * 8)) & 0xFF), line);
    }
}

void sn_chunk_write_i64(SnChunk *chunk, int64_t val, uint32_t line) {
    uint64_t u = (uint64_t)val;
    for (int i = 0; i < 8; i++) {
        sn_chunk_write(chunk, (uint8_t)((u >> (i * 8)) & 0xFF), line);
    }
}

void sn_chunk_write_double(SnChunk *chunk, double val, uint32_t line) {
    union {
        double d;
        uint64_t u;
    } cvt;
    cvt.d = val;
    for (int i = 0; i < 8; i++) {
        sn_chunk_write(chunk, (uint8_t)((cvt.u >> (i * 8)) & 0xFF), line);
    }
}

void sn_bcunit_init(SnBCUnit *unit) {
    unit->string_pool.strings = NULL;
    unit->string_pool.count = 0;
    unit->string_pool.capacity = 0;
    unit->functions = NULL;
    unit->function_count = 0;
    unit->function_capacity = 0;
    unit->main_func_idx = 0;
}

void sn_bcunit_free(SnBCUnit *unit) {
    for (size_t i = 0; i < unit->string_pool.count; i++) {
        free(unit->string_pool.strings[i]);
    }
    free(unit->string_pool.strings);
    for (size_t i = 0; i < unit->function_count; i++) {
        sn_chunk_free(&unit->functions[i]->chunk);
        free(unit->functions[i]->name);
        free(unit->functions[i]);
    }
    free(unit->functions);
    sn_bcunit_init(unit);
}

uint32_t sn_bcunit_add_string(SnBCUnit *unit, const char *str) {
    for (size_t i = 0; i < unit->string_pool.count; i++) {
        if (strcmp(unit->string_pool.strings[i], str) == 0) {
            return (uint32_t)i;
        }
    }
    if (unit->string_pool.count >= UINT32_MAX) {
        fputs("snovac bytecode: too many string constants\n", stderr);
        abort();
    }
    if (unit->string_pool.capacity < unit->string_pool.count + 1u) {
        size_t capacity = sn_bc_next_capacity(unit->string_pool.capacity,
                                              unit->string_pool.count + 1u,
                                              sizeof(char *));
        unit->string_pool.strings = (char **)sn_bc_realloc(
            unit->string_pool.strings, capacity * sizeof(char *));
        unit->string_pool.capacity = capacity;
    }
    uint32_t idx = (uint32_t)unit->string_pool.count;
    unit->string_pool.strings[idx] = strdup(str);
    if (!unit->string_pool.strings[idx]) sn_bc_oom();
    unit->string_pool.count++;
    return idx;
}

uint32_t sn_bcunit_add_function(SnBCUnit *unit, const char *name, uint32_t arity) {
    if (unit->function_count >= UINT32_MAX) {
        fputs("snovac bytecode: too many functions\n", stderr);
        abort();
    }
    if (unit->function_capacity < unit->function_count + 1) {
        size_t capacity = sn_bc_next_capacity(unit->function_capacity,
                                              unit->function_count + 1u,
                                              sizeof(SnFunctionChunk *));
        unit->functions = (SnFunctionChunk **)sn_bc_realloc(
            unit->functions, capacity * sizeof(SnFunctionChunk *));
        unit->function_capacity = capacity;
    }
    SnFunctionChunk *fn = (SnFunctionChunk *)sn_bc_malloc(sizeof(SnFunctionChunk));
    fn->name = name ? strdup(name) : NULL;
    if (name && !fn->name) sn_bc_oom();
    fn->arity = arity;
    fn->local_count = arity;
    sn_chunk_init(&fn->chunk);

    uint32_t idx = (uint32_t)unit->function_count;
    unit->functions[idx] = fn;
    unit->function_count++;
    return idx;
}

int sn_bcunit_merge(SnBCUnit *dest, const SnBCUnit *src) {
    if (!dest || !src) return 0;
    for (size_t i = 0; i < src->function_count; i++) {
        SnFunctionChunk *sfn = src->functions[i];
        uint32_t fidx = sn_bcunit_add_function(dest, sfn->name, sfn->arity);
        dest->functions[fidx]->local_count = sfn->local_count;
        for (size_t ci = 0; ci < sfn->chunk.count; ci++) {
            sn_chunk_write(&dest->functions[fidx]->chunk, sfn->chunk.code[ci], 1);
        }
    }
    return 1;
}
