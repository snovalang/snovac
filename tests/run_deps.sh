# Native backend, target detection, and sandbox testing
assert "target: --target-info outputs host and target" 1 \
  "$("$SNOVAC" --target-info | grep -c 'Target OS:' || true)"

assert "target: env override changes target OS" 1 \
  "$(SNOVA_TARGET_OS=freebsd "$SNOVAC" --target-info | grep -c 'freebsd (overridden)' || true)"

# Compile standalone native binary and test execution
BUILD_OUT="$PROJ/bin_hello"
assert "build: compile standalone native binary" 0 \
  "$(rc_of "$SNOVAC" build "$RP/hello.snl" -o "$BUILD_OUT")"

if [ -f "$BUILD_OUT" ]; then
  got_native="$("$BUILD_OUT" 2>&1 || true)"
  assert "build: native binary output matches run-pass" "$(cat "$RP/hello.stdout")" "$got_native"
  rm -f "$BUILD_OUT"
fi

# ── snovac get & dependency graph tests ─────────────────────────────────────
GET_TEST_DIR="$(pwd)/build/get_test_$$"
mkdir -p "$GET_TEST_DIR"

# Fixture: depC
mkdir -p "$GET_TEST_DIR/depC/src"
cat > "$GET_TEST_DIR/depC/mod.sns" <<'EOF'
module depC
snova "1.0.0"
EOF
cat > "$GET_TEST_DIR/depC/src/DepC.snl" <<'EOF'
package depC
public struct ValC {
    public let val: int
}
EOF

# Fixture: depB (depends on depC)
mkdir -p "$GET_TEST_DIR/depB/src"
cat > "$GET_TEST_DIR/depB/mod.sns" <<EOF
module depB
snova "1.0.0"
dependencies(
    direct = [
        "$GET_TEST_DIR/depC@1.0.0"
    ]
)
EOF
cat > "$GET_TEST_DIR/depB/src/DepB.snl" <<'EOF'
package depB
import depC.ValC
public struct ValB {
    public let c: ValC
}
EOF

# Fixture: depA (depends on depB and depC)
mkdir -p "$GET_TEST_DIR/depA/src"
cat > "$GET_TEST_DIR/depA/mod.sns" <<EOF
module depA
snova "1.0.0"
dependencies(
    direct = [
        "$GET_TEST_DIR/depB@1.0.0",
        "$GET_TEST_DIR/depC@1.0.0"
    ]
)
EOF
cat > "$GET_TEST_DIR/depA/src/DepA.snl" <<'EOF'
package depA
import depB.ValB
public struct ValA {
    public let b: ValB
}
EOF

# Project under test
GET_PROJ="$GET_TEST_DIR/my_app"
mkdir -p "$GET_PROJ/src/app"
cat > "$GET_PROJ/mod.sns" <<'EOF'
module my_app
snova "1.0.0"
EOF
cat > "$GET_PROJ/src/app/Main.snl" <<'EOF'
package app
import depA.ValA

func main(): int {
    return 0
}
EOF

# Test: get direct dependency with transitive resolution
assert "get: fetch direct and transitive deps" 0 \
  "$(rc_of "$SNOVAC" get --project "$GET_PROJ" "$GET_TEST_DIR/depA")"

assert "get: direct dependency listed in mod.sns" 1 \
  "$(grep -c "depA@1.0.0" "$GET_PROJ/mod.sns" || true)"

assert "get: indirect edge depA -> depB listed" 1 \
  "$(grep -c "depA -> depB" "$GET_PROJ/mod.sns" || true)"

assert "get: indirect edge depB -> depC listed" 1 \
  "$(grep -c "depB -> depC" "$GET_PROJ/mod.sns" || true)"

assert "get: project check passes with fetched deps" 0 \
  "$(rc_of "$SNOVAC" check --project "$GET_PROJ/src/app/Main.snl")"

# Test: idempotency (re-running get does not duplicate or fail)
assert "get: re-running get is idempotent" 0 \
  "$(rc_of "$SNOVAC" get --project "$GET_PROJ" "$GET_TEST_DIR/depA")"

