/* eval.h — tree-walking evaluator over the AST.
 *
 * This is the first execution engine that is actually driven by parsed
 * structure. The Rust Stage 0 it replaces scanned raw source text for
 * substrings (`line.contains(".map(")`) and never built a tree; every
 * behaviour here comes from SnDecl/SnStmt/SnExpr nodes instead.
 *
 * Scope today (P3 preview): functions, classes with fields and methods,
 * let/var, assignment, arithmetic and comparison, if/while, return,
 * string concatenation and interpolation, and the Console intrinsics.
 * Generics, pattern matching, async/pulsars and the remaining ~590 @native
 * intrinsics are later phases — see specs/20260719/snovac-c-toolchain/plan.md.
 *
 * Memory: AST nodes are borrowed from the caller's arena. Runtime allocations
 * use a private execution region. Scalar-only call regions are released on
 * return; regions that create values that may escape remain until execution
 * ends. Dynamic array buffers are freed explicitly before their regions.
 * Reference counting belongs to the bytecode value runtime, not this walker.
 */
#ifndef SNOVAC_EVAL_H
#define SNOVAC_EVAL_H

#include "arena.h"
#include "ast.h"
#include "diag.h"

/* Runs `unit`'s `main`. `unit` nodes and their source strings are borrowed from
 * the caller's arena. Runtime values use private evaluator regions; scalar-only
 * call regions are released immediately, while potentially escaping values
 * keep their region until execution ends. Array item buffers are freed before
 * region cleanup. The caller still owns its source arena. Returns main's
 * process exit code, or a negative value when the program could not be run. */
int sn_eval_run(SnArena *arena, SnDiagSink *diag, const SnUnit *unit);

/* Merges every top-level `extension Type { ... }` / `extension Type.method()`
 * block in `unit` into the member list of the class/struct it targets, so
 * `find_member` (dispatch for `recv.method()`) sees extension methods the
 * same way it sees native ones. Single-file only, by design: `snovac run`
 * has no project graph, unlike resolve.c's package-scoped equivalent
 * (`collect_extension_members`). Call once, before `sn_eval_run`, while
 * `unit` is still mutable. Class/struct only — unlike the resolver, which
 * merges into any `SN_SYM_TYPE` (enum and interface included, via
 * `decl_kind_has_member_scope`); this evaluator's tree-walk object model
 * doesn't represent enum/interface instances yet, so there is nothing to
 * merge into for them. The parity with the resolver is specifically about a
 * missing target: when the extended type isn't found in the same file, it
 * is left unmerged here exactly as resolve.c silently skips it there. */
void sn_eval_merge_extensions(SnArena *arena, SnUnit *unit);

#endif /* SNOVAC_EVAL_H */
