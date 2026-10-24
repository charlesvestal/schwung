#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A WIDGET'S EXTRA KEY MAY BE DECLARED LIVE, AND IS THEN READ EVERY TICK.
#
# A widget can name a value that owns no cell (`viz.extra_keys`): a ducking
# meter, a playhead. Those values are read on the controller's value rotation,
# one stop in about twelve, so a meter fed from one updates a few times a second
# and a short envelope can be over before it is ever read.
#
# `"live": true` on a CELL key already means "this moves on its own: refresh its
# effective value every tick" (see test_live_param_values.sh). The same flag on
# an extra key now buys the same treatment, and the widget is handed the fresh
# value in `values`. An extra key that is not live keeps reading on the rotation.
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
const { createController, LAYOUT_MOVY } = await import(R + "/src/shared/param_pages/page_controller.mjs");
const { registerWidget, clearWidgets } = await import(R + "/src/shared/param_pages/widget_registry.mjs");

let fails = 0;
const ok = (c, m) => { if (!c) { console.error("FAIL: " + m); fails++; } };

const HIER = { levels: { root: { label: "M", knobs: ["level", "b"],
  params: [{ key: "level" }, { key: "b" }] } } };
const CP = [
  { key: "level", name: "Level", type: "float", min: 0, max: 1, step: 0.01,
    viz: { kind: "custom:meter", extra_keys: ["peak", "slow"] } },
  { key: "b", name: "B", type: "float", min: 0, max: 1, step: 0.01 },
  /* off the page, live: the meter value */
  { key: "peak", name: "Peak", type: "float", min: 0, max: 1, access: "read", live: true },
  /* off the page, not live: read on the rotation like any extra */
  { key: "slow", name: "Slow", type: "float", min: 0, max: 1, access: "read" },
];

function make(extra) {
  const reads = [];
  let n = 0;
  const store = { ui_hierarchy: JSON.stringify(HIER), chain_params: JSON.stringify(CP),
                  level: "0.5", b: "0.5", slow: "0.25", peak: "0.10" };
  let t = 0;
  const ctrl = createController(Object.assign({
    getParam: (k) => {
      const key = String(k).replace(/^synth:/, "");
      reads.push(key);
      if (key === "peak:effective") return (0.5 + (++n) / 1000).toFixed(3);   /* moves every read */
      return store[key] === undefined ? null : store[key];
    },
    setParam: () => true, announce: () => {}, now: () => (t += 16),
  }, extra || {}));
  ctrl.load({ prefix: "synth" });
  ctrl.setLayout(LAYOUT_MOVY);
  return { ctrl, reads };
}
const count = (reads, key) => reads.filter((k) => k === key).length;
const frame = { fillRect() {}, print() {}, textWidth: (s2) => String(s2).length * 4,
                line() {}, drawCircle() {}, fillCircle() {}, drawArc() {}, width: 128, height: 64 };

/* ---- a live extra key is re-read every tick ---- */
{
  clearWidgets();
  registerWidget("custom:meter", { draw() {} });
  const { ctrl, reads } = make();
  for (let i = 0; i < 30; i++) ctrl.tick();
  const before = count(reads, "peak:effective");
  for (let i = 0; i < 10; i++) ctrl.tick();
  const got = count(reads, "peak:effective") - before;
  ok(got >= 8, `a live extra key is read about every tick (got ${got} in 10 ticks)`);

  /* an extra key that is not live stays on the rotation */
  const slowBefore = count(reads, "slow");
  const slowEff = count(reads, "slow:effective");
  for (let i = 0; i < 10; i++) ctrl.tick();
  ok(count(reads, "slow") - slowBefore <= 3,
     `a non-live extra key stays on the rotation (got ${count(reads, "slow") - slowBefore} reads in 10 ticks)`);
  ok(count(reads, "slow:effective") === slowEff && slowEff === 0,
     "a non-live extra key never asks for :effective");
}

/* ---- the widget is handed the fresh value ---- */
{
  let seen = [];
  clearWidgets();
  registerWidget("custom:meter", { draw(ctx, o) { seen.push(o.values.peak); } });
  const { ctrl } = make();
  for (let i = 0; i < 30; i++) ctrl.tick();
  ctrl.render(frame);
  const first = seen[seen.length - 1];
  for (let i = 0; i < 3; i++) ctrl.tick();
  ctrl.render(frame);
  const second = seen[seen.length - 1];
  ok(first !== undefined && second !== undefined && Number(first) >= 0.5,
     `the widget draws the effective value, not the rotation base 0.10 (got ${JSON.stringify(first)})`);
  ok(Number(second) > Number(first),
     `the widget sees it move between ticks (${first} then ${second})`);
}

/* ---- viz off: nothing is read for the widget ---- */
{
  clearWidgets();
  registerWidget("custom:meter", { draw() {} });
  const { ctrl, reads } = make({ enableViz: false });
  for (let i = 0; i < 30; i++) ctrl.tick();
  ok(count(reads, "peak:effective") === 0,
     "with graphics disabled a live extra key is not read");
}

if (fails) { console.error(fails + " failure(s)"); process.exit(1); }
console.log("PASS: a live extra key is re-read every tick, a plain one is not");
'
