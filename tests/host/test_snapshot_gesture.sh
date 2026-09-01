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

# The gesture FIRES only on a press, with Shift, while Schwung's UI is enabled.
# (The swallowing is separate and covers both edges — see the latch below.)
echo "$block" | grep -q "if (d2 > 0) {" \
  || note "the gesture is not gated on a key press"
echo "$block" | grep -q "shadow_shift_held && shadow_ui_enabled && shadow_control" \
  || note "the gesture is not gated on Shift + shadow_ui_enabled"

# Both CCs are claimed, and only inside that guard.
echo "$block" | grep -q "d1 == CC_COPY"   || note "CC_COPY (60) not claimed"
echo "$block" | grep -q "d1 == CC_DELETE" || note "CC_DELETE (119) not claimed"

# BOTH EDGES are swallowed, via a latch.
#
# This is the assertion that was missing, and it cost a deleted clip on
# hardware. Blocking only the press (d2 > 0) left Move a lone BUTTON-UP for a
# key it never saw go down, and Move acted on it. Every press in the device log
# had been zeroed correctly; the release was the entire leak.
#
# The latch is load-bearing and cannot be replaced by testing shadow_shift_held
# on the release: Shift is usually let go BEFORE the button, so the release
# arrives with shift already down and would sail through.
echo "$block" | grep -q "snapshot_gesture_swallow\[gi\] = 1;" \
  || note "the press does not latch — the release will reach Move"
echo "$block" | grep -q "if (snapshot_gesture_swallow\[gi\]) {" \
  || note "the zeroing is not driven by the latch, so it cannot cover the release"

# BOTH BUFFERS, and this is the one that actually mattered.
#
# `src` is hardware_mmap_addr, the real mailbox — MOVE DOES NOT READ IT. What
# Move reads is `shadow`, the library shadow buffer (see the declarations at
# the top of schwung_shim.c). Zeroing only `src` blocks nothing whatsoever,
# which is how Shift+Delete reached Move and deleted a clip. The press had
# never been blocked; the release was a second, smaller bug on top.
#
# Index-paired and safe only because shadow_midi_in_compact runs LAST in
# shim_post_transfer — nothing may move a slot between the two writes.
echo "$block" | grep -q "midi_in_swallow(shadow + MIDI_IN_OFFSET, src, j);" \
  || note "the gesture does not go through midi_in_swallow — it will silence only one buffer"
echo "$block" | grep -qE "^\s*src\[j\] = 0;" \
  && note "the gesture zeroes the hardware mailbox by hand — Move reads the SHADOW buffer"
echo "$block" | grep -q "if (d2 == 0) snapshot_gesture_swallow\[gi\] = 0;" \
  || note "the latch is never cleared — Copy/Delete would be dead to Move forever"
grep -q "static int snapshot_gesture_swallow\[2\] = {0, 0};" "$SHIM" \
  || note "no snapshot_gesture_swallow latch"

# The zeroing must NOT sit inside the d2 > 0 arm — that is exactly the shape
# that leaked. It belongs under the latch test, which sees both edges.
press_arm=$(echo "$block" | awk '/if \(d2 > 0\) \{/,/^                    \}$/')
echo "$press_arm" | grep -q "src\[j\] = 0" \
  && note "the slot is zeroed inside the press arm — the release leaks (the deleted-clip bug)"

# A press we did not consume must leave the latch alone, or Move loses its own
# bare Copy and Delete entirely.
echo "$block" | grep -q "if (shadow_shift_held && shadow_ui_enabled && shadow_control) {" \
  || note "the latch is set without checking Shift — Move's own Copy/Delete would be swallowed"

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

# --- the queue ------------------------------------------------------------
#
# Shift+Delete keeps ONE meaning; whether it fires now or at the next boundary
# is a setting. The shim owns the decision because it owns the MIDI clock.
gest=$(awk '/^static uint16_t snapshot_recall_gesture/,/^}/' "$SHIM")
[ -n "$gest" ] || note "snapshot_recall_gesture missing"

