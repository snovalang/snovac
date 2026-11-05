#include "eval_internal.h"

#include <stdint.h>

typedef struct { Value left, right; } EvalValuePair;
typedef struct { const VariantVal *left, *right; } EvalVariantPair;
typedef struct {
    EvalVariantPair *items;
    size_t count;
    size_t capacity;
} EvalVariantSet;

static void eval_equality_oom(void) {
    fputs("snovac evaluator: out of memory comparing variants\n", stderr);
    abort();
}

static size_t eval_variant_pair_hash(EvalVariantPair pair) {
    uintptr_t left = (uintptr_t)pair.left;
    uintptr_t right = (uintptr_t)pair.right;
    left ^= left >> 4u;
    right ^= right >> 4u;
    left *= (uintptr_t)0x9e3779b1u;
    right *= (uintptr_t)0x85ebca6bu;
    return (size_t)(left ^ (right + (left << 6u) + (left >> 2u)));
}

static int eval_variant_set_add(EvalVariantSet *set, EvalVariantPair pair) {
    if (!set->capacity || set->count >= set->capacity - set->capacity / 4u) {
        size_t capacity = set->capacity ? set->capacity * 2u : 16u;
        if (capacity < set->capacity || capacity > SIZE_MAX / sizeof(*set->items))
            eval_equality_oom();
        EvalVariantPair *items = (EvalVariantPair *)calloc(capacity, sizeof(*items));
        if (!items) eval_equality_oom();
        for (size_t i = 0; i < set->capacity; ++i) {
            EvalVariantPair old = set->items[i];
            if (!old.left) continue;
            size_t slot = eval_variant_pair_hash(old) & (capacity - 1u);
            while (items[slot].left) slot = (slot + 1u) & (capacity - 1u);
            items[slot] = old;
        }
        free(set->items);
        set->items = items;
        set->capacity = capacity;
    }
    size_t slot = eval_variant_pair_hash(pair) & (set->capacity - 1u);
    while (set->items[slot].left) {
        if (set->items[slot].left == pair.left &&
            set->items[slot].right == pair.right) return 0;
        slot = (slot + 1u) & (set->capacity - 1u);
    }
    set->items[slot] = pair;
    set->count++;
    return 1;
}

static void eval_value_pair_push(EvalValuePair **stack, size_t *count,
                                 size_t *capacity, EvalValuePair *inline_stack,
                                 Value left, Value right) {
    if (*count == *capacity) {
        size_t next = *capacity * 2u;
        if (next < *capacity || next > SIZE_MAX / sizeof(**stack))
            eval_equality_oom();
        EvalValuePair *grown;
        if (*stack == inline_stack) {
            grown = (EvalValuePair *)malloc(next * sizeof(*grown));
            if (grown) memcpy(grown, *stack, *count * sizeof(*grown));
        } else {
            grown = (EvalValuePair *)realloc(*stack, next * sizeof(*grown));
        }
        if (!grown) eval_equality_oom();
        *stack = grown;
        *capacity = next;
    }
    (*stack)[(*count)++] = (EvalValuePair){left, right};
}

static int eval_value_equal_shallow(Value left, Value right) {
    if (left.kind == V_INT && right.kind == V_INT) return left.as.i == right.as.i;
    if (left.kind == V_BOOL && right.kind == V_BOOL) return left.as.b == right.as.b;
    if (left.kind == V_DOUBLE && right.kind == V_DOUBLE) return left.as.d == right.as.d;
    if ((left.kind == V_INT && right.kind == V_DOUBLE) ||
        (left.kind == V_DOUBLE && right.kind == V_INT)) {
        double a = left.kind == V_DOUBLE ? left.as.d : (double)left.as.i;
        double b = right.kind == V_DOUBLE ? right.as.d : (double)right.as.i;
        return a == b;
    }
    if (left.kind == V_STRING && right.kind == V_STRING)
        return strcmp(left.as.str.data, right.as.str.data) == 0;
    return left.kind == V_UNIT && right.kind == V_UNIT;
}

int value_equals(Value left, Value right) {
    EvalValuePair inline_stack[16];
    EvalValuePair *stack = inline_stack;
    size_t count = 1;
    size_t capacity = sizeof(inline_stack) / sizeof(inline_stack[0]);
    stack[0] = (EvalValuePair){left, right};
    EvalVariantSet seen = {0};
    int equal = 1;

    while (count && equal) {
        EvalValuePair pair = stack[--count];
        if (pair.left.kind != V_VARIANT || pair.right.kind != V_VARIANT) {
            if (!eval_value_equal_shallow(pair.left, pair.right)) equal = 0;
            continue;
        }
        const VariantVal *a = pair.left.as.vt;
        const VariantVal *b = pair.right.as.vt;
        if (a == b) continue;
        if (strcmp(a->name, b->name) != 0 || a->payload.len != b->payload.len) {
            equal = 0;
            break;
        }
        if (!eval_variant_set_add(&seen, (EvalVariantPair){a, b})) continue;
        for (size_t i = 0; i < a->payload.len; ++i) {
            eval_value_pair_push(&stack, &count, &capacity, inline_stack,
                                 *(const Value *)a->payload.items[i],
                                 *(const Value *)b->payload.items[i]);
        }
    }

    if (stack != inline_stack) free(stack);
    free(seen.items);
    return equal;
}
