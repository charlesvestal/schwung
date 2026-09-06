#!/usr/bin/env bash
#
# A SEND's Settings, as the knob grid.
#
# It stayed on the scrolling list for two stated reasons and neither survived.
# MASTER_GRID_PARAMS names "master_fx:" keys directly, so reusing the master
# contract would draw the master bus rows under a send title -- fixed by giving
# a send its own contract. And "two rows is not a grid worth", which the master
# bus contradicts: its own settings grid declares ONE knob param, so a send with
# Return and the A->B feed is the larger page of the two.
#
# What this pins is the wiring that is invisible when wrong:
#
#  - the declared keys are the REAL bus keys, so a knob writes the shim
#  - the component prefix is STRIPPED. The controller composes every read as
#    `${prefix}:${key}`, so a pass-through asks for "send_settings:send1:return"
#    -- a key nobody serves, which answers "" and not an error, so every cell
#    draws a confident zero and every turn writes to nothing. The bus prefix
#    must SURVIVE the strip, which is what the assertion below actually checks.
#  - a write PERSISTS, but does NOT save inline. Dropping persistence entirely
#    is the "takes effect now, gone on reboot" failure documented on
#    masterGridIoFor. Doing it inline is the opposite failure and was reported
#    from hardware: saveSendFxChainConfig walks both buses, reads `modules` and
#    a `:bypassed` per position and writes up to sixteen state files -- ~18 IPC
#    round trips and seventeen flash writes. The jog got away with it because it
#    steps by four and a hand turns it slowly; a knob emits a burst of detents
#    and the return took two to three seconds to follow the hand. So the write
#    marks a flag and the tick flushes the LEVELS FILE ALONE.
set -euo pipefail

cd "$(dirname "$0")/../.."
UI="src/shadow/shadow_ui.js"
[ -f "$UI" ] || { echo "FAIL: cannot find $UI"; exit 1; }

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync(process.argv[1], "utf8");
let bad = 0;
const fail = (m) => { console.log("FAIL: " + m); bad = 1; };
const ok = (m) => console.log("  ok  " + m);
const grab = (n) => {
  const m = src.match(new RegExp("^function " + n + "\\([^]*?^}", "m"));
  if (!m) { fail("could not lift " + n + "() out of shadow_ui.js"); process.exit(1); }
  return m[0];
};

/* The REAL max, lifted: a restated 127 here would pass while the two drifted. */
const maxM = src.match(/^const SEND_LEVEL_MAX = (\d+);/m);
if (!maxM) fail("SEND_LEVEL_MAX is gone from shadow_ui.js");
const MAX = maxM ? Number(maxM[1]) : 127;
const stepM = src.match(/^const SEND_LEVEL_STEP = (\d+);/m);
const LIST_STEP = stepM ? Number(stepM[1]) : 4;

function run(busLevelKeys) {
  const body = [
    "const SEND_SETTINGS_COMPONENT = \"send_settings\";",
    "const SEND_LEVEL_MAX = " + MAX + ";",
    "const SEND_LEVEL_ROW_LABELS = { return: \"Return\", to_send2: \"-> Send B\" };",
    "let BUS = { prefix: \"send1:\", busLevelKeys: " + JSON.stringify(busLevelKeys) + " };",
    "function fxBus() { return BUS; }",
    "let reads = [], writes = [], saves = 0;",
    "function getSlotParam(s, k) { reads.push(k); return \"64\"; }",
    "function setSlotParam(s, k, v) { writes.push(k + \"=\" + v); return true; }",
    "function saveSendFxChainConfig() { fullSaves++; }",
    "function saveSendLevels() { saves++; }",
    "let fullSaves = 0; let sendLevelsDirty = false;",
    grab("sendSettingsGridParams"),
    grab("sendSettingsGridIo"),
    "return { params: sendSettingsGridParams(), io: sendSettingsGridIo(),",
    "         reads, writes, saves: () => saves,",
    "         fullSaves: () => fullSaves, dirty: () => sendLevelsDirty };",
  ].join("\n");
  return new Function(body)();
}

