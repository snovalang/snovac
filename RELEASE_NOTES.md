# Snovalang 0.1.1

This patch release adds the first broad, source-based builtin library to the Snovalang compiler and updates compiler, interpreter, and runtime support for its types and APIs.

## Builtin library

- Added builtin Snovalang declarations for collections (`Array`, `Map`, `Set`, `Option`, and `Result`), object and error hierarchies, functional collection operations, text and string parsing, and checked math and time operations.
- Added APIs for console input and output, files and text streams, process arguments and environment values, runtime access, networking and HTTP data, async tasks, channels, synchronization, contexts, memory views, and utilities.
- Moved language-level builtin signatures and implementations into `.snl` source files under `builtin/`. The compiler resolves and checks these declarations through the normal package and type systems instead of maintaining a separate catalog of most builtin methods.
- Added `Object` defaults for string conversion, identity equality, and hashing. Added checked-error handling for declared `Error` types and runtime type matching for unchecked `Exception` types.
- Documented builtin package names, API coverage, and runtime limitations in [`builtin/readme.md`](https://github.com/snovalang/snovac/blob/master/builtin/readme.md).

## Compiler and runtime

- Extended parsing, name resolution, type checking, and generic type handling to support the builtin declarations and their APIs.
- Added interpreter host calls for runtime services including text file access, process arguments and environment values, TCP and UDP sockets, monotonic time, and operating-system mutexes.
- Added a C runtime API for embedders, including status-returning operations and explicit release of buffers returned by read calls.
- Updated bytecode and native execution paths to support the compiler changes and runtime API.
- Improved command-line behavior and bootstrap compatibility alongside the builtin integration.

## Runtime support notes

Builtin declarations describe the available language-level API; some backends do not implement every host operation. In particular, generated native programs do not currently provide the interpreter's socket, clock, or mutex calls. Async task helpers currently run inline, and semaphore and wait-group values do not provide cross-thread synchronization. File streams currently buffer text and do not provide incremental byte I/O. See [`builtin/readme.md`](https://github.com/snovalang/snovac/blob/master/builtin/readme.md) for the full support boundary and API details.

## Validation

The repository includes compiler, interpreter, runtime, and package fixtures for these changes. The release workflow runs `make test` on Linux, macOS, and Windows before publishing the platform archives and SHA-256 checksums.
