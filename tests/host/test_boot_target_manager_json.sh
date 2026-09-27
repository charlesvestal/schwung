#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/../.."

# The boot.json Schwung Manager writes must be readable by the SELECTOR's own
# parser, not merely by Go.
#
# bt_json_field is an awk text matcher, not a JSON parser, and what it can and
# cannot read was MEASURED rather than assumed (2026-09-09). Two rules matter,
# and neither is the "one field per line" the writer happens to produce:
#
#   1. VALUES MUST BE QUOTED STRINGS. `"version": 3` reads back as EMPTY --
#      the regex requires a quote after the colon. An unquoted number is not a
#      parse error anywhere; the field is simply gone.
#   2. THE FIRST TEXTUAL OCCURRENCE WINS, so a nested object carrying the same
#      key SHADOWS the real one: in `{"boot_target": {"name": "V"}, "name":
#      "Outer"}` the reader answers "V".
#
# A value carrying a double quote is truncated at it (`V "x"` -> `V \`), which
# is why validateBootTargetName refuses one rather than escaping it.
#
# On the device none of this looks like a parse failure. It looks like a target
# that is registered and simply does not appear in the picker, with nothing
# logged. Go's TestBootRegistryGoldenMatchesWriter proves the writer still
# produces this golden; only this test proves the golden is something the
# selector can read.

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
pass() { echo "PASS: $*"; }

golden="schwung-manager/testdata/boot.json.golden"
[ -f "$golden" ] || { echo "FAIL: golden missing: $golden"; exit 1; }

fixture=$(mktemp -d)
cleanup() { rm -rf "$fixture"; }
trap cleanup EXIT

mkdir -p "$fixture/v"
cp "$golden" "$fixture/v/boot.json"

export BOOT_TARGETS_DIR="$fixture"
# shellcheck disable=SC1091
. src/host/boot_target_lib.sh

# ---- 1. the selector recognises it as a registered target ---------------
if bt_is_registered v; then
    pass "bt_is_registered accepts the manager-written entry"
else
    fail "bt_is_registered rejects the manager-written entry"
fi

# ---- 2. every field the selector reads comes back whole -----------------
check_field() {
    key="$1"; want="$2"
    got=$(bt_json_field "$fixture/v/boot.json" "$key")
    if [ "$got" = "$want" ]; then
        pass "bt_json_field reads $key as [$want]"
    else
        fail "bt_json_field read $key as [$got], expected [$want]"
    fi
}
check_field name    "V"
check_field version "0.3.0"
check_field owner   "platform:v"

# ---- 3. exec resolves through the selector's own accessor ---------------
got=$(bt_exec_path v)
want="/data/UserData/platforms/v/entry.sh"
[ "$got" = "$want" ] && pass "bt_exec_path reads exec as [$want]" \
    || fail "bt_exec_path read exec as [$got], expected [$want]"

# ---- 4. and it can be booted as the default ----------------------------
echo "v" > "$fixture/default"
got=$(bt_resolve_default)
[ "$got" = "v" ] && pass "bt_resolve_default selects the manager-written target" \
    || fail "bt_resolve_default returned [$got], expected v"

# ---- 5. the two shapes that would silently blank a field ----------------
# These are the mutations that matter: both are things a future writer could
# plausibly emit, and both read as "the target vanished" rather than an error.
probe=$(mktemp -d)
printf '{\n  "name": "V",\n  "version": 3\n}\n' > "$probe/boot.json"
got=$(bt_json_field "$probe/boot.json" version)
[ -z "$got" ] && pass "an UNQUOTED value reads back empty (so the writer must quote everything)" \
    || fail "expected an unquoted value to be unreadable, got [$got]"

printf '{\n  "boot_target": {\n    "name": "Inner"\n  },\n  "name": "V"\n}\n' > "$probe/boot.json"
got=$(bt_json_field "$probe/boot.json" name)
[ "$got" = "Inner" ] && pass "a NESTED key shadows the real one (so the writer must stay flat)" \
    || fail "expected the nested key to shadow, got [$got]"
rm -rf "$probe"

if [ "$fails" -eq 0 ]; then
    echo "OK: manager-written boot.json is readable by the selector"
    exit 0
fi
echo "$fails check(s) failed"
exit 1
