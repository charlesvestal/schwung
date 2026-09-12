#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# pad_block must not outlive the component UI that raised it.
#
# A component's ui_chain.js takes the pads with host_pad_block(1) -- 9W9 does,
# so it can do Shift+Pad lane select and forward the notes to Move itself --
# and lowers them from its OWN tick(), which the draw switch calls from one
# place: `case VIEWS.COMPONENT_EDIT`. Anything that stops that tick strands the
# flag, and because the shim enforces pad_block INSIDE the shadow_display_mode
# branch, the user gets pads dead in the Schwung UI and fine on a Move track,
# with knobs, jog and Back all still working.
#
# Field report 2026-09-08: exactly one "pad_block ON" in the log and no OFF,
# ever -- the byte had been stuck for 13 hours across two shim inits, because
# /dev/shm outlives restart-move.sh and nothing cleared it at boot either.
#
# THE EXITS CANNOT BE ENUMERATED, and the first cut of this fix tried twice.
# unloadModuleUi() is reached by Back and the tool-open paths only, so a jump
# to Global Settings or Master FX changes the view with the module still
# LOADED; a Track tap alone means dismiss or switch-slot depending on Keep
# Schwung; co-run stops the tick with no view change at all. Hardware found
# each gap in turn. So the JS states the INVARIANT every frame, and the shim
# keeps the two drops it can make on its own.
#
# Style-of-house grep pins (see test_claims_ccs.sh): the shim is
# cross-compiled and this runs in the SPI callback, so the seams are pinned by
# their text rather than driven.

fail() { echo "FAIL: $*" >&2; exit 1; }

shim="src/schwung_shim.c"
ui_js="src/shadow/shadow_ui.js"
ui_c="src/shadow/shadow_ui.c"
hdr="src/host/shadow_constants.h"

command grep -q 'volatile uint8_t pad_block;' "$hdr" \
  || fail "shadow_control_t has no pad_block -- this test is pinning a field that moved"

# ---- 1. dropped on the display-mode edge, beside pad_observe -----------------
# Scoped to the edge block itself. A bare file-wide grep would pass on the
# enforcement site (which reads the flag every frame) and prove nothing.
edge=$(sed -n '/static int prev_display_mode = 0;/,/prev_display_mode = shadow_display_mode;/p' "$shim")
[ -n "$edge" ] || fail "the display-mode edge block is gone"
echo "$edge" | command grep -q 'shadow_control->pad_block = 0;' \
  || fail "the display-mode edge does not drop pad_block -- a dismiss from a module that took the pads leaves them dead with no gesture that restores them"
echo "$edge" | command grep -q 'shadow_control->pad_observe = 0;' \
  || fail "the display-mode edge no longer drops pad_observe -- the two are dropped together on purpose"

# ---- 2. cleared at shim init ------------------------------------------------
# /dev/shm/schwung-control survives restart-move.sh, so without this an
# already-stuck device stays stuck across every restart the user tries.
init=$(sed -n '/Reset overtake state on every shim init/,/Initialize TTS defaults/p' "$shim")
[ -n "$init" ] || fail "the shim-init reset block is gone"
echo "$init" | command grep -q 'shadow_control->pad_block' \
  || fail "shim init does not clear pad_block -- a stale segment keeps the pads blocked across a Move restart"
echo "$init" | command grep -q 'shadow_control->pad_observe' \
  || fail "shim init does not clear pad_observe"

# ---- 3. RESTATED every tick, never edged ------------------------------------
command grep -q 'function reconcilePadBlock()' "$ui_js" \
  || fail "reconcilePadBlock() is gone -- pad_block is being edged again, and the exits cannot be enumerated"
command grep -q '^    reconcilePadBlock();' "$ui_js" \
  || fail "reconcilePadBlock() is not called from the top of the shadow UI tick -- a reconcile that does not run is not a reconcile"

recon=$(awk '/^function reconcilePadBlock\(\)/,/^}/' "$ui_js")
echo "$recon" | command grep -q 'isTextEntryActive()' \
  || fail "reconcilePadBlock() ignores the on-screen keyboard -- it uses pads as keys OVER views that are not COMPONENT_EDIT, and clearing under it breaks pad typing"
