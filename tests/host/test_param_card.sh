#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A MODULE MAY DRAW THE CARD THAT FLOATS OVER THE PAGE WHILE ITS KNOB IS TURNED.
#
# Some values only mean something as a picture: a crossfade sitting between two
# named anchors is a position, and a delay feedback that computes to eleven
# repeats and then to silence is a reading no unit can spell. The grid draws
# eight named graphics and a parameter outside that vocabulary has had nowhere
# to put a picture at all.
#
# THREE THINGS HERE ARE EASY TO GET WRONG, AND EACH HAS ITS OWN SECTION.
#
# It must be OFF unless a host offers a loader and a module names a script --
# every existing page has to render exactly as before. It must never LOAD on the
# draw path, because nothing module-side is resident while the grid is up and
# the host loader has no cache, so a load from the draw evaluates a script on
# every frame of a turn. And a drawer that THROWS must leave the ordinary page
# rather than a hole -- this tree has already shipped a hook whose thrower was
# caught but whose caller reported success, which draws whatever came before the
# throw and then nothing, with no error anywhere.
#
# NO APOSTROPHES BELOW THIS LINE inside the node script: it is a single-quoted
# bash string, and one apostrophe ends it early with an error pointing nowhere
# near the real line.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the param card tests" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { createController } from "./src/shared/param_pages/page_controller.mjs";
import { paramCardRect, paramCardContentRect, drawParamCard,
         DEFAULT_CARD_W, DEFAULT_CARD_H, BORDER_W, GAP_W }
  from "./src/shared/param_pages/param_card.mjs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const INSET = BORDER_W + GAP_W;

/* `blend` declares a drawer and a size; `plain` declares nothing and is the
   control for every default-off assertion below. */
const CHAIN_PARAMS = [
  { key: "blend", name: "Blend", type: "float", min: 0, max: 1, step: 0.01,
    card_script: "cards.js#blend_card", card_w: 96, card_h: 44 },
  { key: "plain", name: "Plain", type: "float", min: 0, max: 1, step: 0.01 },
  { key: "shape", name: "Shape", type: "enum",
    options: ["Sine", "Tri", "Saw", "Square", "Noise"],
    card_script: "cards.js#shape_card" },
];
const HIER = { modes: null, levels: { root: { label: "T",
  knobs: ["blend", "plain", "shape"],
  params: CHAIN_PARAMS.map((p) => ({ key: p.key })) } } };

let clock = 1000;
function mk(io = {}) {
  clock = 1000;
  const store = { blend: "0.5", plain: "0.25", shape: "0" };
  const ctl = createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return JSON.stringify(HIER);
      if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
      return b in store ? store[b] : "";
    },
    setParam: (k, v) => { store[String(k).replace(/^[^:]+:/, "")] = String(v); },
    announce: () => {},
    now: () => clock,
    ...io,
  });
  ctl.load({ prefix: "synth" });
  for (let i = 0; i < 12; i++) ctl.tick();
  const slotOf = (key) => (ctl.page.keys || []).indexOf(key);
  return { ctl, store, slotOf };
}

/* A loader that records what it was asked for, so "loaded once, off the draw
   path" is a count rather than an argument. */
function recordingLoader(drawer) {
  const calls = [];
  return {
    calls,
    loadCard: (path, ref) => { calls.push(path + "#" + ref); return drawer; },
  };
}

/* menu_layout reaches for print / fill_rect / set_pixel BY NAME, exactly as it
   does on the device, so the peek half of renderOverlays needs them installed.
   The card half does not -- it draws through the ctx it is handed -- and that
   asymmetry is real rather than test scaffolding. */
const GLOBAL_NAMES = ["print", "fill_rect", "set_pixel", "text_width",
  "host_send_screenreader"];
