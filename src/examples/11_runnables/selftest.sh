#!/usr/bin/env bash
# Self-test for build.cxx — runs against the already-compiled driver at
# build/.bootstrap. Exercises user-visible invariants; any failure
# propagates a non-zero exit.
#
# Usage: invoked by `./build.cxx --run selftest`. The root build.cxx's
# selftest runnable builds a command with std::format() over build::ctx
# (c.root / c.build_dir) and passes the result here as $1 and $2.
set -euo pipefail

ROOT="${1:?missing ROOT}"
BUILD="${2:?missing BUILD}"
DRIVER="$BUILD/.bootstrap"

pass() { printf '  \033[1;32m✓\033[0m %s\n' "$1"; }
fail() { printf '  \033[1;31m✗\033[0m %s\n' "$1"; exit 1; }

[ -x "$DRIVER" ] || fail "driver missing: $DRIVER"
pass "driver exists"

"$DRIVER" --help >/dev/null 2>&1 || fail "--help nonzero"
pass "--help works"

list_out=$("$DRIVER" --list 2>&1)
echo "$list_out" | grep -q "targets:"  || fail "--list has no 'targets:' section"
echo "$list_out" | grep -q "runnables" || fail "--list has no 'runnables:' section"
pass "--list shows targets + runnables"

"$DRIVER" --target ex_01_hello >/dev/null 2>&1 || fail "--target ex_01_hello failed"
[ -x "$BUILD/ex_01_hello" ]                    || fail "ex_01_hello not produced"
pass "--target ex_01_hello builds"

# Incremental: running again must be all-cached.
out=$("$DRIVER" --target ex_01_hello 2>&1)
echo "$out" | grep -q "2 cached" || fail "rerun should be fully cached, got: $out"
pass "rerun is cached"

# --why surfaces a real reason after touch.
touch "$ROOT/src/examples/01_hello/main.cpp"
out=$("$DRIVER" --target ex_01_hello --why 2>&1)
echo "$out" | grep -q "input newer:" || fail "--why should show 'input newer:' after touch"
pass "--why prints rebuild reason"

# Unknown target -> exit 2.
set +e
"$DRIVER" --target does_not_exist >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || fail "unknown target should exit 2, got $rc"
pass "unknown target exits 2"

# Unknown runnable -> exit 2.
set +e
"$DRIVER" --run does_not_exist >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || fail "unknown runnable should exit 2, got $rc"
pass "unknown runnable exits 2"

# compile_commands.json exists and is JSON-y.
[ -f "$BUILD/compile_commands.json" ]                    || fail "no compile_commands.json"
head -c 1 "$BUILD/compile_commands.json" | grep -q '\[' || fail "compile_commands.json not a JSON array"
pass "compile_commands.json looks sane"

# targets.hpp is present and references known targets.
[ -f "$BUILD/targets.hpp" ]                           || fail "no targets.hpp"
grep -q 'targets::ex_01_hello\|ex_01_hello"'          "$BUILD/targets.hpp" || fail "targets.hpp missing ex_01_hello"
grep -q 'runnables::ex_11_test\|ex_11_test"'          "$BUILD/targets.hpp" || fail "targets.hpp missing runnables::ex_11_test"
pass "targets.hpp declares targets and runnables"

printf '\n\033[1;32m● all selftests passed\033[0m\n'
