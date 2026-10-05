# `Task<Result<unit, DataError>>` must close as two separate `>` tokens.
# Snovalang has no shift operators; every `>>` in the corpus is a generic close.
assert "generics: no >> token" 2 "$(toks generics.snl | grep -c '^ *[0-9]*:[0-9]* *> *$')"

# `1.toString()` — the `.` after an int literal is member access, not a fraction.
assert "numbers: 1.toString splits" 1 \
  "$(toks numbers.snl | awk '$1=="6:9"' | grep -c 'int literal')"
assert "numbers: long suffix"    1 "$(toks numbers.snl | grep -c 'long literal')"
assert "numbers: 1.5 and 1e9"    2 "$(toks numbers.snl | grep -c 'double literal')"
assert "numbers: hex is int"     1 "$(toks numbers.snl | grep -c '0xFFFF')"

# `$$` is a literal dollar and must NOT mark the string interpolated;
# a quote inside `${...}` must not terminate the enclosing string.
assert "interp: exactly 2 interpolated" 2 \
  "$(toks interp.snl | grep -c '\[interpolated\]')"
assert "interp: 5 strings total" 5 \
  "$(toks interp.snl | grep -c 'string literal')"

# `and` is a method name in the corpus, so it must lex as an identifier.
assert "softkw: and is identifier" 1 \
  "$(toks softkw.snl | grep -c 'identifier *and$')"

# Property accessors: the absence of an accessor is the whole point of the
# feature, so assert that omitted ones stay omitted and are not defaulted in.
PROPS="$DIR/compile-pass/property_accessors.snl"
[ -f "$PROPS" ] || PROPS="$DIR/../../tests/compile-pass/property_accessors.snl"
if [ -f "$PROPS" ]; then
  props() { "$SNOVAC" --emit=ast "$PROPS"; }
  assert "props: parses clean" 0 "$(props >/dev/null 2>&1; echo $?)"
  assert "props: read-only field has get and no set" 1 \
    "$(props | grep -c 'private let id: string { get }')"
  assert "props: write-only field has set and no get" 1 \
    "$(props | grep -c 'var token: string { set }')"
  assert "props: empty block is not both accessors" 1 \
    "$(props | grep -c 'let salt: string { }')"
  assert "props: no block stays a plain field" 1 \
    "$(props | grep -c 'let attempts: int$')"
  assert "props: projections recorded on both sides" 1 \
    "$(props | grep -c 'var score: int { get: (\.\.\.) set: (\.\.\.) }')"
fi

# End-to-end execution against the repository's own run-pass fixtures. These
# assert real program output, not parser shape.
RP="$DIR/run-pass"
[ -d "$RP" ] || RP="$DIR/../../tests/run-pass"
for name in hello string_comment_url array_field_access counter extension_invocation \
  own_ptr own_stack own_loop_live own_defer_ok own_defer_lifo own_defer_fail own_defer_use \
  overload_show; do
  if [ -f "$RP/$name.snl" ] && [ -f "$RP/$name.stdout" ]; then
    got="$("$SNOVAC" run "$RP/$name.snl" 2>&1 || true)"
    assert "run: $name" "$(cat "$RP/$name.stdout")" "$got"
  fi
done

