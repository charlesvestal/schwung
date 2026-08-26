#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE PROXY FAILS SILENTLY IN BOTH DIRECTIONS, WHICH IS WHY THIS IS PER-METHOD.
#
# Both renderers treat `line` and `drawArc` as OPTIONAL: they guard with
# `typeof ctx.line === "function"` and fall back to a JS Bresenham that draws
# through ctx.fillRect (render_page_movy.mjs:659, viz_draw.mjs:162). So a proxy
# that FORGETS a method still draws the right picture through the fallback, and
# a proxy that forwards it WITHOUT translating draws in the wrong place. Neither
# raises. Asserting "the proxy has a line method" would pass in the broken case.
#
# So: assert that each wrapped method actually shifts x and leaves y alone, and
# assert that an unknown method is OMITTED -- omission degrades to the fallback,
# which is correct-but-slower; passthrough is wrong pixels.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the transition proxy test" >&2
  exit 1
fi

node --input-type=module -e '
import { translateCtx, slideOffsets, scrollFrame, advanceLinear, advanceEased,
         advanceScroll, isSliding, drawSlide }
  from "./src/shared/param_pages/page_transition.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* A recording ctx offering every method the device and the harness expose. */
const calls = [];
const rec = {
  fillRect: (...a) => calls.push(["fillRect", ...a]),
  print: (...a) => calls.push(["print", ...a]),
  textWidth: (t) => { calls.push(["textWidth", t]); return 7; },
  setPixel: (...a) => calls.push(["setPixel", ...a]),
  line: (...a) => calls.push(["line", ...a]),
  fillCircle: (...a) => calls.push(["fillCircle", ...a]),
  drawCircle: (...a) => calls.push(["drawCircle", ...a]),
  drawArc: (...a) => calls.push(["drawArc", ...a]),
};

const p = translateCtx(rec, 10);

/* EVERY METHOD IS PROBED THROUGH THIS, and the typeof check is the point.
   Calling a method the proxy omitted THROWS, which aborts the whole node
   script: the mutant dies, but it takes every later assertion with it and
   prints no FAIL line at all. Verified by dropping drawCircle from the table
   -- the run ended in a TypeError having reported zero failures. Returning
   null instead turns a missing entry into exactly one clean FAIL and lets the
   rest of the file keep measuring. */
const probe = (name, ...args) => {
  calls.length = 0;
  if (typeof p[name] !== "function") return null;
  const ret = p[name](...args);
  return { c: calls[0], ret };
};

let r = probe("fillRect", 5, 20, 3, 4, 1);
ok(r && r.c[1] === 15 && r.c[2] === 20, "fillRect shifts x, leaves y");

r = probe("print", 5, 20, "AB", 1);
ok(r && r.c[1] === 15 && r.c[2] === 20, "print shifts x, leaves y");

r = probe("textWidth", "AB");
ok(r && r.ret === 7 && r.c[1] === "AB", "textWidth passes through unshifted");

r = probe("setPixel", 5, 20, 1);
ok(r && r.c[1] === 15 && r.c[2] === 20, "setPixel shifts x, leaves y");

r = probe("line", 5, 20, 8, 30, 1);
ok(r && r.c[1] === 15 && r.c[2] === 20 && r.c[3] === 18 && r.c[4] === 30,
   "line shifts BOTH x endpoints, leaves both y");

r = probe("fillCircle", 5, 20, 3, 1);
ok(r && r.c[1] === 15 && r.c[2] === 20 && r.c[3] === 3,
   "fillCircle shifts cx, leaves cy and r");

/* Wrapped although no renderer calls it: both real contexts provide it, and
   render_page_movy carries a stale comment claiming it prefers this binding
   over drawArc. If that preference returns, an unwrapped one draws at the
   wrong x -- and nothing raises. */
