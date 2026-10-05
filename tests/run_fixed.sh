agree_exec() {
  label="$1"
  src="$2"
  expect_rc="$3"
  expect_out="$4"
  work="$5"
  out="$SNBC_DIR/$label.snbc"
  out2="$SNBC_DIR/$label-b.snbc"
  assert "emit-snbc: $label succeeds" 0 \
    "$(rc_of "$SNL_ABS" emit-snbc "$src" -o "$out")"
  assert "emit-snbc: $label second run succeeds" 0 \
    "$(rc_of "$SNL_ABS" emit-snbc "$src" -o "$out2")"
  images_match=0
  same_bytes "$out" "$out2" || images_match=$?
  assert "emit-snbc: $label images match" 0 "$images_match"
  listings_match=0
  same_bytes "$out.snbt" "$out2.snbt" || listings_match=$?
  assert "emit-snbc: $label listings match" 0 "$listings_match"
  run_out="$SNBC_DIR/$label.run.out"
  bc_out="$SNBC_DIR/$label.bc.out"
  if (cd "$work" && "$SNL_ABS" run "$src" >"$run_out"); then
    run_rc=0
  else
    run_rc=$?
  fi
  if (cd "$work" && "$SNL_ABS" run-snbc "$out" >"$bc_out"); then
    bc_rc=0
  else
    bc_rc=$?
  fi
  assert "run: $label exit" "$expect_rc" "$run_rc"
  assert "run-snbc: $label exit matches snl run" "$run_rc" "$bc_rc"
  same=0
  same_bytes "$run_out" "$bc_out" || same=$?
  assert "run-snbc: $label stdout matches snl run" 0 "$same"
  if [ -n "$expect_out" ]; then
    printf '%s' "$expect_out" > "$SNBC_DIR/$label.expect"
    got_match=0
    same_bytes "$SNBC_DIR/$label.expect" "$run_out" || got_match=$?
    assert "run: $label stdout" 0 "$got_match"
  else
    assert "run: $label stdout empty" 0 "$(wc -c < "$run_out" | tr -d '[:space:]')"
  fi
}

agree_exec "logic" "$DIR/bootstrap/logic.snl" 0 "" "$SNBC_DIR"
agree_exec "call" "$DIR/bootstrap/call.snl" 42 "" "$SNBC_DIR"
agree_exec "bits" "$DIR/bootstrap/bits.snl" 4 "" "$SNBC_DIR"
agree_exec "array" "$DIR/bootstrap/array.snl" 40 "" "$SNBC_DIR"
agree_exec "struct_field" "$DIR/bootstrap/struct_field.snl" 7 "" "$SNBC_DIR"
agree_exec "array_buf" "$DIR/bootstrap/array_buf.snl" 12 "" "$SNBC_DIR"
BYTES_DIR="$SNBC_DIR/bytes-work"
mkdir -p "$BYTES_DIR"
agree_exec "bytes" "$DIR/bootstrap/bytes.snl" 0 "65
66
67
" "$BYTES_DIR"

printf 'SN' > "$SNBC_DIR/short.snbc"
assert "run-snbc: short image exits non-zero" 1 \
  "$(rc_of "$SNOVAC" run-snbc "$SNBC_DIR/short.snbc" | grep -c '[^0]')"
printf 'XXXX' > "$SNBC_DIR/badmagic.snbc"
assert "run-snbc: bad magic exits non-zero" 1 \
  "$(rc_of "$SNOVAC" run-snbc "$SNBC_DIR/badmagic.snbc" | grep -c '[^0]')"
assert "run-snbc: missing file exits non-zero" 1 \
  "$(rc_of "$SNOVAC" run-snbc "$SNBC_DIR/missing.snbc" | grep -c '[^0]')"
python3 - "$SNBC_DIR/badop.snbc" <<'PY'
import struct, sys
def u32(n):
    return struct.pack("<I", n)
name = b"main"
code = bytes([0xFE])
blob = b"".join([
    u32(0x43424E53),
    u32(1),
    u32(0),
    u32(0),
    u32(1),
    u32(len(name)),
    name,
    u32(0),
    u32(0),
    u32(len(code)),
    code,
])
open(sys.argv[1], "wb").write(blob)
PY
assert "run-snbc: unknown opcode exits non-zero" 1 \
  "$(rc_of "$SNOVAC" run-snbc "$SNBC_DIR/badop.snbc" | grep -c '[^0]')"

