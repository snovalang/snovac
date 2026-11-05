#include "builtins.h"

#include <string.h>

SnTypeRep *sn_builtin_generic(SnTypeTable *t, SnInternTable *it, const char *iname,
                              SnTypeRep **args, uint32_t nargs) {
    if (iname == sn_intern_cstr(it, "Array")) {
        return sn_type_array(t, nargs >= 1 ? args[0] : sn_type_any(t));
    }
    if (iname == sn_intern_cstr(it, "Partial")) {
        /* Partial<T> is checked as T; bare `Partial` has nothing to stand in
         * for, so it degrades to `any` rather than inventing a shape. */
        return nargs >= 1 ? args[0] : sn_type_any(t);
    }
    return NULL;
}

int sn_builtin_is_generic_name(SnInternTable *it, const char *iname) {
    return iname == sn_intern_cstr(it, "Array") ||
           iname == sn_intern_cstr(it, "Partial");
}

SnTypeRep *sn_builtin_index_result(SnTypeTable *t, SnInternTable *it,
                                   const SnTypeRep *recv) {
    (void)it;
    if (!recv) {
        return NULL;
    }
    if (recv->tag == SN_T_ARRAY) {
        return recv->nargs == 1 ? recv->args[0] : sn_type_any(t);
    }
    if (recv->tag == SN_T_STRING) {
        return sn_type_char(t);
    }
    return NULL;
}

typedef struct {
    const char *name;
    const char *detail;
    SnTypeRep *(*make)(SnTypeTable *);
} BuiltinPrimitive;

/* Same set as primitive resolution. Order is shortest-name friendly for
 * completion ranking when scores tie: the sorter breaks ties by length. */
static const BuiltinPrimitive PRIMITIVES[] = {
    {"int", "64-bit signed integer", sn_type_int},
    {"long", "64-bit signed integer, distinct from int", sn_type_long},
    {"int8", "8-bit signed integer", sn_type_int8},
    {"int16", "16-bit signed integer", sn_type_int16},
    {"int32", "32-bit signed integer", sn_type_int32},
    {"int64", "64-bit signed integer, distinct from int", sn_type_int64},
    {"int128", "128-bit signed integer", sn_type_int128},
    {"byte", "8-bit unsigned integer", sn_type_byte},
    {"float", "floating-point number", sn_type_float},
    {"double", "64-bit IEEE 754 floating point", sn_type_double},
    {"decimal", "high-precision decimal number", sn_type_decimal},
    {"bool", "boolean", sn_type_bool},
    {"char", "Unicode character", sn_type_char},
    {"string", "UTF-8 string", sn_type_string},
    {"unit", "unit type, the type of a function that returns nothing", sn_type_unit},
    {"any", "unconstrained type", sn_type_any},
};

size_t sn_builtin_primitive_count(void) {
    return sizeof(PRIMITIVES) / sizeof(PRIMITIVES[0]);
}

const char *sn_builtin_primitive_name(size_t index) {
    if (index >= sn_builtin_primitive_count()) {
        return NULL;
    }
    return PRIMITIVES[index].name;
}

const char *sn_builtin_primitive_detail(size_t index) {
    if (index >= sn_builtin_primitive_count()) {
        return NULL;
    }
    return PRIMITIVES[index].detail;
}

SnTypeRep *sn_builtin_primitive_type(SnTypeTable *t, const char *name) {
    if (!t || !name) {
        return NULL;
    }
    for (size_t i = 0; i < sn_builtin_primitive_count(); i++) {
        if (strcmp(PRIMITIVES[i].name, name) == 0) {
            return PRIMITIVES[i].make(t);
        }
    }
    return NULL;
}

int sn_builtin_is_primitive_name(const char *name) {
    if (!name) {
        return 0;
    }
    for (size_t i = 0; i < sn_builtin_primitive_count(); i++) {
        if (strcmp(PRIMITIVES[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}
