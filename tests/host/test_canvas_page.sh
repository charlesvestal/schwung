#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A MODULE MAY OWN A WHOLE PAGE, AND IT IS STILL A PAGE.
#
# The existing custom-UI surface is a `type: "canvas"` param you CLICK to dive
# into a fullscreen view. That view is not a page: you cannot jog to it, the
# encoders do not reach it, and it draws its own chrome. Asked for on hardware
# as "I have to dive in to see the faces instead of it being its own page, knobs
# do not work on the face, and the face does not animate".
#
# `as_page: true` makes it a page in the level's jog rotation, carrying THAT
# LEVEL'S OWN KNOBS.
#
# THE DESIGN DECISION WORTH KNOWING. It is an ordinary PAGE_KNOBS page that
# happens to carry a `canvas` field -- NOT a new page kind. Twenty-two places in
# page_controller branch on PAGE_KNOBS (reads, knob turns, touch, the touch
# strip, announce, dive targets, the list layout); a new kind would have to be
# threaded through every one, and missing one gives a page that looks right and
# does not respond. As a knobs page it inherits all of that untouched and only
# the picture differs.
#
# What must hold:
#   - the page is planned, in the rotation, with the level's knobs
#   - the canvas key does NOT also get a cell (it did, as an overflow page
#     holding one control -- "one page that is just the face")
#   - the module draws the BODY only; header and footer are the host's
#   - a drawer that throws is retired and the page survives
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
const { planPages, PAGE_KNOBS } = await import(R + "/src/shared/param_pages/page_plan.mjs");
const { renderPage } = await import(R + "/src/shared/param_pages/render_page.mjs");
const { buildMetaIndex } = await import(R + "/src/shared/param_pages/param_meta.mjs");
const { createController, LAYOUT_MOVY } = await import(R + "/src/shared/param_pages/page_controller.mjs");
const { createFramebuffer, drawContext } = await import(R + "/tools/param-pages/harness.mjs");

let fails = 0;
const ok = (c, m) => { if (!c) { console.error("FAIL: " + m); fails++; } };

const HIER = { levels: { root: { label: "Main",
  knobs: ["a", "b", "c"],
  params: [{ key: "a" }, { key: "b" }, { key: "c" }, { key: "face" }] } } };
const CP = [
  { key: "a", name: "A", type: "float", min: 0, max: 1, step: 0.01 },
  { key: "b", name: "B", type: "float", min: 0, max: 1, step: 0.01 },
  { key: "c", name: "C", type: "float", min: 0, max: 1, step: 0.01 },
  { key: "face", name: "Face", type: "canvas", canvas_script: "canvas.js",
    canvas_overlay: "face_page", as_page: true },
];

/* ---- planning ---- */
const plan = planPages({ hierarchy: HIER, chainParams: CP });
const canvasPages = plan.pages.filter((p) => p.canvas);
ok(canvasPages.length === 1, "exactly one custom page is planned");
const cp = canvasPages[0];
ok(cp && cp.kind === PAGE_KNOBS,
   "a custom page IS a knobs page, so it inherits every knobs-page behaviour");
ok(cp && cp.name === "Face", "it is named after the param, not after the level");
ok(cp && JSON.stringify(cp.keys) === JSON.stringify(["a", "b", "c"]),
   "it carries the levels own knobs, so the encoders do what they do on the grid");
ok(cp && cp.canvas.script === "canvas.js" && cp.canvas.overlay === "face_page",
   "the script and overlay travel with the page");

/* The canvas key must NOT also be a cell anywhere. */
const celled = plan.pages.some((p) => (p.keys || []).indexOf("face") >= 0);
ok(!celled, "the canvas key never gets a cell of its own");
ok(!plan.pages.some((p) => !p.canvas && (p.keys || []).length === 1 && p.keys[0] === "face"),
   "and there is no orphan page holding just it");