reject_sns() {
  label="$1"
  src="$2"
  out="$SNBC_DIR/$label.snbc"
  rm -f "$out" "$out.snbt"
  rc="$(rc_of "$SNOVAC" emit-snbc "$src" -o "$out")"
  assert "emit-snbc: $label exits non-zero" 1 "$(printf '%s' "$rc" | grep -c '[^0]')"
  present=0
  if [ -f "$out" ] || [ -f "$out.snbt" ]; then
    present=1
  fi
  assert "emit-snbc: $label writes no image" 0 "$present"
}
printf 'package tests.bootstrap.rejected\n\nfunc main(): int {\n    return 0\n}\n' > "$SNBC_DIR/mod.sns"
printf 'package tests.bootstrap.rejected\n\nfunc main(): int {\n    return 0\n}\n' > "$SNBC_DIR/snova.sns"
printf 'package tests.bootstrap.rejected\n\nfunc main(): int {\n    return 0\n}\n' > "$SNBC_DIR/helper.sns"
reject_sns "mod.sns" "$SNBC_DIR/mod.sns"
reject_sns "snova.sns" "$SNBC_DIR/snova.sns"
reject_sns "helper.sns" "$SNBC_DIR/helper.sns"

rc="$(rc_of "$SNOVAC" emit-snbc "$SRC_SNL")"
assert "emit-snbc: missing -o exits non-zero" 1 "$(printf '%s' "$rc" | grep -c '[^0]')"
rm -f "$SNBC_DIR/no-input.snbc" "$SNBC_DIR/no-input.snbc.snbt"
rc="$(rc_of "$SNOVAC" emit-snbc -o "$SNBC_DIR/no-input.snbc")"
assert "emit-snbc: missing input exits non-zero" 1 "$(printf '%s' "$rc" | grep -c '[^0]')"
present=0
if [ -f "$SNBC_DIR/no-input.snbc" ] || [ -f "$SNBC_DIR/no-input.snbc.snbt" ]; then
  present=1
fi
assert "emit-snbc: missing input writes no image" 0 "$present"

# A lowered program still writes an image. A dropped node must not.
assert "emit-snbc: arithmetic.snl succeeds" 0 \
  "$(rc_of "$SNOVAC" emit-snbc "$DIR/compile-pass/arithmetic.snl" -o "$SNBC_DIR/arith.snbc")"
arith_present=0
if [ -f "$SNBC_DIR/arith.snbc" ] && [ -f "$SNBC_DIR/arith.snbc.snbt" ]; then
  arith_present=1
fi
assert "emit-snbc: arithmetic.snl writes an image" 1 "$arith_present"

reject_lower() {
  label="$1"
  src="$2"
  out="$SNBC_DIR/$label.snbc"
  err="$SNBC_DIR/$label.err"
  rm -f "$out" "$out.snbt" "$err"
  if "$SNOVAC" emit-snbc "$src" -o "$out" >"$SNBC_DIR/$label.stdout" 2>"$err"; then
    rc=0
  else
    rc=$?
  fi
  assert "emit-snbc: $label exits non-zero" 1 "$(printf '%s' "$rc" | grep -c '[^0]')"
  present=0
  if [ -f "$out" ] || [ -f "$out.snbt" ]; then
    present=1
  fi
  assert "emit-snbc: $label writes no image" 0 "$present"
  reported=0
  if grep -q 'SNOVA0400' "$err"; then
    reported=1
  fi
  assert "emit-snbc: $label reports SNOVA0400" 1 "$reported"
}

printf 'package tests.bootstrap.drop_break\n\nfunc main(): int {\n    break\n    return 0\n}\n' > "$SNBC_DIR/drop_break.snl"
reject_lower "drop_break" "$SNBC_DIR/drop_break.snl"

