#include "eval_internal.h"

#include <stdint.h>

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} StringBuilder;

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

static void builder_append(StringBuilder *builder, const char *text) {
    size_t len = strlen(text);
    if (len > SIZE_MAX - builder->len - 1u) {
        fputs("snovac runtime: formatted string size overflow\n", stderr);
        abort();
    }
    size_t needed = builder->len + len + 1u;
    if (needed > builder->capacity) {
        size_t capacity = builder->capacity ? builder->capacity : 64u;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2u) {
                capacity = needed;
                break;
            }
            capacity *= 2u;
        }
        char *data = (char *)realloc(builder->data, capacity);
        if (!data) {
            fputs("snovac runtime: out of memory formatting string\n", stderr);
            abort();
        }
        builder->data = data;
        builder->capacity = capacity;
    }
    memcpy(builder->data + builder->len, text, len);
    builder->len += len;
    builder->data[builder->len] = '\0';
}

static char *builder_finish(Interp *in, StringBuilder *builder) {
    eval_region_mark_escaping(in);
    char *result = sn_arena_strndup(in->arena,
                                    builder->data ? builder->data : "",
                                    builder->len);
    free(builder->data);
    builder->data = NULL;
    builder->len = builder->capacity = 0;
    return result;
}

/* ── strings ──────────────────────────────────────────────────────────────── */

char *arena_sprintf(Interp *in, const char *fmt, ...) {
    eval_region_mark_escaping(in);
    va_list ap;
    va_start(ap, fmt);
    va_list measure;
    va_copy(measure, ap);
    int n = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);
    if (n < 0) {
        va_end(ap);
        return sn_arena_strndup(in->arena, "", 0);
    }
    char *out = (char *)sn_arena_alloc(in->arena, (size_t)n + 1u);
    (void)vsnprintf(out, (size_t)n + 1u, fmt, ap);
    va_end(ap);
    return out;
}

char *str_concat(Interp *in, const char *a, const char *b) {
    eval_region_mark_escaping(in);
    size_t la = strlen(a), lb = strlen(b);
    if (la > SIZE_MAX - lb - 1u) {
        fputs("snovac runtime: concatenated string size overflow\n", stderr);
        abort();
    }
    char *out = (char *)sn_arena_alloc(in->arena, la + lb + 1);
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = '\0';
    return out;
}

const char *to_string(Interp *in, Value v, SnSpan span) {
    switch (v.kind) {
    case V_MOVED:  return "<moved>";
    case V_UNIT:   return "unit";
    case V_INT:    return arena_sprintf(in, "%lld", v.as.i);
    case V_DOUBLE: return arena_sprintf(in, "%g", v.as.d);
    case V_BOOL:   return v.as.b ? "true" : "false";
    case V_STRING: return v.as.s;
    case V_OBJECT: {
        /* Printing an object uses its `asString()`, matching the convention the
         * corpus relies on (tests/run-pass/counter.snova). */
        const SnDecl *m = find_member(v.as.o->cls, "asString");
        if (m && m->body) {
            SnList none = {0};
            Value s = call_method(in, v.as.o, m, &none, NULL, span);
            if (s.kind == V_STRING) {
                return s.as.s;
            }
        }
        return arena_sprintf(in, "<%s>",
                             v.as.o->cls->name ? v.as.o->cls->name : "object");
    }
    case V_VARIANT: {
        if (v.as.vt->payload.len == 0) {
            return v.as.vt->name;
        }
        if (v.as.vt->payload.len == 1 &&
            (strcmp(v.as.vt->name, "Ok") == 0 || strcmp(v.as.vt->name, "Some") == 0)) {
            return to_string(in, *(const Value *)v.as.vt->payload.items[0], span);
        }
        StringBuilder builder = {0};
        builder_append(&builder, v.as.vt->name);
        builder_append(&builder, "(");
        for (size_t i = 0; i < v.as.vt->payload.len; i++) {
            const Value *p = (const Value *)v.as.vt->payload.items[i];
            builder_append(&builder, to_string(in, *p, span));
            if (i + 1 < v.as.vt->payload.len) {
                builder_append(&builder, ", ");
            }
        }
        builder_append(&builder, ")");
        return builder_finish(in, &builder);
    }
    case V_LAMBDA:
        return "<lambda>";
    case V_ARRAY: {
        StringBuilder builder = {0};
        builder_append(&builder, "[");
        for (size_t i = 0; i < v.as.arr->len; i++) {
            const Value *item = &v.as.arr->items[i];
            builder_append(&builder, to_string(in, *item, span));
            if (i + 1 < v.as.arr->len) {
                builder_append(&builder, ", ");
            }
        }
        builder_append(&builder, "]");
        return builder_finish(in, &builder);
    }
    }
    return "?";
}

/* ── variants and equality ────────────────────────────────────────────────── */

Value v_variant(Interp *in, const char *name, SnList payload) {
    eval_region_mark_escaping(in);
    VariantVal *vt = (VariantVal *)sn_arena_calloc(in->arena, sizeof(VariantVal));
    vt->name = name;
    vt->payload = payload;
    Value v;
    v.kind = V_VARIANT;
    v.as.vt = vt;
    return v;
}

int value_equals(Value a, Value b) {
    if (a.kind == V_INT && b.kind == V_INT) return a.as.i == b.as.i;
    if (a.kind == V_BOOL && b.kind == V_BOOL) return a.as.b == b.as.b;
    if (a.kind == V_DOUBLE && b.kind == V_DOUBLE) return a.as.d == b.as.d;
    if ((a.kind == V_INT && b.kind == V_DOUBLE) ||
        (a.kind == V_DOUBLE && b.kind == V_INT)) {
        double x = (a.kind == V_DOUBLE) ? a.as.d : (double)a.as.i;
        double y = (b.kind == V_DOUBLE) ? b.as.d : (double)b.as.i;
        return x == y;
    }
    if (a.kind == V_STRING && b.kind == V_STRING) {
        return strcmp(a.as.s, b.as.s) == 0;
    }
    if (a.kind == V_UNIT && b.kind == V_UNIT) return 1;
    if (a.kind == V_VARIANT && b.kind == V_VARIANT) {
        if (strcmp(a.as.vt->name, b.as.vt->name) != 0 ||
            a.as.vt->payload.len != b.as.vt->payload.len) {
            return 0;
        }
        for (size_t i = 0; i < a.as.vt->payload.len; i++) {
            if (!value_equals(*(const Value *)a.as.vt->payload.items[i],
                              *(const Value *)b.as.vt->payload.items[i])) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

