/* native_scope.h — extra runner opcodes for the self-hosted slice. */
#ifndef SNOVAC_NATIVE_SCOPE_H
#define SNOVAC_NATIVE_SCOPE_H

#include <stdio.h>

void sn_native_write_scope_support(FILE *f, int windows);
void sn_native_write_scope_ops(FILE *f, int windows);

#endif /* SNOVAC_NATIVE_SCOPE_H */