function paint(ctl, opts) {
  const fb = createFramebuffer();
  const g = {
    print: fb.print, fill_rect: fb.fillRect, set_pixel: fb.setPixel,
    text_width: fb.textWidth, host_send_screenreader: () => {},
  };
  for (const k of GLOBAL_NAMES) globalThis[k] = g[k];
  let did;
  try {
    did = ctl.renderOverlays(drawContext(fb), opts || {});
  } finally {
    for (const k of GLOBAL_NAMES) delete globalThis[k];
  }
  return { did, fb };
}
const ink = (fb) => fb.pixels.reduce((n, v) => n + (v ? 1 : 0), 0);

/* ===================================================================== 1 ==
 * GEOMETRY: the module declares the size, and a nonsense size falls back
 * rather than drawing a frame with no inside.
 */
{
  const declared = paramCardRect({ card_w: 96, card_h: 44 });
  ok(declared.w === 96 && declared.h === 44, "a declared size is honoured");
  ok(declared.x === 16 && declared.y === 10,
     "and it is centred on the 128x64 panel, got " + declared.x + "," + declared.y);

  const dflt = paramCardRect({});
  ok(dflt.w === DEFAULT_CARD_W && dflt.h === DEFAULT_CARD_H,
     "no declaration gets the default size");

  const huge = paramCardRect({ card_w: 400, card_h: 400 });
  ok(huge.w <= 128 && huge.h <= 64 && huge.x >= 0 && huge.y >= 0,
     "an oversize card is clamped to the panel, got " + huge.w + "x" + huge.h);

  for (const bad of [0, -5, "wide", null, NaN, 3]) {
    const r = paramCardRect({ card_w: bad, card_h: bad });
    ok(r.w === DEFAULT_CARD_W && r.h === DEFAULT_CARD_H,
       "a nonsense size (" + String(bad) + ") falls back to the default");
  }

  const content = paramCardContentRect(declared);
  ok(content.x === declared.x + INSET && content.y === declared.y + INSET
     && content.w === declared.w - INSET * 2 && content.h === declared.h - INSET * 2,
     "the drawer gets the rect INSIDE the border and its gap");
}

/* ===================================================================== 2 ==
 * THE FRAME IS OURS, THE INSIDE IS THEIRS.
 */
{
  const fb = createFramebuffer();
  let got = null, gotCtx = null;
  const drew = drawParamCard(drawContext(fb), {
    meta: { card_w: 96, card_h: 44 }, name: "Blend", value: "0.50", raw: "0.5",
    draw: (ctx, o) => { gotCtx = ctx; got = o; },
  });
  ok(drew === true, "drawParamCard reports that it drew");
  ok(!!got, "the module drawer was called");
  const r = paramCardRect({ card_w: 96, card_h: 44 });
  ok(got && got.w === r.w - INSET * 2 && got.h === r.h - INSET * 2,
     "it is handed the size of the space INSIDE the border and its gap");
  ok(got && got.x === undefined && got.y === undefined,
     "and NO x/y -- there is no absolute position for a drawer to be given");
  ok(gotCtx && gotCtx.width === got.w && gotCtx.height === got.h,
     "the context it draws through is the cards own, not the panels");
  ok(got && got.name === "Blend" && got.value === "0.50" && got.raw === "0.5",
     "and the name, the formatted reading and the raw wire value");
  ok(ink(fb) > 0, "the frame is drawn even though the drawer painted nothing");
  ok(fb.clipped() === 0, "and nothing landed off the panel, got " + fb.clipped());
}

