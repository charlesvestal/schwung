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
for f in src/modules/chain/dsp/chain_midi.c src/modules/chain/dsp/chain_host.c; do
  command grep -q 'chain_knob_accel(&inst->knob_last_time_ms\[i\])' "$f" \
    || fail "$f does not use chain_knob_accel"
  # Spelled as an if rather than `grep && fail`: in that form a grep that
  # finds nothing is the list's exit status, and whether set -e acts on it is
  # subtle enough that a later reader could "tidy" it either way.
  if command grep -q 'KNOB_ACCEL_SLOW_MS' "$f"; then
    fail "$f still hand-rolls the acceleration curve"
  fi
done

# ...and each still applies the type cap, which is the caller's job because
# only the caller knows the parameter's type.
for f in src/modules/chain/dsp/chain_midi.c src/modules/chain/dsp/chain_host.c; do
  command grep -q 'chain_knob_accel_cap(accel, pinfo->type)' "$f" \
    || fail "$f does not apply the type cap"
done

echo "PASS: one acceleration curve, asked for by every knob path"
