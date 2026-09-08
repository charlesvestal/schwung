#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# pad_block must not outlive the component UI that raised it.
#
# A module takes the pads with host_pad_block(1) (9W9, for its Shift+Pad lane
# select) and lowers them from its own tick(). shadow_ui.js calls that tick
# from exactly ONE place -- `case VIEWS.COMPONENT_EDIT` in the draw switch --
# so every shim-decided exit from that view (track long-press dismiss,
# Shift+Track, a Menu tap) stops the only thing that could lower the flag.
# The shim enforces pad_block INSIDE the shadow_display_mode branch, so the
# result is pads dead in the Schwung UI and fine on a Move track, with knobs,
# jog and Back all still working.
#
# Field report 2026-09-08: exactly one "pad_block ON" in the log and no OFF,
# ever -- the byte had been stuck for 13 hours across two shim inits, because
# /dev/shm outlives restart-move.sh and nothing cleared it at boot either.
#
# Three independent guards, because each covers an exit the others do not:
#
#   1. the shim drops it on the display-mode 1->0 edge  (dismiss)
#   2. the shim clears it at init                       (stale segment)
#   3. shadow_ui.js clears it in unloadModuleUi()       (exits that keep
#                                                        the shadow UI up)
#
# Style-of-house grep pins (see test_claims_ccs.sh): the shim is
# cross-compiled and this runs in the SPI callback, so the seams are pinned by
# their text rather than driven.

fail() { echo "FAIL: $*" >&2; exit 1; }

shim="src/schwung_shim.c"
ui_js="src/shadow/shadow_ui.js"
hdr="src/host/shadow_constants.h"

command grep -q 'volatile uint8_t pad_block;' "$hdr" \
  || fail "shadow_control_t has no pad_block -- this test is pinning a field that moved"

# ---- 1. dropped on the display-mode edge, beside pad_observe -----------------
# Scoped to the edge block itself. A bare file-wide grep would pass on the
# enforcement site (which reads the flag every frame) and prove nothing.
edge=$(sed -n '/static int prev_display_mode = 0;/,/prev_display_mode = shadow_display_mode;/p' "$shim")
[ -n "$edge" ] || fail "the display-mode edge block is gone"
echo "$edge" | command grep -q 'shadow_control->pad_block = 0;' \
  || fail "the display-mode edge does not drop pad_block -- a dismiss from a module that took the pads leaves them dead in the shadow UI with no gesture that restores them"
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

# ---- 3. cleared when the module UI is unloaded -------------------------------
unload=$(awk '/^function unloadModuleUi\(\)/,/^}/' "$ui_js")
[ -n "$unload" ] || fail "unloadModuleUi() is gone from shadow_ui.js"
echo "$unload" | command grep -q 'host_pad_block(0)' \
  || fail "unloadModuleUi() does not clear pad_block -- leaving a component editor while the shadow UI stays up (another slot, Tools, Global Settings) strands it"

# ---- 4. the premise the three guards rest on --------------------------------
# The module's own tick() is the only lowering path a module has, and it is
# reached from one call site. If that ever stops being true the guards above
# are still correct, but the REASON recorded here would be wrong -- so pin the
# fact rather than let the comments rot.
# Comment lines stripped first: one of the matches is the co-run note saying
# never to invoke it, and a pin that counts its own documentation is the
# regex-literal blindness this suite has been bitten by before.
ticks=$(command grep -E '^[[:space:]]*(\*|//|/\*)' -v "$ui_js" \
        | command grep -c 'loadedModuleUi\.tick()' || true)
[ "$ticks" = "1" ] \
  || fail "loadedModuleUi.tick() is called from $ticks places, not 1 -- re-read the reasoning in test_pad_block_lifecycle.sh and in the shim's display-mode edge comment"

echo "PASS: pad_block cannot outlive the component UI that raised it"
