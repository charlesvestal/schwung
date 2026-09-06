#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A MODULE MAY DECLARE A PARAM LIVE, AND ITS PICTURE MUST FOLLOW IT.
#
# `isModulated` asks the chain's modulation system, which knows about LFOs and
# macros. It cannot know that a synth drives its own vowel from pad pressure --
# so the widget drawing that vowel sat frozen at whatever the knob last said
# while the sound moved underneath it. Reported from hardware as "the mouth
# shape should animate with pressure".
#
# `"live": true` on the param says the value moves on its own. It buys the same
# treatment a modulated key gets: the effective value refreshed EVERY TICK
# rather than on the value rotation, which on an eight-knob page lands about
# four times a second -- an animation drawn from that is a slideshow.
#
# The other half is that the picture must actually SEE it. `values` stays the
# base (it is what a turn edits); graphics get base merged with effective. The
# movy renderer always did that; render_page did not, so the same widget
# animated on one layout and froze on the other -- and movy is the layout the
# device uses.
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
const { renderPage } = await import(R + "/src/shared/param_pages/render_page.mjs");
const { renderPageMovy } = await import(R + "/src/shared/param_pages/render_page_movy.mjs");
const { planPages } = await import(R + "/src/shared/param_pages/page_plan.mjs");
const { buildMetaIndex } = await import(R + "/src/shared/param_pages/param_meta.mjs");
const { createFramebuffer, drawContext } = await import(R + "/tools/param-pages/harness.mjs");

let fails = 0;
const ok = (c, m) => { if (!c) { console.error("FAIL: " + m); fails++; } };

const HIER = { levels: { root: { label: "M", knobs: ["vowel", "b"],
  params: [{ key: "vowel" }, { key: "b" }] } } };
const CP = [
  { key: "vowel", name: "Vowel", type: "float", min: 0, max: 1, step: 0.01,
    live: true, viz: { kind: "custom:mouth" } },
  { key: "b", name: "B", type: "float", min: 0, max: 1, step: 0.01 },
];

/* ---- the controller keeps the effective value fresh ---- */
clearWidgets();
registerWidget("custom:mouth", { draw() {} });
{
  const reads = [];
  /* base 0.10, but PRESSURE has driven it to 0.90 */
  const store = { ui_hierarchy: JSON.stringify(HIER), chain_params: JSON.stringify(CP),
                  vowel: "0.10", "vowel:base": "0.10", "vowel:effective": "0.90", b: "0.5" };
  let t = 0;
  const ctrl = createController({
    getParam: (k) => { const b = String(k).replace(/^synth:/, ""); reads.push(b);
                       return store[b] === undefined ? null : store[b]; },
    setParam: () => true, announce: () => {}, now: () => (t += 16),
  });
  ctrl.load({ prefix: "synth" });
  ctrl.setLayout(LAYOUT_MOVY);
  for (let i = 0; i < 30; i++) ctrl.tick();

  ok(reads.indexOf("vowel:effective") >= 0,
     "a live param has its effective value read");

  /* It must be read REPEATEDLY, not once -- that is the difference between an
   * animation and a value that happens to be right when you arrive. */
  const before = reads.filter((k) => k === "vowel:effective").length;
  for (let i = 0; i < 10; i++) ctrl.tick();
  const after = reads.filter((k) => k === "vowel:effective").length;
  ok(after - before >= 5,
     `a live param is re-read every tick, not on the rotation (got ${after - before} in 10 ticks)`);

  /* And the widget must be handed the EFFECTIVE value, not the base. */
  let seen = null;
  clearWidgets();
  registerWidget("custom:mouth", { draw(ctx, o) { seen = o.values.vowel; } });
  for (let i = 0; i < 6; i++) ctrl.tick();
  ctrl.render({ fillRect() {}, print() {}, textWidth: (s2) => String(s2).length * 4,
                line() {}, drawCircle() {}, fillCircle() {}, drawArc() {},
                width: 128, height: 64 });
  ok(seen === "0.90",
     `the widget draws the effective value, not the knob (got ${JSON.stringify(seen)})`);
}

/* ---- BOTH renderers merge, or a widget animates on one layout only ---- */
{
  const plan = planPages({ hierarchy: HIER, chainParams: CP });
  const page = plan.pages.find((p) => (p.keys || []).indexOf("vowel") >= 0);
  const mi = buildMetaIndex({ hierarchy: HIER, chainParams: CP });
  const args = {
    page, metaIndex: mi, values: { vowel: "0.10", b: "0.5" },
    modValues: { vowel: "0.90" },
    title: "M", pageIndex: 0, pageCount: 1, touched: -1,
    rect: { x: 0, y: 0, w: 128, h: 64 },
    viz: [{ kind: "custom:mouth", group: null, roles: { value: "vowel" },
            keys: ["vowel"], slotStart: 0, slotSpan: 1, source: "declared" }],
  };
  for (const [name, fn] of [["render_page", renderPage], ["render_page_movy", renderPageMovy]]) {
    let got = null;
    clearWidgets();
    registerWidget("custom:mouth", { draw(ctx, o) { got = o.values.vowel; } });
    fn(drawContext(createFramebuffer(128, 64)), args);
    ok(got === "0.90", `${name} hands the widget the effective value (got ${JSON.stringify(got)})`);
  }
}

if (fails) { console.error(fails + " failure(s)"); process.exit(1); }
console.log("PASS: a live param is re-read every tick and both renderers draw it");
'
