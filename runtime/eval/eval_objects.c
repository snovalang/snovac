#include "eval_internal.h"

#include <stdint.h>

ArrayVal *array_new(Interp *in) {
    ArrayVal *array = (ArrayVal *)sn_arena_calloc(in->arena, sizeof(ArrayVal));
    array->owner_region = in->current_region;
    if (in->current_region) {
        array->next_owned = in->current_region->arrays;
        in->current_region->arrays = array;
    } else {
        array->next_owned = in->arrays;
        in->arrays = array;
    }
    return array;
}

int array_push(Interp *in, ArrayVal *array, Value value, SnSpan span) {
    if (in->current_region && value.kind != V_UNIT &&
        value.kind != V_INT && value.kind != V_DOUBLE && value.kind != V_BOOL &&
        value.kind != V_MOVED && array->owner_region != in->current_region)
        eval_value_mark_escaping(in, value);
    if (in->array_used_bytes > SIZE_MAX - sizeof(Value)) {
        rt_error(in, SNOVA_TYPE_ERROR, span, "array live memory accounting overflow");
        return 0;
    }
    if (array->len == array->capacity) {
        size_t capacity = array->capacity ? array->capacity * 2u : 4u;
        if (capacity < array->capacity || capacity > SIZE_MAX / sizeof(Value)) {
            rt_error(in, SNOVA_TYPE_ERROR, span, "array capacity overflow");
            return 0;
        }
        size_t old_bytes = array->capacity * sizeof(Value);
        size_t new_bytes = capacity * sizeof(Value);
        if (in->array_reserved_bytes > SIZE_MAX - (new_bytes - old_bytes)) {
            rt_error(in, SNOVA_TYPE_ERROR, span, "array memory accounting overflow");
            return 0;
        }
        Value *items = (Value *)realloc(array->items, capacity * sizeof(Value));
        if (!items) {
            rt_error(in, SNOVA_TYPE_ERROR, span, "out of memory growing array");
            return 0;
        }
        array->items = items;
        array->capacity = capacity;
        in->array_reserved_bytes += new_bytes - old_bytes;
        if (in->array_reserved_bytes > in->array_peak_bytes) {
            in->array_peak_bytes = in->array_reserved_bytes;
        }
    }
    array->items[array->len++] = value;
    in->array_used_bytes += sizeof(Value);
    if (in->array_used_bytes > in->array_peak_used_bytes) {
        in->array_peak_used_bytes = in->array_used_bytes;
    }
    return 1;
}

void array_release_storage(Interp *in, ArrayVal *array) {
    if (!array) return;
    size_t bytes = array->capacity * sizeof(Value);
    if (in->array_reserved_bytes < bytes) {
        fputs("snovac runtime: array memory accounting underflow\n", stderr);
        abort();
    }
    size_t used_bytes = array->len * sizeof(Value);
    if (in->array_used_bytes < used_bytes) {
        fputs("snovac runtime: live array memory accounting underflow\n", stderr);
        abort();
    }
    free(array->items);
    in->array_reserved_bytes -= bytes;
    in->array_used_bytes -= used_bytes;
    array->items = NULL;
    array->len = array->capacity = 0;
}

void array_free_storage(Interp *in, ArrayVal *arrays) {
    while (arrays) {
        ArrayVal *next = arrays->next_owned;
        array_release_storage(in, arrays);
        arrays->next_owned = NULL;
        arrays = next;
    }
}

/* ── objects ──────────────────────────────────────────────────────────────── */

Value default_for(const SnType *t) {
    if (t && t->name) {
        if (strcmp(t->name, "int") == 0 || strcmp(t->name, "long") == 0) return v_int(0);
        if (strcmp(t->name, "double") == 0 || strcmp(t->name, "decimal") == 0) return v_double(0);
        if (strcmp(t->name, "bool") == 0) return v_bool(0);
        if (strcmp(t->name, "string") == 0) return v_str("");
    }
    return v_unit();
}

Object *instantiate(Interp *in, const SnDecl *cls, SnList *args, Env *env,
                    SnSpan span) {
    Object *o = (Object *)sn_arena_calloc(in->arena, sizeof(Object));
    o->cls = cls;
    o->owner_region = in->current_region;

    /* Fields are declared in order; positional constructor arguments fill them
     * in that same order (`LiveWorkbook(path, frame, false)`). */
    size_t argi = 0;
    for (size_t i = 0; i < cls->members.len; i++) {
        const SnDecl *m = (const SnDecl *)cls->members.items[i];
        if (m->kind != SN_DECL_FIELD) {
            continue;
        }
        Value v;
        if (args && argi < args->len) {
            v = eval_expr(in, env, (const SnExpr *)args->items[argi]);
            argi++;
        } else if (m->init) {
            v = eval_expr(in, env, m->init);
        } else {
            v = default_for(m->type);
        }
        Value *slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
        *slot = v;
        sn_list_push(in->arena, &o->names, (void *)m->name);
        sn_list_push(in->arena, &o->slots, slot);
    }
    (void)span;
    return o;
}

Value *object_field(Object *o, const char *name) {
    for (size_t i = 0; i < o->names.len; i++) {
        if (strcmp((const char *)o->names.items[i], name) == 0) {
            return (Value *)o->slots.items[i];
        }
    }
    return NULL;
}

