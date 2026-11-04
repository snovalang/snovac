#include "value_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

size_t sn_val_string_length(SnVal string) {
    if (string.tag != VAL_STRING || !string.as.obj) return 0;
    sn_value_validate_object(string);
    return ((SnStringObj *)string.as.obj)->len;
}

const char *sn_val_string_data(SnVal string) {
    if (string.tag != VAL_STRING || !string.as.obj) return NULL;
    sn_value_validate_object(string);
    return ((SnStringObj *)string.as.obj)->chars;
}

size_t sn_val_array_length(SnVal array) {
    if (array.tag != VAL_ARRAY || !array.as.obj) return 0;
    sn_value_validate_object(array);
    return ((SnArrayObj *)array.as.obj)->count;
}

SnVal sn_val_array_get(SnVal array, size_t index) {
    if (array.tag != VAL_ARRAY || !array.as.obj) return sn_val_unit();
    sn_value_validate_object(array);
    SnArrayObj *obj = (SnArrayObj *)array.as.obj;
    return index < obj->count ? sn_val_clone(obj->items[index]) : sn_val_unit();
}

size_t sn_val_instance_field_count(SnVal instance) {
    if (instance.tag != VAL_OBJECT || !instance.as.obj) return 0;
    sn_value_validate_object(instance);
    return ((SnInstanceObj *)instance.as.obj)->field_count;
}

uint32_t sn_val_instance_class_id(SnVal instance) {
    if (instance.tag != VAL_OBJECT || !instance.as.obj) return 0;
    sn_value_validate_object(instance);
    return ((SnInstanceObj *)instance.as.obj)->class_id;
}

SnVal sn_val_instance_get(SnVal instance, size_t index) {
    if (instance.tag != VAL_OBJECT || !instance.as.obj) return sn_val_unit();
    sn_value_validate_object(instance);
    SnInstanceObj *obj = (SnInstanceObj *)instance.as.obj;
    return index < obj->field_count ? sn_val_clone(obj->fields[index]) : sn_val_unit();
}

bool sn_val_variant_is(SnVal variant, uint32_t tag_id) {
    if (variant.tag != VAL_VARIANT || !variant.as.obj) return false;
    sn_value_validate_object(variant);
    return ((SnVariantObj *)variant.as.obj)->tag_id == tag_id;
}

size_t sn_val_variant_payload_count(SnVal variant) {
    if (variant.tag != VAL_VARIANT || !variant.as.obj) return 0;
    sn_value_validate_object(variant);
    return ((SnVariantObj *)variant.as.obj)->payload_count;
}

SnVal sn_val_variant_get(SnVal variant, size_t index) {
    if (variant.tag != VAL_VARIANT || !variant.as.obj) return sn_val_unit();
    sn_value_validate_object(variant);
    SnVariantObj *obj = (SnVariantObj *)variant.as.obj;
    return index < obj->payload_count ? sn_val_clone(obj->payloads[index]) : sn_val_unit();
}

SnVal sn_val_unit(void) {
    SnVal v;
    v.tag = VAL_UNIT;
    v.as.i = 0;
    return v;
}

SnVal sn_val_bool(bool b) {
    SnVal v;
    v.tag = VAL_BOOL;
    v.as.b = b;
    return v;
}

SnVal sn_val_int(int64_t i) {
    SnVal v;
    v.tag = VAL_INT;
    v.as.i = i;
    return v;
}

SnVal sn_val_double(double d) {
    SnVal v;
    v.tag = VAL_DOUBLE;
    v.as.d = d;
    return v;
}

SnVal sn_val_string(const char *str, size_t len) {
    if (len > SIZE_MAX - sizeof(SnStringObj) - 1u) {
        fputs("snovac runtime: string size overflow\n", stderr);
        abort();
    }
    SnStringObj *s = (SnStringObj *)sn_value_alloc(sizeof(SnStringObj) + len + 1u);
    atomic_init(&s->hdr.rc, 1);
    s->hdr.tag = VAL_STRING;
    s->hdr.release_next = NULL;
    s->hdr.allocation_size = sizeof(SnStringObj) + len + 1u;
    s->len = len;
    if (str && len) {
        memcpy(s->chars, str, len);
    } else if (len) {
        memset(s->chars, 0, len);
    }
    s->chars[len] = '\0';
    sn_value_track_allocation(VAL_STRING, s->hdr.allocation_size);

    SnVal v;
    v.tag = VAL_STRING;
    v.as.obj = (SnObjHeader *)s;
    return v;
}

