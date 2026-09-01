#!/usr/bin/env bash
# Shift+Copy / Shift+Delete: the shim half of the snapshot gesture.
#
# Source-invariant pins, because this code cannot be run off the device — it
# lives inside the SPI ioctl interception. What can be checked here is the shape
# of the branch, and the shape is where this goes wrong:
#
#   * without the Shift guard the gesture fires on a bare Copy, which is a
#     button people press
#   * without zeroing the slot, Move ALSO acts on the press — Shift+Delete
#     reaching Move's own delete while Schwung restores a snapshot is a
#     destructive coincidence, not a cosmetic one
#   * the shim must not do the WORK; ~20 param round trips on the SPI callback
#     is a dropout, so nothing here may read or write params
set -euo pipefail
cd "$(dirname "$0")/../.."

SHIM=src/schwung_shim.c
fails=0
note() { echo "FAIL: $1"; fails=$((fails+1)); }

# The block, isolated: the gesture's own comment marker down to the start of
# the next unrelated shortcut. Anchored on both ends by text that belongs to
# something else, so the range cannot silently shrink to exclude half of what
# is being checked here (it did, when it started at the first flag mention —
# the CC_COPY test sits above that line).
block=$(sed -n '/Snapshot \/ recall: Shift+Copy takes/,/Shift+Vol+Left\/Right: set page navigation/p' "$SHIM")
[ -n "$block" ] || { echo "FAIL: snapshot gesture block not found in $SHIM"; exit 1; }

# The guard line that both branches sit under.
guard=$(grep -n "d2 > 0 && shadow_shift_held && shadow_ui_enabled && shadow_control" "$SHIM" || true)
[ -n "$guard" ] || note "gesture is not gated on (d2 > 0 && shadow_shift_held && shadow_ui_enabled && shadow_control)"

# Both CCs are claimed, and only inside that guard.
echo "$block" | grep -q "d1 == CC_COPY"   || note "CC_COPY (60) not claimed"
echo "$block" | grep -q "d1 == CC_DELETE" || note "CC_DELETE (119) not claimed"

# Each branch zeroes its slot. Two zeroing lines, one per branch — a single
# one would mean one gesture leaks its button to Move.
zeros=$(echo "$block" | grep -c "src\[j\] = 0; src\[j+1\] = 0; src\[j+2\] = 0; src\[j+3\] = 0;" || true)
[ "$zeros" -eq 2 ] || note "expected 2 slot-zeroing lines in the gesture block, found $zeros"

# The shim raises a flag and does nothing else. Any param traffic here is on
# the SPI callback.
for forbidden in shadow_get_param shadow_set_param host_read_file host_write_file fopen; do
    if echo "$block" | grep -q "$forbidden"; then
        note "gesture block calls $forbidden — this is the SPI callback"
    fi
done

# The flags are written to ui_flags_ext, shifted. Writing them to ui_flags
# truncates to zero and the gesture silently never fires — see
# test_ui_flags_layout.c.
echo "$block" | grep -q "ui_flags_ext |=" || note "flags not written to ui_flags_ext"
echo "$block" | grep -q "SHADOW_UI_FLAG_EXT_SHIFT" || note "flags not shifted into the ext field"
if echo "$block" | grep -q "ui_flags |="; then
    note "gesture writes ui_flags (8-bit) — a 0x0100 flag truncates to 0 there"
fi

# The JS side must actually service both flags, or the shim raises them forever.
for f in SHADOW_UI_FLAG_SNAPSHOT_TAKE SHADOW_UI_FLAG_SNAPSHOT_RECALL; do
    grep -q "$f" src/shadow/shadow_ui.js || note "shadow_ui.js never handles $f"
done
grep -q "SHADOW_UI_FLAG_SNAPSHOT_TAKE | SHADOW_UI_FLAG_SNAPSHOT_RECALL" src/shadow/shadow_ui.js \
    || grep -q "shadow_clear_ui_flags(SHADOW_UI_FLAG_SNAPSHOT" src/shadow/shadow_ui.js \
    || note "shadow_ui.js never clears the snapshot flags — the gesture would repeat every tick"

if [ "$fails" -ne 0 ]; then echo "$fails check(s) failed"; exit 1; fi
echo "PASS test_snapshot_gesture"
