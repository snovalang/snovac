# Builtin library

All language-level builtin declarations are kept in this directory as `.snl`
source. Package names follow `builtin.<general>.<abstraction>.<concrete>.<Stem>`:
the concrete segment names a specific area such as `data/types`, not the
literal word `concrete`, and the `.snl` filename uses the declaration's stem.
For example, HTTP response data lives at
`builtin/network/http/data/types/Response.snl` in package
`builtin.network.http.data.types`.

| Area | Package | Files |
| --- | --- | --- |
| Collections | `builtin.collections.collections.types` | `collections/collections/types/{Array,Map,Partial,Set}.snl` |
| Core types and object model | `builtin.core.types.values` | `core/types/values/{Object,Option,PrimitiveExtensions,Result}.snl` |
| Error hierarchy | `builtin.errors.hierarchy.types` | `errors/hierarchy/types/Errors.snl` |
| Console and filesystem operations | `builtin.io.console.output`, `builtin.io.filesystem.operations` | `io/console/output/Console.snl`, `io/filesystem/operations/FileSystem.snl` |
| File streams | `builtin.io.filesystem.streams.contracts`, `builtin.io.filesystem.files.streams.types`, `builtin.io.filesystem.streams.extensions` | `io/filesystem/streams/contracts/*.snl`, `io/filesystem/files/streams/types/{FileInputStream,FileOutputStream}.snl`, `io/filesystem/streams/extensions/FileStreamExtensions.snl` |
| Process and host runtime | `builtin.process.arguments.values`, `builtin.process.environment.values`, `builtin.runtime.host.api` | `process/arguments/values/Args.snl`, `process/environment/values/Env.snl`, `runtime/host/api/Runtime.snl` |
| Text and math | `builtin.text.text.values`, `builtin.math.arithmetic.operations` | `text/text/values/Text.snl`, `math/arithmetic/operations/Math.snl` |
| String parsing | `builtin.text.parsing.results` | `text/parsing/results/Strings.snl` |
| Functional | `builtin.functional.collections.operations` | `functional/collections/operations/Functional.snl` |
| Time | `builtin.time.clock.values` | `time/clock/values/{Duration,Instant,Time}.snl` |
| Networking | `builtin.network.transport.sockets` | `network/transport/sockets/{Net,Socket,SocketAddress,TcpListener,TcpStream,TcpStreamExtensions,UdpSocket}.snl` |
| HTTP data | `builtin.network.http.data.types` | `network/http/data/types/{Method,Header,Request,Response}.snl` |
| Async and synchronization | `builtin.concurrent.async.tasks`, `builtin.concurrent.synchronization.primitives` | `concurrent/async/tasks/*.snl`, `concurrent/synchronization/primitives/*.snl` |
| Channels | `builtin.concurrent.channels.queues` | `concurrent/channels/queues/Channel.snl` |
| Context | `builtin.context.scoped.values` | `context/scoped/values/Context.snl` |
| Memory types | `builtin.memory.addressing.types` | `memory/addressing/types/{Address,ByteSpan,NativeWord,SignedByteSpan}.snl` |
| Utilities | `builtin.util.general.types` | `util/general/types/{Pair,Util}.snl` |

Contracts, shared abstractions, data types, and implementation helpers use
specific package names under their domain. Contracts live under
`streams.contracts`, file stream types under `files.streams.types`, and stream
extensions under `streams.extensions`. This makes the package path describe
what a type does and what it depends on.

`@Native` marks a host boundary and requires a runtime implementation. The
interpreter and generated native runner provide console output, basic array
storage, process arguments/environment, and text file access. The interpreter
also provides TCP listen/connect/accept/receive/send/close, monotonic time,
UDP, and OS mutex operations. `Net`, `TcpListener`, and `TcpStream` compose
those socket calls in Snovalang. Generated native programs do not currently
expose socket, clock, or mutex calls.

Method signatures for strings and scalar primitives live in
`core/types/values/PrimitiveExtensions.snl`; generic array signatures and
source implementations live in `collections/collections/types/Array.snl`. The semantic
checker reads those declarations instead of maintaining a parallel method
catalog. `Object` supplies default `toString()`, identity `equals(Object)`,
and `hashCode()` methods to declared object types. `Error` and its subclasses
are checked: a function that calls a routine with `throws ErrorType`, or throws
an `Error` directly, must catch it or declare it with `throws ErrorType`.
`Exception` and its subclasses are unchecked runtime failures and are matched
by their runtime type in `catch` clauses. `Result<T, E>.unwrap()` also requires
handling when its concrete `E` extends `Error`. Host filesystem failures become
`IoException` instances in the interpreter, allowing the filesystem `try*`
wrappers to return the actual exception as their error value.

