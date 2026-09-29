#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE LFO TARGETS AS ONE LIST, FOR A KNOB; AS A HIERARCHY, FOR THE JOG.
#
# Turning the grid's Target knob shows every target flat: "None", then each
# component's params in the picker's order, a DIVIDER naming each section.
# The cursor skips dividers, so a turn runs straight from one category into
# the next. Hold + jog click still opens the hierarchical picker.
# src/shared/lfo_target_flat.mjs is the pure half; the host wiring is pinned.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import { buildFlatTargetRows, moveFlatCursor, indexOfFlatRoute, fitDividerLabel } from "./src/shared/lfo_target_flat.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };
const P = (...keys) => keys.map((k) => ({ key: k, label: k.toUpperCase() }));

const comps = [
  { key: "synth", label: "Synth: Mini-JV" }, { key: "fx1", label: "FX 1: Freeverb" },
  { key: "fx2", label: "FX 2: Broken" }, { key: "lfo2", label: "LFO 2" },
  { key: "__clear__", label: "[Clear Target]" },
];
const SECTIONS = [
  [{ label: "Filter", params: P("cut", "res") }, { label: "Amp", params: P("att") }],
  [{ label: null, params: P("size", "mix") }],
  [],
  [{ label: null, params: P("depth") }],
];
const rows = buildFlatTargetRows(comps, (c) => SECTIONS[c] || []);
const shape = rows.map((r) => r.type === "divider" ? "[" + r.label + "]" : r.label).join(" ");
const want = "None [SYN Mini-JV] [Filter] CUT RES [Amp] ATT [FX1 Freeverb] SIZE MIX [LFO 2] DEPTH";
if (shape !== want) bad("rows: " + shape + "\n   want: " + want);
if (rows[0].route.target !== "") bad("None must route nowhere");
const res = rows.find((r) => r.label === "RES");
if (!res || res.route.target !== "synth" || res.route.param !== "res") bad("a row carries its routing");

/* The cursor skips dividers and runs across categories. */
const seen = [];
let i = 0;
for (let k = 0; k < 20; k++) { seen.push(rows[i].label); const j = moveFlatCursor(rows, i, 1); if (j === i) break; i = j; }
if (seen.join(",") !== "None,CUT,RES,ATT,SIZE,MIX,DEPTH") bad("walk: " + seen);
if (moveFlatCursor(rows, i, 1) !== i) bad("the end is a stop");
if (rows[moveFlatCursor(rows, i, -99)].label !== "None") bad("a big step back stops on None");
if (rows[moveFlatCursor(rows, 0, 3)].label !== "ATT") bad("three rows from None is ATT");

if (rows[indexOfFlatRoute(rows, "fx1", "mix")].label !== "MIX") bad("stored routing row");
if (indexOfFlatRoute(rows, "", "") !== 0) bad("no routing is None");
if (indexOfFlatRoute(rows, "fx9", "x") !== 0) bad("an unlisted routing falls back to None");

/* The POSITION is named: two of the same module must not read the same. */
{
  const twin = buildFlatTargetRows(
    [{ key: "fx1", label: "FX 1: Freeverb" }, { key: "fx2", label: "FX 2: Freeverb" },
     { key: "midi_fx1", label: "MIDI FX 1: Arp" }],
    () => [{ label: null, params: P("mix") }]);
  const d = twin.filter((r) => r.type === "divider").map((r) => r.label);
  if (d.join("|") !== "FX1 Freeverb|FX2 Freeverb|MF1 Arp") bad("position tags: " + d.join("|"));
}

/* A caption too wide is cut by MEASURE, never to a trailing hyphen. */
{
  const m = (t) => t.length * 6;
  if (fitDividerLabel("SYN Mini-JV", m, 999) !== "SYN Mini-JV") bad("fits whole when it fits");
  const t = fitDividerLabel("SYN Mini-JV Deluxe Edition", m, 60);
  if (m(t) > 60 || !t.endsWith(".")) bad("cut to fit with a dot: " + t);
  if (fitDividerLabel("SYN Mini-JV", m, 9 * 6) !== "SYN Mini.") bad("no trailing hyphen: " + fitDividerLabel("SYN Mini-JV", m, 9 * 6));
}

if (fail) process.exit(1);
'

fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
ui=src/shadow/shadow_ui.js
grep -q "from '/data/UserData/schwung/shared/lfo_target_flat.mjs';" "$ui" || bad "shadow_ui does not use lfo_target_flat.mjs"
grep -q 'LFO_TARGET_FLAT: "lfotargetflat"' "$ui" || bad "no flat target view"
grep -q 'if (isLfoTargetView(view)) {' "$ui" || bad "a knob in a target view is not routed"
grep -q 'lfoTargetKnobCommit === knobIndex && view === VIEWS.LFO_TARGET_FLAT' "$ui" || bad "releasing the knob does not commit"
grep -q '_ctx.turnParamDoor' "$ui" || bad "the grid cannot hand a Target turn to the host"
grep -q 'turnDoor:' src/shadow/shadow_ui_param_pages.mjs || bad "the param-pages glue does not forward io.turnDoor"
grep -q 'io.turnDoor(fullKey(key), direction, slot' src/shared/param_pages/page_controller.mjs \
  || bad "the controller does not offer a door turn to the host"
if grep -q 'targetOptions:' "$ui"; then bad "Target is a knob enum again -- it is the picker door"; fi

[ $fail -eq 0 ] && echo "PASS: LFO targets: one list with dividers under a knob, the hierarchy under the jog"
exit $fail