/* Without as_page it stays a dive-in cell -- the OLD behaviour is untouched. */
const CP2 = CP.map((p) => (p.key === "face" ? { ...p, as_page: false } : p));
const plan2 = planPages({ hierarchy: HIER, chainParams: CP2 });
ok(!plan2.pages.some((p) => p.canvas), "without as_page no custom page is planned");
ok(plan2.pages.some((p) => (p.keys || []).indexOf("face") >= 0),
   "and the key goes back to being an ordinary cell");

/* ---- rendering: the module gets the BODY, the host keeps the chrome ---- */
{
  const fb = createFramebuffer(128, 64);
  let got = null;
  renderPage(drawContext(fb), {
    page: cp, metaIndex: buildMetaIndex({ hierarchy: HIER, chainParams: CP }),
    values: { a: "0.5" }, title: "MOD", pageIndex: 1, pageCount: 2,
    touched: -1, rect: { x: 0, y: 0, w: 128, h: 64 },
    drawCanvasPage: (ctx, band, canvas, extra) => { got = { band, canvas, extra }; },
  });
  ok(got !== null, "the drawer is called for a custom page");
  ok(got && got.band.y > 0, "the band starts BELOW the header");
  ok(got && got.band.y + got.band.h <= 64, "and stays inside the panel");
  ok(got && got.band.x === 0 && got.band.w === 128, "it spans the full width");
  ok(fb.countLit() > 0, "the host still drew its own header");

  /* A page WITHOUT canvas must never call it. */
  let called = false;
  const grid = plan.pages.find((p) => !p.canvas && (p.keys || []).length);
  renderPage(drawContext(createFramebuffer(128, 64)), {
    page: grid, metaIndex: buildMetaIndex({ hierarchy: HIER, chainParams: CP }),
    values: {}, title: "MOD", pageIndex: 0, pageCount: 2, touched: -1,
    rect: { x: 0, y: 0, w: 128, h: 64 },
    drawCanvasPage: () => { called = true; },
  });
  ok(!called, "an ordinary page never calls the canvas drawer");
}

/* ---- the controller reaches it, and knobs still work ---- */
{
  const store = { ui_hierarchy: JSON.stringify(HIER), chain_params: JSON.stringify(CP),
                  a: "0.25", b: "0.5", c: "0.75" };
  const writes = [];
  let t = 0;
  const ctrl = createController({
    getParam: (k) => { const b = String(k).split(":").pop();
                       return store[b] === undefined ? null : store[b]; },
    setParam: (k, v) => { writes.push([String(k).split(":").pop(), v]); return true; },
    announce: () => {}, now: () => (t += 16),
  });
  ctrl.load({ prefix: "synth" });
  ctrl.setLayout(LAYOUT_MOVY);
  for (let i = 0; i < 40; i++) ctrl.tick();

  const idx = ctrl.describePage ? null : null;
  /* Jog to the custom page. */
  let found = -1;
  for (let i = 0; i < 8; i++) {
    ctrl.goToPage(i, { remember: false });
    if (ctrl.onCanvasPage()) { found = i; break; }
  }
  ok(found >= 0, "the custom page is reachable by paging, not only by diving");
  ok(ctrl.onCanvasPage(), "and the controller reports it as one");

  /* Turning knob 0 on the custom page must write the levels first knob. */
  for (let i = 0; i < 10; i++) ctrl.tick();
  ctrl.onKnobTurn(0, 1);
  ok(writes.some((w) => w[0] === "a"),
     "an encoder on the custom page writes the levels own param");
}

/* ---- a thrower is retired, not fatal ---- */
{
  const fb = createFramebuffer(128, 64);
  let threw = false;
  try {
    renderPage(drawContext(fb), {
      page: cp, metaIndex: buildMetaIndex({ hierarchy: HIER, chainParams: CP }),
      values: {}, title: "MOD", pageIndex: 1, pageCount: 2, touched: -1,
      rect: { x: 0, y: 0, w: 128, h: 64 },
      drawCanvasPage: () => { throw new Error("boom"); },
    });
  } catch (e) { threw = true; }
  ok(threw, "render_page does not swallow the throw itself -- the HOST retires the drawer");
}

if (fails) { console.error(fails + " failure(s)"); process.exit(1); }
console.log("PASS: a module can own a page, keep the hosts chrome, and still be turned");
'
