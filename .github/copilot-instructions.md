# snovac — instructions for GitHub Copilot

`snovac` is the Snovalang compiler: ISO C11, built with `make`, no Cargo.
The binary version macro is `SNOVAC_VERSION` in `src/driver/driver_utils.h`. The installed command is `snl`.
Tests live in `tests/` (`compile-pass`, `compile-fail`, `run-pass`, `tests/run.sh`, and the C drivers `tests/test_*.c`).

## New features are unfinished until they are verified

Whenever you add or change a compiler feature (lexer, parser, checker, resolver, bytecode, VM, CLI flag, or diagnostic), do all of the following before you stop:

1. Add a fixture that fails on the old behavior and passes on the new one.
   - Syntax or typecheck that must compile: `tests/compile-pass/`.
   - Syntax or typecheck that must be rejected: `tests/compile-fail/` plus the expected `.stderr` shape used by the neighboring fixtures.
   - Runtime behavior: `tests/run-pass/<name>.snova` and `tests/run-pass/<name>.stdout`.
   - A C-level invariant with no `.snova` surface: extend `tests/test_symbol.c`, `tests/test_package.c`, `tests/test_types.c`, `tests/test_resolve.c`, or `tests/test_check.c`.
2. Run `make test` from the repository root and read the output.
3. If `make test` fails, fix the feature or the fixture and run `make test` again.
4. Do not describe the feature as done, and do not open a pull request, while `make test` is failing or while the new behavior has no fixture.

A docs-only or workflow-only edit does not need a fixture. Any edit under `*.c`, `*.h`, or `tests/` does.

## Releases

Do not invent a version, edit `SNOVAC_VERSION` in `src/driver/driver_utils.h`, or create a git tag yourself during feature work.
A release happens only when a person runs the manual workflow **Release with Copilot semver** (`.github/workflows/copilot-release.yml`). That workflow asks Copilot for the next SemVer, writes it into `SNOVAC_VERSION`, and tags `vX.Y.Z`. The same Copilot CLI writes the English release notes for that tag (`docs/releases/vX.Y.Z/`), with sections for features, bug fixes, and breaking changes. Pull request review is GitHub Copilot code review, requested by the repository ruleset **Copilot code review** (`copilot_code_review`, including new pushes). Do not call Gemini, Cursor, or another agent for review or release notes. The GitHub Release that `install.sh` downloads (`snovac-<os>-<arch>.tar.gz`) publishes the review and those notes.
