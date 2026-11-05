/* arena.h — bump allocator.
 *
 * Every AST node, token vector and interned string lives in an arena and is
 * freed in one shot by sn_arena_free(). Nothing in snovac frees an individual
 * node, so there is no ownership bookkeeping in the front-end at all.
 */
#ifndef SNOVAC_ARENA_H
#define SNOVAC_ARENA_H

#include <stddef.h>
#include <stdio.h>

typedef struct SnArenaBlock SnArenaBlock;

typedef struct {
    size_t reserved_bytes;
    size_t used_bytes;
    size_t requested_bytes;
    size_t peak_used_bytes;
    size_t block_count;
    size_t allocation_count;
} SnArenaStats;

typedef struct {
    SnArenaBlock *head;
    size_t block_size;
    size_t total_bytes; /* payload capacity reserved across all blocks */
    size_t used_bytes;  /* aligned payload consumed */
    size_t requested_bytes;
    size_t peak_used_bytes;
    size_t block_count;
    size_t allocation_count;
} SnArena;

/* Allocation failure is fatal: the process exits with a message instead of
 * returning NULL. A compiler that cannot allocate cannot produce a correct
 * answer, and silently dropping an AST node would be worse. */
void  sn_arena_init(SnArena *a, size_t block_size);
void *sn_arena_alloc(SnArena *a, size_t size);
void *sn_arena_calloc(SnArena *a, size_t size);
char *sn_arena_strndup(SnArena *a, const char *s, size_t n);
SnArenaStats sn_arena_get_stats(const SnArena *a);
void sn_arena_report_stats(FILE *stream, const char *label, const SnArena *a);
void  sn_arena_free(SnArena *a);

#endif /* SNOVAC_ARENA_H */
