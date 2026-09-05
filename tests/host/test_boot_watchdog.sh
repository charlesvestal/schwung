#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/../.."

# boot_target_lib.sh also implements a two-strike boot watchdog: two failed
# attempts at the same target in a row (no intervening "healthy" mark) trips
# bt_watchdog_forced, which shim-entrypoint.sh (a separate task) uses to force
# a fallback boot target rather than looping forever on a broken one. This
# test drives that state machine directly against a fixture registry.

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
pass() { echo "PASS: $*"; }

fixture=$(mktemp -d)
cleanup() { rm -rf "$fixture"; }
trap cleanup EXIT

mkdir -p "$fixture/schwung" "$fixture/v"
cat > "$fixture/schwung/boot.json" <<'EOF'
{
  "name" : "Schwung",
  "exec" : "/data/UserData/schwung/schwung"
}
EOF
cat > "$fixture/v/boot.json" <<'EOF'
{
  "name" : "Third Party V",
  "exec" : "/data/UserData/boot-targets/v/run.sh"
}
EOF

export BOOT_TARGETS_DIR="$fixture"
# shellcheck disable=SC1091
. src/host/boot_target_lib.sh

bt_watchdog_clear

# ---- 1. first enter -> echoes 1, not yet forced --------------------------
got=$(bt_watchdog_enter schwung)
[ "$got" = "1" ] && pass "first enter echoes 1" \
    || fail "first enter echoed [$got], expected 1"
if bt_watchdog_forced schwung; then
    fail "forced after only 1 attempt"
else
    pass "not forced after 1 attempt"
fi

# ---- 2. second enter -> echoes 2, forced now ------------------------------
got=$(bt_watchdog_enter schwung)
[ "$got" = "2" ] && pass "second enter echoes 2" \
    || fail "second enter echoed [$got], expected 2"
if bt_watchdog_forced schwung; then
    pass "forced after 2 attempts"
else
    fail "not forced after 2 attempts at the same target"
fi

# ---- 3. a healthy mark clears the strikes before the next enter ----------
touch "$fixture/schwung/healthy"
got=$(bt_watchdog_enter schwung)
[ "$got" = "1" ] && pass "enter after healthy mark echoes 1 (cleared then incremented)" \
    || fail "enter after healthy mark echoed [$got], expected 1"
if [ -f "$fixture/schwung/healthy" ]; then
    fail "healthy marker still present after bt_watchdog_enter consumed it"
else
    pass "healthy marker is consumed by bt_watchdog_enter"
fi

# ---- 4. switching targets resets the strike count -------------------------
bt_watchdog_clear
bt_watchdog_enter schwung > /dev/null
got=$(bt_watchdog_enter v)
[ "$got" = "1" ] && pass "entering a different target resets the count to 1" \
    || fail "entering a different target echoed [$got], expected 1"
if bt_watchdog_forced v; then
    fail "v forced after a single attempt following a target switch"
else
    pass "v not forced after a single attempt following a target switch"
fi

# ---- 5. entering stock never writes a stamp -------------------------------
bt_watchdog_clear
got=$(bt_watchdog_enter stock)
[ "$got" = "0" ] && pass "entering stock echoes 0" \
    || fail "entering stock echoed [$got], expected 0"
stamp="$(bt_stamp_file)"
if [ -f "$stamp" ]; then
    fail "entering stock created a boot-attempt stamp at $stamp"
else
    pass "entering stock creates no boot-attempt stamp"
fi

# ---- 6. stamp names a DIFFERENT id at count 2 -> not forced for this id ---
bt_watchdog_clear
bt_watchdog_enter v > /dev/null
bt_watchdog_enter v > /dev/null
if bt_watchdog_forced schwung; then
    fail "schwung reported forced by a stamp naming a different id (v)"
else
    pass "a stamp naming a different id does not force this id"
fi

# ---- 7. corrupted stamp with glob-metacharacters is survivable ------------
# `set -- $(cat stamp)` word-splits AND glob-expands the stamp's content, so
# a torn-write stamp reading "s* 9" with matching decoy filenames in cwd used
# to make bt_watchdog_forced return true for an unrelated id. Prove it does
# not depend on cwd content at all: run it from a directory stuffed with
# names an unquoted "s*" would expand to.
stamp="$(bt_stamp_file)"
glob_dir="$fixture/globdir"
mkdir -p "$glob_dir"
touch "$glob_dir/s1" "$glob_dir/s2" "$glob_dir/s3"
printf "s* 9\n" > "$stamp"
(
    cd "$glob_dir" || exit 2
    bt_watchdog_forced schwung
)
rc=$?
if [ "$rc" -ne 0 ]; then
    pass "corrupted stamp (glob metacharacters) does not force schwung"
else
    fail "corrupted stamp (glob metacharacters) forced schwung, rc=$rc"
fi

# ---- 8. corrupted stamp with a non-numeric count must not abort the shell -
# `count=$((count + 1))` aborts dash/BusyBox ash outright on a non-numeric
# operand. Source the library fresh under dash so an abort is visible as a
# nonzero exit / killed script rather than taking this test process with it.
printf "x 2x\n" > "$stamp"
out=$(dash -c '
    export BOOT_TARGETS_DIR="'"$BOOT_TARGETS_DIR"'"
    . src/host/boot_target_lib.sh
    bt_watchdog_enter schwung
' 2>&1)
rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "1" ]; then
    pass "non-numeric stamp count does not abort the sourcing shell"
else
    fail "non-numeric stamp count gave rc=$rc out=[$out], expected rc=0 out=1"
fi

# ---- 9. empty stamp file -> enter echoes 1 (treated as no prior attempt) --
: > "$stamp"
got=$(bt_watchdog_enter schwung)
[ "$got" = "1" ] && pass "empty stamp file: enter echoes 1" \
    || fail "empty stamp file: enter echoed [$got], expected 1"

# ---- 10. empty stamp under `set -u`: prev_id/prev_count must be BOUND -----
# The read-based fix must leave both vars set (possibly empty), never
# unbound, or a `set -u` sourcing script (this library's own real caller,
# shim-entrypoint.sh) would abort on the first reference to them.
: > "$stamp"
out=$(bash -uc '
    export BOOT_TARGETS_DIR="'"$BOOT_TARGETS_DIR"'"
    . src/host/boot_target_lib.sh
    bt_watchdog_enter schwung
' 2>&1)
rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "1" ]; then
    pass "empty stamp under bash -u: no unbound variable, enter echoes 1"
else
    fail "empty stamp under bash -u gave rc=$rc out=[$out], expected rc=0 out=1"
fi

if [ "$fails" -ne 0 ]; then
    echo "FAILED: $fails check(s) did not pass"
    exit 1
fi
echo "PASS: all boot watchdog checks passed"
