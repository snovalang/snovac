#!/bin/sh
# run.sh — assertions for the lexer decisions that are easy to get wrong.
#
# Each case encodes a fact measured from the real corpus. If one of these
# regresses, the lexer has silently changed the language.

set -eu

SNOVAC="${1:-build/snl}"
DIR="$(cd "$(dirname "$0")" && pwd)"
pass=0
fail=0

assert() { # name, expected-count, actual-count
  if [ "$2" = "$3" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    printf 'FAIL %s: expected %s, got %s\n' "$1" "$2" "$3"
  fi
}

toks() { "$SNOVAC" --emit=tokens "$DIR/lex/$1"; }

# Echoes a command's exit status without tripping `set -e`, which is inherited
# by command substitution and would otherwise abort the subshell at the
# failing command, before the status could be printed.
rc_of() {
  if "$@" >/dev/null 2>&1; then echo 0; else echo $?; fi
}

# Byte compare. A minimal MSYS install has no diffutils, so `cmp` exits 127
# and every image check looks like a mismatch. Use cmp when it exists.
same_bytes() {
  if command -v cmp >/dev/null 2>&1; then
    cmp -s "$1" "$2" && return 0
    return $?
  fi
  if [ ! -f "$1" ] || [ ! -f "$2" ]; then
    return 2
  fi
  python3 -c 'import pathlib, sys
a, b = sys.argv[1:]
try:
    left = pathlib.Path(a).read_bytes()
    right = pathlib.Path(b).read_bytes()
except OSError:
    sys.exit(2)
sys.exit(0 if left == right else 1)' "$1" "$2" && return 0
  return $?
}

# Text compare. MinGW opens stdout in text mode, so a captured print is CR LF.
# The expected strings in these scripts are LF. Image checks stay on same_bytes.
same_text() {
  if [ ! -f "$1" ] || [ ! -f "$2" ]; then
    return 2
  fi
  left=$(mktemp)
  right=$(mktemp)
  tr -d '\r' < "$1" > "$left"
  tr -d '\r' < "$2" > "$right"
  rc=0
  same_bytes "$left" "$right" || rc=$?
  rm -f "$left" "$right"
  return "$rc"
}


. "$DIR/run_front.sh"
. "$DIR/run_deps.sh"

# Check and execute the builtin tree as a manifest-backed consumer project.
# This covers package-path agreement and source-level generic/core APIs.
REPO_ROOT="$(cd "$DIR/.." && pwd)"
case "$SNOVAC" in
  /*) SNOVAC_ABS="$SNOVAC" ;;
  *) SNOVAC_ABS="$(pwd)/$SNOVAC" ;;
esac
BUILTIN_PROJECT="$(mktemp -d)"
mkdir -p "$BUILTIN_PROJECT/src/app"
cp -R "$REPO_ROOT/builtin" "$BUILTIN_PROJECT/src/builtin"
cat > "$BUILTIN_PROJECT/mod.sns" <<'EOF'
module builtin_validation
snova "1.0.0"
EOF
cat > "$BUILTIN_PROJECT/src/app/Main.snl" <<'EOF'
package app
import builtin.concurrent.async.tasks
import builtin.process.arguments.values
import builtin.collections.collections.types
import builtin.process.environment.values
import builtin.io.filesystem.operations
import builtin.math.arithmetic.operations
import builtin.network.transport.sockets
import builtin.text.text.values
import builtin.time.clock.values
import builtin.core.types.values
import builtin.util.general.types
import builtin.concurrent.synchronization.primitives

func main(): int {
    let some: Option<int> = Some(7)
    if some.isSome() == false || some.unwrap() != 7 || some.unwrapOr(0) != 7 { return 1 }
    let none: Option<int> = None
    if none.isNone() == false || none.unwrapOr(8) != 8 { return 2 }
    let ok: Result<int, string> = Ok(11)
    if ok.isOk() == false || ok.unwrap() != 11 { return 3 }
    let err: Result<int, string> = Err("bad")
    if err.isErr() == false { return 4 }

    let task = Task { value: 9 }
    if task.get() != 9 || task.map((value: int) -> value * 2).get() != 18 { return 5 }
    let pair = Pair { first: 3, second: "three" }
    if pair.swap().first != "three" { return 6 }

    let values = [1, 2, 3]
    if values.map((n: int) -> n + 1).get(1) != 3 { return 7 }
    if values.filter((n: int) -> n > 1).length() != 2 { return 8 }
    if values.reduce(0, (sum: int, n: int) -> sum + n) != 6 { return 9 }
    let map = Map<string, int>()
    if map.put("one", 1) == false || map.getOrDefault("one", 0) != 1 { return 10 }
    let set = Set<int>()
    if set.add(1) == false || set.contains(1) == false { return 11 }

    if Text.join(["snova", "lang"], "-") != "snova-lang" || Text.repeat("x", 3) != "xxx" { return 12 }
    if Text.isBlank("  ") == false || Math.lcm(12, 18) != 36 || Math.pow(2, 5) != 32 { return 13 }
    if Net.resolve("localhost").isEmpty() { return 14 }
    Time.now()
    Time.sleep(Duration.seconds(0L))

    var semaphore = Concurrent.semaphore(1)
    if semaphore.tryAcquire() == false { return 15 }
    semaphore.release()
    let mutex = Mutex { value: 42 }
    if mutex.tryLock() == false || mutex.tryLock() { return 21 }
    mutex.unlock()
    mutex.lock()
    mutex.unlock()
    mutex.close()
    var group = Concurrent.waitGroup()
    group.add(1)
    group.done()
    if group.isComplete() == false { return 16 }
    var token = CancellationToken { cancelled: false }
    token.cancel()
    if Async.isCancelled(token) == false { return 17 }
    if Args.count() < 0 || Env.get("PATH").isEmpty() { return 18 }
    if FileSystem.writeText("probe.txt", "ok") == false { return 19 }
    if FileSystem.readText("probe.txt") != "ok" { return 20 }
    return 0
}
EOF
assert "builtin: manifest-backed project checks" 0 \
  "$(rc_of "$SNOVAC" check --project "$BUILTIN_PROJECT")"
if (cd "$BUILTIN_PROJECT" && "$SNOVAC_ABS" run --project .); then
  builtin_run_rc=0
else
  builtin_run_rc=$?
fi
assert "builtin: interpreter runs source implementations" 0 "$builtin_run_rc"
rm -rf "$BUILTIN_PROJECT"

# Exercise HTTP handler registration and routing against the source builtin.
HTTP_OUTPUT="$(mktemp)"
if SNOVA_BUILTIN_DIR="$REPO_ROOT/builtin" "$SNOVAC_ABS" run --project \
    "$REPO_ROOT/tests/http_server_dispatch" > "$HTTP_OUTPUT"; then
  http_run_rc=0
else
  http_run_rc=$?
fi
assert "HTTP handlers: dispatch project runs" 0 "$http_run_rc"
if same_text "$HTTP_OUTPUT" "$REPO_ROOT/tests/http_server_dispatch/expected.stdout"; then
  http_output_rc=0
else
  http_output_rc=1
fi
assert "HTTP handlers: GET, 404, and 405" 0 "$http_output_rc"
rm -f "$HTTP_OUTPUT"
. "$DIR/run_fixed.sh"
