#include "eval_internal.h"

#include <stdint.h>
typedef enum { GRAPH_OBJECT, GRAPH_ARRAY, GRAPH_VARIANT, GRAPH_LAMBDA, GRAPH_ENV } GraphKind;
typedef struct { const void *ptr; GraphKind kind; } GraphRef;
typedef struct { GraphRef *items; size_t count; size_t capacity; int owns_items; } GraphSet;

static size_t graph_hash(GraphRef ref) {
    uintptr_t key = (uintptr_t)ref.ptr;
    key ^= key >> 4;
    key *= (uintptr_t)0x9e3779b1u;
    key ^= key >> (sizeof(key) * 4u);
    return (size_t)(key ^ (uintptr_t)ref.kind);
}

static int graph_ref_equal(GraphRef a, GraphRef b) {
    return a.ptr == b.ptr && a.kind == b.kind;
}

static int graph_set_add(GraphSet *set, GraphRef ref) {
    if (!set->capacity || set->count >= set->capacity - set->capacity / 4u) {
        size_t capacity = set->capacity ? set->capacity * 2u : 16u;
        if (capacity < set->capacity || capacity > SIZE_MAX / sizeof(*set->items)) return -1;
        GraphRef *items = (GraphRef *)calloc(capacity, sizeof(*items));
        if (!items) return -1;
        for (size_t i = 0; i < set->capacity; ++i) {
            GraphRef old = set->items[i];
            if (!old.ptr) continue;
            size_t slot = graph_hash(old) & (capacity - 1u);
            while (items[slot].ptr) slot = (slot + 1u) & (capacity - 1u);
            items[slot] = old;
        }
        if (set->owns_items) free(set->items);
        set->items = items;
        set->capacity = capacity;
        set->owns_items = 1;
    }
    size_t slot = graph_hash(ref) & (set->capacity - 1u);
    while (set->items[slot].ptr) {
        if (graph_ref_equal(set->items[slot], ref)) return 0;
        slot = (slot + 1u) & (set->capacity - 1u);
    }
    set->items[slot] = ref;
    set->count++;
    return 1;
}

static int graph_queue_push(GraphRef **items, size_t *count, size_t *capacity,
                            GraphRef *inline_items, GraphRef ref) {
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2u : 16u;
        if (next < *capacity || next > SIZE_MAX / sizeof(**items)) return 0;
        GraphRef *grown;
        if (*items == inline_items) {
            grown = (GraphRef *)malloc(next * sizeof(**items));
            if (grown) memcpy(grown, *items, *count * sizeof(**items));
        } else {
            grown = (GraphRef *)realloc(*items, next * sizeof(**items));
        }
        if (!grown) return 0;
        *items = grown;
        *capacity = next;
    }
    (*items)[(*count)++] = ref;
    return 1;
}

static int graph_queue_ref(GraphSet *seen, GraphRef **items, size_t *count,
                           size_t *capacity, GraphRef *inline_items, GraphRef ref) {
    if (!ref.ptr) return 1;
    int added = graph_set_add(seen, ref);
    if (added < 0) return 0;
    return added == 0 || graph_queue_push(items, count, capacity, inline_items, ref);
}

static int graph_queue_value(GraphSet *seen, GraphRef **items, size_t *count,
                             size_t *capacity, GraphRef *inline_items, Value value) {
    GraphRef ref;
    switch (value.kind) {
    case V_OBJECT: ref = (GraphRef){value.as.o, GRAPH_OBJECT}; break;
    case V_ARRAY: ref = (GraphRef){value.as.arr, GRAPH_ARRAY}; break;
    case V_VARIANT: ref = (GraphRef){value.as.vt, GRAPH_VARIANT}; break;
    case V_LAMBDA: ref = (GraphRef){value.as.lam, GRAPH_LAMBDA}; break;
    default: return 1;
    }
    return graph_queue_ref(seen, items, count, capacity, inline_items, ref);
}

