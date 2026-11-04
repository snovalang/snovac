#include "value_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SnObjHeader **items;
    size_t capacity;
    size_t count;
    bool owns_items;
} SnObjSet;

static size_t sn_obj_hash(const SnObjHeader *obj) {
    uintptr_t key = (uintptr_t)obj;
    key ^= key >> 4;
    key *= (uintptr_t)0x9e3779b1u;
    key ^= key >> (sizeof(key) * 4u);
    return (size_t)key;
}

/* Returns 1 when inserted, 0 when already present, and -1 on allocation failure. */
static int sn_obj_set_add(SnObjSet *set, SnObjHeader *obj) {
    if (!set->capacity || set->count >= set->capacity - set->capacity / 4u) {
        size_t next = set->capacity ? set->capacity * 2u : 16u;
        if (next < set->capacity || next > SIZE_MAX / sizeof(*set->items)) return -1;
        SnObjHeader **items = (SnObjHeader **)calloc(next, sizeof(*items));
        if (!items) return -1;
        for (size_t i = 0; i < set->capacity; ++i) {
            SnObjHeader *entry = set->items[i];
            if (!entry) continue;
            size_t slot = sn_obj_hash(entry) & (next - 1u);
            while (items[slot]) slot = (slot + 1u) & (next - 1u);
            items[slot] = entry;
        }
        if (set->owns_items) free(set->items);
        set->items = items;
        set->capacity = next;
        set->owns_items = true;
    }
    size_t slot = sn_obj_hash(obj) & (set->capacity - 1u);
    while (set->items[slot]) {
        if (set->items[slot] == obj) return 0;
        slot = (slot + 1u) & (set->capacity - 1u);
    }
    set->items[slot] = obj;
    set->count++;
    return 1;
}

static bool sn_obj_queue_push(SnObjHeader ***items, size_t *count, size_t *capacity,
                              SnObjHeader **inline_items, SnObjHeader *value) {
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2u : 16u;
        if (next < *capacity || next > SIZE_MAX / sizeof(**items)) return false;
        SnObjHeader **grown;
        if (*items == inline_items) {
            grown = (SnObjHeader **)malloc(next * sizeof(**items));
            if (grown) memcpy(grown, *items, *count * sizeof(**items));
        } else {
            grown = (SnObjHeader **)realloc(*items, next * sizeof(**items));
        }
        if (!grown) return false;
        *items = grown;
        *capacity = next;
    }
    (*items)[(*count)++] = value;
    return true;
}

static bool sn_obj_reaches(SnObjHeader *start, SnObjHeader *target, bool *traversal_complete) {
    *traversal_complete = false;
    if (!start || !target) { *traversal_complete = true; return false; }
    SnObjHeader *pending_inline[16] = {0};
    SnObjHeader *seen_inline[16] = {0};
    SnObjHeader **pending = pending_inline;
    size_t pending_count = 1, pending_cap = 16;
    pending_inline[0] = start;
    SnObjSet seen = {seen_inline, 16, 0, false};
    bool found = false;
    bool traversal_done = false;
    if (sn_obj_set_add(&seen, start) < 0) goto done;
    while (pending_count) {
        SnObjHeader *obj = pending[--pending_count];
        if (obj == target) { found = true; break; }

        SnVal *children = NULL;
        size_t child_count = 0;
        switch (obj->tag) {
        case VAL_ARRAY: {
            SnArrayObj *array = (SnArrayObj *)obj;
            children = array->items; child_count = array->count;
            break;
        }
        case VAL_OBJECT: {
            SnInstanceObj *instance = (SnInstanceObj *)obj;
            children = instance->fields; child_count = instance->field_count;
            break;
        }
        case VAL_VARIANT: {
            SnVariantObj *variant = (SnVariantObj *)obj;
            children = variant->payloads; child_count = variant->payload_count;
            break;
        }
        default:
            break;
        }
        for (size_t i = 0; i < child_count; ++i) {
            if (children[i].tag >= VAL_STRING && children[i].as.obj) {
                int added = sn_obj_set_add(&seen, children[i].as.obj);
                if (added < 0) goto done;
                if (added == 1 &&
                    !sn_obj_queue_push(&pending, &pending_count, &pending_cap,
                                       pending_inline, children[i].as.obj)) goto done;
            }
        }
    }
    traversal_done = true;
done:
    if (pending != pending_inline) free(pending);
    if (seen.owns_items) free(seen.items);
    *traversal_complete = traversal_done;
    return found;
}