assert "get: direct count remains 1 after rerun" 1 \
  "$(grep -c "depA@1.0.0" "$GET_PROJ/mod.sns" || true)"

# Test: project run and build with fetched dependencies
assert "get: project run with fetched deps" 0 \
  "$(rc_of "$SNOVAC" run --project "$GET_PROJ")"

BUILD_PROJ_OUT="$GET_TEST_DIR/app_bin"
assert "get: project build with fetched deps" 0 \
  "$(rc_of "$SNOVAC" build --project "$GET_PROJ" -o "$BUILD_PROJ_OUT")"
rm -f "$BUILD_PROJ_OUT"

# Test: diamond sharing (depD -> depF, depE -> depF)
mkdir -p "$GET_TEST_DIR/depF/src" "$GET_TEST_DIR/depD/src" "$GET_TEST_DIR/depE/src" "$GET_TEST_DIR/diamond_app/src/app"
cat > "$GET_TEST_DIR/depF/mod.sns" <<'EOF'
module depF
snova "1.0.0"
EOF
cat > "$GET_TEST_DIR/depF/src/DepF.snl" <<'EOF'
package depF
public struct ItemF { public let x: int }
EOF

cat > "$GET_TEST_DIR/depD/mod.sns" <<EOF
module depD
snova "1.0.0"
dependencies(
    direct = [ "$GET_TEST_DIR/depF@1.0.0" ]
)
EOF
cat > "$GET_TEST_DIR/depD/src/DepD.snl" <<'EOF'
package depD
import depF.ItemF
public struct ItemD { public let f: ItemF }
EOF

cat > "$GET_TEST_DIR/depE/mod.sns" <<EOF
module depE
snova "1.0.0"
dependencies(
    direct = [ "$GET_TEST_DIR/depF@1.0.0" ]
)
EOF
cat > "$GET_TEST_DIR/depE/src/DepE.snl" <<'EOF'
package depE
import depF.ItemF
public struct ItemE { public let f: ItemF }
EOF

cat > "$GET_TEST_DIR/diamond_app/mod.sns" <<'EOF'
module diamond_app
snova "1.0.0"
EOF
cat > "$GET_TEST_DIR/diamond_app/src/app/Main.snl" <<'EOF'
package app
import depD.ItemD
import depE.ItemE
func main(): int { return 0 }
EOF

assert "get: diamond direct depD" 0 \
  "$(rc_of "$SNOVAC" get --project "$GET_TEST_DIR/diamond_app" "$GET_TEST_DIR/depD")"
assert "get: diamond direct depE" 0 \
  "$(rc_of "$SNOVAC" get --project "$GET_TEST_DIR/diamond_app" "$GET_TEST_DIR/depE")"

assert "get: diamond has depD direct" 1 \
  "$(grep -c "depD@1.0.0" "$GET_TEST_DIR/diamond_app/mod.sns" || true)"
assert "get: diamond has depE direct" 1 \
  "$(grep -c "depE@1.0.0" "$GET_TEST_DIR/diamond_app/mod.sns" || true)"
assert "get: diamond indirect depD -> depF" 1 \
  "$(grep -c "depD -> depF" "$GET_TEST_DIR/diamond_app/mod.sns" || true)"
assert "get: diamond indirect depE -> depF" 1 \
  "$(grep -c "depE -> depF" "$GET_TEST_DIR/diamond_app/mod.sns" || true)"
assert "get: diamond project check passes" 0 \
  "$(rc_of "$SNOVAC" check --project "$GET_TEST_DIR/diamond_app/src/app/Main.snl")"

# Test: get with no args and no manifest fails cleanly
EMPTY_DIR="$(pwd)/build/empty_test_$$"
mkdir -p "$EMPTY_DIR"
assert "get: no args without manifest errors cleanly" 2 \
  "$(rc_of "$SNOVAC" get --project "$EMPTY_DIR")"
rm -rf "$EMPTY_DIR"

