#ifndef SNOVAC_VALUE_INTERNAL_H
#define SNOVAC_VALUE_INTERNAL_H

#include "value.h"

#include <stdatomic.h>

typedef struct SnStringObj SnStringObj;
typedef struct SnArrayObj SnArrayObj;
typedef struct SnInstanceObj SnInstanceObj;
typedef struct SnVariantObj SnVariantObj;

struct SnObjHeader {
    atomic_uint_fast64_t rc;
    SnValTag tag;
    SnObjHeader *release_next;
    size_t allocation_size;
};

struct SnStringObj {
    SnObjHeader hdr;
    size_t len;
    char chars[];
};

struct SnArrayObj {
    SnObjHeader hdr;
    size_t count;
    size_t capacity;
    SnVal *items;
};

struct SnInstanceObj {
    SnObjHeader hdr;
    uint32_t class_id;
    size_t field_count;
    SnVal fields[];
};

struct SnVariantObj {
    SnObjHeader hdr;
    uint32_t tag_id;
    char *tag_name;
    SnVal *payloads;
    size_t payload_count;
    unsigned char storage[];
};

void sn_value_validate_object(SnVal value);
void *sn_value_alloc(size_t size);
void sn_value_track_allocation(SnValTag tag, size_t bytes);
void sn_value_track_resize(SnValTag tag, size_t old_bytes, size_t new_bytes);

#endif
