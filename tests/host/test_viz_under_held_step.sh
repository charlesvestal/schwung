#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# GRAPHICS DO NOT STAND DOWN FOR A HELD STEP.
#
# They used to -- all of them -- on the argument that "a graphic replacing
# several slots with one picture would hide which of them is locked". That
# reads plausibly and is not true: drawLabelCell sits OUTSIDE the covered[col]
# guard, so every column draws its own label band whether or not a graphic
# covers its knob area, and the band is exactly where a lock shows. A spanning
# graphic never hid anything. What standing down DID hide was the module's own
# reading of the parameter, at the moment the user is editing that parameter.
#
# The second assertion re-checks that claim in the RENDERER rather than
# trusting this comment, because the whole decision rests on it.

fail() { echo "FAIL: $1"; exit 1; }
src=src/shared/param_pages/page_controller.mjs

body=$(awk '/function vizGroupsForDecorations/,/^    }$/' "$src")
[ -n "$body" ] || fail "vizGroupsForDecorations is gone -- graphics are back to all-or-nothing"
echo "$body" | grep -q 'return vizGroups();' \
  || fail "graphics must NOT be filtered while a step is held: the per-cell band is what shows a lock, and it draws under a graphic anyway"

# The claim that makes that safe, checked in the RENDERER rather than trusted:
# drawLabelCell must sit OUTSIDE the covered[col] guard, so a cell whose knob
# area a graphic has taken still draws its own (invertible) label band.
python3 - <<'PY' || fail "drawLabelCell is inside the covered[col] guard -- a spanning graphic WOULD hide which cell is locked, and the stand-down was right after all"
import sys
src = open("src/shared/param_pages/render_page_movy.mjs").read()
i = src.index("if (!covered[col]) {")
j = src.index("drawLabelCell(ctx, g, col, lblY", i)
depth = 0
for ch in src[i:j]:
    if ch == "{": depth += 1
    elif ch == "}": depth -= 1
sys.exit(0 if depth == 0 else 1)
PY

# Both render paths, or a lock looks different depending on which one drew.
n=$(grep -c 'viz: vizEnabled ? vizGroupsForDecorations() : \[\]' "$src" || true)
[ "$n" = "2" ] || fail "expected both renderers to go through one viz decision, found $n"
if grep -q 'viz: (vizEnabled && !s.decorations)' "$src"; then
  fail "a caller still stands every graphic down while a step is held"
fi

# ...and the widget must draw the LOCKED value, which the RENDERER already
# arranges: a p-lock outranks `raw` AND `liveRaw`, because every widget that
# cannot show two values draws the second one. Folding the locked value into
# the `values` map instead looked equivalent and was not -- it also folded the
# MODULATED value in, and the live value then won over the decoration in the
# Movy layout (caught by test_param_pages_embed).
mv=src/shared/param_pages/render_page_movy.mjs
grep -q 'A P-LOCK OUTRANKS THE LIVE VALUE' "$mv" \
  || fail "the renderer no longer states the decoration precedence -- check that liveRaw is still decorated"
raw=$(grep -c 'values: liveValues()' "$src" || true)
[ "$raw" -ge 1 ] || fail "liveValues is unused -- the modulation merge is gone"

echo "PASS: viz under a held step (graphics stay, the per-cell band shows the lock, widgets draw the locked value)"