# A queue with no clock never fires, so with the transport stopped the setting
# must be IGNORED rather than honoured into silence — otherwise the button goes
# dead with nothing on screen to say why.
echo "$gest" | grep -q "!sampler_transport_playing" \
  || note "a queued recall is armed with the transport stopped — it would never fire"

# Pressed again while armed, it cancels. Re-arming to the same boundary is a
# no-op indistinguishable from a dead button, and there is no other way back.
echo "$gest" | grep -q "SHADOW_UI_FLAG_SNAPSHOT_UNQUEUED" \
  || note "a second press while armed does not cancel"

bnd=$(awk '/^static void snapshot_recall_check_boundary/,/^}/' "$SHIM")
[ -n "$bnd" ] || note "snapshot_recall_check_boundary missing"
echo "$bnd" | grep -q "recall_should_fire" || note "the boundary test is hand-rolled, not the tested helper"
echo "$bnd" | grep -q "recall_lead_pulses" || note "no lead compensation — the change lands ~36ms late"
# The clock can stop under an armed recall. Dropping it is the only way the
# on-screen mark ever clears.
echo "$bnd" | grep -q "!sampler_transport_playing" \
  || note "an armed recall survives the transport stopping — the mark never clears"
grep -q "snapshot_recall_check_boundary();" "$SHIM" \
  || note "the boundary check is never called"

# The division comes from ui_flags_ext every time, not a cached copy:
# load_feature_config runs ONCE at init, so a cached setting would not take
# effect until the next reboot.
q=$(awk '/^static int recall_quantize_pulses\(void\)/,/^}/' "$SHIM")
echo "$q" | grep -q "shadow_control->recall_quantize" \
  || note "the quantize setting is not read from shadow_control->recall_quantize"
# It must be a FIELD, not bits packed into ui_flags_ext. It was the latter, on
# the belief that shadow_control_t was full — it was not, the buffer was merely
# sized to the struct with an == assert, and /dev/shm allocates a page either
# way. The packing cost a mask, a shift and one real bug.
echo "$q" | grep -q "ui_flags_ext" \
  && note "the quantize setting is packed into ui_flags_ext — give it a field"
grep -q "SHADOW_UI_RECALL_Q_MASK" "$SHIM" src/host/shadow_constants.h \
  && note "the packed-register defines are back"

# The free-running pulse counter, which is NOT sampler_clock_count (that one
# only advances while the sampler is recording and resets when it starts).
echo "$bnd$gest" | grep -q "sampler_clock_count" \
  && note "the queue uses sampler_clock_count, which is 0 unless the sampler is recording"
grep -q "shadow_transport_pulses" "$SHIM" || note "the queue does not use the free-running pulse count"

# The JS side must actually service both flags, or the shim raises them forever.
for f in SHADOW_UI_FLAG_SNAPSHOT_TAKE SHADOW_UI_FLAG_SNAPSHOT_RECALL; do
    grep -q "$f" src/shadow/shadow_ui.js || note "shadow_ui.js never handles $f"
done
grep -q "shadow_clear_ui_flags(SNAPSHOT_FLAGS)" src/shadow/shadow_ui.js \
    || note "shadow_ui.js never clears the snapshot flags — the gesture would repeat every tick"
# The clear must NOT take the quantize register with it: that is a setting
# living in the same field, not an event, and clearing it would silently reset
# Recall Quantize to Off the first time any snapshot gesture fired.
svc=$(awk '/^function snapshotServiceFlags/,/^}/' src/shadow/shadow_ui.js)
echo "$svc" | grep -q "RECALL_Q" \
  && note "the flag clear touches the quantize register — the setting would reset itself"
for f in SNAPSHOT_TAKE SNAPSHOT_RECALL SNAPSHOT_QUEUED SNAPSHOT_UNQUEUED; do
  echo "$svc" | grep -q "$f" || note "snapshotServiceFlags ignores SHADOW_UI_FLAG_$f"
done

if [ "$fails" -ne 0 ]; then echo "$fails check(s) failed"; exit 1; fi
echo "PASS test_snapshot_gesture"