/* ==================================================================== 2b ==
 * A DRAWER CANNOT EXPRESS A COORDINATE OUTSIDE ITS CARD.
 *
 * The same rule #405 made for a cell widget, for the same reason and through
 * the same file: the cards rect is per PARAMETER and clamped to the panel, so
 * absolute coordinates authored against one card are wrong on the next. Here
 * it matters twice over -- a floating card that could paint outside its border
 * would eat the page it exists to float over.
 *
 * (0,0) IS THE INSIDE OF THE CARD. This test pins that by drawing a rect that
 * starts before the origin and runs far past the far corner: what lands must
 * be exactly the content rect, no more, and the frame must SAY it clipped
 * rather than absorbing it silently.
 */
{
  const fb = createFramebuffer();
  const outer = paramCardRect({ card_w: 96, card_h: 44 });
  const inside = { x: outer.x + INSET, y: outer.y + INSET,
                   w: outer.w - INSET * 2, h: outer.h - INSET * 2 };
  let clipped = -1;
  drawParamCard(drawContext(fb), {
    meta: { card_w: 96, card_h: 44 }, name: "B", value: "1", raw: "1",
    draw: (ctx) => {
      ctx.fillRect(-5, -5, 200, 200, 1);
      ctx.print(-9, -9, "SPILL", 1);
      clipped = ctx.clipped();
    },
  });
  ok(clipped > 0, "the frame COUNTS the overflow rather than hiding it, got " + clipped);

  /* Every lit pixel outside the cards inside is a leak -- except the border
   * itself, which is ours and was drawn before the drawer ran. */
  let outsideContent = 0, outsideCard = 0;
  for (let y = 0; y < fb.height; y++) {
    for (let x = 0; x < fb.width; x++) {
      if (!fb.pixels[y * fb.width + x]) continue;
      const inContent = x >= inside.x && x < inside.x + inside.w
                     && y >= inside.y && y < inside.y + inside.h;
      const inCard = x >= outer.x && x < outer.x + outer.w
                  && y >= outer.y && y < outer.y + outer.h;
      if (!inContent) outsideContent++;
      if (!inCard) outsideCard++;
    }
  }
  ok(outsideCard === 0,
     "nothing the drawer asked for landed outside the CARD, got " + outsideCard);
  /* What is left outside the content rect can only be our own border. */
  const borderInk = 2 * (outer.w * BORDER_W) + 2 * ((outer.h - BORDER_W * 2) * BORDER_W);
  ok(outsideContent === borderInk,
     "and outside the content rect there is only OUR border, got "
     + outsideContent + " want " + borderInk);
}

/* ===================================================================== 3 ==
 * A READ THAT DID NOT ANSWER MUST NOT BECOME A PICTURE. The rule does not
 * stop applying because the pixels belong to a module: raw arrives as null so
 * a drawer can tell "no answer" from a real zero.
 */
{
  const fb = createFramebuffer();
  let got = null;
  drawParamCard(drawContext(fb), {
    meta: {}, name: "X", value: "--",
    draw: (ctx, o) => { got = o; },
  });
  ok(got && got.raw === null, "an unanswered read is handed through as null, not as 0");
}

/* ===================================================================== 4 ==
 * A DRAWER THAT THROWS COSTS ONE FRAME AND IS RETIRED.
 *
 * The failure this guards is not the crash. It is the shape already shipped
 * once here: a hook that threw, was caught, and reported success -- leaving
 * whatever drew before the throw and then nothing, with no error.
 */
{
  const fb = createFramebuffer();
  let errs = 0;
  let drew;
  try {
    drew = drawParamCard(drawContext(fb), {
      meta: { card_w: 96, card_h: 44 }, name: "B", value: "1",
      draw: () => { throw new Error("module bug"); },
      onError: () => { errs++; },
    });
  } catch (e) {
    ok(false, "the throw escaped drawParamCard: " + e.message);
  }
  ok(drew === true, "it still reports the frame it drew");
  ok(errs === 1, "onError fires once so the caller can retire the drawer, got " + errs);
  ok(ink(fb) > 0, "and an empty card is left, never a hole");
}

/* ===================================================================== 5 ==
 * OFF BY DEFAULT, TWICE OVER: no loader, or no declaration.
 */
{
  const { ctl, slotOf } = mk();                       /* no io.loadCard */
  ctl.onKnobTouch(slotOf("blend"), true);
  const { did, fb } = paint(ctl);
  ok(did === false && ink(fb) === 0,
     "a host with no loader draws no card, even for a declaring param");
}
{
  const rec = recordingLoader(() => {});
  const { ctl, slotOf } = mk({ loadCard: rec.loadCard });
  ctl.onKnobTouch(slotOf("plain"), true);
  const { did, fb } = paint(ctl);
  ok(did === false && ink(fb) === 0, "a param declaring nothing draws no card");
  ok(rec.calls.length === 0, "and nothing was loaded for it");
}

