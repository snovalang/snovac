#!/bin/sh
# Join the self-hosted compiler into one compilation unit. emit-snbc reads a
# single file, so the fixed point is this concatenation, not a second link step.
cd "$(dirname "$0")" || exit 1
cat part_unit.snl part_lex.snl part_parse.snl part_parse_decl.snl \
    part_emit.snl part_emit_expr.snl part_emit_stmt.snl compiler.snl \
    part_scope_parse.snl part_scope_emit.snl part_scope_match.snl
