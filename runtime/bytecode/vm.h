/* vm.h — Snovalang stack-based bytecode virtual machine. */
#ifndef SNOVAC_VM_H
#define SNOVAC_VM_H

#include "snbc.h"
#include "value.h"

#define SN_VM_STACK_MAX SN_BC_VM_STACK_CAPACITY
#define SN_VM_FRAMES_MAX 256
#define SN_VM_GLOBALS_MAX 65536u

typedef struct {
    SnFunctionChunk *fn;
    uint8_t *ip;
    SnVal *slots;
} SnCallFrame;

typedef struct {
    SnBCUnit *unit;
    SnCallFrame frames[SN_VM_FRAMES_MAX];
    size_t frame_count;

    SnVal stack[SN_VM_STACK_MAX];
    SnVal *stack_top;

    SnVal *globals;
    size_t global_count;
    size_t global_capacity;
} SnVM;

/* Initializes without reserving globals. Returns 1 on success, 0 for invalid
 * input; global storage grows on demand. A failed initialization leaves the
 * VM safe to pass to sn_vm_free. */
int sn_vm_init(SnVM *vm, SnBCUnit *unit);
/* Interpretation releases frame/operand values on both return and runtime
 * error. The VM retains globals until sn_vm_free, which must still be called
 * to end the VM lifetime; it is safe after either result. */
void sn_vm_free(SnVM *vm);
int sn_vm_interpret(SnVM *vm);

#endif /* SNOVAC_VM_H */
