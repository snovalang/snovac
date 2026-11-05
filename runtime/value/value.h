/* value.h — runtime value representation for Snovalang bytecode VM. */
#ifndef SNOVAC_VALUE_H
#define SNOVAC_VALUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
    VAL_UNIT = 0,
    VAL_BOOL,
    VAL_INT,
    VAL_DOUBLE,
    VAL_STRING,
    VAL_ARRAY,
    VAL_OBJECT,
    VAL_VARIANT,
    VAL_MOVED /* invalidated local slot; never a user-visible value */
} SnValTag;

typedef struct SnObjHeader SnObjHeader;

typedef struct {
    SnValTag tag;
    union {
        bool b;
        int64_t i;
        double d;
        SnObjHeader *obj;
    } as;
} SnVal;

typedef struct {
    size_t live_objects;
    size_t live_bytes;
} SnValueKindMemoryStats;

typedef struct {
    size_t live_objects;
    size_t live_bytes;
    size_t peak_bytes;
    size_t allocations;
    size_t frees;
    /* Indexed by tag - VAL_STRING: string, array, object, variant. */
    SnValueKindMemoryStats by_kind[4];
} SnValueMemoryStats;

/* Process-wide metrics for managed VM value objects. Counters are collected
 * only when SNOVA_MEMORY_STATS is set before the first managed allocation;
 * otherwise this returns all-zero metrics and has no per-object accounting
 * overhead beyond a cached enable check. */
SnValueMemoryStats sn_val_memory_stats(void);
void sn_val_report_memory_stats(FILE *stream, const char *label);

/* Heap constructors return one owned reference. Copying SnVal in C does not
 * change ownership: call retain for an additional owner and release exactly
 * once per owned reference. Reference counts are atomic, but callers must
 * still synchronize container mutation and must not retain through an already
 * unowned/borrowed pointer. Container setters borrow their value argument. */
SnVal sn_val_unit(void);
SnVal sn_val_bool(bool b);
SnVal sn_val_int(int64_t i);
SnVal sn_val_double(double d);
SnVal sn_val_string(const char *str, size_t len);
SnVal sn_val_string_concat(SnVal left, SnVal right);
SnVal sn_val_array(size_t initial_cap);
SnVal sn_val_instance(uint32_t class_id, size_t field_count);
SnVal sn_val_variant(uint32_t tag_id, const char *name, size_t payload_count);

/* Object layouts remain private to value.c. Getters return owned clones for
 * child values; byte pointers are borrowed and valid while the owner lives. */
size_t sn_val_string_length(SnVal string);
const char *sn_val_string_data(SnVal string);
size_t sn_val_array_length(SnVal array);
SnVal sn_val_array_get(SnVal array, size_t index);
size_t sn_val_instance_field_count(SnVal instance);
uint32_t sn_val_instance_class_id(SnVal instance);
SnVal sn_val_instance_get(SnVal instance, size_t index);
bool sn_val_variant_is(SnVal variant, uint32_t tag_id);
size_t sn_val_variant_payload_count(SnVal variant);
SnVal sn_val_variant_get(SnVal variant, size_t index);

/* Retain requires a live owned/borrowed reference whose object still has a
 * nonzero count; retaining a zero-count object aborts as an ownership bug. */
void sn_val_retain(SnVal val);
void sn_val_release(SnVal val);
/* Replace an owned slot with a borrowed value, retaining before releasing the
 * previous owner. Returns false for a null destination. */
bool sn_val_assign(SnVal *slot, SnVal borrowed_value);
/* Clone creates an additional owned reference. Move transfers one owned
 * reference and resets the source slot to unit. */
SnVal sn_val_clone(SnVal val);
SnVal sn_val_move(SnVal *source);
/* Store a borrowed value in a heap container. The container retains it on
 * success. A store is rejected if it would create a strong-reference cycle. */
bool sn_val_array_set(SnVal array, size_t index, SnVal value);
/* Append transfers one owned value; pop returns one owned value or unit when
 * empty. clear releases all children and returns the backing buffer. */
bool sn_val_array_push_move(SnVal array, SnVal *value);
SnVal sn_val_array_pop(SnVal array);
bool sn_val_array_clear(SnVal array);
bool sn_val_instance_set(SnVal instance, size_t index, SnVal value);
/* Transfer an owned value into a preallocated container slot. The source is
 * invalidated on success and remains owned by the caller on failure. */
bool sn_val_array_set_move(SnVal array, size_t index, SnVal *value);
bool sn_val_instance_set_move(SnVal instance, size_t index, SnVal *value);
bool sn_val_variant_set_move(SnVal variant, size_t index, SnVal *value);
/* Equality compares scalars by value, strings by length/bytes, arrays and
 * instances by identity, and variants by tag plus recursively equal payloads.
 * Aggregate traversal is iterative so nested variants do not consume the C
 * call stack. */
bool sn_val_equal(SnVal a, SnVal b);
void sn_val_print(SnVal val, bool newline);

#endif /* SNOVAC_VALUE_H */
