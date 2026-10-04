---
name: snovac-bootstrap
description: Judge snovac bootstrap and release binaries. Use when discussing self-hosting, reproducible builds, diverse double compilation, trusting trust, or whether a clean tree proves build/snl. Covers the guinea-pig compiler, the rewrite, the fixed-point rebuild, and the later update chain.
---

# snovac bootstrap

snovac is written in ISO C11 and built by whatever host C compiler `make` invokes (`build/snl`). That binary is not a Snova program. This tree has no Snova rewrite of the compiler and no fixed-point rebuild of `snl` by `snl`. Do not invent those paths.

The bootstrap account to apply is Huberdeau, Hamel, Monnier, and Feeley, "The Design of a Self-Compiling C Transpiler Targeting POSIX Shell" (SLE 2024), including the works it cites. Thompson's lecture is only the attack those works are answering.

## Stages

Use these stages when judging this compiler or a future self-host. Do not collapse them into one "rebuild it" step.

1. Guinea-pig compiler. A compiler for the language, written in a language that already has a compiler, used to compile the real compiler once. This C11 tree is that guinea pig. In the paper, an existing C compiler builds `pnut.c` into `pnut.exe` (their Figure 1, first step). The guinea pig is not a trusted root.
2. Rewrite. Reimplement the compiler in the language it compiles, then compile that source with the guinea pig. The paper's self-application is `pnut.exe` compiling `pnut.c` into the auditable `pnut.sh`. snovac has not done this. Absence of a rewrite is a fact, not a task to start from a behavior fix.
3. Fixed-point rebuild. Compile the rewrite with the compiler just produced, and repeat until the output matches the compiler that produced it. The paper does this by compiling `pnut-exe.c` with `pnut.sh` to `pnut-exe.sh`, then compiling `pnut-exe.c` again with that script to `pnut-exe.exe` (their Figure 2). A mismatch is disagreement or nondeterminism. A passing `make test` is not a fixed point. snovac does not perform this step.
4. Later update chain. After a fixed point, further versions are compiled by the previous compiler, not by the guinea pig. The paper's chain continues from `pnut-exe.exe` to TCC and then GCC. A backdoor in any compiler on that chain can survive into later binaries after the source is clean. Appel's axiomatic bootstrapping is the cited reason that "compile it with itself and delete the old binary" is not a proof.

## Required check

A clean diff does not show that `build/snl`, an installed `snl`, or a release archive is free of a trusting-trust style backdoor. The host C compiler is inside the trusted computing base.

State that a bootstrap binary still needs both of these, and say if either one was not actually run:

- Reproducible rebuild: same sources and a fixed host compiler, with a bit-for-bit comparison, or an explicit account of why the bytes differ.
- Diverse double compilation, in Wheeler's sense as cited by the paper: two independent implementations must compile the same sources. The paper's diverse root is more than one POSIX shell (`ksh`, `dash`, `bash`, `yash`, `zsh`) executing the auditable shell seed, because a shell is already on the path that fetches sources. For today's snovac the corresponding check is two independent host C compilers, with the comparison reported. If they cannot match, the check is incomplete.

Guix's full-source bootstrap (a small audited seed, then a long chain up to GCC) is a different root. The paper treats it as complementary. It is not the procedure this repository runs.

Do not write steps that insert, hide, or trigger a compiler backdoor. Describe the missing check and stop.