# A .java file is not source, even when its text is Snovalang. .snl still runs.
EXTDIR="$(mktemp -d)"
cat > "$EXTDIR/main.java" <<'EOF'
public struct Snovalang {
    public let field: int
}
func main(): unit {
    let snova = Snovalang(field: "Oi!")
    Console.println(snova.field)
}
EOF
java_out="$("$SNOVAC" run "$EXTDIR/main.java" 2>&1 || true)"
java_rc="$(rc_of "$SNOVAC" run "$EXTDIR/main.java")"
assert "ext: main.java is rejected" 1 "$(printf '%s' "$java_rc" | grep -c '[^0]')"
assert "ext: main.java does not print Oi!" 0 "$(printf '%s' "$java_out" | grep -c '^Oi!$')"
cp "$EXTDIR/main.java" "$EXTDIR/main.sno"
sno_out="$("$SNOVAC" run "$EXTDIR/main.sno" 2>&1 || true)"
sno_rc="$(rc_of "$SNOVAC" run "$EXTDIR/main.sno")"
assert "ext: main.sno is rejected" 1 "$(printf '%s' "$sno_rc" | grep -c '[^0]')"
assert "ext: main.sno does not print Oi!" 0 "$(printf '%s' "$sno_out" | grep -c '^Oi!$')"
cat > "$EXTDIR/mod.sns" <<'EOF'
func main(): unit {
    Console.println("Oi!")
}
EOF
mod_out="$("$SNOVAC" run "$EXTDIR/mod.sns" 2>&1 || true)"
mod_rc="$(rc_of "$SNOVAC" run "$EXTDIR/mod.sns")"
assert "ext: mod.sns manifest is not a script" 1 "$(printf '%s' "$mod_rc" | grep -c '[^0]')"
assert "ext: mod.sns does not print Oi!" 0 "$(printf '%s' "$mod_out" | grep -c '^Oi!$')"
cat > "$EXTDIR/main.snl" <<'EOF'
package tests.ext.ok

func main(): int {
    return 0
}
EOF
assert "ext: main.snl is accepted" 0 "$(rc_of "$SNOVAC" run "$EXTDIR/main.snl")"
printf 'func main(): int { return 0 }\n' > "$EXTDIR/nosuffix"
nosuffix_rc="$(rc_of "$SNOVAC" run "$EXTDIR/nosuffix")"
assert "ext: extensionless path is rejected" 1 "$(printf '%s' "$nosuffix_rc" | grep -c '[^0]')"
cat > "$EXTDIR/bad_field.snl" <<'EOF'
package tests.ext.bad

