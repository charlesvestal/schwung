#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A knob's destination list, RUN rather than grepped.
#
# node --check is the wrong gate for shadow_ui.js -- it passes JavaScript that
# kills the UI at eval -- and a source pin cannot tell a row list that is right
# from one that is one row short. So the real functions are lifted and driven.
#
# It also pins MAX_KNOB_DESTS against the C header. A JS copy that drifted high
# would offer a destination the DSP silently refuses, which reads as a dead row
# rather than as an error.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
const hdr = readFileSync("src/modules/chain/dsp/chain_internal.h", "utf8");

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const check = (c, m) => c ? console.log("  ok  " + m) : fail(m);

/* ---- the cap must match the header ---- */
const cHit = hdr.match(/#define\s+MAX_KNOB_DESTS\s+(\d+)/);
const jsHit = src.match(/const\s+MAX_KNOB_DESTS\s*=\s*(\d+)\s*;/);
if (!cHit) fail("MAX_KNOB_DESTS is gone from chain_internal.h");
if (!jsHit) fail("MAX_KNOB_DESTS is gone from shadow_ui.js");
if (cHit && jsHit) {
  check(cHit[1] === jsHit[1],
        `the JS destination cap (${jsHit && jsHit[1]}) matches the C one (${cHit && cHit[1]})`);
}

/* ---- lift the list, its labels and its gestures as ONE block: they share
       module state (knobDestIndex, knobDestEditing), and lifted separately
       each would close over its own copy and none would see the others. ---- */
const NAMES = ["knobDestRows", "knobDestRowLabel", "knobDestRowValue",
               "knobDestWriteWindow", "knobDestListTurn", "knobDestListClick",
               "knobRowLabel", "knobDestIsRanged", "knobWindowLabel",
               "getKnobAssignmentLabel"];
let body = "";
for (const n of NAMES) {
  const at = src.indexOf("function " + n + "(");
  if (at < 0) { fail(n + " is gone"); continue; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("no end for " + n); continue; }
  body += src.slice(at, end + 2) + "\n";
}
if (failures) process.exit(1);

const writes = [];
const world = () => {
  const state = {
    knobEditorIndex: 0,
    knobEditorSlot: 0,
    knobDestIndex: 0,
    knobDestEditing: null,
    knobEditorDests: [],
    knobEditorAssignments: [],
    knobEditorDestIndex: 0
  };
  const mk = new Function(
    "MAX_KNOB_DESTS", "setSlotParam", "announce", "announceMenuItem",
    "enterKnobParamPicker", "setView", "VIEWS", "loadKnobAssignments",
    "fetchKnobMappings", "invalidateKnobContextCache", "state",
    `
    let { knobEditorIndex, knobEditorSlot, knobDestIndex, knobDestEditing,
          knobEditorDests, knobEditorAssignments, knobEditorDestIndex } = state;
    let needsRedraw = false;
    ${body}
    return {
      rows: () => knobDestRows(),
      label: (r) => knobDestRowLabel(r),
      value: (r) => knobDestRowValue(r),
      knobRowLabel: (i) => knobRowLabel(i),
      turn: (d) => knobDestListTurn(d),
      click: () => knobDestListClick(),
      setDests: (d) => { knobEditorDests = d; },
      cursor: () => knobDestIndex,
      setCursor: (i) => { knobDestIndex = i; },
      editing: () => knobDestEditing,
      dests: () => knobEditorDests,
      destIndex: () => knobEditorDestIndex
    };`);
  return mk(4,
    (slot, key, val) => writes.push(key + "=" + val),
    () => {}, () => {}, () => { writes.push("PICKER"); },
    () => {}, { KNOB_EDITOR: "knobedit", KNOB_DEST_LIST: "knobdests" },
    () => {}, () => {}, () => {}, state);
};

const THREE = [
  { target: "synth", param: "cutoff", lo: 0, hi: 1 },
  { target: "fx1", param: "mix", lo: 0.2, hi: 0.8 },
  { target: "fx2", param: "drive", lo: 1, hi: 0 }
];

/* ---- the rows ---- */
let w = world();
w.setDests([THREE]);
let rows = w.rows();
check(rows.length === 3 * 3 + 2,
      "three destinations give three rows each, plus Add and Remove (" + rows.length + ")");
check(rows[0].kind === "dest" && rows[1].kind === "lo" && rows[2].kind === "hi",
      "each destination is followed by its own Lo and Hi");
check(rows[rows.length - 1].kind === "remove", "Remove knob is last");
check(w.value(rows[0]) === "synth: cutoff", "a destination row shows what it drives");
check(w.value(rows[4]) === "20%" && w.value(rows[5]) === "80%", "its window reads as percentages");
check(w.value(rows[7]) === "100%" && w.value(rows[8]) === "0%",
      "an INVERTED window is shown high-to-low, the way it behaves");

/* At the cap there is no Add row -- offering one the DSP would refuse reads as
   a dead row rather than as a limit. */
w = world();
w.setDests([[...THREE, { target: "synth", param: "res", lo: 0, hi: 1 }]]);
check(!w.rows().some((r) => r.kind === "add"),
      "at the destination cap the Add row is gone");

/* ---- the knob list label ---- */
w = world();
w.setDests([THREE]);
check(w.knobRowLabel(0) === "cutoff +2",
      "a multi-destination knob says what it DRIVES, not what it is");
w.setDests([[{ target: "synth", param: "cutoff", lo: 0, hi: 1 }]]);
check(w.knobRowLabel(0) === "synth: cutoff", "one whole-range destination reads exactly as before");
w.setDests([[{ target: "synth", param: "cutoff", lo: 0, hi: 0.5 }]]);
check(w.knobRowLabel(0) === "synth: cutoff ~", "one RANGED destination is marked");

/* ---- adjusting a window applies on every detent ---- */
w = world();
w.setDests([THREE]);
w.setCursor(1);              /* the Lo row of destination 1 */
w.click();
check(w.editing() !== null, "clicking Lo starts adjusting it");
writes.length = 0;
w.turn(5);
check(writes.length === 1 && writes[0].startsWith("knob_1_dest_1_range="),
      "a detent writes the window immediately, not on leaving the row");
check(w.dests()[0][0].lo > 0.049 && w.dests()[0][0].lo < 0.051,
      "1% a detent -- a window is a boundary you land on, not a value you dial");
w.click();
check(w.editing() === null, "clicking again finishes");

/* ---- clicking a destination row re-points THAT destination ---- */
w = world();
w.setDests([THREE]);
w.setCursor(6);              /* destination 3 */
writes.length = 0;
w.click();
check(writes.includes("PICKER"), "clicking a destination opens the picker");
check(w.destIndex() === 2,
      "and seeds it from the destination being edited -- opening destination 3 on "
      + "destination 1 would read as a wrong answer, never as an error");

/* ---- Add ---- */
w = world();
w.setDests([THREE]);
w.setCursor(9);              /* + Add destination */
w.click();
check(w.destIndex() === 3, "Add aims the picker past the end of the list");

if (failures) { console.error("\n" + failures + " check(s) failed"); process.exit(1); }
console.log("\nPASS: a knob'"'"'s destination list");
'
