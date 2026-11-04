#include "vm_internal.h"

#include <stdio.h>

static int vm_composite_fail(SnVM *vm, const char *message) {
    fprintf(stderr, "runtime error: %s\n", message);
    sn_vm_clear_execution_stack(vm);
    return -1;
}

static int vm_has_stack_values(const SnVM *vm, size_t count) {
    return count <= (size_t)(vm->stack_top - vm->stack);
}

int sn_vm_execute_composite(SnVM *vm, SnOpcode op, uint8_t **ip) {
    switch (op) {
    case OP_NEW_OBJ: {
        uint32_t class_id = sn_vm_read_u32(ip);
        uint32_t field_count = sn_vm_read_u32(ip);
        if (!vm_has_stack_values(vm, field_count))
            return vm_composite_fail(vm, "not enough stack values for object construction");
        SnVal instance = sn_val_instance(class_id, field_count);
        for (size_t i = field_count; i > 0; --i) {
            SnVal field = sn_vm_pop(vm);
            if (!sn_val_instance_set_move(instance, i - 1u, &field)) {
                sn_val_release(field);
                sn_val_release(instance);
                return vm_composite_fail(vm, "object field initialization rejected");
            }
        }
        sn_vm_push(vm, instance);
        return 1;
    }
    case OP_GET_FIELD: {
        uint32_t field_index = sn_vm_read_u32(ip);
        SnVal target = sn_vm_pop(vm);
        if (target.tag == VAL_OBJECT && target.as.obj &&
            (size_t)field_index < sn_val_instance_field_count(target)) {
            SnVal field = sn_val_instance_get(target, field_index);
            sn_val_release(target);
            sn_vm_push(vm, field);
        } else {
            sn_val_release(target);
            sn_vm_push(vm, sn_val_unit());
        }
        return 1;
    }
    case OP_SET_FIELD: {
        uint32_t field_index = sn_vm_read_u32(ip);
        SnVal value = sn_vm_pop(vm), target = sn_vm_pop(vm);
        if (!sn_val_instance_set(target, field_index, value)) {
            sn_val_release(value);
            sn_val_release(target);
            return vm_composite_fail(vm,
                "field assignment rejected (reference cycle, invalid target, or memory pressure)");
        }
        sn_val_release(target);
        sn_vm_push(vm, value);
        return 1;
    }
    case OP_NEW_ARRAY: {
        uint32_t count = sn_vm_read_u32(ip);
        if (!vm_has_stack_values(vm, count))
            return vm_composite_fail(vm, "not enough stack values for array construction");
        SnVal array = sn_val_array(count);
        for (size_t i = count; i > 0; --i) {
            SnVal item = sn_vm_pop(vm);
            if (!sn_val_array_set_move(array, i - 1u, &item)) {
                sn_val_release(item);
                sn_val_release(array);
                return vm_composite_fail(vm, "array element initialization rejected");
            }
        }
        sn_vm_push(vm, array);
        return 1;
    }
    case OP_GET_INDEX: {
        SnVal index = sn_vm_pop(vm), target = sn_vm_pop(vm), result = sn_val_unit();
        if (target.tag == VAL_ARRAY && target.as.obj && index.tag == VAL_INT &&
            index.as.i >= 0 && (uint64_t)index.as.i < sn_val_array_length(target))
            result = sn_val_array_get(target, (size_t)index.as.i);
        sn_val_release(target);
        sn_val_release(index);
        sn_vm_push(vm, result);
        return 1;
    }
    case OP_SET_INDEX: {
        SnVal value = sn_vm_pop(vm), index = sn_vm_pop(vm), target = sn_vm_pop(vm);
        if (index.tag != VAL_INT || index.as.i < 0 ||
            !sn_val_array_set(target, (size_t)index.as.i, value)) {
            sn_val_release(value);
            sn_val_release(target);
            sn_val_release(index);
            return vm_composite_fail(vm,
                "array assignment rejected (reference cycle, invalid target, or memory pressure)");
        }
        sn_val_release(target);
        sn_val_release(index);
        sn_vm_push(vm, value);
        return 1;
    }
    case OP_ARRAY_PUSH: {
        SnVal value = sn_vm_pop(vm), target = sn_vm_pop(vm);
        size_t old_length = target.tag == VAL_ARRAY ? sn_val_array_length(target) : 0;
        if (target.tag != VAL_ARRAY || old_length >= (size_t)INT64_MAX ||
            !sn_val_array_push_move(target, &value)) {
            sn_val_release(value);
            sn_val_release(target);
            return vm_composite_fail(vm,
                old_length >= (size_t)INT64_MAX
                    ? "array length exceeds the language integer range"
                    : "array push rejected (invalid target, reference cycle, or memory pressure)");
        }
        size_t length = sn_val_array_length(target);
        sn_val_release(target);
        sn_vm_push(vm, sn_val_int((int64_t)length));
        return 1;
    }
    case OP_ARRAY_POP: {
        SnVal target = sn_vm_pop(vm), result = sn_val_unit();
        if (target.tag == VAL_ARRAY) result = sn_val_array_pop(target);
        sn_val_release(target);
        sn_vm_push(vm, result);
        return 1;
    }
    case OP_ARRAY_CLEAR: {
        SnVal target = sn_vm_pop(vm);
        if (target.tag != VAL_ARRAY || !sn_val_array_clear(target)) {
            sn_val_release(target);
            return vm_composite_fail(vm, "array clear rejected (invalid target)");
        }
        sn_val_release(target);
        sn_vm_push(vm, sn_val_int(0));
        return 1;
    }
    case OP_VARIANT: {
        uint32_t tag_id = sn_vm_read_u32(ip);
        uint32_t payload_count = sn_vm_read_u32(ip);
        uint32_t name_index = sn_vm_read_u32(ip);
        if (!vm->unit->string_pool.strings ||
            name_index >= vm->unit->string_pool.count ||
            !vm->unit->string_pool.strings[name_index])
            return vm_composite_fail(vm, "variant name index out of bounds");
        if (!vm_has_stack_values(vm, payload_count))
            return vm_composite_fail(vm, "not enough stack values for variant construction");
        SnVal variant = sn_val_variant(tag_id,
                                      vm->unit->string_pool.strings[name_index],
                                      payload_count);
        for (size_t i = payload_count; i > 0; --i) {
            SnVal payload = sn_vm_pop(vm);
            if (!sn_val_variant_set_move(variant, i - 1u, &payload)) {
                sn_val_release(payload);
                sn_val_release(variant);
                return vm_composite_fail(vm, "variant payload initialization rejected");
            }
        }
        sn_vm_push(vm, variant);
        return 1;
    }
    case OP_IS_VARIANT: {
        uint32_t tag_id = sn_vm_read_u32(ip);
        SnVal value = sn_vm_pop(vm);
        bool matches = sn_val_variant_is(value, tag_id);
        sn_val_release(value);
        sn_vm_push(vm, sn_val_bool(matches));
        return 1;
    }
    case OP_UNWRAP_VARIANT: {
        uint32_t payload_index = sn_vm_read_u32(ip);
        SnVal value = sn_vm_pop(vm), result = sn_val_unit();
        if (value.tag == VAL_VARIANT && value.as.obj &&
            (size_t)payload_index < sn_val_variant_payload_count(value))
            result = sn_val_variant_get(value, payload_index);
        sn_val_release(value);
        sn_vm_push(vm, result);
        return 1;
    }
    default:
        return 0;
    }
}
