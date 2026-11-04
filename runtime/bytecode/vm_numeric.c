#include "vm_internal.h"
#include <string.h>

int sn_vm_execute_numeric(SnVM *vm, SnOpcode op) {
    switch (op) {
    case OP_ADD: {
        SnVal b = sn_vm_pop(vm), a = sn_vm_pop(vm);
        if (a.tag == VAL_STRING && b.tag == VAL_STRING) {
            SnVal result = sn_val_string_concat(a, b);
            sn_val_release(a);
            sn_val_release(b);
            sn_vm_push(vm, result);
        } else if (a.tag == VAL_DOUBLE || b.tag == VAL_DOUBLE) {
            double da = a.tag == VAL_DOUBLE ? a.as.d : (double)a.as.i;
            double db = b.tag == VAL_DOUBLE ? b.as.d : (double)b.as.i;
            sn_val_release(a);
            sn_val_release(b);
            sn_vm_push(vm, sn_val_double(da + db));
        } else {
            int64_t ia = a.as.i, ib = b.as.i;
            sn_val_release(a);
            sn_val_release(b);
            sn_vm_push(vm, sn_val_int(ia + ib));
        }
        return 1;
    }
    case OP_SUB: {
        SnVal b = sn_vm_pop(vm), a = sn_vm_pop(vm);
        if (a.tag == VAL_DOUBLE || b.tag == VAL_DOUBLE) {
            double da = a.tag == VAL_DOUBLE ? a.as.d : (double)a.as.i;
            double db = b.tag == VAL_DOUBLE ? b.as.d : (double)b.as.i;
            sn_vm_push(vm, sn_val_double(da - db));
        } else {
            sn_vm_push(vm, sn_val_int(a.as.i - b.as.i));
        }
        sn_val_release(a);
        sn_val_release(b);
        return 1;
    }
    case OP_MUL: {
        SnVal b = sn_vm_pop(vm), a = sn_vm_pop(vm);
        if (a.tag == VAL_DOUBLE || b.tag == VAL_DOUBLE) {
            double da = a.tag == VAL_DOUBLE ? a.as.d : (double)a.as.i;
            double db = b.tag == VAL_DOUBLE ? b.as.d : (double)b.as.i;
            sn_vm_push(vm, sn_val_double(da * db));
        } else {
            sn_vm_push(vm, sn_val_int(a.as.i * b.as.i));
        }
        sn_val_release(a);
        sn_val_release(b);
        return 1;
    }
    case OP_DIV: {
        SnVal b = sn_vm_pop(vm), a = sn_vm_pop(vm);
        if (a.tag == VAL_DOUBLE || b.tag == VAL_DOUBLE) {
            double da = a.tag == VAL_DOUBLE ? a.as.d : (double)a.as.i;
            double db = b.tag == VAL_DOUBLE ? b.as.d : (double)b.as.i;
            sn_vm_push(vm, sn_val_double(db == 0.0 ? 0.0 : da / db));
        } else {
            sn_vm_push(vm, sn_val_int(b.as.i == 0 ? 0 : a.as.i / b.as.i));
        }
        sn_val_release(a);
        sn_val_release(b);
        return 1;
    }
    case OP_MOD: {
        SnVal b = sn_vm_pop(vm), a = sn_vm_pop(vm);
        sn_vm_push(vm, sn_val_int(b.as.i == 0 ? 0 : a.as.i % b.as.i));
        sn_val_release(a);
        sn_val_release(b);
        return 1;
    }
    case OP_NEG: {
        SnVal a = sn_vm_pop(vm);
        sn_vm_push(vm, a.tag == VAL_DOUBLE ? sn_val_double(-a.as.d)
                                           : sn_val_int(-a.as.i));
        sn_val_release(a);
        return 1;
    }
    case OP_NOT: {
        SnVal a = sn_vm_pop(vm);
        sn_vm_push(vm, sn_val_bool(!a.as.b));
        sn_val_release(a);
        return 1;
    }
    case OP_EQ:
    case OP_NE: {
        SnVal b = sn_vm_pop(vm), a = sn_vm_pop(vm);
        bool equal = sn_val_equal(a, b);
        sn_val_release(a);
        sn_val_release(b);
        sn_vm_push(vm, sn_val_bool(op == OP_EQ ? equal : !equal));
        return 1;
    }
    case OP_LT:
    case OP_LE:
    case OP_GT:
    case OP_GE: {
        SnVal b = sn_vm_pop(vm), a = sn_vm_pop(vm);
        bool result;
        if (a.tag == VAL_DOUBLE || b.tag == VAL_DOUBLE) {
            double da = a.tag == VAL_DOUBLE ? a.as.d : (double)a.as.i;
            double db = b.tag == VAL_DOUBLE ? b.as.d : (double)b.as.i;
            if (op == OP_LT) result = da < db;
            else if (op == OP_LE) result = da <= db;
            else if (op == OP_GT) result = da > db;
            else result = da >= db;
        } else if (a.tag == VAL_STRING && b.tag == VAL_STRING &&
                   (op == OP_LT || op == OP_GT)) {
            int cmp = strcmp(sn_val_string_data(a), sn_val_string_data(b));
            result = op == OP_LT ? cmp < 0 : cmp > 0;
        } else if (op == OP_LT) result = a.as.i < b.as.i;
        else if (op == OP_LE) result = a.as.i <= b.as.i;
        else if (op == OP_GT) result = a.as.i > b.as.i;
        else result = a.as.i >= b.as.i;
        sn_val_release(a);
        sn_val_release(b);
        sn_vm_push(vm, sn_val_bool(result));
        return 1;
    }
    default:
        return 0;
    }
}