r = probe("drawCircle", 5, 20, 3, 1);
ok(r && r.c[1] === 15 && r.c[2] === 20 && r.c[3] === 3,
   "drawCircle shifts cx, leaves cy and r");

r = probe("drawArc", 5, 20, 3, 90, 180, 1);
ok(r && r.c[1] === 15 && r.c[2] === 20 && r.c[3] === 3 &&
   r.c[4] === 90 && r.c[5] === 180,
   "drawArc shifts cx, leaves cy, r, start and sweep");

/* Omission, not passthrough. A method nobody taught the table about must not
   reach the renderer at all -- the guarded fallback then draws it correctly
   through fillRect, which IS translated. */
const odd = { fillRect: () => {}, somethingNew: () => {},
              drawRect: () => {}, drawLine: () => {} };
const op = translateCtx(odd, 10);
ok(typeof op.somethingNew !== "function",
   "an unknown method is OMITTED from the proxy, not passed through untranslated");

/* drawRect and drawLine are provided by NEITHER real context -- the device
   binds its native draw_line to the key `line` -- so they were dropped from
   the table as dead entries. Pinned so a future reader does not add them back
   on the assumption they were an oversight. */
ok(typeof op.drawRect !== "function" && typeof op.drawLine !== "function",
   "drawRect and drawLine are not in the table: no real context provides them");

/* Identity at zero: no wrapper allocated, so the no-transition path pays
   nothing and cannot be subtly different from the un-proxied one. */
ok(translateCtx(rec, 0) === rec, "dx of 0 returns the original ctx object");

/* Offsets abut exactly, at every frac -- this is what makes clipping
   unnecessary, so it is asserted across the range rather than at the ends.
   NOT A TAUTOLOGY, though the current implementation makes it look like one:
   it guards the natural symmetrical refactor that rounds `to` in its own
   right, as -Math.round((frac - 1) * width). Those two roundings can disagree
   by one, giving a 127 or 129 gap -- a one-pixel seam of stale ink between
   the pages, or a one-pixel column of one page overwritten by the other. */
let abut = true;
for (let i = 0; i <= 40; i++) {
  const o = slideOffsets(i / 40, 128);
  if (o.to - o.from !== 128) abut = false;
}
ok(abut, "the two pages abut exactly at every frac");
ok(slideOffsets(0, 128).from === 0, "at frac 0 the base page is home");

/* scrollFrame: which two pages are on screen, and how far along. */
const f0 = scrollFrame(3);
ok(f0.base === 3 && f0.frac === 0, "an integer position is one page, fully home");
const f1 = scrollFrame(3.25);
ok(f1.base === 3 && Math.abs(f1.frac - 0.25) < 1e-9,
   "a fractional position straddles base and base+1");
let fracInRange = true;
for (let i = 0; i < 50; i++) {
  const fr = scrollFrame(2 + i / 50).frac;
  if (!(fr >= 0 && fr < 1)) fracInRange = false;
}
ok(fracInRange, "frac stays in [0, 1) across a whole page of travel");

/* THE EPSILON, tested on the values that can distinguish it from a bare
   Math.floor. The clean 2.00..2.98 walk above cannot: every value there is
   far from an integer. Defensive rather than observed -- no current advance
   can produce 2.9999999, because both snap onto the integer target at a
   SNAP_PAGES three orders of magnitude coarser -- so it is pinned here or it
   is not pinned at all. */
const fHi = scrollFrame(2.9999999);
ok(fHi.base === 3 && fHi.frac === 0,
   "a position a hair BELOW an integer reads as that integer, not as frac 0.9999");
const fLo = scrollFrame(3.0000001);
ok(fLo.base === 3 && fLo.frac === 0,
   "a position a hair ABOVE an integer reads as that integer too");
const fNeg = scrollFrame(-1.25);
ok(fNeg.base === -2 && Math.abs(fNeg.frac - 0.75) < 1e-6,
   "a negative position still floors downward with frac in [0, 1)");

