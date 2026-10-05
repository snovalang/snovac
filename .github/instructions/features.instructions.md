---
description: "Verify every new or changed snovac feature before finishing."
applyTo: "**/*.{c,h,snova,stderr,stdout}"
---

# Feature verification

A change to the compiler or to a `.snova` fixture is a feature change.

- Pair the change with a fixture under `tests/compile-pass/`, `tests/compile-fail/`, or `tests/run-pass/`, or with an assertion in `tests/test_*.c`.
- Run `make test` and keep the output. The task is unfinished when that command exits non-zero.
- Leave `SNOVAC_VERSION` alone. The release tag is chosen when a person runs the manual workflow Release Snovac Compiler.