static int graph_queue_list_values(GraphSet *seen, GraphRef **queue, size_t *count, size_t *capacity,
                                   GraphRef *inline_items, const SnList *list) {
    for (size_t i = 0; i < list->len; ++i) {
        const Value *value = (const Value *)list->items[i];
        if (!graph_queue_value(seen, queue, count, capacity, inline_items, *value)) return 0;
    }
    return 1;
}

static int eval_mark_enqueue_value(GraphSet *seen, GraphRef **queue, size_t *count,
                                   size_t *capacity, GraphRef *inline_items,
                                   Value value) {
    if (value.kind == V_STRING) {
        if (value.as.str.owner_region) value.as.str.owner_region->may_escape = 1;
        return 1;
    }
    return graph_queue_value(seen, queue, count, capacity, inline_items, value);
}

static int eval_mark_enqueue_list(GraphSet *seen, GraphRef **queue, size_t *count,
                                  size_t *capacity, GraphRef *inline_items,
                                  const SnList *list) {
    for (size_t i = 0; i < list->len; ++i) {
        const Value *value = (const Value *)list->items[i];
        if (!eval_mark_enqueue_value(seen, queue, count, capacity, inline_items,
                                     *value)) return 0;
    }
    return 1;
}

static void eval_mark_all_regions_conservatively(Interp *in) {
    for (EvalRegion *region = in->current_region; region;
         region = region->parent_region) region->may_escape = 1;
    for (EvalRegion *region = in->retained_regions; region;
         region = region->next_retained) region->may_escape = 1;
}

/* Retain exactly the arena regions that own an escaping value graph. This is
 * promotion bookkeeping, not collection: no reachable object is reclaimed by
 * this walk. If scratch allocation fails, keep all active and retained regions
 * alive so that an incomplete traversal can never create a dangling pointer. */
void eval_value_mark_escaping(Interp *in, Value value) {
    GraphRef queue_inline[16] = {0};
    GraphRef seen_inline[16] = {0};
    GraphRef *queue = queue_inline;
    size_t count = 0, capacity = 16;
    GraphSet seen = {seen_inline, 0, 16, 0};
    int complete = 1;
    if (!eval_mark_enqueue_value(&seen, &queue, &count, &capacity,
                                 queue_inline, value)) {
        complete = 0;
        goto done;
    }
    while (count) {
        GraphRef current = queue[--count];
        int ok = 1;
        switch (current.kind) {
        case GRAPH_OBJECT: {
            const Object *object = (const Object *)current.ptr;
            if (object->owner_region) object->owner_region->may_escape = 1;
            ok = eval_mark_enqueue_list(&seen, &queue, &count, &capacity,
                                        queue_inline, &object->slots);
            break;
        }
        case GRAPH_ARRAY: {
            const ArrayVal *array = (const ArrayVal *)current.ptr;
            if (array->owner_region) array->owner_region->may_escape = 1;
            for (size_t i = 0; i < array->len; ++i) {
                if (!eval_mark_enqueue_value(&seen, &queue, &count, &capacity,
                                             queue_inline, array->items[i])) {
                    ok = 0;
                    break;
                }
            }
            break;
        }
        case GRAPH_VARIANT: {
            const VariantVal *variant = (const VariantVal *)current.ptr;
            if (variant->owner_region) variant->owner_region->may_escape = 1;
            ok = eval_mark_enqueue_list(&seen, &queue, &count, &capacity,
                                        queue_inline, &variant->payload);
            break;
        }
        case GRAPH_LAMBDA: {
            const LambdaVal *lambda = (const LambdaVal *)current.ptr;
            if (lambda->owner_region) lambda->owner_region->may_escape = 1;
            if (lambda->env)
                ok = graph_queue_ref(&seen, &queue, &count, &capacity,
                                     queue_inline,
                                     (GraphRef){lambda->env, GRAPH_ENV});
            break;
        }
        case GRAPH_ENV: {
            const Env *env = (const Env *)current.ptr;
            if (env->owner_region) env->owner_region->may_escape = 1;
            if (env->parent)
                ok = graph_queue_ref(&seen, &queue, &count, &capacity,
                                     queue_inline,
                                     (GraphRef){env->parent, GRAPH_ENV});
            if (ok)
                ok = eval_mark_enqueue_list(&seen, &queue, &count, &capacity,
                                            queue_inline, &env->slots);
            break;
        }
        }
        if (!ok) {
            complete = 0;
            break;
        }
    }
done:
    if (queue != queue_inline) free(queue);
    if (seen.owns_items) free(seen.items);
    if (!complete) eval_mark_all_regions_conservatively(in);
}

