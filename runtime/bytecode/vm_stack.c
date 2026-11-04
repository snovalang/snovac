#include "vm_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define read_u32 sn_vm_read_u32
int sn_vm_init(SnVM *vm, SnBCUnit *unit) {
    if (!vm) return 0;
    memset(vm, 0, sizeof(*vm));
    vm->stack_top = vm->stack;
    if (!unit) return 0;
    vm->unit = unit;
    return 1;
}

int sn_vm_reserve_globals(SnVM *vm, size_t needed) {
    if (!vm) return 0;
    if (needed > SN_VM_GLOBALS_MAX) return 0;
    if (needed <= vm->global_capacity) {
        if (vm->global_count < needed) vm->global_count = needed;
        return 1;
    }
    size_t capacity = vm->global_capacity ? vm->global_capacity : 8u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > SIZE_MAX / sizeof(*vm->globals)) return 0;
    size_t old_capacity = vm->global_capacity;
    SnVal *globals = (SnVal *)realloc(vm->globals, capacity * sizeof(*globals));
    if (!globals) return 0;
    vm->globals = globals;
    for (size_t i = old_capacity; i < capacity; ++i)
        vm->globals[i] = sn_val_unit();
    vm->global_capacity = capacity;
    vm->global_count = needed;
    return 1;
}

void sn_vm_free(SnVM *vm) {
    if (!vm) return;
    for (SnVal *s = vm->stack; s < vm->stack_top; s++) {
        SnVal owned = *s;
        *s = sn_val_unit();
        sn_val_release(owned);
    }
    vm->stack_top = vm->stack;
    if (vm->globals) {
        for (size_t i = 0; i < vm->global_count; i++) {
            SnVal owned = vm->globals[i];
            vm->globals[i] = sn_val_unit();
            sn_val_release(owned);
        }
    }
    free(vm->globals);
    vm->globals = NULL;
    vm->global_count = 0;
    vm->global_capacity = 0;
    vm->frame_count = 0;
    sn_val_report_memory_stats(stderr, "vm-values");
}

void sn_vm_push(SnVM *vm, SnVal val) {
    if (vm->stack_top >= vm->stack + SN_VM_STACK_MAX) {
        fputs("runtime error: VM stack overflow\n", stderr);
        abort();
    }
    *vm->stack_top++ = val;
}

SnVal sn_vm_pop(SnVM *vm) {
    if (vm->stack_top <= vm->stack) {
        fputs("runtime error: VM stack underflow\n", stderr);
        abort();
    }
    return sn_val_move(--vm->stack_top);
}

SnVal sn_vm_peek(SnVM *vm, int distance) {
    if (distance < 0 || (size_t)distance >= (size_t)(vm->stack_top - vm->stack)) {
        fputs("runtime error: VM stack access out of bounds\n", stderr);
        abort();
    }
    return vm->stack_top[-1 - distance];
}

uint32_t sn_vm_read_u32(uint8_t **ip) {
    uint32_t val = (uint32_t)((*ip)[0]) |
                   ((uint32_t)((*ip)[1]) << 8) |
                   ((uint32_t)((*ip)[2]) << 16) |
                   ((uint32_t)((*ip)[3]) << 24);
    *ip += 4;
    return val;
}

int32_t sn_vm_read_i32(uint8_t **ip) {
    return (int32_t)read_u32(ip);
}

int64_t sn_vm_read_i64(uint8_t **ip) {
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= ((uint64_t)((*ip)[i]) << (i * 8));
    }
    *ip += 8;
    return (int64_t)val;
}

double sn_vm_read_double(uint8_t **ip) {
    union {
        double d;
        uint64_t u;
    } cvt;
    cvt.u = 0;
    for (int i = 0; i < 8; i++) {
        cvt.u |= ((uint64_t)((*ip)[i]) << (i * 8));
    }
    *ip += 8;
    return cvt.d;
}

