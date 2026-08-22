#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# There must be exactly ONE knob model.
#
# There were two: a time-based divisor curve in shared/knob_engine.mjs
# stepping by the module's declared step, and the ported Movy model in
# param_pages/movy_knob.mjs normalising to a fraction of the param's range.
# A knob therefore behaved differently depending on which screen you touched
# it from -- 79,917 detents to cross a 20..20000 param from the overlay
# against 200 on the knob grid -- and shift-fine was honoured in one for
# floats only and could not move an int at all in the other.
#
# The behaviour is pinned in tests/shadow/test_knob_engine.sh. What is pinned
# HERE is the structural half, because that is the half that decays: nothing
# stops a future call site from growing its own stepping arithmetic, and the
# two models did not start out as a deliberate decision either.

fail() { echo "FAIL: $1" >&2; exit 1; }

# 1. The second model file must stay deleted.
[ -e src/shared/param_pages/movy_knob.mjs ] && \
  fail "src/shared/param_pages/movy_knob.mjs is back -- the model lives in shared/knob_engine.mjs"

# 2. One entry point. The config-shaped pair the old call sites used is gone,
#    not aliased: an alias is a second name for the next reader to wire
#    something up to, which is how the two models drifted apart.
for gone in knobTick knobConfigFromMeta movyKnobTick movyKnobInit; do
  if command grep -q "export function $gone\|export { .*$gone" src/shared/knob_engine.mjs; then
    fail "knob_engine still exports \`$gone\` -- one model means one entry point"
  fi
done
command grep -q "export function knobStep" src/shared/knob_engine.mjs || \
  fail "knob_engine does not export knobStep"

# 3. Every caller reaches it through that entry point. A file that turns a
#    knob and does NOT import the engine is either doing its own arithmetic
#    or has grown a third path.
for f in src/shadow/shadow_ui.js src/shadow/shadow_ui_patches.mjs \
         src/shared/param_pages/page_controller.mjs; do
  command grep -q "knobStep" "$f" || fail "$f does not use knobStep"
  command grep -q "knob_engine.mjs" "$f" || fail "$f does not import knob_engine.mjs"
done

# 4. Nobody may keep a private copy of the step arithmetic. The range fraction
#    is the number that defines the feel; a second literal of it somewhere
#    else is a second model in the making.
stray=$(command grep -rln "MIN_STEP_RANGE_FRAC" src/ | command grep -v "^src/shared/knob_engine.mjs$" || true)
[ -n "$stray" ] && fail "MIN_STEP_RANGE_FRAC is referenced outside the engine: $stray"

echo "  ok  one model file, one entry point, no aliases"
echo "  ok  all three knob-turning call sites go through knobStep"
echo "PASS: there is exactly one knob model"