/* isSliding is the callers gate for stop compositing -- a composited frame is
   TWO full page renders, and at frac 0 the second is drawn entirely offscreen
   at dx 128, so it is pure waste. */
ok(isSliding(2.5) === true, "isSliding is true mid-travel");
ok(isSliding(3) === false, "isSliding is false at rest on a page");
ok(isSliding(2.9999999) === false,
   "isSliding agrees with scrollFrame at the epsilon, rather than deciding again");
ok(isSliding(NaN) === false, "isSliding refuses to composite on a NaN position");

/* THE ADVANCE MUST LAND EXACTLY, not approach forever.
   An eased chase is naturally asymptotic, and a position that is 0.0001 of a
   page from home leaves the transition running for ever: two renders per frame
   and a page that never settles. */
/* advanceScroll IS IN THE LOOP because it is the one that actually ships.
   The alias is re-pointed when the duration and easing are chosen from the
   filmed GIFs, and at that moment the shipping function would otherwise be
   the only one here with no test -- with both existing tests still green. */
for (const [name, adv] of [["linear", advanceLinear], ["eased", advanceEased],
                           ["scroll", advanceScroll]]) {
  let pos = 0;
  let steps = 0;
  while (pos !== 1 && steps < 500) { pos = adv(pos, 1, 18, 200); steps++; }
  ok(pos === 1, name + ": lands EXACTLY on the target");
  ok(steps < 500, name + ": lands in finite time, rather than approaching");

  /* Backwards, and no overshoot in either direction. */
  let back = 5, over = false;
  for (let i = 0; i < 500 && back !== 4; i++) {
    const next = adv(back, 4, 18, 200);
    if (next < 4) over = true;
    back = next;
  }
  ok(back === 4 && !over, name + ": travels backwards and never overshoots");

  ok(adv(2, 2, 18, 200) === 2, name + ": a position already at its target does not move");
  ok(adv(0, 1, 18, 0) === 1, name + ": a zero duration arrives immediately");

  /* A BACKWARDS CLOCK IS A SETTLE, NOT A VELOCITY.
     Unguarded, 1 - exp(+dt/tau) is unbounded below: advanceEased(0, 1, -500,
     200) measured -1807. anim_state.mjs already treats a backwards clock as a
     settle for exactly this reason, and this is the same shape. */
  ok(adv(0, 1, -18, 200) === 1, name + ": a small backwards dt settles rather than reversing");
  ok(adv(0, 1, -500, 200) === 1, name + ": a large backwards dt cannot explode the position");

  /* NaN IS ABSORBING, and pos is stored state: once it is NaN both escape
     hatches fail open (Math.abs(NaN) <= SNAP is false, step >= Math.abs(d) is
     false) and every later frame is NaN too, permanently. Settling makes it
     self-heal on the very next frame. */
  ok(adv(NaN, 1, 18, 200) === 1, name + ": a NaN position self-heals to the target");
  ok(adv(0, 1, NaN, 200) === 1, name + ": a NaN dt settles rather than propagating");
  ok(adv(0, 1, Infinity, 200) === 1, name + ": an infinite dt settles");
}

/* Composite order: both pages, then the fixed chrome ON TOP.
   AND WHICH OFFSET EACH PAGE GETS. Recording only the strings would pin the
   order and nothing else -- it would pass with fromDx and toDx swapped, which
   is exactly the mutation that inverts the slide direction. So each callback
   draws through the ctx it was handed and the recorded x is asserted. */
const order = [];
calls.length = 0;
drawSlide(rec, {
  fromDx: -40, toDx: 88,
  drawFrom: (c) => { order.push("from"); c.fillRect(0, 0, 1, 1, 1); },
  drawTo: (c) => { order.push("to"); c.fillRect(0, 0, 1, 1, 1); },
  drawChrome: (c) => { order.push("chrome"); c.fillRect(0, 0, 1, 1, 1); },
});
ok(order.join(",") === "from,to,chrome",
   "drawSlide draws both pages then the fixed chrome over them");
