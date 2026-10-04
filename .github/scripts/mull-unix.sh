#!/bin/sh
# Mutation-test the symbol unit binary with Mull (LLVM 18).
#
# Each translation unit is compiled on its own. A single clang invocation that
# lists several .c files records more than one compiler job, and Mull then
# reports no mutants.
#
# --allow-surviving keeps equivalent mutants from failing the process by
# themselves. Mull still exits 0 when the score is under
# --mutation-score-threshold in that mode, so this script compares the
# reported score and fails when it drops below the measured baseline.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

CLANG=${CLANG:-clang-18}
PLUGIN=${MULL_PLUGIN:-/usr/lib/mull-ir-frontend-18}
RUNNER=${MULL_RUNNER:-mull-runner-18}
# Measured on origin/master at cfa2b2c with Mull 0.34.1 / LLVM 18.1.3
# against build/mull-symbol/test_symbol (src/base/arena.c, src/base/intern.c,
# src/sema/symbol.c, tests/test_symbol.c): 33 killed / 20 survived = 62%, and 32/21 = 60% when
# the out-of-bounds `i <= nbuckets` mutant in symbol.c happens to survive.
# 60 is the lower score those runs reported. CI fails below it.
THRESHOLD=${MULL_SCORE_THRESHOLD:-60}
WORKERS=${MULL_WORKERS:-2}

OUT=build/mull-symbol
rm -rf "$OUT"
mkdir -p "$OUT"

INCLUDES="-Isrc/base -Isrc/lex -Isrc/parse -Isrc/ast -Isrc/sema -Isrc/eval -Isrc/bc -Isrc/native -Isrc/driver"
for src in src/base/arena.c src/base/intern.c src/sema/symbol.c tests/test_symbol.c; do
  obj="$OUT/$(basename "${src%.c}").o"
  "$CLANG" -std=c11 -O0 -g -grecord-command-line -pthread \
    -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE \
    -fpass-plugin="$PLUGIN" \
    $INCLUDES \
    -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
    -Wmissing-prototypes -Wconversion -Wno-sign-conversion \
    -c -o "$obj" "$src"
done

"$CLANG" -pthread -o "$OUT/test_symbol" "$OUT"/*.o
"$OUT/test_symbol"

log=$(mktemp)
trap 'rm -f "$log"' EXIT INT TERM

"$RUNNER" \
  --workers "$WORKERS" \
  --allow-surviving \
  --mutation-score-threshold "$THRESHOLD" \
  --reporters IDE \
  "$OUT/test_symbol" >"$log" 2>&1
cat "$log"

score=$(sed 's/\x1b\[[0-9;]*m//g' "$log" | sed -n 's/.*Mutation score: \([0-9][0-9]*\)%.*/\1/p' | tail -n 1)
if [ -z "$score" ]; then
  echo "Mull did not report a mutation score" >&2
  exit 1
fi

echo "Mutation score ${score}% (CI fails below ${THRESHOLD}%)"
if [ "$score" -lt "$THRESHOLD" ]; then
  echo "Mutation score ${score}% is below the required ${THRESHOLD}%" >&2
  exit 1
fi
