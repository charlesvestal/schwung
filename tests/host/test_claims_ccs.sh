#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A module may claim buttons -- and ONLY a module that asks, ONLY the buttons
# the host does not own.
#
# #154 withheld CC 56 / 60 / 119 from Move firmware whenever the shadow display
# was up, and #175 reverted it: Move's native Undo was gone during ordinary
# chain use. The revert named the acceptable shape, a capability opt-in. This
# pins the properties that make the opt-in honest, each of which a plausible
# simplification would lose:
#
#   1. the firmware filter is gated on the RUNTIME claim bitmap, never on the
#      display being up (the #154 regression, in one line)
#   2. the press/release pairing is latched per button, so a claim changing
#      mid-hold cannot leave Move or the module believing a button is held
#   3. a Shift-held press is never claimed -- Shift+Copy / Shift+Delete are
#      the host's snapshot and recall
#   4. the host-owned controls are refused BY THE SHIM, whatever was written
#   5. the JS derives the claim from module metadata in ONE place, re-checked
#      every tick, honours the claims_edit_ccs shorthand, and the shim drops
#      every claim itself when the display closes
#
# Style-of-house grep pins (see test_contract_capture_scope.sh): the shim is
# cross-compiled and the routing runs in the SPI callback, so the seams are
# pinned by their text rather than driven.

fail() { echo "FAIL: $*" >&2; exit 1; }

shim="src/schwung_shim.c"
ui_c="src/shadow/shadow_ui.c"
ui_js="src/shadow/shadow_ui.js"
hdr="src/host/shadow_constants.h"
docs="docs/MODULES.md"

# ---- 1. gated on the claim, not on the display -------------------------------
command grep -q 'volatile uint8_t claim_cc_bits\[16\];' "$hdr" \
  || fail "shadow_control_t has no claim_cc_bits -- the runtime claim has nowhere to live"

# The struct is a contract between two binaries: the field was APPENDED, which
# means nothing sits in front of it that was not already there. Pinned as its
# predecessor rather than as "last" -- a later register appended after it is
# correct, and a last-field check would fail on every one of them.
prev=$(command grep -oE 'volatile [a-z0-9_]+ [a-z_0-9]+(\[[0-9]+\])?;' "$hdr" | command grep -B1 'claim_cc_bits' | head -1 | awk '{print $3}' | tr -d ';')
[ "$prev" = "save_stems" ] || fail "claim_cc_bits no longer directly follows save_stems (preceded by \"$prev\") -- a field was INSERTED, which moves every field behind it"

command grep -q 'claim_press_blocked\[d1\] =' "$shim" \
  || fail "the per-button press latch is gone from the firmware filter"
command grep -A1 'claim_press_blocked\[d1\] =' "$shim" | command grep -q 'claim_cc_set(d1)' \
  || fail "the press latch is not derived from the claim bitmap -- that is #154 again"
if command grep -B2 -A2 'claim_press_blocked\[d1\] =' "$shim" | command grep -q 'shadow_display_mode'; then
  fail "the claim site tests the display, not the claim -- the #175 revert exists because of exactly this"
fi
echo "  ok  the firmware filter withholds a button only under the runtime claim"

# ---- 2. the latch pairs release with press -----------------------------------
command grep -q 'if (claim_press_blocked\[d1\]) filter = 1;' "$shim" \
  || fail "a release is not routed by the latch its press set"
command grep -q '(d1 < 128 && claim_press_blocked\[d1\])' "$shim" \
  || fail "the forward-to-shadow_ui site does not consult the same latch as the filter"
echo "  ok  whoever received the press receives the release"

# ---- 3. Shift-held presses are never claimed ---------------------------------
command grep -A1 'claim_press_blocked\[d1\] =' "$shim" | command grep -q '!shadow_shift_held' \
  || fail "a Shift-held button can be claimed -- Shift+Copy / Shift+Delete are the host's snapshot/recall"
echo "  ok  Shift+<button> stays the host's"

# ---- 4. the host-owned controls are refused by the shim ----------------------
command grep -A1 'claim_press_blocked\[d1\] =' "$shim" | command grep -q '!claim_denied_cc(d1)' \
  || fail "the latch does not consult claim_denied_cc -- a module could claim Shift, Mute or the jog"
denied=$(sed -n '/^static int claim_denied_cc(uint8_t cc) {/,/^}/p' "$shim")
[ -n "$denied" ] || fail "claim_denied_cc is gone"
for sym in CC_SHIFT CC_MENU CC_BACK CC_JOG_WHEEL CC_JOG_CLICK CC_KNOB1 CC_KNOB8 CC_MASTER_KNOB CC_MUTE; do
  echo "$denied" | command grep -q "$sym" || fail "claim_denied_cc no longer refuses $sym"
done
echo "$denied" | command grep -q 'cc >= 40 && cc <= 43' || fail "claim_denied_cc no longer refuses the track buttons"
# ...and NOT the buttons a module may want: a denylist that grew to cover the
# transport or the edit trio would make the capability decorative.
for sym in CC_PLAY CC_REC CC_UNDO CC_COPY CC_DELETE CC_LOOP CC_CAPTURE; do
  if echo "$denied" | command grep -q "$sym"; then fail "claim_denied_cc refuses $sym -- that button is a module's to claim"; fi
done
echo "  ok  Shift/Menu/Back/jog/knobs/Mute/tracks can never be claimed; Play, Rec and the edit trio can"

# ---- 5. one reconcile point, the shorthand, and the shim's own drop -----------
command grep -q '"host_claim_ccs"' "$ui_c" || fail "host_claim_ccs is not bound for the shadow UI"
n=$(command grep -c 'host_claim_ccs(claim ? claim.split' "$ui_js" || true)
[ "$n" -eq 1 ] || fail "host_claim_ccs must be written from exactly ONE reconcile site (found $n)"
command grep -q '^    reconcileCcClaim();' "$ui_js" || fail "reconcileCcClaim() is not called from the tick"
command grep -q 'caps.claims_edit_ccs' "$ui_js" || fail "the claims_edit_ccs shorthand is not honoured"
command grep -q 'const EDIT_CCS = \[56, 60, 119\];' "$ui_js" || fail "claims_edit_ccs no longer means Undo/Copy/Delete"
command grep -q 'Array.isArray(caps.claims_ccs)' "$ui_js" || fail "the general claims_ccs list is not read"
command grep -q 'CC_CLAIM_VIEWS\[VIEWS.PARAM_PAGES\] = true' "$ui_js" \
  || fail "the knob grid (PARAM_PAGES) cannot hold a claim -- it is the default param view"
command grep -B3 'memset((void \*)shadow_control->claim_cc_bits, 0' "$shim" | command grep -q 'prev_display_mode && !shadow_display_mode' \
  || fail "the shim does not drop the claims when the shadow display closes -- a crashed shadow_ui strands Move's buttons"
echo "  ok  claims are reconciled from metadata in one place and dropped by the shim on display close"

# ---- docs say what the code does ---------------------------------------------
command grep -q '| `claims_ccs` |' "$docs" || fail "docs/MODULES.md capability table has no claims_ccs row"
command grep -q '| `claims_edit_ccs` |' "$docs" || fail "docs/MODULES.md capability table has no claims_edit_ccs row"
if command grep -q 'are delivered exclusively to the loaded module' "$docs"; then
  fail "docs/MODULES.md still describes #154's unconditional block as current behaviour"
fi
echo "  ok  docs describe the opt-in"

echo "PASS: test_claims_ccs"
