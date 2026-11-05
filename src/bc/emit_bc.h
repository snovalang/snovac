/* emit_bc.h — AST to SnBC bytecode compiler. */
#ifndef SNOVAC_EMIT_BC_H
#define SNOVAC_EMIT_BC_H

#include "ast.h"
#include "diag.h"
#include "snbc.h"

/* Bytecode band: 0400-0499. One code: a node the emitter does not lower. */
#define SNOVA_EMIT_UNSUPPORTED 400

/* Returns 1 only when every declaration, statement, and expression was
 * lowered. Returns 0 after SNOVA_EMIT_UNSUPPORTED. The unit is not a
 * compiled program in that case, and the caller must not write it. */
int sn_emit_bytecode(SnArena *arena, SnDiagSink *diag, const SnUnit *unit, SnBCUnit *out);

#endif /* SNOVAC_EMIT_BC_H */
