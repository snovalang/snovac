#include "builtins.h"

#include <string.h>

/* Where a member's result type comes from. Anything the member NAME alone
 * doesn't determine is B_ANY on purpose — see the header. */
typedef enum {
    B_UNIT,
    B_INT,
    B_BOOL,
    B_STRING,
    B_CHAR,
    B_ANY,
    B_ELEM,         /* the receiver array's element type */
    B_SELF,         /* the receiver type unchanged */
    B_ARRAY_ANY,    /* Array<any> */
    B_ARRAY_STRING  /* Array<string> */
} BSlot;

typedef struct {
    const char *name;
    BSlot ret;
    uint8_t arity;
    BSlot p0;
    BSlot p1;
} BMember;

/* crates/snovalang/src/native/selfcheck/static_facts.rs, "Array" entry, in the
 * same order. Result types are this module's addition — that table has none. */
static const BMember ARRAY_MEMBERS[] = {
    {"length", B_INT, 0, B_ANY, B_ANY}, {"len", B_INT, 0, B_ANY, B_ANY},
    {"isEmpty", B_BOOL, 0, B_ANY, B_ANY}, {"push", B_INT, 1, B_ELEM, B_ANY},
    {"pop", B_ELEM, 0, B_ANY, B_ANY}, {"clear", B_INT, 0, B_ANY, B_ANY},
    {"get", B_ELEM, 1, B_INT, B_ANY}, {"set", B_UNIT, 2, B_INT, B_ELEM},
    {"map", B_ARRAY_ANY, 1, B_ANY, B_ANY}, {"filter", B_SELF, 1, B_ANY, B_ANY},
    {"forEach", B_UNIT, 1, B_ANY, B_ANY}, {"reduce", B_ANY, 2, B_ANY, B_ANY},
    {"flatMap", B_ARRAY_ANY, 1, B_ANY, B_ANY}, {"take", B_SELF, 1, B_INT, B_ANY},
    {"drop", B_SELF, 1, B_INT, B_ANY}, {"first", B_ELEM, 0, B_ANY, B_ANY},
    {"last", B_ELEM, 0, B_ANY, B_ANY}, {"join", B_STRING, 1, B_STRING, B_ANY},
    {"concurrentMapping", B_ARRAY_ANY, 1, B_ANY, B_ANY},
    {"concurrentMappingWith", B_ARRAY_ANY, 2, B_ANY, B_ANY},
    {"concurrentForEach", B_UNIT, 1, B_ANY, B_ANY},
};

/* Same source, "string" entry. Indexing and `charAt` yield `char`: a string
 * is a concatenation of char values. `substring` stays a string. */
static const BMember STRING_MEMBERS[] = {
    {"length", B_INT, 0, B_ANY, B_ANY}, {"len", B_INT, 0, B_ANY, B_ANY},
    {"toString", B_STRING, 0, B_ANY, B_ANY}, {"asString", B_STRING, 0, B_ANY, B_ANY},
    {"charAt", B_CHAR, 1, B_INT, B_ANY}, {"substring", B_STRING, 2, B_INT, B_INT},
    {"trim", B_STRING, 0, B_ANY, B_ANY}, {"trimStart", B_STRING, 0, B_ANY, B_ANY},
    {"trimEnd", B_STRING, 0, B_ANY, B_ANY}, {"startsWith", B_BOOL, 1, B_STRING, B_ANY},
    {"endsWith", B_BOOL, 1, B_STRING, B_ANY}, {"contains", B_BOOL, 1, B_STRING, B_ANY},
    {"indexOf", B_INT, 1, B_STRING, B_ANY}, {"lastIndexOf", B_INT, 1, B_STRING, B_ANY},
    {"split", B_ARRAY_STRING, 1, B_STRING, B_ANY},
    {"replace", B_STRING, 2, B_STRING, B_STRING}, {"isEmpty", B_BOOL, 0, B_ANY, B_ANY},
};

/* Same source: int/long/decimal/float/double/bool all carry exactly these. */
static const BMember SCALAR_MEMBERS[] = {
    {"toString", B_STRING, 0, B_ANY, B_ANY},
    {"asString", B_STRING, 0, B_ANY, B_ANY},
};