/* ===================================================================== 6 ==
 * IT DRAWS, from a touch, into a real framebuffer.
 */
{
  let seen = null;
  const rec = recordingLoader((ctx, o) => {
    seen = o;
    ctx.fillRect(o.x, o.y, o.w, o.h, 1);
  });
  const { ctl, slotOf } = mk({ loadCard: rec.loadCard });
  ctl.onKnobTouch(slotOf("blend"), true);
  const { did, fb } = paint(ctl);
  ok(did === true, "touching a declaring knob draws its card");
  ok(rec.calls[0] === "cards.js#blend_card",
     "the declared script and export are what got loaded, got " + rec.calls[0]);
  ok(seen && seen.name === "Blend", "the drawer is told which param it is drawing");
  ok(ink(fb) > 0 && fb.clipped() === 0, "pixels landed, and none off the panel");
}

/* ===================================================================== 7 ==
 * IT FLOATS, SO IT NEEDS NO CLEAR -- the difference from the enum peek beside
 * it, and the reason an embedded consumer that owns no frame still gets one.
 */
{
  const rec = recordingLoader((ctx, o) => ctx.fillRect(o.x, o.y, o.w, o.h, 1));
  const { ctl, slotOf } = mk({ loadCard: rec.loadCard });
  ctl.onKnobTouch(slotOf("blend"), true);
  let cleared = 0;
  const { did, fb } = paint(ctl, { clearScreen: () => { cleared++; } });
  ok(did === true && cleared === 0,
     "it draws WITHOUT clearing the frame -- the page stays readable around it");
  ok(ink(fb) < fb.pixels.length,
     "and it does not cover the whole panel, which is what full-screen would mean");
}

/* ===================================================================== 8 ==
 * THE LOAD IS ON THE GESTURE, NEVER ON THE DRAW.
 *
 * A load from the draw path evaluates the module script on every frame of a
 * turn. Counting the loader is the only way to see that, because the pixels
 * are identical either way.
 */
{
  const rec = recordingLoader((ctx, o) => ctx.fillRect(o.x, o.y, o.w, o.h, 1));
  const { ctl, slotOf } = mk({ loadCard: rec.loadCard });
  const slot = slotOf("blend");
  ctl.onKnobTouch(slot, true);
  for (let i = 0; i < 30; i++) paint(ctl);
  ok(rec.calls.length === 1,
     "30 frames later it has still been loaded exactly once, got " + rec.calls.length);
  ctl.onKnobTouch(slot, false);
  ctl.onKnobTouch(slot, true);
  ok(rec.calls.length === 1,
     "and a second touch reuses the cached drawer, got " + rec.calls.length);
}
{
  /* A loader that answers null is asked ONCE. A missing file must not be
     re-evaluated on every touch for the rest of the session. */
  let calls = 0;
  const { ctl, slotOf } = mk({ loadCard: () => { calls++; return null; } });
  const slot = slotOf("blend");
  for (let i = 0; i < 5; i++) { ctl.onKnobTouch(slot, true); ctl.onKnobTouch(slot, false); }
  const { did } = paint(ctl);
  ok(calls === 1, "a null answer is cached, not retried, got " + calls);
  ok(did === false, "and no card is drawn");
}

/* ===================================================================== 9 ==
 * RELEASE TAKES IT DOWN, and it rides the SAME law as every other
 * follow-the-knob surface here rather than a timer of its own.
 */
{
  const rec = recordingLoader((ctx, o) => ctx.fillRect(o.x, o.y, o.w, o.h, 1));
  const { ctl, slotOf } = mk({ loadCard: rec.loadCard });
  const slot = slotOf("blend");
  ctl.onKnobTouch(slot, true);
  ok(paint(ctl).did === true, "up while held");
  ctl.onKnobTouch(slot, false);
  ok(paint(ctl).did === false, "and gone the moment the finger leaves");
}

