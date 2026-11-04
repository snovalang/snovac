#include "value_internal.h"

#include <stdio.h>
#include <string.h>

bool sn_val_equal(SnVal a, SnVal b) {
    if (a.tag != b.tag) {
        if ((a.tag == VAL_INT && b.tag == VAL_DOUBLE)) {
            return (double)a.as.i == b.as.d;
        }
        if ((a.tag == VAL_DOUBLE && b.tag == VAL_INT)) {
            return a.as.d == (double)b.as.i;
        }
        return false;
    }
    switch (a.tag) {
    case VAL_MOVED:
        return false;
    case VAL_UNIT:
        return true;
    case VAL_BOOL:
        return a.as.b == b.as.b;
    case VAL_INT:
        return a.as.i == b.as.i;
    case VAL_DOUBLE:
        return a.as.d == b.as.d;
    case VAL_STRING: {
        SnStringObj *sa = (SnStringObj *)a.as.obj;
        SnStringObj *sb = (SnStringObj *)b.as.obj;
        return sa->len == sb->len && strcmp(sa->chars, sb->chars) == 0;
    }
    case VAL_VARIANT: {
        SnVariantObj *va = (SnVariantObj *)a.as.obj;
        SnVariantObj *vb = (SnVariantObj *)b.as.obj;
        if (va->tag_id != vb->tag_id || va->payload_count != vb->payload_count) {
            return false;
        }
        for (size_t i = 0; i < va->payload_count; i++) {
            if (!sn_val_equal(va->payloads[i], vb->payloads[i])) {
                return false;
            }
        }
        return true;
    }
    default:
        return a.as.obj == b.as.obj;
    }
}

void sn_val_print(SnVal val, bool newline) {
    switch (val.tag) {
    case VAL_MOVED:
        printf("<moved>");
        break;
    case VAL_UNIT:
        printf("unit");
        break;
    case VAL_BOOL:
        printf("%s", val.as.b ? "true" : "false");
        break;
    case VAL_INT:
        printf("%ld", (long)val.as.i);
        break;
    case VAL_DOUBLE:
        printf("%g", val.as.d);
        break;
    case VAL_STRING: {
        SnStringObj *s = (SnStringObj *)val.as.obj;
        printf("%s", s ? s->chars : "");
        break;
    }
    case VAL_ARRAY: {
        SnArrayObj *a = (SnArrayObj *)val.as.obj;
        printf("[");
        if (a) {
            for (size_t i = 0; i < a->count; i++) {
                if (i > 0) printf(", ");
                sn_val_print(a->items[i], false);
            }
        }
        printf("]");
        break;
    }
    case VAL_OBJECT: {
        SnInstanceObj *inst = (SnInstanceObj *)val.as.obj;
        printf("<Instance #%u>", inst ? inst->class_id : 0);
        break;
    }
    case VAL_VARIANT: {
        SnVariantObj *var = (SnVariantObj *)val.as.obj;
        if (var) {
            printf("%s", var->tag_name ? var->tag_name : "Variant");
            if (var->payload_count > 0) {
                printf("(");
                for (size_t i = 0; i < var->payload_count; i++) {
                    if (i > 0) printf(", ");
                    sn_val_print(var->payloads[i], false);
                }
                printf(")");
            }
        }
        break;
    }
    }
    if (newline) {
        printf("\n");
    }
}
