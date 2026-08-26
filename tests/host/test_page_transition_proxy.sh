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
import { translateCtx, slideOffsets, scrollFrame, advanceLinear, advanceEased, drawSlide }
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
  drawRect: (...a) => calls.push(["drawRect", ...a]),
  line: (...a) => calls.push(["line", ...a]),
  drawLine: (...a) => calls.push(["drawLine", ...a]),
  fillCircle: (...a) => calls.push(["fillCircle", ...a]),
  drawArc: (...a) => calls.push(["drawArc", ...a]),
};

const p = translateCtx(rec, 10);

calls.length = 0; p.fillRect(5, 20, 3, 4, 1);
ok(calls[0][1] === 15 && calls[0][2] === 20, "fillRect shifts x, leaves y");

calls.length = 0; p.print(5, 20, "AB", 1);
ok(calls[0][1] === 15 && calls[0][2] === 20, "print shifts x, leaves y");

calls.length = 0; const w = p.textWidth("AB");
ok(w === 7 && calls[0][1] === "AB", "textWidth passes through unshifted");

calls.length = 0; p.setPixel(5, 20, 1);
ok(calls[0][1] === 15 && calls[0][2] === 20, "setPixel shifts x, leaves y");

calls.length = 0; p.drawRect(5, 20, 3, 4, 1);
ok(calls[0][1] === 15 && calls[0][2] === 20, "drawRect shifts x, leaves y");

calls.length = 0; p.line(5, 20, 8, 30, 1);
ok(calls[0][1] === 15 && calls[0][2] === 20 && calls[0][3] === 18 && calls[0][4] === 30,
   "line shifts BOTH x endpoints, leaves both y");

calls.length = 0; p.drawLine(5, 20, 8, 30, 1);
ok(calls[0][1] === 15 && calls[0][3] === 18, "drawLine shifts both x endpoints");

calls.length = 0; p.fillCircle(5, 20, 3, 1);
ok(calls[0][1] === 15 && calls[0][2] === 20 && calls[0][3] === 3,
   "fillCircle shifts cx, leaves cy and r");

calls.length = 0; p.drawArc(5, 20, 3, 90, 180, 1);
ok(calls[0][1] === 15 && calls[0][2] === 20 && calls[0][3] === 3 &&
   calls[0][4] === 90 && calls[0][5] === 180,
   "drawArc shifts cx, leaves cy, r, start and sweep");

/* Omission, not passthrough. A method nobody taught the table about must not
   reach the renderer at all -- the guarded fallback then draws it correctly
   through fillRect, which IS translated. */
const odd = { fillRect: () => {}, somethingNew: () => {} };
const op = translateCtx(odd, 10);
ok(typeof op.somethingNew !== "function",
   "an unknown method is OMITTED from the proxy, not passed through untranslated");

/* Identity at zero: no wrapper allocated, so the no-transition path pays
   nothing and cannot be subtly different from the un-proxied one. */
ok(translateCtx(rec, 0) === rec, "dx of 0 returns the original ctx object");

/* Offsets abut exactly, at every frac -- this is what makes clipping
   unnecessary, so it is asserted across the range rather than at the ends. */
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

/* THE ADVANCE MUST LAND EXACTLY, not approach forever.
   An eased chase is naturally asymptotic, and a position that is 0.0001 of a
   page from home leaves the transition running for ever: two renders per frame
   and a page that never settles. */
for (const [name, adv] of [["linear", advanceLinear], ["eased", advanceEased]]) {
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
}

/* Composite order: both pages, then the fixed chrome ON TOP. */
const order = [];
drawSlide(rec, {
  fromDx: -40, toDx: 88,
  drawFrom: () => order.push("from"),
  drawTo: () => order.push("to"),
  drawChrome: () => order.push("chrome"),
});
ok(order.join(",") === "from,to,chrome",
   "drawSlide draws both pages then the fixed chrome over them");

process.exit(fail ? 1 : 0);
'