/* ==================================================================== 10 ==
 * THE PEEK WINS. A card is an aid to reading ONE value; the peek is the list
 * of values you are moving between, and it is full-screen on purpose.
 */
{
  const rec = recordingLoader((ctx, o) => ctx.fillRect(o.x, o.y, o.w, o.h, 1));
  const { ctl, slotOf } = mk({ loadCard: rec.loadCard });
  const s = slotOf("shape");
  for (let i = 0; i < 6; i++) { clock += 20; ctl.onKnobTurn(s, 1, clock); }
  ok(!!ctl.enumPeek(), "the fixture really does raise a peek");
  let cleared = 0;
  const { did } = paint(ctl, { clearScreen: () => { cleared++; } });
  ok(did === true && cleared === 1,
     "the peek is what draws, and it still takes the frame clear");
}

/* ==================================================================== 11 ==
 * NO EXISTING PAGE MOVES. The whole default-off claim, stated as pixels: the
 * same fixture with no loader must render byte-identically to one whose params
 * carry no declaration at all.
 */
{
  const render = (io) => {
    const { ctl } = mk(io);
    const fb = createFramebuffer();
    ctl.render(drawContext(fb), {});
    return fb.pixels.join(",");
  };
  ok(render({}) === render({ loadCard: () => () => {} }),
     "the PAGE renders identically whether or not a loader is present");
}

/* ==================================================================== 12 ==
 * THE HOST TAKES A CARD EXPORT AS A FUNCTION, NOT AS AN OVERLAY FACTORY.
 *
 * A source pin, because the failure it guards is host-side and silent. The card
 * loader first reused loadCanvasOverlayScript, which resolves a canvas OVERLAY
 * OBJECT — and resolveOverlayObject treats a function candidate as a FACTORY:
 * it CALLS it and keeps whatever object comes back. So the drawer was invoked
 * with no arguments, threw on the rect it was never given, and was discarded as
 * "overlay factory returned invalid value". The loader answered null, the
 * controller cached that null exactly as designed, and the knob had no picture
 * — no error anywhere, and only on the consumer that used this loader.
 */
{
  const host = readFileSync("src/shadow/shadow_ui.js", "utf8");

  ok(/function loadCardDrawer\(/.test(host),
     "the host has a loader dedicated to card drawers");

  const at = host.indexOf("function loadCardDrawer(");
  const body = host.slice(at, host.indexOf("\nfunction ", at + 10));

  ok(/typeof fn === "function"/.test(body),
     "it accepts the export as a FUNCTION");
  ok(!/loadCanvasOverlayScript|resolveOverlayFromGlobals|resolveOverlayObject/.test(body),
     "and does NOT route a card through the canvas-overlay resolver, which "
     + "would CALL it as a factory");

  /* The call site must use it. A loader nothing calls is the same bug back. */
  const ctxAt = host.indexOf("_ctx.loadCardScript");
  const ctxBody = host.slice(ctxAt, ctxAt + 600);
  ok(/loadCardDrawer\(/.test(ctxBody),
     "and the ctx hook goes through it");
  ok(!/loadCanvasOverlayScript\(/.test(ctxBody),
     "not through the canvas one");

  /* The premise, so this section cannot quietly stop meaning anything: the
     overlay resolver really does call a function candidate. */
  const roAt = host.indexOf("function resolveOverlayObject(");
  const roBody = host.slice(roAt, host.indexOf("\nfunction ", roAt + 10));
  ok(/typeof candidate === "function"/.test(roBody) && /candidate\(\)/.test(roBody),
     "PREMISE: resolveOverlayObject still calls a function candidate as a factory "
     + "— if this ever stops being true, the section above is obsolete, not wrong");
}

console.log(fail === 0 ? "ALL PARAM CARD CHECKS PASSED" : (fail + " FAILED"));
process.exit(fail === 0 ? 0 : 1);
'
