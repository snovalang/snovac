#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "value_internal.h"

#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static bool sn_obj_release_ref(SnObjHeader *obj) {
    uint_fast64_t current = atomic_load_explicit(&obj->rc, memory_order_relaxed);
    for (;;) {
        if (current == 0) {
            fputs("snovac runtime: invalid release of an unowned value\n", stderr);
            abort();
        }
        if (atomic_compare_exchange_weak_explicit(&obj->rc, &current, current - 1u,
                                                  memory_order_acq_rel,
                                                  memory_order_relaxed))
            return current == 1u;
    }
}

static atomic_flag sn_value_stats_lock = ATOMIC_FLAG_INIT;
/* 0 = uninitialized, 1 = initializing, 2 = disabled, 3 = enabled. */
static atomic_int sn_value_stats_state;
static size_t sn_value_live_objects;
static size_t sn_value_live_bytes;
static size_t sn_value_kind_objects[4];
static size_t sn_value_kind_bytes[4];
static size_t sn_value_peak_bytes;
static size_t sn_value_allocations;
static size_t sn_value_frees;

static bool sn_value_stats_enabled(void) {
    int state = atomic_load_explicit(&sn_value_stats_state, memory_order_acquire);
    if (state == 0) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&sn_value_stats_state, &expected, 1,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            int enabled = getenv("SNOVA_MEMORY_STATS") != NULL;
            atomic_store_explicit(&sn_value_stats_state, enabled ? 3 : 2, memory_order_release);
            return enabled != 0;
        }
        state = expected;
    }
    while (state == 1) {
        state = atomic_load_explicit(&sn_value_stats_state, memory_order_acquire);
    }
    return state == 3;
}

static void sn_value_stats_lock_acquire(void) {
    while (atomic_flag_test_and_set_explicit(&sn_value_stats_lock, memory_order_acquire)) {}
}

static void sn_value_stats_lock_release(void) {
    atomic_flag_clear_explicit(&sn_value_stats_lock, memory_order_release);
}

static size_t sn_value_kind_index(SnValTag tag) {
    if (tag < VAL_STRING || tag > VAL_VARIANT) {
        fputs("snovac runtime: invalid managed value kind for accounting\n", stderr);
        abort();
    }
    return (size_t)(tag - VAL_STRING);
}

void sn_value_track_allocation(SnValTag tag, size_t bytes) {
    if (!sn_value_stats_enabled()) return;
    size_t kind = sn_value_kind_index(tag);
    sn_value_stats_lock_acquire();
    if (sn_value_live_bytes > SIZE_MAX - bytes || sn_value_live_objects == SIZE_MAX ||
        sn_value_kind_bytes[kind] > SIZE_MAX - bytes ||
        sn_value_kind_objects[kind] == SIZE_MAX || sn_value_allocations == SIZE_MAX) {
        sn_value_stats_lock_release();
        fputs("snovac runtime: managed value memory accounting overflow\n", stderr);
        abort();
    }
    sn_value_live_bytes += bytes;
    sn_value_live_objects++;
    sn_value_kind_bytes[kind] += bytes;
    sn_value_kind_objects[kind]++;
    sn_value_allocations++;
    if (sn_value_peak_bytes < sn_value_live_bytes) sn_value_peak_bytes = sn_value_live_bytes;
    sn_value_stats_lock_release();
}

void sn_value_track_resize(SnValTag tag, size_t old_bytes, size_t new_bytes) {
    if (!sn_value_stats_enabled() || old_bytes == new_bytes) return;
    size_t kind = sn_value_kind_index(tag);
    sn_value_stats_lock_acquire();
    if (new_bytes > old_bytes) {
        size_t growth = new_bytes - old_bytes;
        if (sn_value_live_bytes > SIZE_MAX - growth ||
            sn_value_kind_bytes[kind] > SIZE_MAX - growth) {
            sn_value_stats_lock_release();
            fputs("snovac runtime: managed value memory accounting overflow\n", stderr);
            abort();
        }
        sn_value_live_bytes += growth;
        sn_value_kind_bytes[kind] += growth;
        if (sn_value_peak_bytes < sn_value_live_bytes)
            sn_value_peak_bytes = sn_value_live_bytes;
    } else {
        size_t shrink = old_bytes - new_bytes;
        if (sn_value_live_bytes < shrink || sn_value_kind_bytes[kind] < shrink) {
            sn_value_stats_lock_release();
            fputs("snovac runtime: managed value memory accounting underflow\n", stderr);
            abort();
        }
        sn_value_live_bytes -= shrink;
        sn_value_kind_bytes[kind] -= shrink;
    }
    sn_value_stats_lock_release();
}

static void sn_value_track_free(SnValTag tag, size_t bytes) {
    if (!sn_value_stats_enabled()) return;
    size_t kind = sn_value_kind_index(tag);
    sn_value_stats_lock_acquire();
    if (!sn_value_live_objects || sn_value_live_bytes < bytes ||
        !sn_value_kind_objects[kind] || sn_value_kind_bytes[kind] < bytes ||
        sn_value_frees == SIZE_MAX) {
        sn_value_stats_lock_release();
        fputs("snovac runtime: managed value memory accounting underflow\n", stderr);
        abort();
    }
    sn_value_live_objects--;
    sn_value_live_bytes -= bytes;
    sn_value_kind_objects[kind]--;
    sn_value_kind_bytes[kind] -= bytes;
    sn_value_frees++;
    sn_value_stats_lock_release();
}

static void sn_value_free_object(SnObjHeader *obj) {
    sn_value_track_free(obj->tag, obj->allocation_size);
    free(obj);
}