echo "$recon" | command grep -q 'coRunUiActive()' \
  || fail "reconcilePadBlock() ignores co-run -- that branch never invokes loadedModuleUi.tick(), so the module cannot lower the flag it raised"
echo "$recon" | command grep -q 'VIEWS.COMPONENT_EDIT' \
  || fail "reconcilePadBlock() no longer mirrors the draw switch's tick gate"
echo "$recon" | command grep -q 'host_pad_block(0)' \
  || fail "reconcilePadBlock() never lowers the flag"

# One JS answer, not several: two sites tracking one invariant is how they
# drift -- the same reasoning the shim's display-mode edge records for keeping
# ONE static.
edges=$(command grep -c 'host_pad_block(0)' "$ui_js" || true)
[ "$edges" = "1" ] \
  || fail "host_pad_block(0) is called from $edges places in shadow_ui.js, not 1 -- an edge guard has come back alongside the reconcile"

# ---- 4. the restate must be FREE, or it cannot run every frame ---------------
# An unchanged restate has to cost a compare, not a write and a log line: at
# ~60 Hz the old unconditional logger would have flooded debug.log and made
# the reconcile above unshippable. Comparing against the SHM rather than a
# remembered value is also what lets the caller restate instead of memoise --
# the shim drops this flag unilaterally and a JS mirror would latch.
pb=$(awk '/js_host_pad_block\(JSContext/,/^}/' "$ui_c")
[ -n "$pb" ] || fail "js_host_pad_block is gone from shadow_ui.c"
echo "$pb" | command grep -q 'if (shadow_control->pad_block == (uint8_t)val) return' \
  || fail "js_host_pad_block is not idempotent -- a per-frame restate would write and log on every tick"
echo "$pb" | command grep -q 'shadow_ui_log_line' \
  || fail "js_host_pad_block no longer logs the transition -- that log line is what identified this bug in the field"

# ---- 5. the premise the reconcile mirrors -----------------------------------
# Comment lines stripped first: one match is the co-run note saying never to
# invoke it, and a pin that counts its own documentation is the regex-literal
# blindness this suite has been bitten by before.
ticks=$(command grep -E '^[[:space:]]*(\*|//|/\*)' -v "$ui_js" \
        | command grep -c 'loadedModuleUi\.tick()' || true)
[ "$ticks" = "1" ] \
  || fail "loadedModuleUi.tick() is called from $ticks places, not 1 -- reconcilePadBlock() mirrors a single gate, so re-read it before changing this"

# ---- 6. ...and the KEYBOARD is not a second claimant ------------------------
# text_entry.mjs raises pad_block because it types with the pads, and its close
# used to write 0 straight back. That was right while a keyboard could only be
# raised over views that are not COMPONENT_EDIT -- which is the premise
# reconcilePadBlock() is written on, and which stopped being true when a
# module-owned param grid gained a "Save As" row. A component that owns the
# pads (9W9) had its claim stomped by a keyboard opened and closed over it:
# pads to Move, mid-mode, healed only if the module happens to re-state the
# flag every tick rather than on entering the mode.
#
# The reconcile already answers this every frame and skips only while a
# keyboard is up, so the close hands the decision back rather than guessing.
# Same rule as rule 3 above: ONE answer.
te="src/shared/text_entry.mjs"
[ -f "$te" ] || fail "missing $te"
command grep -q 'host_pad_block(1)' "$te" \
  || fail "text entry no longer takes the pads -- pad typing is broken, or this pin has moved"
raises=$(command grep -E '^[[:space:]]*(\*|//|/\*)' -v "$te" | command grep -c 'host_pad_block(0)' || true)
[ "$raises" = "0" ] \
  || fail "closeTextEntry lowers pad_block itself ($raises call(s)) -- that overwrites a component UI's own claim; reconcilePadBlock() owns the drop"

echo "PASS: pad_block cannot outlive the component UI that raised it"
