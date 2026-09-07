#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

#   RUN   the real acceleration curve (tests/host/test_chain_knob_accel.c).
#
#   PIN   that the three knob paths ask for it rather than carrying their own
#         copy. Two copies had already drifted apart unnoticed; neither TU can
#         be compiled natively, so this half is a source pin.

fail() { echo "FAIL: $1"; exit 1; }

bin="build/tests/test_chain_knob_accel"
mkdir -p "$(dirname "$bin")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
printf '#include <stdlib.h>\n' > "$work/malloc.h"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_chain_knob_accel.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  -o "$bin"

"$bin"

# ------------------------------------------------------------------ pin half

# Nobody hand-rolls the curve any more. KNOB_ACCEL_SLOW_MS appearing outside
# the header and chain_params.c means a fourth copy has grown back.
P=src/modules/chain/dsp/chain_params.c
command grep -q 'chain_knob_accel(&inst->knob_last_time_ms\[idx\])' "$P" \
  || fail "knob_turn does not use chain_knob_accel"
command grep -q 'chain_knob_accel_cap(' "$P" \
  || fail "knob_turn does not apply the type cap for a single destination"

# The two TUs that cannot be compiled natively must hold NO knob arithmetic at
# all -- they decode an input and delegate. That is a stronger claim than "they
# call the shared helper", and it is the one worth pinning, because the whole
# reason this moved is that nothing can run what lives in those files.
for f in src/modules/chain/dsp/chain_midi.c src/modules/chain/dsp/chain_host.c; do
  command grep -q 'knob_turn(inst, i,' "$f" \
    || fail "$f does not delegate its knob turn to knob_turn()"
  # Spelled as an if rather than `grep && fail`: in that form a grep that
  # finds nothing is the list's exit status, and whether set -e acts on it is
  # subtle enough that a later reader could "tidy" it either way.
  for token in KNOB_ACCEL_SLOW_MS chain_knob_accel_cap KNOB_STEP_INT; do
    if command grep -q "$token" "$f"; then
      fail "$f still carries knob-turn arithmetic ($token); it should decode and delegate"
    fi
  done
done

# knob_N_adjust hands knob_turn ONE detent per message, deliberately.
#
# handleKnobTurn in shadow_ui.js sums every detent for a knob between frames,
# so its `val` is an accumulated COUNT and the sign discards a fast turn.
# Honouring the count would make the device's own encoders travel further for
# the same gesture -- a feel change on every existing chain knob, which this
# work must not make. Pinned in both directions so neither drifts back.
H=src/modules/chain/dsp/chain_host.c
command grep -q 'knob_turn(inst, i, (delta_int > 0) ? 1 : -1,' "$H" \
  || fail "knob_N_adjust no longer passes ONE detent per message; upstream knob feel would change"
if command grep -q 'knob_turn(inst, i, delta_int,' "$H"; then
  fail "knob_N_adjust passes its accumulated detent count, which changes the feel of every chain knob"
fi

echo "PASS: one acceleration curve, asked for by every knob path"