SnValueMemoryStats sn_val_memory_stats(void) {
    SnValueMemoryStats stats;
    memset(&stats, 0, sizeof(stats));
    if (!sn_value_stats_enabled()) return stats;
    sn_value_stats_lock_acquire();
    stats.live_objects = sn_value_live_objects;
    stats.live_bytes = sn_value_live_bytes;
    stats.peak_bytes = sn_value_peak_bytes;
    stats.allocations = sn_value_allocations;
    stats.frees = sn_value_frees;
    for (size_t i = 0; i < 4; ++i) {
        stats.by_kind[i].live_objects = sn_value_kind_objects[i];
        stats.by_kind[i].live_bytes = sn_value_kind_bytes[i];
    }
    sn_value_stats_lock_release();
    return stats;
}

void sn_val_report_memory_stats(FILE *stream, const char *label) {
    if (!stream || !sn_value_stats_enabled()) return;
    SnValueMemoryStats stats = sn_val_memory_stats();
    fprintf(stream, "memory[%s]: objects=%zu live=%zu peak=%zu allocations=%zu frees=%zu "
                    "strings=%zu/%zu arrays=%zu/%zu instances=%zu/%zu variants=%zu/%zu\n",
            label ? label : "vm-values", stats.live_objects, stats.live_bytes,
            stats.peak_bytes, stats.allocations, stats.frees,
            stats.by_kind[0].live_objects, stats.by_kind[0].live_bytes,
            stats.by_kind[1].live_objects, stats.by_kind[1].live_bytes,
            stats.by_kind[2].live_objects, stats.by_kind[2].live_bytes,
            stats.by_kind[3].live_objects, stats.by_kind[3].live_bytes);
}

void *sn_value_alloc(size_t size) {
    void *ptr = malloc(size ? size : 1u);
    if (!ptr) {
        fputs("snovac runtime: out of memory\n", stderr);
        abort();
    }
    return ptr;
}

void sn_value_validate_object(SnVal val) {
    if (val.tag < VAL_STRING || !val.as.obj) return;
    if (val.as.obj->tag != val.tag) {
        fputs("snovac runtime: value tag does not match object header\n", stderr);
        abort();
    }
}

void sn_val_retain(SnVal val) {
    if (val.tag >= VAL_STRING && val.as.obj) {
        sn_value_validate_object(val);
        uint_fast64_t current = atomic_load_explicit(&val.as.obj->rc, memory_order_relaxed);
        for (;;) {
            if (current == UINT_FAST64_MAX) {
                fputs("snovac runtime: reference count overflow\n", stderr);
                abort();
            }
            if (atomic_compare_exchange_weak_explicit(&val.as.obj->rc, &current, current + 1u,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) break;
        }
    }
}

bool sn_val_assign(SnVal *slot, SnVal borrowed_value) {
    if (!slot) return false;
    SnVal next = sn_val_clone(borrowed_value);
    SnVal previous = *slot;
    *slot = next;
    sn_val_release(previous);
    return true;
}

SnVal sn_val_clone(SnVal val) {
    if (val.tag == VAL_MOVED) {
        fputs("snovac runtime: cannot clone a moved value\n", stderr);
        abort();
    }
    sn_val_retain(val);
    return val;
}

SnVal sn_val_move(SnVal *source) {
    if (!source) return sn_val_unit();
    if (source->tag == VAL_MOVED) {
        fputs("snovac runtime: cannot move a value more than once\n", stderr);
        abort();
    }
    SnVal moved = *source;
    source->tag = VAL_MOVED;
    source->as.i = 0;
    return moved;
}

void sn_val_release(SnVal val) {
    if (val.tag < VAL_STRING || !val.as.obj) {
        return;
    }
    sn_value_validate_object(val);
    if (!sn_obj_release_ref(val.as.obj)) return;

    /* Iterative, allocation-free destruction avoids C stack overflow for
     * deeply nested values. The intrusive link is safe once rc reaches zero. */
    SnObjHeader *pending = val.as.obj;
    pending->release_next = NULL;
    while (pending) {
        SnObjHeader *obj = pending;
        pending = obj->release_next;
        SnVal *children = NULL;
        size_t child_count = 0;
        switch (obj->tag) {
        case VAL_STRING: {
            sn_value_free_object(obj);
            continue;
        }
        case VAL_ARRAY: {
            SnArrayObj *a = (SnArrayObj *)obj;
            children = a->items; child_count = a->count;
            break;
        }
        case VAL_OBJECT: {
            SnInstanceObj *inst = (SnInstanceObj *)obj;
            children = inst->fields; child_count = inst->field_count;
            break;
        }
        case VAL_VARIANT: {
            SnVariantObj *var = (SnVariantObj *)obj;
            children = var->payloads; child_count = var->payload_count;
            break;
        }
        default:
            sn_value_free_object(obj);
            continue;
        }
        for (size_t i = 0; i < child_count; ++i) {
            SnVal child = children[i];
            if (child.tag < VAL_STRING || !child.as.obj) continue;
            sn_value_validate_object(child);
            if (sn_obj_release_ref(child.as.obj)) {
                child.as.obj->release_next = pending;
                pending = child.as.obj;
            }
        }
        switch (obj->tag) {
        case VAL_ARRAY: {
            free(((SnArrayObj *)obj)->items);
            sn_value_free_object(obj);
            break;
        }
        case VAL_OBJECT: {
            sn_value_free_object(obj);
            break;
        }
        case VAL_VARIANT: {
            sn_value_free_object(obj);
            break;
        }
        default: break;
        }
    }
}

