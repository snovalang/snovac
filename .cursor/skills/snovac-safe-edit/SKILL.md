---
name: snovac-safe-edit
description: Edit the snovac C compiler safely. Use when changing src/, the Makefile, or tests that lock lexer, type, pointer, async, or pulsar behavior.
paths:
  - "src/**/*.c"
  - "src/**/*.h"
  - "Makefile"
  - "tests/**"
---

# Edit snovac safely

Read `.cursor/rules/snovac-pipeline.mdc` and the header of the phase you are changing before editing. The compiler is already split into `src/base`, `src/lex`, `src/parse`, `src/ast`, `src/sema`, `src/eval`, `src/bc`, `src/native`, and `src/driver`.

## Procedure

1. Name the phase that owns the bug. A parse bug does not get a special case in the checker. A type bug does not get a new token.
2. Keep includes as basenames (`#include "arena.h"`). Match the prefix already used in that namespace.
3. Leave `KEYWORDS[]` in `src/lex/lex_token.c` unchanged, including the `trait` spelling. `tests/run.sh` treats a lexer change as a language change.
4. Preserve type and pointer behavior. Do not add an implicit conversion between `int`, `int64`, and `long`. Do not make `SN_T_REF` nullable by default. Keep `SnExpr.adjust` in agreement between `src/sema/check_ptr.c` and `src/eval/rt_ptr.c` (`1` deref, `2` take reference).
5. Preserve parallelism and asynchrony. Do not drop borrow diagnostics (`SNOVA_USE_AFTER_MOVE`, `SNOVA_NULL_DEREF`, pulsar capture errors), `src/eval/async.c`, `src/eval/pulsar.c`, or the pulsar queue in `src/native/native_backend.c`. Do not restore `vm.c`, `value.c`, `link_append`, `sn_bcunit_read_file`, or `sn_bcunit_merge`.
6. Write new identifiers, comments, and diagnostic text in English. Do not translate syntax keywords.
7. Diagnostic codes are fixture-backed. Reuse a code only when the new diagnostic has the same meaning. `src/sema/resolve.h` records which ranges are already taken.
8. Validate with `make test` (`tests/run.sh` plus `test_symbol`, `test_package`, `test_types`, `test_resolve`, `test_check`). Use `make conformance` when the edit can change what the lexer accepts. Do not treat a green run as a check of the host compiler that built `build/snl`.

## Out of scope for a behavior fix

Do not reformat unrelated phases, rename prefixes, or move a file across namespaces to make an edit compile.