static int eval_would_create_cycle_target(GraphRef target, Value value) {
    if (!target.ptr) return 0;
    if ((value.kind == V_OBJECT && target.kind == GRAPH_OBJECT && value.as.o == target.ptr) ||
        (value.kind == V_ARRAY && target.kind == GRAPH_ARRAY && value.as.arr == target.ptr) ||
        (value.kind == V_LAMBDA && target.kind == GRAPH_LAMBDA && value.as.lam == target.ptr)) {
        return 1;
    }

    GraphRef queue_inline[16] = {0};
    GraphRef seen_inline[16] = {0};
    GraphRef *queue = queue_inline;
    size_t count = 0, capacity = 16;
    GraphSet seen = {seen_inline, 0, 16, 0};
    int result = 0;
    if (!graph_queue_value(&seen, &queue, &count, &capacity, queue_inline, value)) {
        result = -1;
        goto done;
    }
    while (count) {
        GraphRef current = queue[--count];
        if (graph_ref_equal(current, target)) { result = 1; break; }

        switch (current.kind) {
        case GRAPH_OBJECT: {
            const Object *object = (const Object *)current.ptr;
            if (!graph_queue_list_values(&seen, &queue, &count, &capacity, queue_inline, &object->slots)) result = -1;
            break;
        }
        case GRAPH_ARRAY: {
            const ArrayVal *array = (const ArrayVal *)current.ptr;
            for (size_t i = 0; i < array->len; ++i) {
                if (!graph_queue_value(&seen, &queue, &count, &capacity, queue_inline, array->items[i])) {
                    result = -1;
                    break;
                }
            }
            break;
        }
        case GRAPH_VARIANT: {
            const VariantVal *variant = (const VariantVal *)current.ptr;
            if (!graph_queue_list_values(&seen, &queue, &count, &capacity, queue_inline, &variant->payload)) result = -1;
            break;
        }
        case GRAPH_LAMBDA: {
            const LambdaVal *lambda = (const LambdaVal *)current.ptr;
            if (lambda->env && !graph_queue_ref(&seen, &queue, &count, &capacity, queue_inline,
                                                (GraphRef){lambda->env, GRAPH_ENV})) result = -1;
            break;
        }
        case GRAPH_ENV: {
            const Env *env = (const Env *)current.ptr;
            if (env->parent && !graph_queue_ref(&seen, &queue, &count, &capacity, queue_inline,
                                                (GraphRef){env->parent, GRAPH_ENV})) result = -1;
            if (result == 0 && !graph_queue_list_values(&seen, &queue, &count, &capacity, queue_inline, &env->slots)) result = -1;
            break;
        }
        }
        if (result < 0) break;
    }
done:
    if (queue != queue_inline) free(queue);
    if (seen.owns_items) free(seen.items);
    return result;
}

int eval_would_create_cycle(Value container, Value value) {
    GraphRef target;
    switch (container.kind) {
    case V_OBJECT: target = (GraphRef){container.as.o, GRAPH_OBJECT}; break;
    case V_ARRAY: target = (GraphRef){container.as.arr, GRAPH_ARRAY}; break;
    default: return 0;
    }
    return eval_would_create_cycle_target(target, value);
}