`Console.readLine()` and the `FileSystem.try*` methods are Snovalang wrappers
over the runtime boundary. File operations return their host exception through
`Result` in the `try*` variants; `writeText` also reports a false runtime
result as an error.

`builtin.io.filesystem.streams.contracts` defines `InputStream` and
`OutputStream`; `builtin.io.filesystem.files.streams.types` provides text file
stream implementations, and `builtin.io.filesystem.streams.extensions` adds
`readAll()` on `InputStream`.
The current native boundary exposes whole-file text reads and writes, so
`FileInputStream` buffers the file contents at construction and
`FileOutputStream` buffers writes until `close()`, which replaces the target
file. These APIs are text-oriented and do not provide byte streams or
incremental host I/O yet.

`builtin.text.parsing.results` parses signed integers with radix and overflow
validation, bounded integer widths, floating-point values, decimals, and
booleans into `Result` values. Its `tryParse*` helpers convert parse failures
into `Option.None`.

`Option` and `Result` provide extraction, mapping, chaining, recovery, and
inspection operations in Snovalang. `Array` provides generic mapping, flattening,
folding, slicing, insertion, removal, and predicate operations. `Functional`
provides eager array mapping, flattening, filtering, folding, traversal, and
predicate queries. `Map` and
`Set` provide equality-based lookup and set algebra; lookup is linear and
preserves insertion order. They also expose value-returning removal and set
equality operations. `Math` provides checked integer and long arithmetic,
including multiplication, division, powers, gcd, and lcm, plus an iterative
square root, integer powers, and range helpers for floating-point values. `Duration` and
`Instant` provide checked arithmetic
that returns `Result`, alongside convenience methods that throw on invalid
operations.

HTTP request/response values validate status and header fields, preserve
repeated headers, and provide case-insensitive header lookup. The network
wrappers validate ports and socket handles, expose `Result`-returning open,
accept, connect, and send operations, and make `close()` idempotent.

`CancellationToken` is a shared class value, so cancellation is visible to all
holders of the same token. `Task<T>` stores a typed success, failure, or
cancellation outcome and supports mapping, chaining, result inspection, and
separate failure and cancellation recovery. The current
interpreter runs `Async.spawn`, `Async.scope`, and `TaskScope.spawn` inline;
`Async.sleep` blocks the current execution. These APIs do not provide
concurrent task scheduling. Semaphore and wait-group enforce counter
invariants but are single-threaded coordination values;
they do not block or provide atomic cross-thread synchronization. `Mutex<T>`
uses a host OS mutex; call `close()` after the final unlock. UDP bind, send,
receive, and close use the host socket boundary. DNS resolution returns numeric
address strings. Parsing arbitrary string-form socket addresses is not
provided; construct a `SocketAddress` from separate host and port values.

`Channel<T>` is a bounded, mutex-protected FIFO with atomic `trySend` and
`tryReceive` operations. It reports full, empty, and closed states explicitly,
drains buffered values after close, and requires `dispose()` after the channel
is closed, drained, and no longer in use. Capacity zero creates a channel that
cannot accept values through the non-blocking API. Blocking operations, select,
and scheduler integration are not implemented; generated native programs also
do not currently provide the mutex host calls that this implementation uses.

`Context` supports parent cancellation, inherited deadlines, and scoped values.
Context values support the builtin scalar types (`string`, `char`, `bool`,
`byte`, the signed integer widths, and floating/decimal numbers), with typed
setters and getters. `withTypedValue<T>` also stores other Snovalang values;
retrieve them with `valueAs<T>(key, typeWitness)`, passing a value of the
expected type as the witness because the current runtime has no general nominal
type-test operation. A value stored under the same key in a child shadows its
parent's value. Deadlines are checked when `isDone()` or `err()` is called;
they do not interrupt blocked work.

The builtins intentionally use existing language types and syntax. `Map` and
`Set` use equality-based linear lookup and preserve insertion order; the
language does not currently define a hashing protocol. `ByteSpan` and
`SignedByteSpan` are zero-copy views over shared Snovalang arrays, with checked
indexed access, optional reads, range copies, and content comparison. They are
not raw host pointers. `Address` is an opaque native-word value and cannot be dereferenced;
the language does not currently define pointer arithmetic or a C-compatible
struct layout.
