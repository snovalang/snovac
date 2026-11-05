#include "value_internal.h"

#include <stdio.h>
#include <string.h>

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