agree_exec "break_while" "$DIR/bootstrap/break_while.snl" 3 "" "$SNBC_DIR"
agree_exec "continue_while" "$DIR/bootstrap/continue_while.snl" 12 "" "$SNBC_DIR"
agree_exec "null_print" "$DIR/bootstrap/null_print.snl" 0 "null
" "$SNBC_DIR"
agree_exec "bodyless" "$DIR/bootstrap/bodyless.snl" 4 "" "$SNBC_DIR"
agree_exec "bodyless_main" "$DIR/bootstrap/bodyless_main.snl" 1 "" "$SNBC_DIR"
agree_exec "for_int" "$DIR/bootstrap/for_int.snl" 6 "" "$SNBC_DIR"
agree_exec "for_array" "$DIR/bootstrap/for_array.snl" 4 "" "$SNBC_DIR"
agree_exec "match_int" "$DIR/bootstrap/match_int.snl" 20 "" "$SNBC_DIR"
agree_exec "defer_order" "$DIR/bootstrap/defer_order.snl" 5 "first
second
" "$SNBC_DIR"
agree_exec "async_await" "$DIR/bootstrap/async_await.snl" 4 "" "$SNBC_DIR"
agree_exec "pulsar_launch" "$DIR/bootstrap/pulsar_launch.snl" 3 "p
" "$SNBC_DIR"
agree_exec "class_method" "$DIR/bootstrap/class_method.snl" 4 "" "$SNBC_DIR"
agree_exec "enum_match" "$DIR/bootstrap/enum_match.snl" 1 "" "$SNBC_DIR"
agree_exec "trait_decl" "$DIR/bootstrap/trait_decl.snl" 6 "" "$SNBC_DIR"
agree_exec "classes" "$DIR/compile-pass/classes.snl" 0 "ok
" "$SNBC_DIR"

# Snova compiler. V0 emits it, run-snbc executes it, and the image it writes
# matches the image V0 writes. On its own source that match is the guinea pig
# and the first Snova generation. A second run of that image is the next
# generation.
CC_ROOT="$(mktemp -d)"
COMPILER="$CC_ROOT/compiler-unit.snl"
sh "$DIR/../bootstrap/src/join.sh" > "$COMPILER"
C1="$CC_ROOT/compiler.snbc"
C1B="$CC_ROOT/compiler-b.snbc"
assert "emit-snbc: compiler unit succeeds" 0 \
  "$(rc_of "$SNL_ABS" emit-snbc "$COMPILER" -o "$C1")"
assert "emit-snbc: compiler unit second run succeeds" 0 \
  "$(rc_of "$SNL_ABS" emit-snbc "$COMPILER" -o "$C1B")"
compiler_images=0
same_bytes "$C1" "$C1B" || compiler_images=$?
assert "emit-snbc: compiler unit images match" 0 "$compiler_images"

compiler_case() {
  label="$1"
  expect_rc="$2"
  src="$3"
  work="$CC_ROOT/cc-$label"
  mkdir -p "$work/by-run" "$work/by-bc"
  cp "$src" "$work/by-run/in.snl"
  cp "$src" "$work/by-bc/in.snl"
  if (cd "$work/by-run" && "$SNL_ABS" run "$COMPILER" >"$work/run.out"); then
    run_rc=0
  else
    run_rc=$?
  fi
  if (cd "$work/by-bc" && "$SNL_ABS" run-snbc "$C1" >"$work/bc.out"); then
    bc_rc=0
  else
    bc_rc=$?
  fi
  assert "compiler: $label snl run exits 0" 0 "$run_rc"
  assert "compiler: $label run-snbc exits 0" 0 "$bc_rc"
  same=0
  same_bytes "$work/run.out" "$work/bc.out" || same=$?
  assert "compiler: $label stdout matches" 0 "$same"
  img=0
  same_bytes "$work/by-run/out.snbc" "$work/by-bc/out.snbc" || img=$?
  assert "compiler: $label images match across executors" 0 "$img"
  assert "compiler: $label v0 emit succeeds" 0 \
    "$(rc_of "$SNL_ABS" emit-snbc "$work/by-run/in.snl" -o "$work/v0.snbc")"
  v0=0
  same_bytes "$work/by-run/out.snbc" "$work/v0.snbc" || v0=$?
  assert "compiler: $label image matches v0" 0 "$v0"
  if "$SNL_ABS" run-snbc "$work/by-run/out.snbc" >/dev/null 2>&1; then
    prod_rc=0
  else
    prod_rc=$?
  fi
  assert "compiler: $label program exit" "$expect_rc" "$prod_rc"
  if "$SNL_ABS" run "$work/by-run/in.snl" >/dev/null 2>&1; then
    host_rc=0
  else
    host_rc=$?
  fi
  assert "compiler: $label matches snl run" "$host_rc" "$prod_rc"
}

