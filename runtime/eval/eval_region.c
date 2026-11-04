/* eval_region.c — region arenas and recycled region descriptor slabs. */
#include "eval_internal.h"

static EvalRegion *eval_region_acquire(Interp *in) {
    if (!in->free_regions) {
        EvalRegionBlock *block = (EvalRegionBlock *)calloc(1, sizeof(*block));
        if (!block) {
            fputs("snovac evaluator: out of memory allocating region slab\n", stderr);
            abort();
        }
        block->next = in->region_blocks;
        in->region_blocks = block;
        for (size_t i = 0; i < SN_EVAL_REGION_SLAB_COUNT; ++i) {
            block->regions[i].next_free = in->free_regions;
            in->free_regions = &block->regions[i];
        }
    }
    EvalRegion *region = in->free_regions;
    in->free_regions = region->next_free;
    memset(region, 0, sizeof(*region));
    return region;
}

static void eval_region_recycle(Interp *in, EvalRegion *region) {
    memset(region, 0, sizeof(*region));
    region->next_free = in->free_regions;
    in->free_regions = region;
}

void eval_region_pool_free(Interp *in) {
    EvalRegionBlock *block = in->region_blocks;
    size_t blocks = 0;
    for (EvalRegionBlock *it = block; it; it = it->next) blocks++;
    if (getenv("SNOVA_MEMORY_STATS")) {
        fprintf(stderr,
                "memory[evaluator-region-pool]: slabs=%zu descriptors=%zu bytes=%zu\n",
                blocks, blocks * (size_t)SN_EVAL_REGION_SLAB_COUNT,
                blocks * sizeof(*block));
    }
    while (block) {
        EvalRegionBlock *next = block->next;
        free(block);
        block = next;
    }
    in->region_blocks = NULL;
    in->free_regions = NULL;
}

EvalRegion *eval_region_begin(Interp *in) {
    EvalRegion *region = eval_region_acquire(in);
    sn_arena_init(&region->arena, SN_EVAL_ARENA_BLOCK_SIZE);
    region->parent_arena = in->arena;
    region->parent_region = in->current_region;
    in->arena = &region->arena;
    in->current_region = region;
    return region;
}

void eval_region_end(Interp *in, EvalRegion *region) {
    in->arena = region->parent_arena;
    in->current_region = region->parent_region;
    if (region->may_escape) {
        region->parent_arena = NULL;
        region->parent_region = NULL;
        region->next_retained = in->retained_regions;
        in->retained_regions = region;
    } else {
        array_free_storage(in, region->arrays);
        region->arrays = NULL;
        sn_arena_free(&region->arena);
        eval_region_recycle(in, region);
    }
}

void eval_region_mark_current(Interp *in) {
    if (in && in->current_region) in->current_region->may_escape = 1;
}

