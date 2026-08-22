#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Shift is fine adjust EVERYWHERE, including the chain editor's knob overlay.
#
# Reported from the device: "shift doesn't work on an overlay knob from the
# chain editor". The gesture existed only on the param-pages grid, which passes
# `fine` into the knob model -- the overlay's four call sites never did, so the
# same knob refined under shift on one screen and ignored it on the other.
#
# That was invisible while there were two knob engines, because the overlay's
# engine had no fine parameter to pass. Unifying them made the omission a
# missing argument rather than a missing feature.

fail() { echo "FAIL: $1" >&2; exit 1; }
file="src/shadow/shadow_ui.js"

# Every knobStep call in the overlay must decide about shift, one way or the
# other. A bare four-argument call is the bug.
bare=$(command grep -n "knobStep(st, [^)]*Date.now())" "$file" || true)
if [ -n "$bare" ]; then
  echo "FAIL: a knob turn in the overlay does not pass a fine flag:" >&2
  echo "$bare" >&2
  exit 1
fi

for want in "isShiftHeld()"; do
  n=$(command grep -c "knobStep(.*$want" "$file" || true)
  [ "$n" -ge 4 ] || fail "only $n overlay knobStep call(s) consult shift; expected every one"
done

# wav_position is the exception and must STAY one: it folds shift into its own
# step multiplier (getWavPositionShiftMultiplier), so also passing `fine` would
# apply the gesture twice -- a shift turn would be 10x finer than intended on
# top of the module's own multiplier.
command grep -q "!wavPos && isShiftHeld()" "$file" || \
  fail "wav_position no longer opts out of the engine's fine mode -- shift would apply twice"
command grep -q "ui_type === \"wav_position\"" "$file" || \
  fail "the wav_position opt-out lost its test"

echo "  ok  every overlay knob passes shift into the knob model"
echo "  ok  wav_position still opts out, so its own shift multiplier is not doubled"
echo "PASS: shift is fine adjust in the chain editor overlay too"