printf 'package p\n\nfunc main(): int {\n    return 0\n}\n' > "$CC_ROOT/tiny-zero.snl"
printf 'package p\n\nfunc main(): int {\n    return 1 + 2 * 3\n}\n' > "$CC_ROOT/tiny-prec.snl"
printf 'package p\n\nfunc main(): int {\n    return (1 + 2) * 3\n}\n' > "$CC_ROOT/tiny-paren.snl"
printf 'package p\n\nfunc main(): int {\n    return -4 + 10\n}\n' > "$CC_ROOT/tiny-neg.snl"
printf 'package p\n\nfunc main(): int {\n    return 20 / 4 %% 3\n}\n' > "$CC_ROOT/tiny-div.snl"
printf 'package p.q\n\n// kept\nfunc main(): int {\n    return 8 - 3\n}\n' > "$CC_ROOT/tiny-comment.snl"
compiler_case "zero" 0 "$CC_ROOT/tiny-zero.snl"
compiler_case "prec" 7 "$CC_ROOT/tiny-prec.snl"
compiler_case "paren" 9 "$CC_ROOT/tiny-paren.snl"
compiler_case "neg" 6 "$CC_ROOT/tiny-neg.snl"
compiler_case "div" 2 "$CC_ROOT/tiny-div.snl"
compiler_case "comment" 5 "$CC_ROOT/tiny-comment.snl"

printf 'package p\n\nfunc main(): int {\n    let x = 1\n    return x\n}\n' > "$CC_ROOT/tiny-let.snl"
compiler_case "let" 1 "$CC_ROOT/tiny-let.snl"

compiler_case "break" 3 "$DIR/bootstrap/break_while.snl"
compiler_case "continue" 12 "$DIR/bootstrap/continue_while.snl"
compiler_case "null" 0 "$DIR/bootstrap/null_print.snl"
compiler_case "bodyless" 4 "$DIR/bootstrap/bodyless.snl"
compiler_case "for" 6 "$DIR/bootstrap/for_int.snl"
compiler_case "match" 20 "$DIR/bootstrap/match_int.snl"
compiler_case "defer" 5 "$DIR/bootstrap/defer_order.snl"
compiler_case "async" 4 "$DIR/bootstrap/async_await.snl"
compiler_case "pulsar" 3 "$DIR/bootstrap/pulsar_launch.snl"
compiler_case "class" 4 "$DIR/bootstrap/class_method.snl"
compiler_case "enum" 1 "$DIR/bootstrap/enum_match.snl"
compiler_case "trait" 6 "$DIR/bootstrap/trait_decl.snl"
compiler_case "classes" 0 "$DIR/compile-pass/classes.snl"

# Guinea pig image, then the same image compiling its source again.
SELF="$CC_ROOT/self"
mkdir -p "$SELF"
cp "$COMPILER" "$SELF/in.snl"
cp "$C1" "$SELF/v0.snbc"
if (cd "$SELF" && "$SNL_ABS" run-snbc "$SELF/v0.snbc" >"$SELF/gen.out"); then
  gen_rc=0
else
  gen_rc=$?
fi
assert "compiler: first generation exits 0" 0 "$gen_rc"
self_cmp=0
same_bytes "$SELF/v0.snbc" "$SELF/out.snbc" || self_cmp=$?
assert "compiler: first generation matches v0" 0 "$self_cmp"
cp "$SELF/out.snbc" "$SELF/gen1.snbc"
rm -f "$SELF/out.snbc"
if (cd "$SELF" && "$SNL_ABS" run-snbc "$SELF/gen1.snbc" >"$SELF/gen2.out"); then
  gen2_rc=0
else
  gen2_rc=$?
fi
assert "compiler: second generation exits 0" 0 "$gen2_rc"
self2=0
same_bytes "$SELF/gen1.snbc" "$SELF/out.snbc" || self2=$?
assert "compiler: second generation matches" 0 "$self2"

rm -rf "$SNBC_DIR" "$CC_ROOT"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