public struct Snovalang {
    public let field: int
}
func main(): unit {
    let snova = Snovalang(field: "Oi!")
    return
}
EOF
bad_out="$("$SNOVAC" run "$EXTDIR/bad_field.snl" 2>&1 || true)"
bad_rc="$(rc_of "$SNOVAC" run "$EXTDIR/bad_field.snl")"
assert "ext: string field value is a compile error" 1 "$(printf '%s' "$bad_rc" | grep -c '[^0]')"
assert "ext: rejected program does not print Oi!" 0 "$(printf '%s\n' "$bad_out" | grep -c '^Oi!$')"
case "$SNOVAC" in
  /*) snl_abs="$SNOVAC" ;;
  *) snl_abs="$(pwd)/$SNOVAC" ;;
esac
rel_out="$(cd "$EXTDIR" && "$snl_abs" run ./bad_field.snl 2>&1 || true)"
rel_rc="$(cd "$EXTDIR" && rc_of "$snl_abs" run ./bad_field.snl)"
assert "ext: relative path is a compile error" 1 "$(printf '%s' "$rel_rc" | grep -c '[^0]')"
assert "ext: relative path does not print Oi!" 0 "$(printf '%s\n' "$rel_out" | grep -c '^Oi!$')"

cat > "$EXTDIR/opt_field.snl" <<'EOF'
package com.aggitech

public struct Snovalang {
    public let field: int?
}

func main() {
    let snova = Snovalang{
        field: "afad"
    }
    let snova2 = snova
    Console.println(snova?.field)
}
EOF
rm -f "$EXTDIR/opt_field"
opt_rc="$(rc_of "$SNOVAC" build "$EXTDIR/opt_field.snl" -o "$EXTDIR/opt_field")"
opt_present=0
if [ -e "$EXTDIR/opt_field" ]; then
  opt_present=1
fi
assert "build: string into int? exits non-zero" 1 "$(printf '%s' "$opt_rc" | grep -c '[^0]')"
assert "build: string into int? writes no executable" 0 "$opt_present"

cat > "$EXTDIR/inner_call.snl" <<'EOF'
package tests.inner.call

func main(): int {
    func test() {
        Console.println("inner")
    }()
    return 0
}
EOF
inner_out="$("$SNOVAC" run "$EXTDIR/inner_call.snl" 2>&1 || true)"
inner_rc="$(rc_of "$SNOVAC" run "$EXTDIR/inner_call.snl")"
assert "iife: plain inner call exits 0" 0 "$inner_rc"
assert "iife: plain inner call prints" 1 "$(printf '%s\n' "$inner_out" | grep -c '^inner$')"

cat > "$EXTDIR/pulsar_call.snl" <<'EOF'
package tests.inner.pulsar

func main(): int {
    pulsar func test(x: int) {
        Console.println(x)
    }(1)
    return 0
}
EOF
pulsar_out="$("$SNOVAC" run "$EXTDIR/pulsar_call.snl" 2>&1 || true)"
pulsar_rc="$(rc_of "$SNOVAC" run "$EXTDIR/pulsar_call.snl")"
assert "iife: pulsar inner call exits 0" 0 "$pulsar_rc"
assert "iife: pulsar inner call prints" 1 "$(printf '%s\n' "$pulsar_out" | grep -c '^1$')"

cat > "$EXTDIR/missing_call.snl" <<'EOF'
package tests.inner.missing

func main(): int {
    func test() {
        Console.println("no")
    }
    return 0
}
EOF
miss_out="$("$SNOVAC" run "$EXTDIR/missing_call.snl" 2>&1 || true)"
miss_rc="$(rc_of "$SNOVAC" run "$EXTDIR/missing_call.snl")"
assert "iife: missing call exits non-zero" 1 "$(printf '%s' "$miss_rc" | grep -c '[^0]')"
assert "iife: missing call reports SNOVA0148" 1 "$(printf '%s\n' "$miss_out" | grep -c 'SNOVA0148')"
assert "iife: missing call does not print" 0 "$(printf '%s\n' "$miss_out" | grep -c '^no$')"
rm -rf "$EXTDIR"

if [ -z "${SNOVA_BUILTIN_DIR:-}" ]; then
  if [ -d "$DIR/../builtin" ]; then
    export SNOVA_BUILTIN_DIR="$DIR/../builtin"
  elif [ -d "$DIR/../../builtin" ]; then
    export SNOVA_BUILTIN_DIR="$DIR/../../builtin"
  fi
fi

# Project-wide analysis: the regression that motivated it was a syntax error
# in a NON-entry file passing every gate and the program running anyway,
# because only the entry file was ever looked at. These assert that the whole
# project is analysed, and that a clean one still passes.
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM
mkdir -p "$PROJ/src/app"
cat > "$PROJ/src/app/Main.snl" <<'EOF'
package app

import app.Models

func main(): int {
    return 0
}
EOF
cat > "$PROJ/src/app/Models.snl" <<'EOF'
package app

public struct Config {
    public let name: string
}
EOF

assert "project: clean project passes" 0 \
  "$(rc_of "$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snl")"

# The entry file itself stays valid; only the sibling module is broken.
cat > "$PROJ/src/app/Models.snl" <<'EOF'
package app

public struct Config
    public let name: string
}
EOF

assert "project: sibling syntax error is caught" 1 \
  "$(rc_of "$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snl")"
assert "project: single-file mode still cannot see it" 0 \
  "$(rc_of "$SNOVAC" --check-parse "$PROJ/src/app/Main.snl")"
assert "project: error names the sibling, not the entry file" 1 \
  "$("$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snl" 2>&1 | grep -c 'Models.snl:5')"

# `import a.b.C` names a SYMBOL in package `a.b` as often as it names a whole
# package; treating the symbol spelling as a missing package made SNOVA0050
# fire on correct code across an entire project.
mkdir -p "$PROJ/src/lib"
cat > "$PROJ/src/lib/Util.snl" <<'EOF'
package app.lib

public struct Helper {
    public let id: int
}
EOF
cat > "$PROJ/src/app/Models.snl" <<'EOF'
package app

import app.lib.Helper

public struct Config {
    public let name: string
}
EOF

assert "project: symbol import resolves to its package" 0 \
  "$(rc_of "$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snl")"

# The above only proves the IMPORT LINE doesn't trip SNOVA0050 (package-graph
# linking); it never actually referenced `Helper` by name anywhere, so it
# could not catch resolve.c's own copy of the same bug. Every import-scope
# lookup in resolve.c (sn_resolve_ident, sn_resolve_type_name,
# sn_resolve_member_path) looked up `sn_resolver_package_scope(r, imp_pkg)`
# with the RAW import string as written — for `import app.lib.Helper` that's
# "app.lib.Helper", which matches no real package (the real one is
# "app.lib"), so the exact-pointer-equality scope lookup always failed
# silently. Only the bare `import app.lib` form ever worked.
#
# This needs its own project (with a real snova.toml, not $PROJ, which has
# none): without a manifest, source_root falls back to just the entry file's
# own directory (project_discover's documented behavior), so a sibling
# package directory like src/lib/ is never scanned at all — resolving
# `app.lib.Helper` would then hit resolve_import_target()'s longest-match
# fallthrough all the way down to "app" itself (the importer's own,
# genuinely-scanned package), which happens to exist and masks the bug
# behind a coincidental self-package match instead of a real cross-package
# one. A manifest makes source_root the project's whole `src/`, covering
# both directories for real.
SYMPROJ="$(mktemp -d)"
mkdir -p "$SYMPROJ/src/app" "$SYMPROJ/src/lib"
cat > "$SYMPROJ/snova.toml" <<'EOF'
[package]
name = "symproj"
EOF
cat > "$SYMPROJ/src/lib/Util.snl" <<'EOF'
package app.lib

public struct Helper {
    public let id: int
}
EOF
cat > "$SYMPROJ/src/app/Main.snl" <<'EOF'
package app

import app.lib.Helper

func main(): int {
    let h = Helper { id: 1 }
    return h.id
}
EOF

assert "project: symbol import resolves the symbol itself, not just the package" 0 \
  "$(rc_of "$SNOVAC" check --project "$SYMPROJ/src/app/Main.snl")"
rm -rf "$SYMPROJ"

cat > "$PROJ/src/app/Main.snl" <<'EOF'
package app

import app.Models

func main(): int {
    return 0
}
EOF

# No prefix of this names a declared package, and it is not under a
# toolchain-provided namespace, so it stays a reported error.
cat > "$PROJ/src/app/Models.snl" <<'EOF'
package app

import nowhere.at.All

public struct Config {
    public let name: string
}
EOF

assert "project: genuinely missing import still reported" 1 \
  "$(rc_of "$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snl")"

# `builtin.*` / `stdlib.*` packages actually registered in
# `compiler/src/lsp/NativePackages.snl` (and generated into
# `builtin/native-packages.list` by `scripts/gen-packages.sh`) have no
# `.snl` file anywhere snovac can see, so absence there must not be
# reported as a missing package.
cat > "$PROJ/src/app/Models.snl" <<'EOF'
package app

import builtin.syntax.Syntax

public struct Config {
    public let name: string
}
EOF

assert "project: registered native package is not judged" 0 \
  "$(rc_of "$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snl")"

# `builtin.http.Http` is NOT in native-packages.list (no such package is
# registered anywhere yet) and has no `.snl` file either — this used to be
# silently accepted by a blanket `builtin.*`/`stdlib.*` prefix rule (the exact
# gap `tests/conformance/` flagged as "missing_import is not rejected by
# snovac check"). Fixed by sn_pkggraph_load_native_manifest(): only names
# actually present in the manifest are treated as toolchain-provided now.
cat > "$PROJ/src/app/Models.snl" <<'EOF'
package app

import builtin.http.Http

public struct Config {
    public let name: string
}
EOF

assert "project: unregistered builtin.* namespace is still reported" 1 \
  "$(rc_of "$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snl")"

