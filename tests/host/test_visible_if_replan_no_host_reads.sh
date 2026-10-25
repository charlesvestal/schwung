#!/usr/bin/env bash
# Turning an enum that is a visible_if CONDITION KEY must cost what turning a
# plain enum costs.
#
# REPORTED FROM THE DEVICE: on lost-and-found (sync gates tempo, routing gates
# spill), genloss (classic gates hysteresis) and mood, a gating enum "skips
# options, flickers and snaps back" while plain enums on the same pages step
# cleanly.
#
# MEASURED, 2026-10-22. The values were never corrupted: the raw encoder stream
# captured in the shim (spi_midi_log_on) replayed through knob_engine gives the
# same sequence the device announced, and a strictly one-direction INJECTED turn
# came out monotonic at 38/s, ~170/s and a jittery human pace. What WAS
# gate-specific was the frame rate -- 56 fps -> 25-33 fps for the whole turn,
# against ~55 fps for an un-gated enum turned by the same hand -- and OTLP put
# the difference in the JS tick: 22.7 ms mean against 13.5 ms. Feedback that lags
# by a frame and a half, on exactly the controls that also re-lay-out the page,
# is what reads as a value that will not stay put.
#
# THE CAUSE: every visible_if re-plan called io.trailingMenus(), and the host's
# componentTrailingMenus() performs a BLOCKING `<prefix>:state` read (6-7 ms on
# lost-and-found) to draw the My Presets row. A gating enum re-plans on every
# write that changes it, so a turn paid a state-blob round trip on most ticks.
# Nothing a visible_if flip can change lives in those rows -- they belong to the
# component, not to a param -- so a visibility re-plan reuses the menus the
# last load()/refreshTrailing() fetched.
#
# Driven through a real controller: the property is a COUNT of host calls
# during a turn, with a positive control that the turn really re-planned.
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v node >/dev/null 2>&1 || { echo "FAIL: node required"; exit 1; }

node --input-type=module -e '
import { createController } from "./src/shared/param_pages/page_controller.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "  ok  " : "FAIL ") + " -- " + m); if (!c) fail++; };

/* lost-and-found Setup level, verbatim in shape. */
const PARAMS = [
  { key: "sync", name: "Sync", type: "enum", options: ["Host", "Manual", "Off"] },
  { key: "tempo", name: "Tempo", type: "float", min: 40, max: 300, step: 0.5 },
  { key: "trails", name: "Trails", type: "enum", options: ["Off", "On"] },
  { key: "dry", name: "Dry", type: "float", min: 0, max: 1, step: 0.01 },
];
const HIER = { modes: null, levels: { root: {
  name: "Setup",
  knobs: ["sync", "tempo", "trails", "dry"],
  params: ["sync", { key: "tempo", visible_if: { param: "sync", equals: "Manual" } }, "trails", "dry"],
} } };

const plug = { sync: "Host", tempo: "120", trails: "On", dry: "1.0" };
let clock = 1000, evals = 0, menuCalls = 0, presetName = "Init";
const ctl = createController({
  getParam: (k) => {
    const b = String(k).replace(/^[^:]+:/, "");
    if (b === "ui_hierarchy") return JSON.stringify(HIER);
    if (b === "chain_params") return JSON.stringify(PARAMS);
    return b in plug ? plug[b] : "";
  },
  setParam: (k, v) => { plug[String(k).replace(/^[^:]+:/, "")] = String(v); return true; },
  /* The host hook under test. On the device each call is a blocking state read. */
  trailingMenus: () => { menuCalls++; return [{ name: "My Presets", entries: [{ label: "Preset", value: presetName, action: "up_load" }] }]; },
  announce: () => {},
  now: () => clock,
});
const visible = (c) => { evals++; const v = ctl.state.values[c.param] ?? plug[c.param]; return String(v) === String(c.equals); };
ctl.load({ slot: 0, component: "fx1", prefix: "fx1", visible });
for (let i = 0; i < 5; i++) { clock += 23; ctl.tick(); }

const slot = ctl.page.keys.indexOf("sync");
ok(slot === 0, "the Setup page is up with Sync on knob 1 (keys " + JSON.stringify(ctl.page.keys) + ")");
const hasTrailing = () => ctl.state.pages.some((p) => p.trailing && p.name === "My Presets");
ok(hasTrailing(), "the trailing My Presets page is planned at load");

