/* emit_bc.h — AST to SnBC bytecode compiler. */
#ifndef SNOVAC_EMIT_BC_H
#define SNOVAC_EMIT_BC_H

#include "ast.h"
#include "diag.h"
#include "snbc.h"

/* Backend diagnostic: one function exceeded the VM's simultaneous slot cap. */
#define SNOVA_TOO_MANY_LOCALS 148
/* The emitter refuses syntax whose runtime semantics are not implemented. */
#define SNOVA_BYTECODE_UNSUPPORTED 149

int sn_emit_bytecode(SnArena *arena, SnDiagSink *diag, const SnUnit *unit, SnBCUnit *out);

#endif /* SNOVAC_EMIT_BC_H */
