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
. "$DIR/run_fixed.sh"