# Test: cycle detection (cycleA -> cycleB -> cycleA)
mkdir -p "$GET_TEST_DIR/cycleA" "$GET_TEST_DIR/cycleB" "$GET_TEST_DIR/cycle_proj"
cat > "$GET_TEST_DIR/cycleA/mod.sns" <<EOF
module cycleA
snova "1.0.0"
dependencies(
    direct = [
        "$GET_TEST_DIR/cycleB@1.0.0"
    ]
)
EOF
cat > "$GET_TEST_DIR/cycleB/mod.sns" <<EOF
module cycleB
snova "1.0.0"
dependencies(
    direct = [
        "$GET_TEST_DIR/cycleA@1.0.0"
    ]
)
EOF
cat > "$GET_TEST_DIR/cycle_proj/mod.sns" <<'EOF'
module cycle_proj
snova "1.0.0"
EOF

CYCLE_OUT="$("$SNOVAC" get --project "$GET_TEST_DIR/cycle_proj" "$GET_TEST_DIR/cycleA" 2>&1 || true)"
assert "get: cycle detection fails command" 1 \
  "$(rc_of "$SNOVAC" get --project "$GET_TEST_DIR/cycle_proj" "$GET_TEST_DIR/cycleA")"
assert "get: cycle detection reports cycle error" 1 \
  "$(echo "$CYCLE_OUT" | grep -c "dependency cycle detected" || true)"

rm -rf "$GET_TEST_DIR"

# Canonical SnBC. Two runs must match, including the listing beside -o.
# .sns paths, including manifests, stay off this compile path.
SNBC_DIR="$(mktemp -d)"
SRC_SNL="$DIR/bootstrap/return_zero.snl"
A="$SNBC_DIR/a.snbc"
B="$SNBC_DIR/b.snbc"
assert "emit-snbc: return_zero.snl succeeds" 0 \
  "$(rc_of "$SNOVAC" emit-snbc "$SRC_SNL" -o "$A")"
assert "emit-snbc: second run succeeds" 0 \
  "$(rc_of "$SNOVAC" emit-snbc "$SRC_SNL" -o "$B")"
images_match=0
same_bytes "$A" "$B" || images_match=$?
assert "emit-snbc: images are byte-identical" 0 "$images_match"
listings_match=0
same_bytes "$A.snbt" "$B.snbt" || listings_match=$?
assert "emit-snbc: listings are byte-identical" 0 "$listings_match"
dd if="$A" of="$SNBC_DIR/head4" bs=1 count=4 >/dev/null 2>&1 || true
printf 'SNBC' > "$SNBC_DIR/magic"
magic_match=0
same_bytes "$SNBC_DIR/head4" "$SNBC_DIR/magic" || magic_match=$?
assert "emit-snbc: magic is 53 4e 42 43" 0 "$magic_match"
image_size="$(wc -c < "$A" | tr -d '[:space:]')"
assert "emit-snbc: image is the return_zero container" 52 "$image_size"
assert "emit-snbc: listing names snbc 1" 1 "$(grep -c '^; snbc 1$' "$A.snbt")"
assert "emit-snbc: listing names main 0" 1 "$(grep -c '^; main 0$' "$A.snbt")"
assert "emit-snbc: listing has OP_CONST_INT 0" 1 \
  "$(grep -c '^0 OP_CONST_INT 0$' "$A.snbt")"
assert "emit-snbc: listing has the trailing OP_CONST_UNIT" 1 \
  "$(grep -c '^10 OP_CONST_UNIT$' "$A.snbt")"
assert "emit-snbc: listing has both OP_RETURN bytes" 2 \
  "$(grep -c 'OP_RETURN$' "$A.snbt")"
assert "emit-snbc: listing has no source path" 0 \
  "$(grep -c 'return_zero' "$A.snbt" || true)"
assert "emit-snbc: .snl still parses" 0 \
  "$(rc_of "$SNOVAC" --check-parse "$SRC_SNL")"
assert "emit-snbc: .snl still runs" 0 \
  "$(rc_of "$SNOVAC" run "$SRC_SNL")"
assert "run-snbc: return_zero exits 0" 0 \
  "$(rc_of "$SNOVAC" run-snbc "$A")"

case "$SNOVAC" in
  /*) SNL_ABS="$SNOVAC" ;;
  *) SNL_ABS="$(pwd)/$SNOVAC" ;;
esac

