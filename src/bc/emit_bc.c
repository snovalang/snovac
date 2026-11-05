/* emit_bc.c — split across emit_bc_partN.inc so each file stays under 400 lines.
 * The parts are one translation unit, included in order. */
#include "emit_bc_part1.inc"
#include "emit_bc_part2.inc"
#include "emit_bc_part3.inc"
