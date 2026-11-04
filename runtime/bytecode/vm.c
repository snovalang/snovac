#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "vm.h"
#include "vm_internal.h"
#define push sn_vm_push
#define pop sn_vm_pop
#define peek sn_vm_peek
#define read_u32 sn_vm_read_u32
#define read_i32 sn_vm_read_i32
#define read_i64 sn_vm_read_i64
#define read_double sn_vm_read_double

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The VM owns every value on its operand/local stack. End execution without
 * retaining frame-local values while the caller still owns VM globals. */
void sn_vm_clear_execution_stack(SnVM *vm) {
    if (!vm) return;
    while (vm->stack_top > vm->stack) {
        SnVal owned = *--vm->stack_top;
        *vm->stack_top = sn_val_unit();
        sn_val_release(owned);
    }
    vm->frame_count = 0;
}

int sn_vm_interpret(SnVM *vm) {
    if (!vm->unit || vm->unit->function_count == 0) {
        return 0;
    }
    if (!vm->unit->functions || vm->unit->main_func_idx >= vm->unit->function_count) {
        fprintf(stderr, "runtime error: invalid bytecode entry function\n");
        sn_vm_clear_execution_stack(vm);
        return 1;
    }
    SnFunctionChunk *main_fn = vm->unit->functions[vm->unit->main_func_idx];
    if (!main_fn || !main_fn->chunk.code) {
        fprintf(stderr, "runtime error: incomplete VM or bytecode entry\n");
        sn_vm_clear_execution_stack(vm);
        return 1;
    }

    SnCallFrame *frame = &vm->frames[vm->frame_count++];
    frame->fn = main_fn;
    frame->ip = main_fn->chunk.code;
    frame->slots = vm->stack;

#define VM_REQUIRE(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "runtime error: %s\n", (message)); \
            sn_vm_clear_execution_stack(vm); \
            return 1; \
        } \
    } while (0)

    /* Pad stack for local variables */
    for (size_t i = 0; i < main_fn->local_count; i++) {
        push(vm, sn_val_unit());
    }

    for (;;) {
        SnOpcode op = (SnOpcode)(*frame->ip++);
        if (sn_vm_execute_numeric(vm, op)) continue;
        int composite = sn_vm_execute_composite(vm, op, &frame->ip);
        if (composite < 0) return 1;
        if (composite > 0) continue;
        switch (op) {
        case OP_NOP:
            break;
        case OP_CONST_INT: {
            int64_t val = read_i64(&frame->ip);
            push(vm, sn_val_int(val));
            break;
        }
        case OP_CONST_DOUBLE: {
            double val = read_double(&frame->ip);
            push(vm, sn_val_double(val));
            break;
        }
        case OP_CONST_STRING: {
            uint32_t s_idx = read_u32(&frame->ip);
            VM_REQUIRE(vm->unit->string_pool.strings &&
                       s_idx < vm->unit->string_pool.count &&
                       vm->unit->string_pool.strings[s_idx],
                       "string constant index out of bounds");
            const char *str = vm->unit->string_pool.strings[s_idx];
            push(vm, sn_val_string(str, strlen(str)));
            break;
        }
        case OP_CONST_BOOL: {
            uint8_t b = *frame->ip++;
            push(vm, sn_val_bool(b != 0));
            break;
        }
        case OP_CONST_UNIT: {
            push(vm, sn_val_unit());
            break;
        }
        case OP_POP: {
            SnVal v = pop(vm);
            sn_val_release(v);
            break;
        }
        case OP_DUP: {
            SnVal v = peek(vm, 0);
            push(vm, sn_val_clone(v));
            break;
        }
        case OP_GET_LOCAL: {
            uint32_t slot = read_u32(&frame->ip);
            VM_REQUIRE(slot < frame->fn->local_count, "local slot index out of bounds");
            SnVal v = frame->slots[slot];
            if (v.tag == VAL_MOVED) {
                fprintf(stderr, "runtime error: read of moved local\n");
                sn_vm_clear_execution_stack(vm);
                return 1;
            }
            push(vm, sn_val_clone(v));
            break;
        }
        case OP_SET_LOCAL: {
            uint32_t slot = read_u32(&frame->ip);
            VM_REQUIRE(slot < frame->fn->local_count, "local slot index out of bounds");
            SnVal v = peek(vm, 0);
            (void)sn_val_assign(&frame->slots[slot], v);
            break;
        }
        case OP_MOVE_LOCAL: {
            uint32_t slot = read_u32(&frame->ip);
            VM_REQUIRE(slot < frame->fn->local_count, "local slot index out of bounds");
            if (frame->slots[slot].tag == VAL_MOVED) {
                fprintf(stderr, "runtime error: local moved more than once\n");
                sn_vm_clear_execution_stack(vm);
                return 1;
            }
            push(vm, sn_val_move(&frame->slots[slot]));
            break;
        }
        case OP_CLEAR_LOCAL: {
            uint32_t slot = read_u32(&frame->ip);
            VM_REQUIRE(slot < frame->fn->local_count, "local slot index out of bounds");
            SnVal old = frame->slots[slot];
            frame->slots[slot] = sn_val_unit();
            sn_val_release(old);
            break;
        }
        case OP_GET_GLOBAL: {
            uint32_t slot = read_u32(&frame->ip);
            VM_REQUIRE(slot < vm->global_count, "global slot index out of bounds");
            SnVal v = vm->globals[slot];
            push(vm, sn_val_clone(v));
            break;
        }
        case OP_SET_GLOBAL: {
            uint32_t slot = read_u32(&frame->ip);
            VM_REQUIRE(slot < SN_VM_GLOBALS_MAX &&
                       sn_vm_reserve_globals(vm, (size_t)slot + 1u),
                       "global slot exceeds VM limit or table allocation failed");
            SnVal v = peek(vm, 0);
            (void)sn_val_assign(&vm->globals[slot], v);
            break;
        }
        case OP_JUMP: {
            int32_t offset = read_i32(&frame->ip);
            frame->ip += offset;
            break;
        }
        case OP_JUMP_IF_FALSE: {
            int32_t offset = read_i32(&frame->ip);
            SnVal cond = pop(vm);
            if (!cond.as.b) {
                frame->ip += offset;
            }
            sn_val_release(cond);
            break;
        }
        case OP_JUMP_IF_TRUE: {
            int32_t offset = read_i32(&frame->ip);
            SnVal cond = pop(vm);
            if (cond.as.b) {
                frame->ip += offset;
            }
            sn_val_release(cond);
            break;
        }
        case OP_CALL: {
            uint32_t fn_idx = read_u32(&frame->ip);
            uint32_t argc = read_u32(&frame->ip);
            VM_REQUIRE(fn_idx < vm->unit->function_count,
                       "function index out of bounds");
            SnFunctionChunk *callee = vm->unit->functions[fn_idx];
            VM_REQUIRE(callee && argc == callee->arity &&
                       callee->local_count >= argc && callee->chunk.code,
                       "invalid function call signature");
            VM_REQUIRE((size_t)argc <= (size_t)(vm->stack_top - vm->stack),
                       "not enough stack arguments for call");
            if (vm->frame_count >= SN_VM_FRAMES_MAX) {
                fprintf(stderr, "Stack overflow\n");
                sn_vm_clear_execution_stack(vm);
                return 1;
            }
            SnCallFrame *nframe = &vm->frames[vm->frame_count++];
            nframe->fn = callee;
            nframe->ip = callee->chunk.code;
            nframe->slots = vm->stack_top - argc;
            /* Expand local slots if function has more locals than args */
            for (size_t i = argc; i < callee->local_count; i++) {
                push(vm, sn_val_unit());
            }
            frame = nframe;
            break;
        }
        case OP_RETURN: {
            SnVal res = pop(vm);
            vm->frame_count--;
            if (vm->frame_count == 0) {
                int exit_code = res.tag == VAL_INT ? (int)res.as.i : 0;
                sn_val_release(res);
                sn_vm_clear_execution_stack(vm);
                return exit_code;
            }
            /* Clean up frame stack slots */
            while (vm->stack_top > frame->slots) {
                sn_val_release(pop(vm));
            }
            push(vm, res);
            frame = &vm->frames[vm->frame_count - 1];
            break;
        }
        case OP_PRINT: {
            uint8_t is_newline = *frame->ip++;
            SnVal v = pop(vm);
            sn_val_print(v, is_newline != 0);
            sn_val_release(v);
            break;
        }
        case OP_HALT:
            sn_vm_clear_execution_stack(vm);
            return 0;
        default:
            fprintf(stderr, "Unknown opcode: %u\n", (unsigned)op);
            sn_vm_clear_execution_stack(vm);
            return 1;
        }
    }
#undef VM_REQUIRE
}

int sn_vm_run_unit(SnBCUnit *unit) {
    SnVM vm;
    if (!sn_vm_init(&vm, unit)) {
        sn_vm_free(&vm);
        return 1;
    }
    int result = sn_vm_interpret(&vm);
    sn_vm_free(&vm);
    return result;
}
