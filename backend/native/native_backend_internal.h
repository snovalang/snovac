#ifndef SNOVAC_NATIVE_BACKEND_INTERNAL_H
#define SNOVAC_NATIVE_BACKEND_INTERNAL_H

#include <stdio.h>

#include "native_backend.h"

void sn_native_emit_vm_source(FILE *file, const SnBCUnit *bytecode,
                              const SnTargetInfo *target);
void sn_native_emit_vm_value_runtime(FILE *file);

#endif
