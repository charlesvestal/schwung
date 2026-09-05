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

if [ "$fails" -ne 0 ]; then
    echo "FAILED: $fails check(s) did not pass"
    exit 1
fi
echo "PASS: all boot watchdog checks passed"
