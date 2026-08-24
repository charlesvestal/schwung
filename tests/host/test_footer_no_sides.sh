#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# drawMenuFooter TAKES AN ORDERED LIST. IT HAS NO SIDES.
#
# It used to take {left, right}. Underneath, movy's drawFooter pulls a BACK
# hint to the right edge wherever the caller put it -- so
# `{left: "Back: slots", right: "Click: edit"}` rendered as
# [CLICK] EDIT ... [BACK] SLOTS. The placement was right; the NAME was wrong,
# and a name that lies is worse than no name.
#
# WHY THIS FILE EXISTS RATHER THAN A RUNTIME THROW. The old shape does not
# fail loudly -- it cannot. drawMenuFooter runs on the draw path under QuickJS
# on the device, where throwing once per tick is worse than any footer bug it
# would report. So the object shape flows through `Array.isArray(hints) ? hints
# : [hints]`, yields no pair, and is filtered out: the footer silently draws
# EMPTY. That is a real hazard -- a missed call site loses its footer with no
# signal at all -- and a static check is the only place it can be caught
# loudly. This is that place.
#
# ITS LIMIT, STATED: this pins the {left,right} spelling, not the property. A
# caller inventing some other object shape passes this while being equally
# wrong. It is a tripwire on the one shape that used to be correct and is now
# silent, which is the shape a stale example or an old branch will reintroduce.

fail() { echo "FAIL: $*" >&2; exit 1; }

# ---- 1. no caller passes a side ---------------------------------------------
# Matches `left:` / `right:` within three lines of a footer call, which covers
# the multiline object literals several call sites used.
hits=$(command grep -rn -A3 'drawMenuFooter(\|drawFooter(' src \
       | command grep -E '(left|right):' \
       | command grep -v 'src/modules/tools/ui-test/' \
       | command grep -v 'render_page_movy.mjs' || true)

if [ -n "$hits" ]; then
  echo "$hits" >&2
  fail "a footer call still names a side. drawMenuFooter takes a string or an
      ORDERED ARRAY of hints -- movy decides where BACK lands, not the caller:
          drawMenuFooter([\"Click: edit\", \"Back: slots\"])
      The {left,right} shape does not error; it draws an EMPTY footer."
fi

# ui-test/ui.js is excluded above on purpose: its `left:` is a JSON config key
# for a layout-tuning tool that draws its own flush-left footer with print(),
# not a drawMenuFooter call. The name is honest there.

# ---- 2. the signature itself has no sides -----------------------------------
sig=$(command grep -n 'export function drawMenuFooter' -A 4 src/shared/menu_layout.mjs || true)
[ -n "$sig" ] || fail "could not find drawMenuFooter in src/shared/menu_layout.mjs"
if printf '%s' "$sig" | command grep -qE '\b(left|right)\b'; then
  printf '%s\n' "$sig" >&2
  fail "drawMenuFooter's signature names a side again."
fi

# ---- 3. the guard is not vacuous --------------------------------------------
# If the grep in step 1 stops matching real call sites, this file passes
# forever while proving nothing. Assert there ARE footer calls to police.
calls=$(command grep -rn 'drawMenuFooter(\|drawFooter(' src | command grep -cv 'render_page_movy.mjs' || true)
[ "$calls" -ge 20 ] || fail "only $calls footer call sites found (expected 20+) --
      the search stopped matching and this guard has gone blind."

echo "  ok  no footer call names a side ($calls call sites policed)"
echo "PASS: the footer has no sides"
