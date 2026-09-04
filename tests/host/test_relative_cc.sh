#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The ARITHMETIC is run by tests/host/test_relative_cc.c against relative_cc.h.
# This is the other half: chain_midi.c cannot be compiled natively (it dlopens
# plugins and owns the get_param/set_param surface), so the two things the C
# test cannot see are pinned at the source level here —
#
#   1. that the relative CC path still CALLS the header rather than carrying a
#      second copy of the decode, and
#   2. that the constants the C test mirrors still match the ones that ship.
#
# Without (2) the C test goes on asserting "no cliff at accel 4" long after
# KNOB_ACCEL_MAX_MULT has moved, which is a green test measuring a range the
# code no longer has.

fail() { echo "FAIL: $1"; exit 1; }

midi="src/modules/chain/dsp/chain_midi.c"
hdr="src/modules/chain/dsp/relative_cc.h"
internal="src/modules/chain/dsp/chain_internal.h"
ctest="tests/host/test_relative_cc.c"

[ -f "$hdr" ] || fail "$hdr is missing"

# ------------------------------------------------------- 1. the call site
grep -q '#include "relative_cc.h"' "$midi" \
  || fail "chain_midi.c no longer includes relative_cc.h"
grep -q 'relative_cc_ticks(' "$midi" \
  || fail "the relative CC path no longer decodes through relative_cc_ticks()"
# The scaling moved into knob_turn (chain_params.c) when a knob became able to
# drive several destinations, so chain_midi.c decodes and delegates. Both ends
# are still required: a decode that reached a hand-rolled scale, or a delegation
# that lost the decode, would each pass a check on only one of them.
params=src/modules/chain/dsp/chain_params.c
grep -q 'knob_turn(inst, i, ticks,' "$midi" \
  || fail "the relative CC path no longer hands its decoded detents to knob_turn()"
grep -q 'relative_cc_multiplier(' "$params" \
  || fail "knob_turn no longer scales through relative_cc_multiplier()"
grep -q '#include "relative_cc.h"' "$params" \
  || fail "chain_params.c no longer includes relative_cc.h"

# A SECOND decode is the failure mode worth naming: the original bug was a
# hand-rolled two-branch decode inline here, and the fix is only a fix while
# there is exactly one of them. `msg[2] == 127` was its signature.
if grep -q 'msg\[2\] == 127' "$midi"; then
    fail "chain_midi.c decodes a relative CC value inline again (msg[2] == 127) — \
the decode belongs to relative_cc.h, and two copies is how the first one drifted"
fi

# The magnitude must not be pinned before it reaches the multiplier: that was
# the enum regression (one option per MESSAGE rather than per detent).
if grep -qE 'KNOB_TYPE_ENUM\)[[:space:]]*mag = 1' "$midi"; then
    fail "the enum path pins the detent count to 1 again — an accelerated \
encoder answers a faster spin with a bigger message, not more of them, so this \
makes a long enum uncrossable at any speed"
fi

# ------------------------------------------------------- 2. the constants
# Read each one out of the shipped header and require the C test's mirror to
# agree, so a change to either side fails here rather than going quiet.
for name in KNOB_ACCEL_MIN_MULT KNOB_ACCEL_MAX_MULT KNOB_ACCEL_MAX_MULT_INT KNOB_ACCEL_ENUM_MULT; do
    shipped="$(awk -v n="$name" '$1 == "#define" && $2 == n { print $3; exit }' "$internal")"
    mirror="$(awk -v n="$name" '$1 == "#define" && $2 == n { print $3; exit }' "$ctest")"
    [ -n "$shipped" ] || fail "$name is gone from chain_internal.h"
    [ -n "$mirror" ]  || fail "$name is no longer mirrored in test_relative_cc.c"
    [ "$shipped" = "$mirror" ] \
      || fail "$name drifted: chain_internal.h says $shipped, test_relative_cc.c mirrors $mirror"
done

echo "PASS: relative CC wiring + constant mirrors"
