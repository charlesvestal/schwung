#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE REFERENCE MODULE, DRIVEN THROUGH THE REAL PIPELINE.
#
# Every other widget test registers its widget by calling registerWidget()
# directly, which proves the registry works and proves NOTHING about whether a
# module can actually reach it. This one starts from the shipped files --
# src/modules/audio_fx/widget-test/{module.json,canvas.js} -- and drives them
# through the real resolveViz, the real drawVizGroup and the real frame ctx.
#
# Writing it is what found the defect it now guards: widgets were being
# registered from openCanvasPreview, which only runs when the user CLICKS a
# type:"canvas" param. So an in-grid widget did not appear until the fullscreen
# view had been opened once, vanished again on the way out (both open and close
# call resetCanvasState), and never appeared at all for a module with no canvas
# param. A source-level test cannot see call ORDERING, so it passed throughout.
#
# What this cannot cover: QuickJS shadow_load_ui_module, which reads the file on
# device. The evaluation below mirrors it -- run the script, take
# globalThis.canvas_overlay, apply the SAME guard shadow_ui applies -- so the
# contract is exercised even though the file IO is not.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the POC module test" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { resolveViz } from "./src/shared/param_pages/viz.mjs";
import { drawVizGroup } from "./src/shared/param_pages/viz_draw.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { validateContract } from "./src/shared/param_pages/validate_contract.mjs";
import { registerWidget, clearWidgets, isWidgetAvailable }
  from "./src/shared/param_pages/widget_registry.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const DIR = "./src/modules/audio_fx/widget-test";
const mod = JSON.parse(readFileSync(`${DIR}/module.json`, "utf8"));
const caps = mod.capabilities || {};
const CP = caps.chain_params || [];

/* ---- the module declares what the contract requires ---- */
ok(typeof caps.canvas_script === "string", "module.json declares a canvas_script");
const declared = CP.filter((p) => p.viz && typeof p.viz.kind === "string" &&
                                  p.viz.kind.startsWith("custom:"));
ok(declared.length === 1, "exactly one param declares a custom viz kind");
const KIND = declared[0].viz.kind;

/* The validator is happy with it -- no errors, and no missing-script warning. */
const findings = validateContract({ id: mod.id, hierarchy: mod.ui_hierarchy,
                                    chainParams: CP, capabilities: caps }).findings;
ok(findings.filter((f) => f.level === "error").length === 0,
   "the reference module produces no contract errors: " +
   findings.filter((f) => f.level === "error").map((f) => f.rule).join(","));
ok(!findings.some((f) => f.rule === "custom-widget-no-script"),
   "and is not warned about a missing canvas_script");

/* ---- load canvas.js the way shadow_load_ui_module does ---- */
const src = readFileSync(`${DIR}/${caps.canvas_script}`, "utf8");
const g = {};
new Function("globalThis", src)(g);
const overlay = g.canvas_overlay;
ok(!!overlay, "canvas.js sets globalThis.canvas_overlay");

/* THE SAME GUARD shadow_ui applies before registering. */
const registrable = !!(overlay && typeof overlay.drawCell === "function" &&
                       typeof overlay.widgetKind === "string");
ok(registrable, "the overlay satisfies the drawCell + widgetKind guard");
ok(overlay.widgetKind === KIND,
   `the overlays widgetKind matches the chain_params declaration (${KIND})`);
ok(typeof overlay.draw === "function",
   "the same overlay also supplies the fullscreen draw");

/* ---- register it exactly as shadow_ui would, then RESOLVE and DRAW ---- */
clearWidgets();
registerWidget(overlay.widgetKind, {
  draw: overlay.drawCell.bind(overlay),
  nominal: overlay.widgetNominal || null,
});
ok(isWidgetAvailable(KIND), "the widget registers");

const keys = ["level", null, null, null, null, null, null, null];
const metaIndex = buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: CP });
const r = resolveViz({ keys, metaIndex });
const group = r.groups.find((x) => x.kind === KIND);
ok(!!group, "resolveViz gives the modules key to the modules widget");

const draw = (v, frame) => {
  const calls = [];
  drawVizGroup({ fillRect: (...a) => calls.push(a), print() {},
                 textWidth: (t) => String(t).length * 4 },
               frame, group, { level: String(v) }, metaIndex);
  return calls;
};

const FRAME = { x: 8, y: 9, w: 17, h: 15 };
const lit = (calls) => calls.reduce((n, [, , w, h]) => n + w * h, 0);

ok(draw(0, FRAME).length > 0, "at 0 the widget still draws (a baseline, not a blank cell)");
ok(lit(draw(1, FRAME)) > lit(draw(0, FRAME)), "a full value lights more than an empty one");
ok(lit(draw(0.5, FRAME)) > lit(draw(0, FRAME)) && lit(draw(0.5, FRAME)) < lit(draw(1, FRAME)),
   "a mid value sits between the two");

/* Contained in every frame the renderers can hand it. */
let escaped = 0;
for (const f of [{ w: 17, h: 15 }, { w: 32, h: 14 }, { w: 32, h: 26 },
                 { w: 16, h: 15 }, { w: 25, h: 15 }, { w: 64, h: 15 },
                 { w: 3, h: 3 }, { w: 1, h: 1 }]) {
  const box = { x: 8, y: 9, ...f };
  for (const v of [0, 0.37, 1]) {
    if (!draw(v, box).every(([x, y, w, h]) =>
          x >= 8 && y >= 9 && x + w <= 8 + f.w && y + h <= 9 + f.h)) escaped++;
  }
}
ok(escaped === 0, "the reference widget stays in frame at every size and value");

/* A garbage value must not produce a garbage picture. */
const bad = draw("not-a-number", FRAME);
ok(bad.every(([x, y, w, h]) => x >= 8 && y >= 9 && x + w <= 25 && y + h <= 24),
   "a non-numeric value is contained rather than drawn as NaN");

/* ---- the DSP and module.json must not drift ----
 *
 * The chain host reads chain_params from the DSPs get_param, NOT from
 * module.json -- so module.jsons copy is documentation, and documentation that
 * disagrees with the code is worse than none. Pin the parts that matter. */
const csrc = readFileSync(`${DIR}/widget_test.c`, "utf8");
ok(csrc.includes(`\\"kind\\":\\"${KIND}\\"`) || csrc.includes(KIND),
   "the DSP declares the same custom kind as module.json");
for (const k of CP.map((p) => p.key)) {
  ok(csrc.includes(`\\"key\\":\\"${k}\\"`),
     `the DSP declares the "${k}" param that module.json documents`);
}
ok(/move_audio_fx_init_v2/.test(csrc), "the DSP exports the v2 audio FX entry point");
ok(/process_block/.test(csrc), "and a process_block, so it is a loadable chain component");

/* ---- and the fallback story, from the shipped declaration ---- */
clearWidgets();
const r2 = resolveViz({ keys, metaIndex });
ok(!r2.groups.some((x) => x.kind === KIND),
   "with the widget unregistered the module falls back rather than claiming");

process.exit(fail ? 1 : 0);
'
