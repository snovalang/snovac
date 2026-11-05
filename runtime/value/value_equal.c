#include "value_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SnVal left;
    SnVal right;
} SnValPair;

typedef struct {
    SnObjHeader *left;
    SnObjHeader *right;
    SnValTag tag;
} SnValPairKey;

typedef struct {
    SnValPairKey *items;
    size_t count;
    size_t capacity;
} SnValPairSet;

static void equality_oom(void) {
    fputs("snovac runtime: out of memory comparing values\n", stderr);
    abort();
}

static size_t pair_hash(SnValPairKey pair) {
    uintptr_t left = (uintptr_t)pair.left;
    uintptr_t right = (uintptr_t)pair.right;
    left ^= left >> 4u;
    right ^= right >> 4u;
    left *= (uintptr_t)0x9e3779b1u;
    right *= (uintptr_t)0x85ebca6bu;
    return (size_t)(left ^ (right + (left << 6u) + (left >> 2u)) ^
                    (uintptr_t)pair.tag);
}

static int pair_equal(SnValPairKey a, SnValPairKey b) {
    return a.left == b.left && a.right == b.right && a.tag == b.tag;
}

static int pair_set_add(SnValPairSet *set, SnValPairKey pair) {
    if (!set->capacity || set->count >= set->capacity - set->capacity / 4u) {
        size_t capacity = set->capacity ? set->capacity * 2u : 16u;
        if (capacity < set->capacity || capacity > SIZE_MAX / sizeof(*set->items))
            equality_oom();
        SnValPairKey *items = (SnValPairKey *)calloc(capacity, sizeof(*items));
        if (!items) equality_oom();
        for (size_t i = 0; i < set->capacity; ++i) {
            SnValPairKey old = set->items[i];
            if (!old.left) continue;
            size_t slot = pair_hash(old) & (capacity - 1u);
            while (items[slot].left) slot = (slot + 1u) & (capacity - 1u);
            items[slot] = old;
        }
        free(set->items);
        set->items = items;
        set->capacity = capacity;
    }
    size_t slot = pair_hash(pair) & (set->capacity - 1u);
    while (set->items[slot].left) {
        if (pair_equal(set->items[slot], pair)) return 0;
        slot = (slot + 1u) & (set->capacity - 1u);
    }
    set->items[slot] = pair;
    set->count++;
    return 1;
}

static void pair_stack_push(SnValPair **stack, size_t *count, size_t *capacity,
                            SnValPair *inline_stack, SnVal left, SnVal right) {
    if (*count == *capacity) {
        size_t next = *capacity * 2u;
        if (next < *capacity || next > SIZE_MAX / sizeof(**stack)) equality_oom();
        SnValPair *grown;
        if (*stack == inline_stack) {
            grown = (SnValPair *)malloc(next * sizeof(*grown));
            if (grown) memcpy(grown, *stack, *count * sizeof(*grown));
        } else {
            grown = (SnValPair *)realloc(*stack, next * sizeof(*grown));
        }
        if (!grown) equality_oom();
        *stack = grown;
        *capacity = next;
    }
    (*stack)[(*count)++] = (SnValPair){left, right};
}

static int values_equal_shallow(SnVal a, SnVal b, int *aggregate) {
    *aggregate = 0;
    if (a.tag != b.tag) {
        if (a.tag == VAL_INT && b.tag == VAL_DOUBLE) return (double)a.as.i == b.as.d;
        if (a.tag == VAL_DOUBLE && b.tag == VAL_INT) return a.as.d == (double)b.as.i;
        return 0;
    }
    switch (a.tag) {
    case VAL_MOVED: return 0;
    case VAL_UNIT: return 1;
    case VAL_BOOL: return a.as.b == b.as.b;
    case VAL_INT: return a.as.i == b.as.i;
    case VAL_DOUBLE: return a.as.d == b.as.d;
    case VAL_STRING: {
        if (!a.as.obj || !b.as.obj) return a.as.obj == b.as.obj;
        sn_value_validate_object(a);
        sn_value_validate_object(b);
        SnStringObj *left = (SnStringObj *)a.as.obj;
        SnStringObj *right = (SnStringObj *)b.as.obj;
        return left->len == right->len &&
               (!left->len || memcmp(left->chars, right->chars, left->len) == 0);
    }
    case VAL_ARRAY:
    case VAL_OBJECT:
        if (a.as.obj) sn_value_validate_object(a);
        if (b.as.obj) sn_value_validate_object(b);
        return a.as.obj == b.as.obj;
    case VAL_VARIANT:
        if (!a.as.obj || !b.as.obj) return a.as.obj == b.as.obj;
        sn_value_validate_object(a);
        sn_value_validate_object(b);
        *aggregate = 1;
        return 1;
    }
    return 0;
}

bool sn_val_equal(SnVal a, SnVal b) {
    SnValPair inline_stack[16];
    SnValPair *stack = inline_stack;
    size_t count = 1, capacity = sizeof(inline_stack) / sizeof(inline_stack[0]);
    stack[0] = (SnValPair){a, b};
    SnValPairSet seen = {0};
    bool equal = true;

    while (count && equal) {
        SnValPair pair = stack[--count];
        int aggregate = 0;
        if (!values_equal_shallow(pair.left, pair.right, &aggregate)) {
            equal = false;
            break;
        }
        if (!aggregate) continue;
        if (pair.left.as.obj == pair.right.as.obj) continue;
        SnValPairKey key = {pair.left.as.obj, pair.right.as.obj, pair.left.tag};
        if (!pair_set_add(&seen, key)) continue;

        SnVariantObj *left = (SnVariantObj *)pair.left.as.obj;
        SnVariantObj *right = (SnVariantObj *)pair.right.as.obj;
        if (left->tag_id != right->tag_id ||
            left->payload_count != right->payload_count) {
            equal = false;
            break;
        }
        for (size_t i = 0; equal && i < left->payload_count; ++i)
            pair_stack_push(&stack, &count, &capacity, inline_stack,
                            left->payloads[i], right->payloads[i]);
    }

    if (stack != inline_stack) free(stack);
    free(seen.items);
    return equal;
}