static SnTypeRep *slot_type(SnTypeTable *t, BSlot slot, const SnTypeRep *recv) {
    switch (slot) {
    case B_UNIT:
        return sn_type_unit(t);
    case B_INT:
        return sn_type_int(t);
    case B_BOOL:
        return sn_type_bool(t);
    case B_STRING:
        return sn_type_string(t);
    case B_CHAR:
        return sn_type_char(t);
    case B_ANY:
        return sn_type_any(t);
    case B_ELEM:
        return (recv->tag == SN_T_ARRAY && recv->nargs == 1) ? recv->args[0]
                                                             : sn_type_any(t);
    case B_SELF:
        return (SnTypeRep *)recv;
    case B_ARRAY_ANY:
        return sn_type_array(t, sn_type_any(t));
    case B_ARRAY_STRING:
        return sn_type_array(t, sn_type_string(t));
    }
    return sn_type_any(t);
}

static SnTypeRep *method_of(SnTypeTable *t, const BMember *member,
                            const SnTypeRep *recv) {
    SnTypeRep *params[2];
    if (member->arity > 0) params[0] = slot_type(t, member->p0, recv);
    if (member->arity > 1) params[1] = slot_type(t, member->p1, recv);
    return sn_type_func(t, member->arity ? params : NULL, member->arity,
                        slot_type(t, member->ret, recv));
}

static const BMember *find_member(const BMember *table, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(table[i].name, name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

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
    if (iname == sn_intern_cstr(it, "Option") || iname == sn_intern_cstr(it, "Result")) {
        static SnSymbol opt_sym = { .name = "Option", .kind = SN_SYM_TYPE };
        static SnSymbol res_sym = { .name = "Result", .kind = SN_SYM_TYPE };
        SnSymbol *sym = (iname == sn_intern_cstr(it, "Option")) ? &opt_sym : &res_sym;
        return sn_type_named(t, sym, args, nargs);
    }
    return NULL;
}

int sn_builtin_is_generic_name(SnInternTable *it, const char *iname) {
    return iname == sn_intern_cstr(it, "Array") || iname == sn_intern_cstr(it, "Partial") ||
           iname == sn_intern_cstr(it, "Option") || iname == sn_intern_cstr(it, "Result");
}

SnTypeRep *sn_builtin_member(SnTypeTable *t, SnInternTable *it, const SnTypeRep *recv,
                             const char *iname) {
    (void)it;
    if (!recv) {
        return NULL;
    }

    const BMember *m = NULL;
    switch (recv->tag) {
    case SN_T_ARRAY:
        m = find_member(ARRAY_MEMBERS, sizeof(ARRAY_MEMBERS) / sizeof(ARRAY_MEMBERS[0]),
                        iname);
        break;
    case SN_T_STRING:
        m = find_member(STRING_MEMBERS, sizeof(STRING_MEMBERS) / sizeof(STRING_MEMBERS[0]),
                        iname);
        break;
    case SN_T_INT:
    case SN_T_LONG:
    case SN_T_DOUBLE:
    case SN_T_DECIMAL:
    case SN_T_FLOAT:
    case SN_T_BYTE:
    case SN_T_INT8:
    case SN_T_INT16:
    case SN_T_INT32:
    case SN_T_INT64:
    case SN_T_INT128:
    case SN_T_BOOL:
    case SN_T_CHAR:
        m = find_member(SCALAR_MEMBERS, sizeof(SCALAR_MEMBERS) / sizeof(SCALAR_MEMBERS[0]),
                        iname);
        break;
    default:
        return NULL;
    }
    return m ? method_of(t, m, recv) : NULL;
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

SnTypeRep *sn_builtin_static_member(SnTypeTable *t, SnInternTable *it,
                                    const SnTypeRep *recv, const char *iname) {
    (void)it;
    if (!recv || recv->tag != SN_T_ARRAY) {
        return NULL;
    }
    /* `Array<Post>.new()` — tests/compile-pass/pipeline-element-type.snl.
     * The only static member the corpus uses on an intrinsic. */
    if (strcmp(iname, "new") == 0) {
        return sn_type_func(t, NULL, 0, (SnTypeRep *)recv);
    }
    return NULL;
}
