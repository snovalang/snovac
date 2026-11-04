#ifndef SNOVAC_VM_INTERNAL_H
#define SNOVAC_VM_INTERNAL_H
#include "vm.h"
int sn_vm_init(SnVM *, SnBCUnit *);
void sn_vm_free(SnVM *);
void sn_vm_push(SnVM *, SnVal);
SnVal sn_vm_pop(SnVM *);
SnVal sn_vm_peek(SnVM *, int);
uint32_t sn_vm_read_u32(uint8_t **);
int32_t sn_vm_read_i32(uint8_t **);
int64_t sn_vm_read_i64(uint8_t **);
double sn_vm_read_double(uint8_t **);
int sn_vm_execute_numeric(SnVM *, SnOpcode);
int sn_vm_execute_composite(SnVM *, SnOpcode, uint8_t **);
void sn_vm_clear_execution_stack(SnVM *);
int sn_vm_reserve_globals(SnVM *, size_t);
#endif
