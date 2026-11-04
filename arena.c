#include "arena.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SN_ARENA_ALIGN 16u

struct SnArenaBlock {
    SnArenaBlock *next;
    size_t used;
    size_t cap;
    _Alignas(SN_ARENA_ALIGN) unsigned char data[];
};

static size_t align_up(size_t n) {
    if (n > SIZE_MAX - (SN_ARENA_ALIGN - 1u)) {
        return 0;
    }
    return (n + (SN_ARENA_ALIGN - 1u)) & ~((size_t)SN_ARENA_ALIGN - 1u);
}

void sn_arena_init(SnArena *a, size_t block_size) {
    a->head = NULL;
    a->block_size = block_size ? block_size : (size_t)(64 * 1024);
    a->total_bytes = 0;
    a->used_bytes = 0;
    a->requested_bytes = 0;
    a->peak_used_bytes = 0;
    a->block_count = 0;
    a->allocation_count = 0;
}

static void oom(size_t size) {
    fprintf(stderr, "snovac: fatal: out of memory allocating %zu bytes\n", size);
    exit(70); /* EX_SOFTWARE */
}

static SnArenaBlock *push_block(SnArena *a, size_t need) {
    size_t cap = a->block_size;
    if (cap < need) {
        cap = align_up(need);
    }
    if (!cap || cap > SIZE_MAX - sizeof(SnArenaBlock) ||
        a->total_bytes > SIZE_MAX - cap || a->block_count == SIZE_MAX) {
        oom(need);
    }
    SnArenaBlock *b = (SnArenaBlock *)malloc(sizeof(SnArenaBlock) + cap);
    if (!b) {
        oom(cap);
    }
    b->next = a->head;
    b->used = 0;
    b->cap = cap;
    a->head = b;
    a->total_bytes += cap;
    a->block_count++;
    return b;
}

void *sn_arena_alloc(SnArena *a, size_t size) {
    size_t need = align_up(size ? size : 1u);
    if (!need || a->requested_bytes > SIZE_MAX - size ||
        a->used_bytes > SIZE_MAX - need || a->allocation_count == SIZE_MAX) {
        oom(size);
    }
    SnArenaBlock *b = a->head;
    if (!b || b->cap - b->used < need) {
        b = push_block(a, need);
    }
    void *p = b->data + b->used;
    b->used += need;
    a->used_bytes += need;
    a->requested_bytes += size;
    a->allocation_count++;
    if (a->used_bytes > a->peak_used_bytes) a->peak_used_bytes = a->used_bytes;
    return p;
}

void *sn_arena_calloc(SnArena *a, size_t size) {
    void *p = sn_arena_alloc(a, size);
    memset(p, 0, size);
    return p;
}

char *sn_arena_strndup(SnArena *a, const char *s, size_t n) {
    if (n == SIZE_MAX) oom(n);
    char *p = (char *)sn_arena_alloc(a, n + 1u);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

SnArenaStats sn_arena_get_stats(const SnArena *a) {
    SnArenaStats stats;
    stats.reserved_bytes = a ? a->total_bytes : 0;
    stats.used_bytes = a ? a->used_bytes : 0;
    stats.requested_bytes = a ? a->requested_bytes : 0;
    stats.peak_used_bytes = a ? a->peak_used_bytes : 0;
    stats.block_count = a ? a->block_count : 0;
    stats.allocation_count = a ? a->allocation_count : 0;
    return stats;
}

void sn_arena_report_stats(FILE *stream, const char *label, const SnArena *a) {
    SnArenaStats stats = sn_arena_get_stats(a);
    if (!stream) stream = stderr;
    fprintf(stream,
            "memory[%s]: requested=%zu used=%zu reserved=%zu slack=%zu peak=%zu blocks=%zu allocations=%zu\n",
            label ? label : "arena", stats.requested_bytes, stats.used_bytes,
            stats.reserved_bytes,
            stats.reserved_bytes >= stats.used_bytes
                ? stats.reserved_bytes - stats.used_bytes : 0,
            stats.peak_used_bytes, stats.block_count, stats.allocation_count);
}

void sn_arena_free(SnArena *a) {
    SnArenaBlock *b = a->head;
    while (b) {
        SnArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
    a->total_bytes = 0;
    a->used_bytes = 0;
    a->requested_bytes = 0;
    a->peak_used_bytes = 0;
    a->block_count = 0;
    a->allocation_count = 0;
}
