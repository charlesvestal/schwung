#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The MINIMUM on-device surface for automation lanes: a way to clear them that
# is reachable from both forms of slot settings, and a refusal you can hear.
#
# The plan's draft of this test grepped shadow_ui.js for both arrays. Neither
# is in that file -- SLOT_SETTINGS lives in shadow_ui_slots.mjs and
# SLOT_GRID_ACTIONS in shadow_ui_slot_grid.mjs -- so it would have passed with
# the row on neither surface. The file each array actually lives in is named
# here, which is also what makes a future move of one of them fail loudly.

fail() { echo "FAIL: $1"; exit 1; }

slots=src/shadow/shadow_ui_slots.mjs
grid=src/shadow/shadow_ui_slot_grid.mjs
ui=src/shadow/shadow_ui.js

# THE KNOB GRID IS THE DEFAULT slot-settings surface (enterChainSettings gates
# on paramPagesEnabled), so a row present only on the list forms is
# unreachable for most users -- which docs/SHADOW_UI.md records as having
# happened before.
awk '/export const SLOT_SETTINGS = \[/,/^\];/' "$slots" | grep -q 'Clear Lanes' \
  || fail "Clear Lanes missing from SLOT_SETTINGS ($slots)"
awk '/export const SLOT_GRID_ACTIONS = \[/,/^\];/' "$grid" | grep -q 'Clear Lanes' \
  || fail "Clear Lanes missing from SLOT_GRID_ACTIONS ($grid)"
# The third form of the same screen. A row on two of the three is the
# asymmetry SLOT_SETTINGS' own `buses` comment calls worse than either.
awk '/^const CHAIN_SETTINGS_ITEMS = \[/,/^\];/' "$ui" | grep -q 'Clear Lanes' \
  || fail "Clear Lanes missing from CHAIN_SETTINGS_ITEMS ($ui)"

# ...and all three must reach the SAME implementation. Three copies of the
# read-and-announce is three chances for one of them to announce a count it
# did not read.
[ "$(grep -c 'function clearSlotLanes' "$ui")" = "1" ] \
  || fail "clearSlotLanes is not defined exactly once in $ui"

# A FAILED READ IS NOT A COUNT OF ZERO. `null` means the read did not
# complete; announcing "0 cleared" for it is the confidently-wrong answer this
# codebase keeps paying for. Asserted as the null branch STANDING BEFORE the
# count is formatted, because a `parseInt(null) || 0` after the fact reads the
# same in a grep and says zero.
grep -q 'Clear lanes: no answer' "$ui" \
  || fail "no null-read branch on the clear announcement"
node -e '
const fs = require("fs");
const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const m = /function clearSlotLanes\([\s\S]*?\n}/.exec(src);
if (!m) { console.log("FAIL: cannot isolate clearSlotLanes"); process.exit(1); }
const body = m[0];
const nullAt = body.indexOf("Clear lanes: no answer");
const countAt = body.search(/Cleared /);
if (nullAt < 0) { console.log("FAIL: clearSlotLanes has no null-read branch"); process.exit(1); }
if (countAt < 0) { console.log("FAIL: clearSlotLanes announces no count"); process.exit(1); }
if (!(nullAt < countAt)) {
  console.log("FAIL: the count is announced before the null read is ruled out");
  process.exit(1);
}
/* The count comes from the DSP. A literal would make a clear that cleared
 * nothing indistinguishable from one that worked. */
if (!/lanes:cleared/.test(body)) {
  console.log("FAIL: clearSlotLanes does not read lanes:cleared");
  process.exit(1);
}
' || exit 1

# THE REFUSAL COSTS NOTHING PER DETENT. An IPC round trip is ~2.8 ms against a
# 1.68 ms whole-page render, so a read per detent is slower than redrawing the
# screen on every one of them. The read sits behind the same once-per-gesture
# gate as the speech, so a sweep pays for it once.
grep -q 'laneRefusalGesture' "$ui" \
  || fail "no once-per-gesture gate on the lane refusal"
node -e '
const fs = require("fs");
const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const m = /function noteLaneWriteRefusal\([\s\S]*?\n}/.exec(src);
if (!m) { console.log("FAIL: cannot isolate noteLaneWriteRefusal"); process.exit(1); }
const body = m[0];
/* The gate must be decided BEFORE either read, or the read happens per detent
 * and only the speech is throttled -- which is the whole failure mode. */
const gateAt = body.search(/laneRefusalGesture/);
const readAt = body.search(/getSlotParam/);
if (readAt < 0) { console.log("FAIL: the refusal never reads the DSP"); process.exit(1); }
if (!(gateAt >= 0 && gateAt < readAt)) {
  console.log("FAIL: the refusal reads before it checks the gesture gate -- "
            + "that is a ~2.8ms IPC round trip per detent");
  process.exit(1);
}
' || exit 1

node --check "$ui" || fail "shadow_ui.js does not parse"
node --check "$slots" || fail "shadow_ui_slots.mjs does not parse"
node --check "$grid" || fail "shadow_ui_slot_grid.mjs does not parse"

echo "PASS: lane UI surface"
