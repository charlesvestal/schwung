#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A FILE CELL, TURNED, CHANGES THE FILE WITHIN ITS FOLDER; A JOG CLICK
# NAVIGATES.
#
# The file browser is a hierarchy the jog walks (hold + click, click into
# folders, Back out). Turning the cell instead lists the files in the current
# file's folder -- files only -- and the knob scrolls them; release loads,
# Back cancels. The controller half: a turn on a filepath door reaches the
# host with the key and its meta (io.turnDoor), and the host writes the pick
# back through commitValue so the cell updates at once and writes ONCE.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import { createController } from "./src/shared/param_pages/page_controller.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

const CHAIN = [
  { key: "sample", name: "Sample", type: "filepath", root: "/data/UserData", filter: ".wav" },
  { key: "gain", name: "Gain", type: "float", min: 0, max: 1 },
];
const HIER = { modes: null, levels: { root: { label: "T", knobs: ["sample", "gain"],
  params: CHAIN.map((p) => ({ key: p.key })) } } };
const state = { clock: 1000, store: { sample: "/data/UserData/a/kick.wav", gain: "0.5" }, writes: [], doors: [] };
const io = {
  getParam: (k) => {
    const b = String(k).replace(/^[^:]+:/, "");
    if (b === "ui_hierarchy") return JSON.stringify(HIER);
    if (b === "chain_params") return JSON.stringify(CHAIN);
    return b in state.store ? state.store[b] : "";
  },
  setParam: (k, v) => {
    const b = String(k).replace(/^[^:]+:/, "");
    state.writes.push(b + "=" + v);
    state.store[b] = String(v);
  },
  turnDoor: (fullKey, dir, knob, held, info) => { state.doors.push({ fullKey, dir, knob, held, info }); return true; },
  announce: () => {},
  now: () => state.clock,
};
const ctl = createController(io);
ctl.load({ prefix: "synth" });
for (let i = 0; i < 12; i++) ctl.tick();
const slot = ctl.page.keys.indexOf("sample");

ctl.onKnobTouch(slot, true);
state.clock += 40; ctl.onKnobTurn(slot, 1, state.clock);
const d = state.doors[0];
if (!d) bad("turning a file cell did not reach the host");
else {
  if (d.fullKey !== "synth:sample") bad("door fullKey: " + d.fullKey);
  if (!d.info || d.info.key !== "sample") bad("the host needs the bare key to commit through");
  if (!d.info || !d.info.meta || d.info.meta.type !== "filepath") bad("the host needs the meta (root, filter)");
  if (d.held !== true) bad("the host must know the knob is held (a release is coming)");
}
if (state.writes.length) bad("a turn on a file cell wrote directly: " + state.writes);

ctl.onKnobTouch(slot, false);
ctl.commitValue("sample", "/data/UserData/a/snare.wav");
const w = state.writes.filter((x) => x.startsWith("sample="));
if (w.length !== 1 || w[0] !== "sample=/data/UserData/a/snare.wav") bad("commitValue should write once: " + w);
if (ctl.state.values.sample !== "/data/UserData/a/snare.wav") bad("the cell must show the pick at once");

if (fail) process.exit(1);
'

fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
ui=src/shadow/shadow_ui.js
grep -q 'FILE_FLAT: "fileflat"' "$ui" || bad "no file-list view"
grep -q 'fileFlatEnter(slot, fullKey, knob, held, info)' "$ui" || bad "a file turn is not routed to the file list"
grep -q 'it && it.kind === "file"' "$ui" || bad "the file list must hold FILES only (no folders, no ..)"
grep -q 'fileFlatKnob === knobIndex && view === VIEWS.FILE_FLAT' "$ui" || bad "releasing the knob does not load"
grep -q 'commitParamPagesValue(fileFlatKey, f.path)' "$ui" || bad "the pick is not written through the grid controller"
grep -q 'export function commitParamPagesValue' src/shadow/shadow_ui_param_pages.mjs || bad "no glue to commit through the controller"

[ $fail -eq 0 ] && echo "PASS: a turned file cell lists its folder and loads on release; the jog still navigates"
exit $fail
