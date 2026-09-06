#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Choosing a preset changes EVERY parameter, so every cached value is stale.
#
# stepPreset wrote the index and re-read the NAME, and nothing else. Nothing
# reads on the draw path -- values arrive on touch-down, on the rotation or in
# the entry warm -- and onKnobTurn steps FROM the cached value. So the first
# knob move after choosing a preset departed from the PREVIOUS presets number
# and wrote that back over the one just loaded. Reported from hardware as
# "after changing a preset, when i turn a knob it turns from the last value,
# not the presets".
#
# It is the same defect the snapshot recall has, reached by another door.
# revalue() existed for the recall, and its own comment asserted a preset load
# did not need it "because it comes back through the browser, and the re-entry
# replans and re-warms". That is true of the User Presets VIEW, which really
# does re-enter the grid. It is false of an in-grid preset PAGE, which never
# leaves -- and an in-grid page is what a module gets by declaring list_param.
#
# NO APOSTROPHES BELOW THIS LINE inside the node script: it is a single-quoted
# bash string, and one apostrophe ends it early with an error pointing nowhere
# near the real line.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
const R = process.cwd();
const PC = await import(R + "/src/shared/param_pages/page_controller.mjs");
const { PAGE_PRESET } = await import(R + "/src/shared/param_pages/page_plan.mjs");

let fails = 0;
const ok = (c, m) => { if (!c) { console.error("FAIL: " + m); fails++; } };

/* A module shaped like Hinge: one knobs level that also owns a preset list. */
const HIER = { levels: { root: {
  name: "Main",
  list_param: "preset", count_param: "preset_count", name_param: "preset_name",
  knobs: ["ratio", "bright"],
  params: [{ key: "ratio" }, { key: "bright" }] } } };
const CP = [
  { key: "ratio",  name: "Ratio",  type: "float", min: 0, max: 1, step: 0.01 },
  { key: "bright", name: "Bright", type: "float", min: 0, max: 1, step: 0.01 },
  { key: "preset", name: "Preset", type: "int", min: 0, max: 31 },
];

/* Two presets that differ in every macro, which is what a preset IS. */
const BANK = [ { ratio: "0.20", bright: "0.20" }, { ratio: "0.80", bright: "0.80" } ];
const dev = { preset: "0", preset_count: "2", preset_name: "one", ...BANK[0] };
const writes = [];
const io = {
  getParam: (k) => {
    const key = k.replace(/^synth:/, "");
    if (key === "ui_hierarchy") return JSON.stringify(HIER);
    if (key === "chain_params") return JSON.stringify(CP);
    if (key === "is_loading") return "0";
    if (key === "module") return "hinge";
    return key in dev ? dev[key] : null;
  },
  setParam: (k, v) => {
    const key = k.replace(/^synth:/, "");
    dev[key] = String(v);
    writes.push([key, String(v)]);
    /* The module loads the preset: every other parameter changes at once. */
    if (key === "preset") {
      const b = BANK[Number(v)] || BANK[0];
      for (const kk of Object.keys(b)) dev[kk] = b[kk];
      dev.preset_name = Number(v) === 0 ? "one" : "two";
    }
  },
  announce: () => {}, now: () => Date.now(),
};

const c = PC.createController(io);
c.load({ slot: 0, component: "synth", prefix: "synth" });
c.setLayout("movy");
const spin = (n) => { for (let i = 0; i < n; i++) c.tick(); };
spin(60);

const knobPage = c.pages.findIndex((p) => p.kind !== PAGE_PRESET && (p.keys || []).indexOf("ratio") >= 0);
const presetPage = c.pages.findIndex((p) => p.kind === PAGE_PRESET);
ok(knobPage >= 0, "a knobs page carrying ratio was planned");
ok(presetPage >= 0, "declaring list_param plans an in-grid preset page");
if (fails) process.exit(1);

/* 1. USE the knob first, the way anyone would before reaching for a preset.
 *    Touching latches a base value in knobStates, and that latch is what the
 *    entry warm does NOT refresh -- s.values is only half the state. */
c.goToPage(knobPage); spin(40);
c.onKnobTouch(0, true);
c.onKnobTurn(0, +1, Date.now());
spin(10);
c.onKnobTouch(0, false);
spin(10);

/* 2. choose the other preset, which the fake module loads: ratio becomes 0.80 */
c.goToPage(presetPage); spin(10);
/* The preset page is a DOOR: the jog pages out until you click into it, so
 * scrolling past a preset list never loads anything by accident. */
c.onClick(); spin(5);
writes.length = 0;
c.onJog(1);
spin(10);
ok(dev.preset === "1", "the new index was written, got " + dev.preset);
ok(dev.ratio === "0.80", "harness: the module loaded the preset");

/* 3. back to the knobs page and turn knob 1 ONE detent up */
c.goToPage(knobPage); spin(40);
writes.length = 0;
c.onKnobTouch(0, true);
c.onKnobTurn(0, +1, Date.now());
spin(20);
c.onKnobTouch(0, false);
const w = writes.filter(([k]) => k === "ratio").pop();
ok(!!w, "turning the knob wrote ratio");
if (w) {
  const v = Number(w[1]);
  /* One step of 0.01 from the LOADED value, not from the one before it. */
  ok(v > 0.78 && v < 0.83,
     "ratio was written as " + v.toFixed(3) + "; expected ~0.81, a step from the " +
     "preset value 0.80. A value near 0.21 means the knob stepped from the " +
     "PREVIOUS presets cached number and has just overwritten the preset.");
}

if (fails) process.exit(1);
console.log("PASS: choosing a preset drops the cached values, so the next knob turn steps from it");
'