/* A turn that crosses every option repeatedly, 3 detents per ~23 ms tick. */
const loadCalls = menuCalls;
evals = 0;
const seen = new Set();
const dirs = [];
for (let r = 0; r < 4; r++) { for (let i = 0; i < 12; i++) dirs.push(1); for (let i = 0; i < 12; i++) dirs.push(-1); }
dirs.forEach((d, i) => {
  clock += 7; ctl.onKnobTurn(slot, d, clock);
  if (i % 3 === 2) { clock += 2; ctl.tick(); seen.add(ctl.page.keys.join()); }
});
for (let i = 0; i < 10; i++) { clock += 23; ctl.tick(); }

/* Positive control: the turn genuinely re-planned, both ways. Without this a
 * controller that never re-plans would pass the count below trivially. */
ok(evals > 0, "control: the turn ran visible_if planning passes (" + evals + " evaluations)");
ok([...seen].some((k) => k.includes("tempo")) && [...seen].some((k) => !k.includes("tempo")),
   "control: Tempo appeared and disappeared during the turn (" + seen.size + " distinct page shapes)");

ok(menuCalls === loadCalls,
   "a visibility re-plan never asks the host for its trailing menus (" + (menuCalls - loadCalls) + " calls during the turn)");
ok(hasTrailing(), "the trailing My Presets page survives every re-plan");

/* The menus are not frozen: the explicit refresh still reaches the host and its rows. */
presetName = "Saved";
ctl.refreshTrailing();
ok(menuCalls === loadCalls + 1, "refreshTrailing() still asks the host");
const mp = ctl.state.pages.find((p) => p.trailing && p.name === "My Presets");
ok(!!mp && JSON.stringify(mp).includes("Saved"), "...and its rows reach the page");

/* ...and a later re-plan keeps the REFRESHED rows, not the ones from load. */
clock += 7; for (let i = 0; i < 8; i++) { clock += 7; ctl.onKnobTurn(slot, 1, clock); } clock += 23; ctl.tick();
const mp2 = ctl.state.pages.find((p) => p.trailing && p.name === "My Presets");
ok(!!mp2 && JSON.stringify(mp2).includes("Saved"), "a re-plan after a refresh keeps the refreshed rows");

/*
 * A WRITE THAT REPEATS THE VALUE PLANS NOTHING.
 *
 * The enum gate is four detents per option, so most writes in a turn carry the
 * value already planned against ("Host Host Host Manual ..."). Each of them
 * used to buy a full planPages pass anyway -- 40 passes for 10 value changes in
 * a 120-detent sweep. Measured on the device (OTLP, instrumented flushReplan):
 * 9.5 ms per tick spent re-planning, 340 ms of every second of the turn, which
 * is what took the grid from ~56 fps to ~36. QuickJS on the Move runs the
 * planner ~50x slower than node does here, so the count is the observable.
 */
{
  /* Settle on a known value with nothing owed. */
  for (let i = 0; i < 12; i++) { clock += 7; ctl.onKnobTurn(slot, -1, clock); }
  for (let i = 0; i < 4; i++) { clock += 23; ctl.tick(); }
  ok(plug.sync === "Host", "settled at Host (" + plug.sync + ")");

  /* Three detents the 4-detent gate absorbs: three writes of the SAME value,
   * each in its own tick so the throttle lets every one through. */
  evals = 0; let writes = 0;
  for (let i = 0; i < 3; i++) {
    const w = ctl.onKnobTurn(slot, -1, clock += 30);
    if (w !== null && w !== undefined) writes++;
    clock += 23; ctl.tick();
  }
  ok(writes === 3, "control: the absorbed detents still wrote (" + writes + " writes)");
  ok(evals === 0, "repeated writes of the planned value cost NO planning pass (" + evals + " evaluations)");

  /* Count passes against changes over a back-and-forth sweep. */
  let passes = 0, changes = 0, prev = plug.sync;
  for (let r = 0; r < 3; r++) for (const d of [-1, 1]) for (let i = 0; i < 20; i++) {
    clock += 8; ctl.onKnobTurn(slot, d, clock);
    if (i % 3 === 2) {
      clock += 3; evals = 0; ctl.tick(); if (evals) passes++;
      if (plug.sync !== prev) { changes++; prev = plug.sync; }
    }
  }
  ok(changes > 0, "control: the sweep changed the value (" + changes + " changes)");
  ok(passes <= changes, "planning passes do not exceed value changes (" + passes + " passes, " + changes + " changes)");
}

if (fail) { console.log(fail + " failure(s)"); process.exit(1); }
'