ok(calls[0][1] === -40, "the OUTGOING page is drawn at fromDx, not at toDx");
ok(calls[1][1] === 88, "the INCOMING page is drawn at toDx");
ok(calls[2][1] === 0, "the chrome is drawn unproxied, at no offset");

/* THE BANK BAR IS SUPPRESSED BY AN OPTION, NOT BY A LIE.
   drawBankBar returns early when pageCount <= 1, so passing pageCount: 1 would
   also blank it -- and would be telling the renderer the page set has one page
   in order to obtain a drawing side effect. Every other thing that reads
   pageCount (and anything that later will) would be reading a fiction.

   Same abort hazard as probe() above: a missing export makes RM.BAR_Y
   undefined, and rowInk over an undefined row would read NaN indices and
   report a silent zero rather than a failure. So the export is asserted
   FIRST, and the row assertions are skipped when it is absent -- one clean
   FAIL instead of a run that either crashes or passes vacuously. */
const RM = await import("./src/shared/param_pages/render_page_movy.mjs");
const H = await import("./tools/param-pages/harness.mjs");

const PAGE = { kind: 0, name: "P", keys: ["a", "b"], level: "root" };
const META = { getOrGuess: () => ({ key: "a", label: "A", kind: 0, min: 0, max: 1, step: 0.01 }) };
const shotPage = (extra) => {
  const fb = H.createFramebuffer();
  RM.renderPageMovy(H.drawContext(fb), {
    page: PAGE, metaIndex: META, values: { a: "0.5", b: "0.5" },
    title: "T", pageIndex: 1, pageCount: 5, touched: -1, viz: [], ...extra,
  });
  return fb;
};
const rowInk = (fb, y) => {
  let n = 0;
  for (let x = 0; x < 128; x++) if (fb.pixels[y * 128 + x]) n++;
  return n;
};

const barYExported = typeof RM.BAR_Y === "number";
ok(barYExported, "render_page_movy exports BAR_Y, the indicator row");
if (barYExported) {
  const withBar = shotPage({});
  const noBar = shotPage({ bankBar: false });
  ok(rowInk(withBar, RM.BAR_Y) > 0, "the bank bar draws ink by default");
  ok(rowInk(noBar, RM.BAR_Y) === 0, "bankBar:false leaves the indicator row empty");
  /* Byte-identical, not "the bar is there": the gate sits in the middle of the
     render, so a wrong guard could change the frame in ways an ink count on
     one row cannot see. */
  ok(Buffer.from(withBar.pixels).toString("base64") ===
     Buffer.from(shotPage({ bankBar: undefined }).pixels).toString("base64"),
     "omitting the option is byte-identical to before");

  /* THE PREVIOUS ASSERTION COMPARES TWO SPELLINGS OF THE SAME OPTION, so a
     gate stuck at ALWAYS SUPPRESS passes it -- both frames lose the bar
     together. Mutation-tested: `if (false)` killed only the ink count. This
     pins the rest of the frame instead: suppression must change the indicator
     band and NOTHING else, so a guard that suppresses too much (or draws in
     the wrong place) shows up as a differing row somewhere it has no
     business touching. */
  const noBarPx = noBar.pixels, withBarPx = withBar.pixels;
  let diffRows = [];
  for (let y = 0; y < 64; y++) {
    for (let x = 0; x < 128; x++) {
      if (withBarPx[y * 128 + x] !== noBarPx[y * 128 + x]) { diffRows.push(y); break; }
    }
  }
  ok(diffRows.length > 0, "suppressing the bar changes the frame at all");
  ok(diffRows.every((y) => y >= RM.BAR_Y && y < RM.BAR_Y + 3),
     "suppression touches only the indicator band, no other row");
}

process.exit(fail ? 1 : 0);
'