/* ---- Send A: two levels ---------------------------------------------- */
{
  const e = run(["return", "to_send2"]);
  const keys = e.params.map((p) => p.key);
  if (JSON.stringify(keys) !== JSON.stringify(["send1:return", "send1:to_send2"])) {
    fail("declared keys are " + JSON.stringify(keys) + " -- they must be the REAL "
         + "bus keys, or a knob writes a param nobody serves");
  }
  for (const p of e.params) {
    if (p.type !== "int" || p.min !== 0 || p.max !== MAX) {
      fail(p.key + " is not an int over 0.." + MAX);
    }
    /* STEP 1, not the list step: the list steps by four because a detent per
       unit is 127 turns of the jog, and a knob has the travel. Same split slot
       Volume already makes between its row and its cell. */
    if (p.step !== 1) {
      fail(p.key + " steps by " + p.step + "; a knob cell steps by 1, not the "
           + "list step of " + LIST_STEP);
    }
  }
  ok("declares the real bus keys as ints over 0.." + MAX + ", stepping by 1");

  const h = JSON.parse(e.io.getParam("send_settings:ui_hierarchy"));
  if (JSON.stringify(h.levels.root.knobs) !== JSON.stringify(keys)) {
    fail("the hierarchy knobs disagree with the declared params");
  }
  const cp = JSON.parse(e.io.getParam("send_settings:chain_params"));
  if (cp.length !== keys.length) fail("chain_params does not match the params");
  ok("serves its own ui_hierarchy and chain_params");

  /* THE PREFIX STRIP. */
  e.io.getParam("send_settings:send1:return");
  const last = e.reads[e.reads.length - 1];
  if (last !== "send1:return") {
    fail("a read asked for " + JSON.stringify(last) + ", expected \"send1:return\" "
         + "-- the component prefix must come off and the BUS prefix must "
         + "survive; dropping neither, or both, reads a key nobody serves");
  }
  ok("strips the component prefix, keeping the bus prefix");

  /* THE WRITE: reaches the bus key, marks dirty, and does NO file I/O. */
  e.io.setParam("send_settings:send1:return", 100);
  const w = e.writes[e.writes.length - 1];
  if (w !== "send1:return=100") {
    fail("a write went to " + JSON.stringify(w) + ", expected \"send1:return=100\"");
  }
  if (!e.dirty()) {
    fail("a grid write left sendLevelsDirty false -- nothing will ever flush it, "
         + "so the change takes effect now and is gone on reboot");
  }
  if (e.fullSaves() !== 0) {
    fail("a grid write called saveSendFxChainConfig() inline (" + e.fullSaves()
         + "x). That walks both buses, reads a :bypassed per position and writes "
         + "up to sixteen state files -- per DETENT. It is what put two to three "
         + "seconds between the knob and the value.");
  }
  if (e.saves() !== 0) {
    fail("a grid write wrote the levels file inline; the tick flushes it");
  }
  /* A BURST costs nothing extra: the flag is idempotent, which is the whole
     point of deferring rather than throttling at the call site. */
  for (let i = 0; i < 50; i++) e.io.setParam("send_settings:send1:return", i);
  if (e.fullSaves() !== 0 || e.saves() !== 0) {
    fail("50 detents produced " + e.fullSaves() + " full saves and " + e.saves()
         + " level writes; a burst must produce none");
  }
  ok("a write reaches the bus key and marks dirty, with no file I/O per detent");

  if (e.io.isModulated("send1:return") !== false) {
    fail("isModulated must answer false without IPC -- a send bus has no LFOs");
  }
  ok("isModulated answers false without a round trip");
}

/* ---- Send B: ONE level, and that is still a page --------------------- */
{
  const e = run(["return"]);
  const h = JSON.parse(e.io.getParam("send_settings:ui_hierarchy"));
  if (h.levels.root.knobs.length !== 1) {
    fail("a one-level send declares " + h.levels.root.knobs.length + " knobs");
  }
  ok("a send with one level is still a grid page (the master bus has one too)");
}

/* ---- no levels at all: NO CONTRACT, not an empty one ----------------- */
{
  const e = run([]);
  if (e.io.getParam("send_settings:ui_hierarchy") !== null) {
    fail("a bus with no levels must answer null, not an empty contract -- an "
         + "empty one is a CLAIM, and the claim is a settings page with nothing "
         + "on it");
  }
  ok("no levels answers null rather than an empty contract");
}

if (bad) process.exit(1);
console.log("PASS");
' "$UI"
