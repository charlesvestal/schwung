#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# ONE LIST: no screen may draw its own selection row.
#
# Before this work there were SIX hand-rolled `for` + fill_rect + print loops
# alongside ~53 drawMenuList call sites, concentrated in the slot / Master FX /
# knob / LFO family. drawChainSettings's Save-As block and drawMasterNamePreview
# were byte-identical apart from variable names: fix one, the other stayed
# broken. That is the whole reason this file exists.
#
# ITS LIMIT, STATED: this pins one IDIOM, not the property. A future hand-rolled
# list reaching for its own constants instead of LIST_HIGHLIGHT_HEIGHT passes
# this while being exactly the thing it forbids -- a green matrix only proves
# the axis you chose. It is a tripwire on the known copy-paste path; review
# still owes the general case.

fail() { echo "FAIL: $*" >&2; exit 1; }

hits=$(command grep -rn 'LIST_HIGHLIGHT_HEIGHT' src \
       | command grep 'fill_rect' \
       | command grep -v '^src/shared/menu_layout.mjs:' || true)

if [ -n "$hits" ]; then
  echo "$hits" >&2
  fail "hand-rolled list row(s) outside menu_layout.mjs -- use drawMenuList
      (or drawNamePreview / drawConfirmModal for the two-row selectors)."
fi

sanctioned=$(command grep -c 'fill_rect.*LIST_HIGHLIGHT_HEIGHT' src/shared/menu_layout.mjs || true)
[ "$sanctioned" -ge 1 ] || fail "menu_layout.mjs no longer draws a selection row -- \
      this test is now vacuous and must be re-aimed."

echo "PASS: one list"
