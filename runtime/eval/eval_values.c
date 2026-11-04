#include "eval_internal.h"

#include <stdint.h>

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} StringBuilder;

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
    case V_STRING: return v.as.str.data;
    case V_OBJECT: {
        /* Printing an object uses its `asString()`, matching the convention the
         * corpus relies on (tests/run-pass/counter.snova). */
        const SnDecl *m = find_member(v.as.o->cls, "asString");
        if (m && m->body) {
            SnList none = {0};
            Value s = call_method(in, v.as.o, m, &none, NULL, span);
            if (s.kind == V_STRING) {
                return s.as.str.data;
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
    VariantVal *vt = (VariantVal *)sn_arena_calloc(in->arena, sizeof(VariantVal));
    vt->name = name;
    vt->payload = payload;
    vt->owner_region = in->current_region;
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
        return strcmp(a.as.str.data, b.as.str.data) == 0;
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

