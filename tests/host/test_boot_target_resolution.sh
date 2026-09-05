#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/../.."

# boot_target_lib.sh resolves which boot target to launch (schwung, stock
# MoveOriginal, or a third-party registry entry) from a flat file registry
# under BOOT_TARGETS_DIR. This test points that variable at a throwaway
# fixture directory so the real /data/UserData/boot-targets is never touched,
# and exercises bt_resolve_default and bt_exec_path against it.

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

# ---- 1. no default file -> schwung (schwung is registered) --------------
rm -f "$fixture/default"
got=$(bt_resolve_default)
[ "$got" = "schwung" ] && pass "no default file resolves to schwung" \
    || fail "no default file resolved to [$got], expected schwung"

# ---- 2. default=v -> v (a registered non-schwung target wins) -----------
echo "v" > "$fixture/default"
got=$(bt_resolve_default)
[ "$got" = "v" ] && pass "default=v resolves to v" \
    || fail "default=v resolved to [$got], expected v"

# ---- 3. default=ghost (dangling, not registered) -> schwung --------------
echo "ghost" > "$fixture/default"
got=$(bt_resolve_default)
[ "$got" = "schwung" ] && pass "dangling default falls back to schwung" \
    || fail "dangling default resolved to [$got], expected schwung"

# ---- 4. schwung removed, default still dangling -> stock -----------------
rm -rf "$fixture/schwung"
got=$(bt_resolve_default)
[ "$got" = "stock" ] && pass "no schwung registration falls back to stock" \
    || fail "with schwung unregistered, resolved to [$got], expected stock"

# recreate schwung for the remaining cases
mkdir -p "$fixture/schwung"
cat > "$fixture/schwung/boot.json" <<'EOF'
{
  "name" : "Schwung",
  "exec" : "/data/UserData/schwung/schwung"
}
EOF

# ---- 5. default=stock -> stock, even though schwung IS registered --------
echo "stock" > "$fixture/default"
got=$(bt_resolve_default)
[ "$got" = "stock" ] && pass "default=stock resolves to stock" \
    || fail "default=stock resolved to [$got], expected stock"

# ---- 6. bt_exec_path v -> the exec field from v/boot.json ----------------
got=$(bt_exec_path v)
if [ "$got" = "/data/UserData/boot-targets/v/run.sh" ]; then
    pass "bt_exec_path v returns v exec path"
else
    fail "bt_exec_path v returned [$got]"
fi

# ---- 7. bt_exec_path stock -> rc 1, nothing echoed -----------------------
got=$(bt_exec_path stock)
rc=$?
if [ "$rc" -ne 0 ] && [ -z "$got" ]; then
    pass "bt_exec_path stock fails with no output"
else
    fail "bt_exec_path stock returned rc=$rc out=[$got], expected rc!=0 and empty"
fi

# ---- 8. bt_exec_path ghost -> rc 1, nothing echoed -----------------------
got=$(bt_exec_path ghost)
rc=$?
if [ "$rc" -ne 0 ] && [ -z "$got" ]; then
    pass "bt_exec_path ghost fails with no output"
else
    fail "bt_exec_path ghost returned rc=$rc out=[$got], expected rc!=0 and empty"
fi

if [ "$fails" -ne 0 ]; then
    echo "FAILED: $fails check(s) did not pass"
    exit 1
fi
echo "PASS: all boot target resolution checks passed"
