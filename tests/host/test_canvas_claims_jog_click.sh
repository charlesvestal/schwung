#!/usr/bin/env bash
#
# A canvas param may CLAIM the jog click, and then Back is the only exit.
#
# MoveMainButton is CC 3 -- the jog click -- and shadow_ui steals it to close
# the fullscreen canvas BEFORE dispatchCanvasMidi runs, so an overlay can never
# see it. That is right for a viewer you look at and leave, and wrong for an
# editor meant to be worked in: the click is its primary gesture.
#
# The opt-in is why this is safe. #154 took Undo/Copy/Delete unconditionally
# whenever the shadow display was up and had to be reverted (#175) because it
# stole them from everyone; claims_ccs was the capability answer to that, and
# this is the same shape. So the pins below are BOTH directions: a param that
# declares nothing must behave exactly as it did before.
set -uo pipefail
cd "$(dirname "$0")/../.."
F=src/shadow/shadow_ui.js
fail=0
bad(){ echo "FAIL: $1"; fail=1; }

grep -q 'canvasClaimsClick' "$F" || bad "no canvasClaimsClick gate in shadow_ui.js"

# The gate must sit on the CLOSE, not on the dispatch: the overlay only ever
# sees the click because the close no longer swallows it.
grep -q 'd1 === MoveMainButton && d2 > 0 && !canvasClaimsClick' "$F" \
  || bad "the close still swallows the click unconditionally"

# Declaring nothing must be indistinguishable from today.
grep -q 'canvasParamMeta.claims_jog_click === true' "$F" \
  || bad "the claim is not read from the param's own declaration"

# A co-run peer shares the surface and never agreed to give up the click.
grep -q 'var canvasClaimsClick = !canvasInCorun' "$F" \
  || bad "the claim is honoured in co-run, where the click belongs to the peer"

node --check "$F" 2>/dev/null || bad "shadow_ui.js does not parse"

[ "$fail" -eq 0 ] || exit 1
echo "PASS: $(basename "$0")  (a canvas param can claim CC 3, opt-in only)"
