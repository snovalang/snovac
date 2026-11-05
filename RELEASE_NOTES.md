# Snovalang 0.1.0

The first stable release of the Snovalang reference compiler, `snl`.

## Install

macOS and Linux:

```sh
curl -fsSL https://raw.githubusercontent.com/snovalang/snovac/master/install.sh | sh
```

Windows PowerShell:

```powershell
irm https://raw.githubusercontent.com/snovalang/snovac/master/install.ps1 | iex
```

The installer downloads the matching release binary and puts `snl` on `PATH`. If a prebuilt binary is unavailable, it builds from source. Release archives include SHA-256 checksums.

## Highlights

- C11 reference compiler with lexer, parser, semantic checking, and stable diagnostics.
- Interpreter, bytecode emission, native compilation, and dependency management.
- Pulsar concurrency and streaming support.
- Runtime memory safety and concurrency fixes.