SnVal sn_val_string_concat(SnVal left, SnVal right) {
    if (left.tag != VAL_STRING || right.tag != VAL_STRING ||
        !left.as.obj || !right.as.obj) {
        fputs("snovac runtime: string concatenation requires two strings\n", stderr);
        abort();
    }
    sn_value_validate_object(left);
    sn_value_validate_object(right);
    const SnStringObj *a = (const SnStringObj *)left.as.obj;
    const SnStringObj *b = (const SnStringObj *)right.as.obj;
    if (a->len > SIZE_MAX - b->len) {
        fputs("snovac runtime: string concatenation size overflow\n", stderr);
        abort();
    }
    size_t len = a->len + b->len;
    if (len > SIZE_MAX - sizeof(SnStringObj) - 1u) {
        fputs("snovac runtime: string concatenation size overflow\n", stderr);
        abort();
    }
    SnStringObj *joined = (SnStringObj *)sn_value_alloc(sizeof(SnStringObj) + len + 1u);
    atomic_init(&joined->hdr.rc, 1);
    joined->hdr.tag = VAL_STRING;
    joined->hdr.release_next = NULL;
    joined->hdr.allocation_size = sizeof(SnStringObj) + len + 1u;
    joined->len = len;
    memcpy(joined->chars, a->chars, a->len);
    memcpy(joined->chars + a->len, b->chars, b->len);
    joined->chars[len] = '\0';
    sn_value_track_allocation(VAL_STRING, joined->hdr.allocation_size);

    SnVal result;
    result.tag = VAL_STRING;
    result.as.obj = (SnObjHeader *)joined;
    return result;
}

SnVal sn_val_array(size_t initial_cap) {
    if (initial_cap > (SIZE_MAX - sizeof(SnArrayObj)) / sizeof(SnVal)) {
        fputs("snovac runtime: array allocation size overflow\n", stderr);
        abort();
    }
    size_t allocation_size = sizeof(SnArrayObj) + initial_cap * sizeof(SnVal);
    SnArrayObj *a = (SnArrayObj *)sn_value_alloc(sizeof(*a));
    atomic_init(&a->hdr.rc, 1);
    a->hdr.tag = VAL_ARRAY;
    a->hdr.release_next = NULL;
    a->hdr.allocation_size = allocation_size;
    a->count = 0;
    a->capacity = initial_cap;
    a->items = initial_cap
        ? (SnVal *)sn_value_alloc(initial_cap * sizeof(SnVal)) : NULL;
    if (initial_cap) memset(a->items, 0, initial_cap * sizeof(SnVal));
    sn_value_track_allocation(VAL_ARRAY, allocation_size);

    SnVal v;
    v.tag = VAL_ARRAY;
    v.as.obj = (SnObjHeader *)a;
    return v;
}

SnVal sn_val_instance(uint32_t class_id, size_t field_count) {
    if (field_count > (SIZE_MAX - sizeof(SnInstanceObj)) / sizeof(SnVal)) {
        fputs("snovac runtime: instance allocation size overflow\n", stderr);
        abort();
    }
    size_t allocation_size = sizeof(SnInstanceObj) + field_count * sizeof(SnVal);
    SnInstanceObj *inst = (SnInstanceObj *)sn_value_alloc(allocation_size);
    atomic_init(&inst->hdr.rc, 1);
    inst->hdr.tag = VAL_OBJECT;
    inst->hdr.release_next = NULL;
    inst->hdr.allocation_size = allocation_size;
    inst->class_id = class_id;
    inst->field_count = field_count;
    if (field_count) memset(inst->fields, 0, field_count * sizeof(SnVal));
    sn_value_track_allocation(VAL_OBJECT, allocation_size);

    SnVal v;
    v.tag = VAL_OBJECT;
    v.as.obj = (SnObjHeader *)inst;
    return v;
}

SnVal sn_val_variant(uint32_t tag_id, const char *name, size_t payload_count) {
    size_t name_bytes = name ? strlen(name) + 1u : 0u;
    size_t payload_offset = sizeof(SnVariantObj);
    if (name_bytes > SIZE_MAX - payload_offset) {
        fputs("snovac runtime: variant allocation size overflow\n", stderr);
        abort();
    }
    payload_offset += name_bytes;
    const size_t alignment = _Alignof(SnVal);
    if (payload_offset > SIZE_MAX - (alignment - 1u)) {
        fputs("snovac runtime: variant allocation size overflow\n", stderr);
        abort();
    }
    payload_offset = (payload_offset + alignment - 1u) & ~(alignment - 1u);
    if (payload_count > (SIZE_MAX - payload_offset) / sizeof(SnVal)) {
        fputs("snovac runtime: variant allocation size overflow\n", stderr);
        abort();
    }
    size_t allocation_size = payload_offset + payload_count * sizeof(SnVal);
    SnVariantObj *var = (SnVariantObj *)sn_value_alloc(allocation_size);
    atomic_init(&var->hdr.rc, 1);
    var->hdr.tag = VAL_VARIANT;
    var->hdr.release_next = NULL;
    var->hdr.allocation_size = allocation_size;
    var->tag_id = tag_id;
    if (name) {
        var->tag_name = (char *)var->storage;
        memcpy(var->tag_name, name, name_bytes);
    } else {
        var->tag_name = NULL;
    }
    var->payload_count = payload_count;
    var->payloads = payload_count
                        ? (SnVal *)((unsigned char *)var + payload_offset)
                        : NULL;
    if (payload_count) memset(var->payloads, 0, payload_count * sizeof(SnVal));
    sn_value_track_allocation(VAL_VARIANT, allocation_size);

    SnVal v;
    v.tag = VAL_VARIANT;
    v.as.obj = (SnObjHeader *)var;
    return v;
}