static bool sn_val_container_store(SnObjHeader *container, SnVal *slot, SnVal value) {
    if (!container || !slot) return false;
    /* Strings are leaves, so they cannot reach the destination container. */
    if ((value.tag == VAL_ARRAY || value.tag == VAL_OBJECT || value.tag == VAL_VARIANT) &&
        value.as.obj) {
        if (value.as.obj == container) return false;
        bool complete = false;
        bool creates_cycle = sn_obj_reaches(value.as.obj, container, &complete);
        if (!complete || creates_cycle) return false;
    }
    return sn_val_assign(slot, value);
}

bool sn_val_array_set(SnVal array, size_t index, SnVal value) {
    if (array.tag != VAL_ARRAY || !array.as.obj) return false;
    sn_value_validate_object(array);
    SnArrayObj *obj = (SnArrayObj *)array.as.obj;
    return index < obj->count && sn_val_container_store(&obj->hdr, &obj->items[index], value);
}

static bool sn_val_container_move_is_safe(SnObjHeader *container, SnVal *source) {
    if (!container || !source || source->tag == VAL_MOVED) return false;
    sn_value_validate_object(*source);
    if ((source->tag == VAL_ARRAY || source->tag == VAL_OBJECT ||
         source->tag == VAL_VARIANT) && source->as.obj) {
        if (source->as.obj == container) return false;
        bool complete = false;
        if (sn_obj_reaches(source->as.obj, container, &complete) || !complete)
            return false;
    }
    return true;
}

static bool sn_val_container_commit_move(SnVal *slot, SnVal *source) {
    if (!slot || !source || source->tag == VAL_MOVED) return false;
    SnVal previous = *slot;
    *slot = sn_val_move(source);
    sn_val_release(previous);
    return true;
}

static bool sn_val_container_store_move(SnObjHeader *container, SnVal *slot,
                                        SnVal *source) {
    return sn_val_container_move_is_safe(container, source) &&
           sn_val_container_commit_move(slot, source);
}

static bool sn_val_array_reserve(SnArrayObj *array, size_t needed) {
    if (needed <= array->capacity) return true;
    size_t capacity = array->capacity ? array->capacity : 4u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > (SIZE_MAX - sizeof(SnArrayObj)) / sizeof(SnVal)) return false;
    size_t old_capacity = array->capacity;
    size_t old_size = sizeof(SnArrayObj) + old_capacity * sizeof(SnVal);
    size_t new_size = sizeof(SnArrayObj) + capacity * sizeof(SnVal);
    SnVal *items = (SnVal *)realloc(array->items, capacity * sizeof(SnVal));
    if (!items) return false;
    memset(items + old_capacity, 0, (capacity - old_capacity) * sizeof(SnVal));
    array->items = items;
    array->capacity = capacity;
    sn_value_track_resize(VAL_ARRAY, old_size, new_size);
    array->hdr.allocation_size = new_size;
    return true;
}

bool sn_val_array_push_move(SnVal array, SnVal *value) {
    if (array.tag != VAL_ARRAY || !array.as.obj || !value || value->tag == VAL_MOVED)
        return false;
    sn_value_validate_object(array);
    SnArrayObj *obj = (SnArrayObj *)array.as.obj;
    if (obj->count == SIZE_MAX ||
        !sn_val_container_move_is_safe(&obj->hdr, value) ||
        !sn_val_array_reserve(obj, obj->count + 1u))
        return false;
    if (!sn_val_container_commit_move(&obj->items[obj->count], value))
        return false;
    obj->count++;
    return true;
}

