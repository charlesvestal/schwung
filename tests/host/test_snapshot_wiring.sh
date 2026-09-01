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
grep -q "shadow_clear_ui_flags(SHADOW_UI_FLAG_SNAPSHOT_TAKE" "$UI" || note "snapshot flags never cleared"

# 6. The toast clears itself and drives its own redraws.
grep -q "snapshotToastFrames--" "$UI"     || note "toast frame counter never decrements"
grep -q "snapshotToastLines = null" "$UI" || note "toast never clears"
grep -q "!snapshotToastActive()" "$UI"    || note "toast does not force a redraw (it would draw at 1/Nth rate)"

# 7. The blit rect is the geometry the box was drawn with, not a recomputation.
grep -q "shadow_set_display_overlay(1, g.x, g.y, g.w, g.h)" "$UI" \
  || note "toast blit rect is not the returned geometry"

# 8. Toast geometry, for real.
#    The shared modules import by DEVICE path (/data/UserData/schwung/shared/),
#    which node cannot resolve, so rewrite it into the source tree the same way
#    test_param_pages_view.sh does.
REPO="$(pwd)"
sed "s#/data/UserData/schwung/shared/#${REPO}/src/shared/#g" \
    src/shared/snapshot_toast.mjs > "$TMP/toast_local.mjs"
node --input-type=module -e '
import { toastGeometry } from "'"$TMP"'/toast_local.mjs";
let f = 0;
const w = (s) => s.length * 6;          /* deterministic stand-in for text_width */
const ok = (what, c) => { if (!c) { console.error("FAIL " + what); f++; } };

const one = toastGeometry(["Snapshot saved"], w);
ok("box is on screen horizontally", one.x >= 0 && one.x + one.w <= 128);
ok("box is on screen vertically",   one.y >= 0 && one.y + one.h <= 64);
ok("box is wider than its text",    one.w > w("Snapshot saved"));
ok("one line, one row",             one.rows.length === 1);
ok("nothing clipped",               one.clipped === 0);

const two = toastGeometry(["Snapshot restored", "2 skipped"], w);
ok("two lines are taller",          two.h > one.h);
ok("two lines still fit",           two.y >= 0 && two.y + two.h <= 64);
ok("width follows the WIDEST line", two.w > w("Snapshot restored"));
ok("both lines kept",               two.rows.length === 2);
ok("still nothing clipped",         two.clipped === 0);

/* Empty/absent lines are dropped rather than drawn as blank rows. */
ok("blank rows dropped", toastGeometry(["a", "", null], w).rows.length === 1);

/* A line too wide for the screen must REPORT — it is drawn off the edge with
 * no error, which is exactly the class of bug a character-count budget hides.
 * And the BOX must still be on screen: without the clamp it grows with the
 * text and x goes negative, so the frame vanishes too and the only evidence
 * left is text running off both edges. */
const over = toastGeometry(["x".repeat(60)], w);
ok("overflow is reported", over.clipped === 1);
ok("overflowing box stays on screen", over.x >= 0 && over.x + over.w <= 128);

if (f) process.exit(1);
' || note "toast geometry assertions failed"

if [ "$fails" -ne 0 ]; then echo "$fails check(s) failed"; exit 1; fi
echo "PASS test_snapshot_wiring"
