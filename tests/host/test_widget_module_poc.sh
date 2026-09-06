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
import { registerOverlayWidgets, clearWidgets, isWidgetAvailable }
  from "./src/shared/param_pages/widget_registry.mjs";
import { drawParamCard, paramCardRect } from "./src/shared/param_pages/param_card.mjs";

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
/* TWO kinds, from ONE module. The fixture declares both on purpose: a module
 * used to be limited to a single widget, not by the registry (always a map) but
 * by a call site that read one string, and the second kind fell through to a
 * built-in dial with no error. This test is the consumer that proves it does
 * not any more. */
ok(declared.length === 2, "the fixture declares two custom viz kinds");
const KIND = "custom:wtmeter";
const KIND2 = "custom:wtmode";
ok(declared.some((p) => p.viz.kind === KIND) && declared.some((p) => p.viz.kind === KIND2),
   "both expected kinds are declared in chain_params");
const declaredCard = CP.find((p) => typeof p.card_script === "string") || null;

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

ok(overlay.widgetKind === KIND,
   `the overlays widgetKind matches the chain_params declaration (${KIND})`);
ok(overlay.widgetKinds && typeof overlay.widgetKinds === "object",
   "and it declares a second kind through widgetKinds");
ok(typeof overlay.draw === "function",
   "the same overlay also supplies the fullscreen draw");

/* ---- register exactly as shadow_ui does, then RESOLVE and DRAW ---- */
clearWidgets();
const reg = registerOverlayWidgets(overlay);
ok(reg.skipped.length === 0,
   "nothing the fixture declares is skipped: " + JSON.stringify(reg.skipped));
ok(isWidgetAvailable(KIND), "the widget registers");
ok(isWidgetAvailable(KIND2), "and so does the SECOND kind, from the same module");

/* EVERY declared kind must actually be registered. The old failure was silent
 * and partial -- one of two -- so counting is the assertion that catches it. */
for (const d of declared) {
  ok(isWidgetAvailable(d.viz.kind), `declared kind ${d.viz.kind} is available`);
}

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

/* ---- the SECOND widget resolves and draws too ---- */
{
  const keys2 = ["mode", null, null, null, null, null, null, null];
  const r2 = resolveViz({ keys: keys2, metaIndex });
  const g2 = r2.groups.find((x) => x.kind === KIND2);
  ok(!!g2, "resolveViz gives the second key to the second widget");
  const calls = [];
  drawVizGroup({ fillRect: (...a) => calls.push(a), print() {},
                 textWidth: (t) => String(t).length * 4 },
               FRAME, g2, { mode: "1" }, metaIndex);
  ok(calls.length > 0, "the second widget draws");
}
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

/* ---- THE CARD: the modules real cards.js, through the real drawParamCard ----
 *
 * test_param_card.sh injects its own loadCard stub and a fixture drawer, so no
 * real file is ever loaded on any path there. This starts from the shipped
 * cards.js. The equivalent gap on drawCell is what let a call-ordering defect
 * survive nine green tasks, so the card gets a real consumer from the start. */
const cardSpec = declaredCard && declaredCard.card_script;
ok(typeof cardSpec === "string" && cardSpec.indexOf("#") > 0,
   "the level param declares a card_script with an export fragment");
const [cardFile, cardExport] = String(cardSpec).split("#");

const cardSrc = readFileSync(`${DIR}/${cardFile}`, "utf8");
const cg = {};
new Function("globalThis", cardSrc)(cg);
const drawer = cg[cardExport];
ok(typeof drawer === "function",
   `cards.js exports ${cardExport} as a FUNCTION (a card is not an overlay factory)`);

const cardPx = (raw, frame) => {
  const px = new Set();
  const cctx = {
    fillRect(x, y, w, h, c) {
      for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
        const k = (x + i) + "," + (y + j);
        if (c) px.add(k); else px.delete(k);
      }
    },
    print() {}, textWidth: (t) => String(t).length * 4,
  };
  let threw = 0;
  drawParamCard(cctx, {
    meta: declaredCard, draw: drawer, frame,
    name: "Level", value: raw === null ? "" : String(raw), raw,
    onError: () => { threw++; },
  });
  return { px, threw };
};

const c50 = cardPx("0.5", null);
ok(c50.threw === 0, "the shipped card drawer does not throw");
ok(c50.px.size > 0, "and it lights pixels");
ok(cardPx("1", null).px.size > cardPx("0", null).px.size,
   "a full value lights more of the card than an empty one");

/* THE NULL CONTRACT, on the real drawer. */
const cNull = cardPx(null, null);
ok(cNull.threw === 0, "a null raw does not throw");
ok(cNull.px.size < c50.px.size,
   "a null raw draws LESS than a real value -- no bar invented from no answer");

/* Contained in the card, and in an EMBEDDED frame. */
const outer = paramCardRect(declaredCard);
ok([...c50.px].every((k) => {
     const a = k.split(","), x = Number(a[0]), y = Number(a[1]);
     return x >= outer.x && x < outer.x + outer.w &&
            y >= outer.y + 0 && y < outer.y + outer.h;
   }), "everything the card draws stays inside the card");

const EMB = { x: 0, y: 20, w: 64, h: 44 };
const embOuter = paramCardRect(declaredCard, EMB);
ok([...cardPx("0.5", EMB).px].every((k) => {
     const a = k.split(","), x = Number(a[0]), y = Number(a[1]);
     return x >= EMB.x && x < EMB.x + EMB.w && y >= EMB.y && y < EMB.y + EMB.h;
   }), "and inside an EMBEDDED page frame, not across the panel");

/* The DSP must declare the card too -- the chain host reads the DSP, not
 * module.json, so a card declared only in module.json would never appear. */
ok(csrc.includes("card_script"),
   "the DSP declares card_script, which is what the chain host actually serves");
ok(csrc.includes(cardFile),
   `the DSP names ${cardFile}, matching module.json`);

/* ---- and the fallback story, from the shipped declaration ---- */
clearWidgets();
const r2 = resolveViz({ keys, metaIndex });
ok(!r2.groups.some((x) => x.kind === KIND),
   "with the widget unregistered the module falls back rather than claiming");

process.exit(fail ? 1 : 0);
'