static void sn_val_array_shrink(SnArrayObj *array, size_t capacity) {
    size_t old_size = array->hdr.allocation_size;
    if (!capacity) {
        free(array->items);
        array->items = NULL;
        array->capacity = 0;
    } else {
        SnVal *items = (SnVal *)realloc(array->items, capacity * sizeof(SnVal));
        if (!items) return;
        array->items = items;
        array->capacity = capacity;
    }
    size_t new_size = sizeof(SnArrayObj) + array->capacity * sizeof(SnVal);
    sn_value_track_resize(VAL_ARRAY, old_size, new_size);
    array->hdr.allocation_size = new_size;
}

SnVal sn_val_array_pop(SnVal array) {
    if (array.tag != VAL_ARRAY || !array.as.obj) return sn_val_unit();
    sn_value_validate_object(array);
    SnArrayObj *obj = (SnArrayObj *)array.as.obj;
    if (!obj->count) return sn_val_unit();
    size_t index = --obj->count;
    SnVal result = sn_val_move(&obj->items[index]);
    obj->items[index] = sn_val_unit();
    if (!obj->count) {
        sn_val_array_shrink(obj, 0);
    } else if (obj->capacity > 4u && obj->count <= obj->capacity / 4u) {
        size_t capacity = obj->capacity / 2u;
        if (capacity < 4u) capacity = 4u;
        sn_val_array_shrink(obj, capacity);
    }
    return result;
}

bool sn_val_array_clear(SnVal array) {
    if (array.tag != VAL_ARRAY || !array.as.obj) return false;
    sn_value_validate_object(array);
    SnArrayObj *obj = (SnArrayObj *)array.as.obj;
    for (size_t i = 0; i < obj->count; ++i) {
        SnVal child = obj->items[i];
        obj->items[i] = sn_val_unit();
        sn_val_release(child);
    }
    obj->count = 0;
    sn_val_array_shrink(obj, 0);
    return true;
}

bool sn_val_instance_set(SnVal instance, size_t index, SnVal value) {
    if (instance.tag != VAL_OBJECT || !instance.as.obj) return false;
    sn_value_validate_object(instance);
    SnInstanceObj *obj = (SnInstanceObj *)instance.as.obj;
    return index < obj->field_count && sn_val_container_store(&obj->hdr, &obj->fields[index], value);
}

bool sn_val_array_set_move(SnVal array, size_t index, SnVal *value) {
    if (array.tag != VAL_ARRAY || !array.as.obj) return false;
    sn_value_validate_object(array);
    SnArrayObj *obj = (SnArrayObj *)array.as.obj;
    if (index >= obj->capacity ||
        !sn_val_container_store_move(&obj->hdr, &obj->items[index], value)) return false;
    if (obj->count <= index) obj->count = index + 1u;
    return true;
}

bool sn_val_instance_set_move(SnVal instance, size_t index, SnVal *value) {
    if (instance.tag != VAL_OBJECT || !instance.as.obj) return false;
    sn_value_validate_object(instance);
    SnInstanceObj *obj = (SnInstanceObj *)instance.as.obj;
    return index < obj->field_count &&
           sn_val_container_store_move(&obj->hdr, &obj->fields[index], value);
}

bool sn_val_variant_set_move(SnVal variant, size_t index, SnVal *value) {
    if (variant.tag != VAL_VARIANT || !variant.as.obj) return false;
    sn_value_validate_object(variant);
    SnVariantObj *obj = (SnVariantObj *)variant.as.obj;
    return index < obj->payload_count &&
           sn_val_container_store_move(&obj->hdr, &obj->payloads[index], value);
}

