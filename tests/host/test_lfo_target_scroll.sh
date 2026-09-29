#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE LFO TARGET PICKER, WALKED BY A KNOB AS ONE LIST.
#
# The jog walks the picker as a hierarchy (component > section > param). A
# knob walks the same menus flat: every param in order, crossing section and
# component boundaries, skipping a component with nothing to offer, ending on
# [Clear Target]. src/shared/lfo_target_scroll.mjs is the pure half; the host
# wiring is pinned below.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import { landOn, stepOne, step, CLEAR_KEY } from "./src/shared/lfo_target_scroll.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };
const P = (...keys) => keys.map((k) => ({ key: k, label: k }));

const comps = [
  { key: "synth" }, { key: "fx1" }, { key: "fx2" }, { key: "lfo2" }, { key: CLEAR_KEY },
];
const SECTIONS = [
  [{ label: "Filter", params: P("cut", "res") }, { label: "Amp", params: P("att") }],
  [{ label: null, params: P("size", "mix") }],
  [],                                   /* fx2: nothing to offer -- skipped */
  [{ label: null, params: P("depth") }],
];
const sectionsOf = (c) => SECTIONS[c] || [];
const keyAt = (pos) => pos.sec < 0 ? comps[pos.comp].key
  : sectionsOf(pos.comp)[pos.sec].params[pos.param].key;

/* Walk forward from the first leaf to the end, and back again. */
let at = landOn(comps, sectionsOf, 0, "first");
const fwd = [keyAt(at)];
for (let n; (n = stepOne(comps, sectionsOf, at, 1)); at = n) fwd.push(keyAt(n));
const wantF = ["cut", "res", "att", "size", "mix", "depth", CLEAR_KEY];
if (fwd.join(",") !== wantF.join(",")) bad("forward walk: " + fwd + " want " + wantF);
if (stepOne(comps, sectionsOf, at, 1) !== null) bad("past the end should be null");

const back = [keyAt(at)];
for (let n; (n = stepOne(comps, sectionsOf, at, -1)); at = n) back.push(keyAt(n));
if (back.join(",") !== wantF.slice().reverse().join(",")) bad("backward walk: " + back);

/* Sections are kept: crossing Filter -> Amp changes section, not component. */
const res = landOn(comps, sectionsOf, 0, "stored", "res");
const att = stepOne(comps, sectionsOf, res, 1);
if (att.comp !== 0 || att.sec !== 1 || att.param !== 0) bad("Filter end -> Amp start: " + JSON.stringify(att));

/* landOn: stored routing, last, clear, and an empty component. */
if (keyAt(landOn(comps, sectionsOf, 0, "stored", "att")) !== "att") bad("landOn stored");
if (keyAt(landOn(comps, sectionsOf, 0, "last")) !== "att") bad("landOn last");
if (keyAt(landOn(comps, sectionsOf, 0, "stored", "nope")) !== "cut") bad("unknown stored should land first");
if (landOn(comps, sectionsOf, 2, "first") !== null) bad("an empty component has no leaf");
const clr = landOn(comps, sectionsOf, 4, "first");
if (!clr || clr.sec !== -1) bad("clear is a leaf of its own");

/* step(n) stops at an end rather than failing. */
const far = step(comps, sectionsOf, landOn(comps, sectionsOf, 0, "first"), 99);
if (keyAt(far) !== CLEAR_KEY) bad("a big step should stop on the last item, got " + keyAt(far));
const two = step(comps, sectionsOf, landOn(comps, sectionsOf, 0, "first"), 3);
if (keyAt(two) !== "size") bad("step 3 from cut should be size, got " + keyAt(two));

if (fail) process.exit(1);
'

fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
ui=src/shadow/shadow_ui.js
grep -q "import \* as LFO_SCROLL from '/data/UserData/schwung/shared/lfo_target_scroll.mjs';" "$ui" \
  || bad "shadow_ui does not use lfo_target_scroll.mjs"
grep -q 'if (isLfoTargetView(view)) {' "$ui" || bad "a knob in the target picker is not routed to the scroll"
grep -q '_ctx.turnParamDoor' "$ui" || bad "the grid cannot hand a Target turn to the host"
grep -q 'turnDoor:' src/shadow/shadow_ui_param_pages.mjs || bad "the param-pages glue does not forward io.turnDoor"
grep -q 'io.turnDoor(fullKey(key), direction, slot' src/shared/param_pages/page_controller.mjs \
  || bad "the controller does not offer a door turn to the host"
if grep -q 'targetOptions:' "$ui"; then bad "Target is a knob enum again -- it is the picker door"; fi

[ $fail -eq 0 ] && echo "PASS: the target picker walks as one list under a knob, as a hierarchy under the jog"
exit $fail
