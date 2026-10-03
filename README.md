# snovac — Snovalang Compiler

The official, reference compiler for **Snovalang** written in pure, zero-dependency **ISO C11**.

[![Release](https://github.com/supernovalang/snovac/actions/workflows/release.yml/badge.svg)](https://github.com/supernovalang/snovac/actions/workflows/release.yml)

## Quick Install

The installers put the `snl` command on `PATH`. With no extra arguments, `install.sh` and `install.ps1` install `snl` when it is missing and update it when it is already installed. Pass `update` / `--update` (sh) or `-Update` (PowerShell) to update explicitly. That downloads the latest release, or clones this repository and builds it with `make`, so you do not pull and rebuild by hand.

The subcommands (`run`, `build`, `check`, `get`, `tidy`, and the `--emit` / `--check-*` flags) stay the same.

### Windows (PowerShell)

`install.ps1` installs `snl.exe` into `%USERPROFILE%\.snova\bin`. Set `SNOVA_INSTALL_DIR` to choose another prefix; the executable is still copied into the `bin` directory under that prefix.

Install, or update if `snl` is already installed:

```powershell
irm https://raw.githubusercontent.com/snovalang/snovac/master/install.ps1 | iex
```

Update explicitly:

```powershell
$env:SNOVA_UPDATE = '1'; irm https://raw.githubusercontent.com/snovalang/snovac/master/install.ps1 | iex
```

From a clone of this repository:

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1
powershell -ExecutionPolicy Bypass -File install.ps1 -Update
```

If the latest release does not include `snovac-windows-x86_64.zip`, PowerShell clones this repository into a temporary directory and builds `snl` there. `irm | iex` has no script path, so the installer does not treat the command text or the current directory as the source tree.

### macOS and Linux

`install.sh` is a POSIX `sh` script. It installs `snl` into `$HOME/.snova/bin`. Set `SNOVA_INSTALL_DIR` to choose another directory. On Unix that variable is the directory that receives the `snl` binary, not a prefix above `bin`.

Install, or update if `snl` is already installed:

```sh
curl -fsSL https://raw.githubusercontent.com/snovalang/snovac/master/install.sh | sh
```

Update explicitly:

```sh
curl -fsSL https://raw.githubusercontent.com/snovalang/snovac/master/install.sh | sh -s -- --update
```

Or using `wget`:

```sh
wget -qO- https://raw.githubusercontent.com/snovalang/snovac/master/install.sh | sh
wget -qO- https://raw.githubusercontent.com/snovalang/snovac/master/install.sh | sh -s -- --update
```

From a clone of this repository:

```sh
sh install.sh
sh install.sh --update
```

If the latest GitHub release has no prebuilt archive, the script clones this repository and builds it with `make`.

## Features

- **Pure C11 Implementation**: Zero dependencies, extremely fast compilation speed.
- **Diagnostics & Error Reporting**: Colorized source snippets with precise diagnostic codes (`SNOVA0001` - `SNOVA0040`).
- **Module Management**: Discovers root packages with `mod.sno`.
- **Bytecode VM & Native Compilation**: Ahead-of-time bytecode compilation and interpretation.
- **Pulsar Concurrency**: Actor model and streaming concurrency support.

## Building from Source

### Prerequisites
- C11-compliant C compiler (`clang` or `gcc`)
- GNU `make`

### Build Command
```bash
git clone https://github.com/snovalang/snovac.git
cd snovac
make
```

`make` writes the toolchain binary to `build/snl`. The same file is also copied to `build/snovac` so that local path keeps working. The command users run is `snl`.

Install that binary with the Makefile:

```bash
make install
```

On Linux and macOS this copies `build/snl` to `$HOME/.snova/bin/snl`, installs `libsnovart.a` under `$HOME/.snova/lib` and the headers under `$HOME/.snova/include`, and runs `scripts/install_path.sh` so bash, zsh, and fish pick up `$HOME/.snova/bin`. Override the prefix with `make install PREFIX=/your/prefix` (`snl` is then `/your/prefix/bin/snl`).

On Windows, `make install` runs `scripts/install_windows.ps1` and installs `%USERPROFILE%\.snova\bin\snl.exe`.

Remove the installed binary, runtime library, and headers with:

```bash
make uninstall
```

`make uninstall` leaves the `PATH` lines in shell startup files in place.

## Usage & CLI Reference

### Informational & Diagnostics

- **`snl --version`** (`-V`):
  Displays the current version of the compiler.

- **`snl --help`** (`-h`):
  Displays command-line usage instructions and available options.

- **`snl --target-info`**:
  Prints host and target architectures, detected operating system, executable paths, and active environment overrides (`SNOVA_TARGET_OS`, `SNOVA_TARGET_ARCH`, `SNOVA_TARGET`).

### Single-File Inspection & Compilation

- **`snl --emit=tokens <file.snl>`**:
  Runs lexical analysis and dumps the token stream with source spans (line:col).

- **`snl --check-lex <file.snl>`**:
  Validates lexical tokens without generating an AST; exits with non-zero code on syntax/lexer errors.

- **`snl --emit=ast <file.snl>`**:
  Parses the source file and dumps the formatted AST (Abstract Syntax Tree).

- **`snl --check-parse <file.snl>`**:
  Performs lexical analysis and syntax parsing; reports syntax errors with diagnostics.

- **`snl check <file.snl>`**:
  Performs symbol resolution, scope analysis, and static type-checking on a single file.

- **`snl run <file.snl>`**:
  Compiles and directly executes a single Snovalang source file in the bytecode VM runtime.

- **`snl build <file.snl> [-o output] [--target=triple]`**:
  Compiles a single file to a standalone native binary or bytecode unit. Supports cross-compilation target triples (e.g. `aarch64-apple-darwin`, `x86_64-linux-gnu`).

### Package & Dependency Management

- **`snl get [<repo-url>] [--version=<ver>] [--project=<path>]`**:
  Fetches dependencies into `.snovalang/deps/` and manages the `mod.sno` manifest.
  - **Adding a direct dependency**:
    ```bash
    # Add dependency with automatic or default version
    snl get https://github.com/supernovalang/snova-http

    # Add dependency with a specific version or tag
    snl get https://github.com/supernovalang/snova-http --version 1.0.0
    snl get github.com/supernovalang/snova-http@1.0.0
    ```
  - **Sychronizing existing dependencies**:
    ```bash
    # Resolves and downloads all dependencies declared in mod.sno
    snl get
    snl get --project ./my-project
    ```
  - **Features**:
    - **Transitive Resolution**: Recursively fetches dependencies declared in dependencies' manifests.
    - **Deduplication & Diamond Graphs**: Shared dependencies ($A \to C$, $B \to C$) are cloned once and reused across all modules.
    - **Cycle Detection**: Identifies circular dependency loops ($A \to B \to A$) and reports the diagnostic path.
    - **Direct vs Indirect Classification**: Classifies root dependencies under `direct = [...]` and transitive edges under `indirect = ["from -> to"]` in `mod.sno`.
    - **Safe Execution**: Uses direct process invocation without shell string interpolation.
    - **Idempotency**: Running `get` multiple times preserves modifications, does not re-clone existing folders, and outputs deterministic manifests.

- **`snl tidy [--project] [<path>]`**:
  Scans project imports across all source files, removes unused dependencies, and synchronizes `mod.sno`.
  ```bash
  snl tidy
  snl tidy --project ./my-project
  ```

### Project-Wide Operations

A project is discovered by locating the nearest `mod.sno`, `snova.sno`, or `snova.toml` manifest. Source roots include `src/` (or project root) and vendored `.snovalang/deps/`. Source files are `.snl`. Script files are `.sns` and may omit `package` (they belong to package `main`). `mod.sno` and `snova.sno` are manifests, not sources. `run`, `build`, and `check` reject any other extension, including extensionless paths, without reading the file as source. Project discovery skips those files.

- **`snl --check-parse-project <path>`**:
  Recursively discovers and parses all `.snl` and `.sns` files across the project and dependencies.

- **`snl check --project <path> [--no-typecheck]`**:
  Builds the project package graph, links imports, resolves types and symbols across packages, and type-checks declaration bodies. Adding `--no-typecheck` skips body checks while verifying interface signatures and imports.

- **`snl run --project <path> [--offline-cache[=<dir>]]`**:
  Executes a multi-file project across its packages and resolved dependencies.

- **`snl build --project <path> [-o output] [--target=triple] [--runtime] [--offline-cache[=<dir>]]`**:
  Compiles an entire project and its dependencies into a bundled executable. `--runtime` links the native runtime library into the final binary.

## Types

`int` is a 64-bit signed integer. It is the same width as `int64` and `long`, and it is a distinct type: there is no implicit conversion between them. The other integer types are `int8`, `int16`, `int32`, `int64`, and `int128` (all signed) and `byte` (unsigned 8-bit). `char` is not an integer.

An unsuffixed integer literal may adopt an expected integer type when the value fits. A suffixed literal such as `42L` stays `long`. Narrowing — a wider integer, a `float`/`double`/`decimal`, or a `string` into a smaller integer — is a compile error even when written with `as`. Same-width and widening conversions are explicit `as` casts (`int as long`, `int8 as int16`, `char as string`).

`string` is a concatenation of `char` values. `string + string`, `string + char`, `char + string`, and `char + char` produce `string`. Indexing a string, and `charAt`, yields `char`. A string is not an integer, and `+` does not coerce other types to string.

Two fields of the same name in one class or struct are a compile error. Methods may share a name when their parameter types differ; the checker picks one candidate, and an ambiguous call is a compile error. The same signature twice is still a duplicate declaration.

## Environment Variables

- `SNOVA_TARGET_OS`: Override target OS (`darwin`, `linux`, `windows`, `freebsd`).
- `SNOVA_TARGET_ARCH`: Override target architecture (`arm64`, `aarch64`, `x86_64`, `arm`).
- `SNOVA_TARGET`: Override target triple (e.g. `x86_64-unknown-linux-gnu`).
- `SNOVA_STD_DIR`: Explicit path to standard library sources (`snova-std/src`).
- `SNOVA_BUILTIN_DIR`: Explicit path to built-in definition files (`builtin/`).

## Documentation Standard

Snovalang uses structured doc comments formatted as:
```snova
/* -- Doc:{funcName}
 *
 * -- Description: Function description text.
 *
 * -- Param{paramName}: Parameter details.
 * -- Returns: Return value information.
 */
```

## License
MIT License
