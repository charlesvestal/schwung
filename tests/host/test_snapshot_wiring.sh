#!/usr/bin/env bash
# The shadow_ui.js half of snapshot / recall, plus the toast geometry.
#
# shadow_ui.js cannot run off-device, so this parses it and pins the wiring
# that would otherwise fail silently — a seed that never runs, a recall that
# reloads modules, a toast that never clears. The toast geometry IS runnable
# and is checked for real.
set -euo pipefail
cd "$(dirname "$0")/../.."

command -v node >/dev/null 2>&1 || { echo "FAIL: node required" >&2; exit 1; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
UI=src/shadow/shadow_ui.js
fails=0
note() { echo "FAIL: $1"; fails=$((fails+1)); }

# 1. Everything still parses as a module. `node --check` on a .js treats it as
#    CommonJS and passes source that is broken as ESM, so the copy matters.
for f in "$UI" src/shared/snapshot.mjs src/shared/snapshot_toast.mjs; do
  cp "$f" "$TMP/$(basename "${f%.js}").mjs"
done
for f in "$TMP"/*.mjs; do
  node --check "$f" 2>"$TMP/err" || { echo "FAIL: $f does not parse:"; cat "$TMP/err"; exit 1; }
done

# 2. Both seed call sites exist, with the right forcing.
#    A set load must OVERWRITE (force=true) or a snapshot from the previous set
#    survives into this one and Shift+Delete restores an unrelated rig.
#    Startup must NOT (force=false) or a snapshot taken this session is
#    destroyed by the shadow_ui restart it is on disk to survive.
grep -q "snapshotSeed(true)"  "$UI" || note "no forced re-seed on set load"
grep -q "snapshotSeed(false)" "$UI" || note "no conditional seed at startup"

# 3. Recall writes state, never load_file. load_file reinstantiates — it cuts
#    reverb tails and resets arp phase, which is the opposite of an A/B.
recall=$(awk '/^function snapshotRecall\(\)/,/^}/' "$UI")
[ -n "$recall" ] || note "snapshotRecall not found"
echo "$recall" | grep -q 'load_file' && note "recall uses load_file — it must write state only"
echo "$recall" | grep -q ':state"' || note "recall never writes a :state key"

# 4. A take goes through the EXISTING writers. A parallel serializer would have
#    to re-derive every guard those have accumulated.
take=$(awk '/^function snapshotTake\(\)/,/^}/' "$UI")
echo "$take" | grep -q "autosaveAllSlots()"        || note "take does not flush slots via autosaveAllSlots"
echo "$take" | grep -q "saveMasterFxChainConfig()" || note "take does not flush Master FX"

# 5. The flags are serviced and cleared. Without the clear the shim's flag
#    stays raised and the gesture repeats every tick, forever.
grep -q "snapshotServiceFlags(flags)" "$UI" || note "snapshotServiceFlags never called from the flag block"
grep -q "shadow_clear_ui_flags(SNAPSHOT_FLAGS)" "$UI" || note "snapshot flags never cleared"

# 6. The toast clears itself and drives its own redraws.
grep -q "snapshotToastFrames--" "$UI"     || note "toast frame counter never decrements"
grep -q "snapshotToastLines = null" "$UI" || note "toast never clears"
grep -q "!snapshotToastActive()" "$UI"    || note "toast does not force a redraw (it would draw at 1/Nth rate)"

# 7. The blit rect is the geometry the box was drawn with, not a recomputation.
grep -q "shadow_set_display_overlay(1, g.blit.x, g.blit.y, g.blit.w, g.blit.h)" "$UI" \
  || note "toast blit rect is not the card's returned blit rect"

# 7b. TWO toast paths, and only the Move-native one may clear the screen.
#     When the shadow UI is displayed, the shadow display IS the screen —
#     clear_screen() there wipes the view behind the toast and the early
#     return means nothing redraws it. Reported from hardware as "it didn't
#     overlay, it blanked the screen behind it".
grep -q "function shadowDisplayHidden" "$UI"    || note "no display-mode split for the toast"
grep -q "snapshotToastActive() && shadowDisplayHidden()" "$UI" \
  || note "the clearing toast branch is not gated on Move's screen being up"
ontop=$(awk '/^function drawSnapshotToastOnTop/,/^}/' "$UI")
[ -n "$ontop" ] || note "drawSnapshotToastOnTop missing"
echo "$ontop" | grep -q "clear_screen" \
  && note "the on-top toast clears the screen — that is the blanking bug"
echo "$ontop" | grep -q "shadow_set_display_overlay" \
  && note "the on-top toast sets an overlay rect — it would composite the screen onto itself"
grep -q "drawSnapshotToastOnTop();" "$UI" || note "drawSnapshotToastOnTop is never called"

# 7c. A recall must invalidate the grid's cached values. Nothing reads on the
#     draw path and onKnobTurn steps FROM the cache, so without this the first
#     knob move after a recall departs from the PRE-recall value and writes the
#     tweak back over what was just restored. Reported from hardware.
echo "$recall" | grep -q "paramPagesRevalue()" || note "recall does not re-read the grid's values"
grep -q "export function paramPagesRevalue" src/shadow/shadow_ui_param_pages.mjs \
  || note "paramPagesRevalue not exported"
grep -q "load, reloadIfChanged, tick, refreshTrailing, revalue," src/shared/param_pages/page_controller.mjs \
  || note "controller does not expose revalue"
rev=$(awk '/^    function revalue\(\)/,/^    }/' src/shared/param_pages/page_controller.mjs)
echo "$rev" | grep -q "s.values = Object.create(null)" || note "revalue does not drop cached values"
echo "$rev" | grep -q "s.knobStates = Object.create(null)" || note "revalue does not drop knob states"
echo "$rev" | grep -q "warmCurrentPage()" || note "revalue does not re-read the page"
echo "$rev" | grep -q "flushDueWritesUnconditionally()" \
  || note "revalue discards an in-flight write instead of flushing it"

# 8. The toast is a CARD, not a hand-rolled box. Its own geometry moved into
#    overlay_card.mjs and is asserted in test_overlay_card.sh; what matters
#    here is that this file no longer draws its own frame.
toast=src/shared/snapshot_toast.mjs
grep -q "drawOverlayCard" "$toast" || note "the toast does not use the shared card"
for own in "fill_rect" "drawRect" "BOX_W" "PAD_X"; do
  grep -q "$own" "$toast" && note "the toast still draws its own chrome ($own)"
done

if [ "$fails" -ne 0 ]; then echo "$fails check(s) failed"; exit 1; fi
echo "PASS test_snapshot_wiring"
