#!/usr/bin/env bash
# Pins the clip-phase seam: it is a dlsym'd chain export, NOT a host_api field,
# and it is pushed ahead of the idle gate.
#
# WHAT IS LEFT HERE, AND WHY IT IS ONLY THIS.
#
# This file used to carry seven assertions, five of which read stronger than
# they were: `if (shadow_chain_set_clip_phase)` grepped for ANYWHERE in a
# 9000-line file (so a guard left in a comment passed), `clip_phase=%p` ditto,
# and three that were line-number arithmetic, which cannot see a conditional
# wrapped around the line it is counting. This project has paid for that shape
# before: a grep pin is blind inside a comment and inside a regex literal, and
# 316 green tests once missed a dead UI door.
#
# The behaviour they were reaching for is now DRIVEN, in
# tests/host/test_slot_clip_phase.c -- identity surviving a missing anchor, the
# loop_start rebasing, the bounds, the NaN. What stays here is the set of facts
# that only the source can express: the host ABI is untouched, the seam is a
# dlsym rather than a field, a failed resolution is logged, the two length
# guards agree, and the push sits ahead of the idle gate.
set -euo pipefail
cd "$(dirname "$0")/../.."
fail() { echo "FAIL: $1"; exit 1; }

shim=src/schwung_shim.c
mgmt=src/host/shadow_chain_mgmt.c

# 1. The host ABI is untouched. The front of `reserved` IS +120 -- the offset a
#    shipped breakbeat build calls as get_project_bpm() -- so consuming it puts
#    a live pointer there and boot-loops the device.
#
#    That is now enforced where it belongs, by a _Static_assert on
#    offsetof(host_api_v1_t, reserved) in the header itself, plus a runtime
#    CHECK in test_host_api_reserved_tail.c. Both were verified to FIRE against
#    a field inserted before `reserved`; the old grep for the declaration text
#    did not, because the declaration is unchanged by an insertion in front of
#    it. So what is pinned here is that the enforcement still EXISTS -- deleting
#    the assert is the one move those two guards cannot catch.
grep -q '_Static_assert(offsetof(host_api_v1_t, reserved) == 120' \
  src/host/plugin_api_v1.h \
  || fail "the +120 static assert is gone from plugin_api_v1.h — nothing now \
stops a field being inserted before \`reserved\`, which puts a live pointer at \
the offset breakbeat over-reads and boot-loops the device"
# ...and it did not arrive as a FIELD. Matched as a declaration rather than as
# the bare word: the header's own prose now names chain_set_clip_phase, as the
# example of the dlsym'd route to take instead, and a word-match on that reads
# the advice as the violation.
if grep -qE 'clip_phase\)\(|clip_phase;' src/host/plugin_api_v1.h; then
  fail "clip phase leaked into host_api_v1_t as a field"
fi

# 2. The phase seam is a dlsym, like chain_take_midi_tick_wake and
#    move_plugin_render_split, and NOT a host_api field.
grep -q 'dlsym(shadow_dsp_handle, "chain_set_clip_phase")' "$mgmt" \
  || fail "chain_set_clip_phase is not dlsym'd"

# 3. A failed resolution is visible, and the log call is REACHED. An optional
#    export that resolves to NULL and says nothing is a feature that silently
#    does not exist.
#
#    Checked inside the loader's body rather than file-wide: the old grep for
#    `clip_phase=%p` matched the string wherever it sat, including a commented
#    or unreachable call.
loader=$(awk '/^int shadow_inprocess_load_chain\(/,/^\}/' "$mgmt")
[ -n "$loader" ] || fail "could not find shadow_inprocess_load_chain in $mgmt"
printf '%s\n' "$loader" | grep -qE '^[[:space:]]*unified_log\(.*' \
  || fail "shadow_inprocess_load_chain logs nothing at all"
printf '%s\n' "$loader" | grep -q 'clip_phase=%p' \
  || fail "a failed chain_set_clip_phase dlsym is not logged from \
shadow_inprocess_load_chain"

