# Snovalang Compiler (`snovac`) — compiler invariants

CI review and release notes use GitHub Copilot (`.github/copilot-instructions.md`). Do not start Gemini, Cursor, or another agent for those jobs.

You are reviewing code changes for **snovac**, the reference compiler for **Snovalang** written in pure, zero-dependency **ISO C11**.

---

## 🎯 Core Project Invariants

When reviewing Pull Requests, enforce these hard invariants:

### 1. Zero External Dependencies & ISO C11
- Must compile under standard ISO C11 (`-std=c11 -Wall -Wextra -pedantic`).
- No external libraries. Only the C standard library and OS headers where explicitly guarded for platform support.
- No garbage collector. Memory is managed via arenas (`sn_arena_`) and explicit lifetime scopes (`rt_mem.c`, `rt_defer.c`).

### 2. Architecture & Pipeline Phases
`src/` is the namespaced tree. Never place source files in the repository root or invent unnecessary directories:
1. `src/base`: Arena memory management, diagnostics (`sn_diag_`), string interner (`sn_intern_`).
2. `src/driver`: CLI entrypoint (`snl`). Source files are `.snl`, scripts are `.sns`.
3. `src/lex`: Lexer (`sn_lex`). Keyword spellings reside in `src/lex/lex_token.c`. Keywords are contextual.
4. `src/parse`: Parser (`sn_parse`), recovers and returns partial `SnUnit`.
5. `src/ast`: Arena-allocated AST structures and dumps.
6. `src/sema`: Symbol resolution, hash-consed types (`SnTypeRep`), type checker, borrow checker (`sn_borrow_`).
7. `src/eval`: Tree-walk interpreter for `snl run`, runtime pointers, async loop, pulsar thread pool.
8. `src/bc` & `src/native`: Bytecode emitter (`SnBC`) and native C runner generator.

### 3. Snova Syntax & Type System Invariants
- **Do not alter or weaken language syntax:** The grammar and keyword spellings (`async`, `await`, `pulsar`, `trait`, `&`, `*`, `null`) are frozen. `and` / `or` / `not` are identifiers, not operators.
- **Strict Type Checking:** Hash-consed `SnTypeRep` equality (`==`). No implicit conversions. `int`, `int64`, and `long` are distinct types. `char` is not an integer.
- **Pointer & Borrow Semantics:** `&T` / `&T?` nullable references, borrow errors, and `Send` checks in pulsar tasks must remain intact.
- **English Only:** All identifiers, comments, diagnostics, commit messages, and documentation must remain in English.

---

## 🔍 Code Review Focus Areas

When evaluating changes in a Pull Request:
- **Memory Safety & Leaks:** Ensure all allocations use the proper arenas or have matching deallocations/defer handlers. Watch out for buffer overflows, use-after-free, and dangling pointers.
- **Undefined Behavior:** Check for signed integer overflow, strict aliasing violations, uninitialized memory, or non-standard C behavior.
- **Compiler Invariants:** Verify that changes do not violate the compiler phases or introduce circular header dependencies.
- **Concurrency & Concurrency Hazards:** In `async` and `pulsar` runtime code, scrutinize thread safety, race conditions, memory barriers, and mutex/channel deadlocks.
- **Test Coverage:** Ensure new syntax, semantic checks, or runtime features have corresponding unit/regression tests in `tests/`.
