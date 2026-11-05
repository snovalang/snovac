/* eval_run.c — evaluator bootstrap, execution, and runtime teardown. */
#include "eval_internal.h"
#include "sn_native_dispatch.h"

static void eval_stats_add(size_t *total, size_t value) {
    if (*total > SIZE_MAX - value) *total = SIZE_MAX;
    else *total += value;
}

int sn_eval_run(SnArena *arena, SnDiagSink *diag, const SnUnit *unit) {
    (void)arena; /* The unit's AST is borrowed; runtime allocations are private. */
    if (!sn_runtime_init()) {
        SnSpan z = {0, 0, 1, 1};
        sn_diag_emit(diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED, z,
                     "could not initialize native runtime subsystems");
        sn_runtime_shutdown();
        return -1;
    }
    SnArena runtime_arena;
    sn_arena_init(&runtime_arena, SN_EVAL_ARENA_BLOCK_SIZE);

    Interp in;
    memset(&in, 0, sizeof(in));
    /* Keep transient environments, objects and generated strings out of the
     * caller's AST/token arena. Source declarations remain borrowed from it. */
    in.arena = &runtime_arena;
    in.root_arena = &runtime_arena;
    in.diag = diag;
    in.unit = unit;
    in.globals = env_new(&in, NULL);
    in.arrays = NULL;
    in.array_reserved_bytes = 0;
    in.array_used_bytes = 0;
    in.array_peak_bytes = 0;
    in.array_peak_used_bytes = 0;
    in.current_region = NULL;
    in.retained_regions = NULL;
    in.ret = v_unit();
    in.flow = FLOW_NORMAL;
    in.failed = 0;

    const SnDecl *main_fn = find_top(&in, "main", SN_DECL_FUNC);
    if (!main_fn) {
        SnSpan z = {0, 0, 1, 1};
        sn_diag_emit(diag, SN_DIAG_ERROR, SNOVA_NO_MAIN, z,
                     "no `func main()` found in this file");
        if (getenv("SNOVA_MEMORY_STATS"))
            sn_arena_report_stats(stderr, "evaluator-runtime", &runtime_arena);
        sn_arena_free(&runtime_arena);
        eval_region_pool_free(&in);
        sn_runtime_shutdown();
        return -1;
    }

    Env *global = env_new(&in, NULL);
    SnList no_args = {0};
    Value r = call_function(&in, main_fn, &no_args, global, NULL, main_fn->span);

    int result = in.failed ? -1 : (r.kind == V_INT) ? (int)r.as.i : 0;
    array_free_storage(&in, in.arrays);
    in.arrays = NULL;
    EvalRegion *region = in.retained_regions;
    size_t retained_count = 0;
    SnArenaStats retained_stats = {0};
    int memory_stats = getenv("SNOVA_MEMORY_STATS") != NULL;
    while (region) {
        EvalRegion *next = region->next_retained;
        if (memory_stats) {
            SnArenaStats current = sn_arena_get_stats(&region->arena);
            eval_stats_add(&retained_count, 1u);
            eval_stats_add(&retained_stats.reserved_bytes, current.reserved_bytes);
            eval_stats_add(&retained_stats.used_bytes, current.used_bytes);
            eval_stats_add(&retained_stats.requested_bytes, current.requested_bytes);
            eval_stats_add(&retained_stats.peak_used_bytes, current.peak_used_bytes);
            eval_stats_add(&retained_stats.block_count, current.block_count);
            eval_stats_add(&retained_stats.allocation_count, current.allocation_count);
        }
        array_free_storage(&in, region->arrays);
        region->arrays = NULL;
        sn_arena_free(&region->arena);
        region = next;
    }
    in.retained_regions = NULL;
    eval_region_pool_free(&in);
    if (memory_stats) {
        fprintf(stderr,
                "memory[evaluator-retained-regions]: count=%zu reserved=%zu used=%zu requested=%zu sum-peak-used=%zu blocks=%zu allocations=%zu\n",
                retained_count, retained_stats.reserved_bytes,
                retained_stats.used_bytes, retained_stats.requested_bytes,
                retained_stats.peak_used_bytes, retained_stats.block_count,
                retained_stats.allocation_count);
        fprintf(stderr,
                "memory[evaluator-closures]: closures=%zu captured-slots=%zu full-frame-fallbacks=%zu\n",
                in.closure_count, in.captured_slot_count,
                in.capture_fallback_count);
        fprintf(stderr,
                "memory[evaluator-arrays]: used=%zu reserved=%zu slack=%zu peak-used=%zu peak-reserved=%zu\n",
                in.array_used_bytes, in.array_reserved_bytes,
                in.array_reserved_bytes - in.array_used_bytes,
                in.array_peak_used_bytes, in.array_peak_bytes);
        sn_arena_report_stats(stderr, "evaluator-runtime", &runtime_arena);
    }
    sn_arena_free(&runtime_arena);
    sn_runtime_shutdown();
    return result;
}
