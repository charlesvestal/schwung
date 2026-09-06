#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A MODULE MAY NAME OFF-PAGE VALUES ITS WIDGET NEEDS.
#
# A widget cannot read: it is handed the page's value map. So a picture that
# depends on a value with no cell on this page had exactly one route -- give
# that value a knob. A real module did, and the result was a read-only cell
# occupying one of eight slots on a 128x64 screen to draw a 17x15 blob nobody
# could interpret, purely so the cell beside it could see the number.
#
# `extraKeys` already existed for this shape (detectSample uses it for an
# off-page filepath) and the controller already adds them to the value
# rotation. This lets a MODULE declare them, on the viz object:
#
#     "viz": { "kind": "custom:mouth", "extra_keys": ["face"] }
#
# What must hold: they reach the group, they are actually READ by the lane, the
# value lands in the map a widget is handed, and the count is capped -- a module
# asking for twenty keys would spend the page's whole read budget on one cell.
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
const { resolveViz } = await import(R + "/src/shared/param_pages/viz.mjs");
const { buildMetaIndex } = await import(R + "/src/shared/param_pages/param_meta.mjs");
const { registerWidget, clearWidgets } = await import(R + "/src/shared/param_pages/widget_registry.mjs");
const { createController, LAYOUT_MOVY } = await import(R + "/src/shared/param_pages/page_controller.mjs");

let fails = 0;
const ok = (c, m) => { if (!c) { console.error("FAIL: " + m); fails++; } };

const HIER = { levels: { root: { label: "M",
  knobs: ["vowel", "level"],
  params: [{ key: "vowel" }, { key: "level" }, { key: "face" }] } } };
const CP = [
  { key: "vowel", name: "Vowel", type: "float", min: 0, max: 1, step: 0.01,
    viz: { kind: "custom:mouth", extra_keys: ["face"] } },
  { key: "level", name: "Level", type: "float", min: 0, max: 1, step: 0.01 },
  { key: "face",  name: "Who",   type: "int",   min: 0, max: 11, step: 1, access: "read" },
];

/* ---- it reaches the resolved group ---- */
clearWidgets();
registerWidget("custom:mouth", { draw() {} });
const mi = buildMetaIndex({ hierarchy: HIER, chainParams: CP });
const r = resolveViz({ keys: ["vowel", "level", null, null, null, null, null, null], metaIndex: mi });
const g = r.groups.find((x) => x.kind === "custom:mouth");
ok(!!g, "the custom group resolves");
ok(g && Array.isArray(g.extraKeys) && g.extraKeys[0] === "face",
   "the modules extra_keys reaches the group as extraKeys");

/* An UNREGISTERED kind must not smuggle its extra keys into the rotation --
 * the group is abandoned entirely, so there is nothing to read for. */
clearWidgets();
const r2 = resolveViz({ keys: ["vowel", "level", null, null, null, null, null, null], metaIndex: mi });
ok(!r2.groups.some((x) => Array.isArray(x.extraKeys) && x.extraKeys.indexOf("face") >= 0),
   "an unavailable custom kind contributes no extra keys");

/* ---- the CONTROLLER actually reads it ---- */
clearWidgets();
registerWidget("custom:mouth", { draw() {} });
{
  const reads = [];
  /* The controller READS its contract from the device, so serve it. */
  const store = {
    ui_hierarchy: JSON.stringify(HIER),
    chain_params: JSON.stringify(CP),
    vowel: "0.5", level: "1", face: "7",
  };
  let t = 0;
  const ctrl = createController({
    getParam: (k) => { const b = String(k).split(":").pop(); reads.push(b);
                       return store[b] === undefined ? null : store[b]; },
    setParam: () => true,
    announce: () => {},
    now: () => (t += 16),
  });
  ctrl.load({ prefix: "synth" });
  ctrl.setLayout(LAYOUT_MOVY);
  for (let i = 0; i < 60; i++) ctrl.tick();

  ok(reads.indexOf("face") >= 0,
     "the read lane fetches the declared extra key (it has no cell of its own)");

  /*
   * AND IT REACHES THE WIDGET. Asserted by DRAWING, not by peeking at a
   * private map: the controller exposes no values accessor, and a test that
   * quietly skips when one is missing is a test that measures nothing. The
   * widget records what it was handed.
   */
  let handed = null;
  clearWidgets();
  registerWidget("custom:mouth", { draw(ctx, o) { handed = o.values; } });
  for (let i = 0; i < 12; i++) ctrl.tick();
  ctrl.render({ fillRect() {}, print() {}, textWidth: (t) => String(t).length * 4,
                line() {}, drawCircle() {}, fillCircle() {}, drawArc() {},
                width: 128, height: 64 });
  ok(handed !== null, "the custom widget was actually drawn");
  ok(handed && handed.face === "7",
     "and the off-page value is in the map it was handed");
}

/* ---- capped ---- */
{
  const many = { key: "vowel", name: "V", type: "float", min: 0, max: 1,
                 viz: { kind: "custom:mouth", extra_keys: ["a","b","c","d","e","f"] } };
  const mi2 = buildMetaIndex({ hierarchy: HIER, chainParams: [many, CP[1], CP[2]] });
  const r3 = resolveViz({ keys: ["vowel","level",null,null,null,null,null,null], metaIndex: mi2 });
  const g3 = r3.groups.find((x) => x.kind === "custom:mouth");
  ok(g3 && g3.extraKeys.length <= 4,
     "extra keys are capped, so one cell cannot eat the pages read budget");
}

/* ---- junk is ignored, not thrown on ---- */
for (const bad of [null, "face", 42, [1, 2], [""], {}]) {
  const cp = [{ key: "vowel", name: "V", type: "float", min: 0, max: 1,
                viz: { kind: "custom:mouth", extra_keys: bad } }, CP[1]];
  const mix = buildMetaIndex({ hierarchy: HIER, chainParams: cp });
  let threw = false;
  try { resolveViz({ keys: ["vowel","level",null,null,null,null,null,null], metaIndex: mix }); }
  catch (e) { threw = true; }
  ok(!threw, "a malformed extra_keys is ignored rather than thrown on");
}

if (fails) { console.error(fails + " failure(s)"); process.exit(1); }
console.log("PASS: a module can name off-page values its widget needs, and they are read");
'