# 4. THE TWO LENGTH GUARDS AGREE ON SPELLING. `loop_len <= 0.0` is FALSE for a
#    NaN and `!(loop_len > 0.0)` is TRUE, so only the second form catches a torn
#    read of the regions table. clip_phase_beats() uses the second; the resolver
#    must too, or the pair is defended in one place and the backstop is one
#    edit away from being the only guard.
#
#    Behaviour cannot see this -- the two guards are redundant today, and
#    reverting the resolver's to the NaN-blind form leaves all 95 checks in
#    test_slot_clip_phase.c green (measured). Hence a source pin, and hence it
#    reads the resolver's BODY rather than the file.
resolver=$(awk '/^int shadow_slot_clip_phase\(/,/^\}/' "$mgmt")
[ -n "$resolver" ] || fail "could not find shadow_slot_clip_phase in $mgmt"
printf '%s\n' "$resolver" | grep -q '!(r->loop_len > 0.0)' \
  || fail "shadow_slot_clip_phase does not spell its loop-length guard \
!(r->loop_len > 0.0) — the <= 0.0 form is FALSE for a NaN, so a torn regions \
read would reach clip_phase_beats() as a live length"
if printf '%s\n' "$resolver" | grep -qE 'loop_len[[:space:]]*<=[[:space:]]*0'; then
  fail "shadow_slot_clip_phase still carries a NaN-blind \`loop_len <= 0\` guard"
fi

# 5. THE PUSH PRECEDES THE IDLE GATE, or a silent slot's lane freezes: the shim
#    skips render_block on a silent slot for 171 frames in 172.
#
#    Line arithmetic is the only tool available -- schwung_shim.c cannot be
#    compiled natively -- so it is tightened rather than trusted:
#
#      (a) the guard must sit within a few lines ABOVE the push, not merely
#          somewhere in the file (the old form passed with the guard deleted and
#          left in a comment, or guarding something else entirely);
#      (b) the push must not be nested inside a shadow_slot_idle conditional,
#          which is the mutation that keeps the line number and reinstates the
#          freeze.
#
#    What it still cannot see: a different conditional wrapped around the push
#    (a `if (some_other_flag)`), the push being moved into a function that is
#    itself called after the gate, or the gate being restructured so that
#    `shadow_slot_idle[s]` no longer appears literally. Those need the shim on a
#    host compiler, which is a much larger job than this seam.
# Every derivation below ends in `|| true`: grep exits non-zero on no match, and
# under `set -o pipefail` that aborts the whole script at the ASSIGNMENT -- exit
# 1 with no diagnostic, which tells a reader nothing about which guard tripped.
# (Measured: two of the mutation checks for this file failed exactly that way
# before this line existed.) The emptiness checks after each one are what should
# speak.
push=$(grep -n 'shadow_chain_set_clip_phase(shadow_chain_slots' "$shim" | head -1 | cut -d: -f1 || true)
[ -n "$push" ] || fail "could not find the clip-phase push in $shim"
gate=$(grep -n 'if (shadow_slot_idle\[s\]) {' "$shim" | head -1 | cut -d: -f1 || true)
[ -n "$gate" ] || fail "could not find the idle gate in $shim"
[ "$push" -lt "$gate" ] \
  || fail "clip phase is pushed after the idle gate (push=$push gate=$gate) — a \
silent slot's lane would freeze and then jump"

# (a) The NULL-dlsym guard, immediately above the push. A NULL must degrade to
#     "phase unknown" for every slot, the way every other optional chain export
#     does -- and the guard has to be the one guarding THIS call.
guard=$(grep -n '^[[:space:]]*if (shadow_chain_set_clip_phase) {' "$shim" | head -1 | cut -d: -f1 || true)
[ -n "$guard" ] || fail "the clip-phase push is not guarded on a successful \
dlsym (no \`if (shadow_chain_set_clip_phase) {\` at statement position in $shim)"
[ "$guard" -lt "$push" ] && [ $((push - guard)) -le 12 ] \
  || fail "the \`if (shadow_chain_set_clip_phase)\` guard is at line $guard but \
the push is at $push — too far apart to be guarding it. A chain DSP built before \
lanes resolves NULL here, and an unguarded call is a null deref on the SPI \
callback."

# (b) Nothing between the guard and the push may test the idle flag: that is the
#     nesting that keeps the line ordering and freezes the lane anyway.
if sed -n "${guard},${push}p" "$shim" | grep -q 'shadow_slot_idle'; then
  fail "the clip-phase push is nested inside a shadow_slot_idle conditional — \
it is ahead of the gate by line number and behind it in effect, which is the \
exact freeze this ordering exists to prevent"
fi

echo "PASS: lane phase seam"
