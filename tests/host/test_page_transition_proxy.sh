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
         advanceScroll, isSliding, drawSlide, SLIDE_MS }
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

/* `ms` MEANS THE SETTLE TIME, FOR BOTH ADVANCES, AND NOTHING ELSE PINS IT.
   advanceEased used to run at tau = ms/3 -- three time constants, 95% of the
   travel -- and called that "about ms". The last 5% still has to reach
   SNAP_PAGES, another 2.5 time constants, so the real settle was 1.85x the
   number at the call site: a nominal 200 filmed at 364ms. Nothing was red.
   Every existing assertion above drives an advance to completion and checks
   that it LANDS; not one of them looks at WHEN, so the tau could mean anything
   and the file stayed green.

   Measured at the device cadence rather than a round number, because that is
   the clock the constant will actually be paced by. The tolerance is ONE FRAME
   -- arrival is quantized to a tick, so a tighter bound would be asserting
   something the hardware cannot express, and a looser one (say two frames)
   would pass the ms/3 mutation at the shortest durations. */
const TICK = 1000 / 55;
const settleOf = (adv, ms) => {
  let pos = 0, n = 0;
  while (pos !== 1 && n < 5000) { pos = adv(pos, 1, TICK, ms); n++; }
  return n * TICK;
};
for (const [name, adv] of [["linear", advanceLinear], ["eased", advanceEased],
                           ["scroll", advanceScroll]]) {
  for (const ms of [90, 160, 200, 280]) {
    const got = settleOf(adv, ms);
    /*
     * NEVER LATE, and never early by more than the invisible tail.
     *
     * This was a symmetric +/- one frame, and the step-based termination broke
     * it honestly: once a frame can no longer move the picture the slide ends,
     * so a long eased duration finishes EARLY -- 280ms lands at 236. That is
     * the animation actually being over, not the constant lying, and cutting
     * dead frames is the fix for the "one pixel too far, then snap" report.
     *
     * So the two directions are asserted differently, because they mean
     * different things. LATE is still a hard failure at one frame: it would
     * mean tau no longer matches the snap threshold, which is the ms/3
     * regression this assertion exists to catch. EARLY is bounded at 20%,
     * which the exponential tail can reach and a broken tau cannot fake --
     * the ms/3 mutant runs LONG, not short.
     */
    ok(got - ms <= TICK,
       name + ": ms=" + ms + " settles at " + Math.round(got) +
       "ms, never LATER than the duration it names");
    /*
     * How early is acceptable depends on the CURVE, and one bound for both
     * would hide the difference that matters when choosing between them.
     *
     * LINEAR travels at a constant rate, so its sub-pixel tail is one frame at
     * most and `ms` really is the settle time -- measured within 6% at every
     * duration. That is asserted tightly, because a linear slide finishing
     * early would mean something else is wrong.
     *
     * EASED decays, so the stretch that cannot move a pixel grows with the
     * duration: measured 9% early at 200ms, 22% at 280, 27% at 400. That is
     * the animation genuinely being over, not the constant lying -- but it
     * does mean `ms` is a nominal duration for eased and a real one for
     * linear, which is worth knowing before picking one.
     */
    const earlyBudget = (adv === advanceLinear) ? 0.10 : 0.30;
    ok(ms - got <= ms * earlyBudget,
       name + ": ms=" + ms + " settles at " + Math.round(got) + "ms, within " +
       Math.round(earlyBudget * 100) + "% -- only the sub-pixel tail may be cut");
  }
}
/* The consequence worth stating in its own right: the two curves are now
   comparable, which is what lets SLIDE_MS be one number instead of a number
   plus a note about which advance it was measured against. */
ok(Math.abs(settleOf(advanceLinear, SLIDE_MS) - settleOf(advanceEased, SLIDE_MS)) <= TICK,
   "the two advances settle at the same time for the same ms");
ok(SLIDE_MS > 0 && settleOf(advanceScroll, SLIDE_MS) <= SLIDE_MS + TICK,
   "the SHIPPING duration and advance settle within one frame of SLIDE_MS");

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
    /* A FOOTER IS SUPPLIED SO THAT THE BAND-CONTAINMENT ASSERTION HAS SOMETHING
       TO CATCH. Without it drawFooter returns at its own no-hints guard, so a
       mutant that wrongly gated the footer behind bankBar drew nothing either
       way and the assertion reported green -- verified, it survived. The
       footer is the nearest band to the sliding region and the one Task 5 also
       redraws fixed, which makes it the realistic over-suppression target. */
    title: "T", pageIndex: 1, pageCount: 5, touched: -1, viz: [],
    footer: ["BACK EXIT", "CLK OPEN"], ...extra,
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

  /* OVER-SUPPRESSION IS THE UNCOVERED FAILURE, and nothing above can see it.
     Every assertion so far looks at row BAR_Y alone or compares two spellings
     of the same option, so a guard that also swallowed the footer, or the
     second knob row, would pass all four. Suppression must change the
     indicator band and NOTHING else. */
  const noBarPx = noBar.pixels, withBarPx = withBar.pixels;
  let diffRows = [];
  for (let y = 0; y < 64; y++) {
    for (let x = 0; x < 128; x++) {
      if (withBarPx[y * 128 + x] !== noBarPx[y * 128 + x]) { diffRows.push(y); break; }
    }
  }
  /* No companion "the frames differ at all" check: with ink at BAR_Y by
     default and none with the option, they differ at BAR_Y by construction --
     it could only fail alongside an assertion above, which is what a vacuous
     one looks like. */
  ok(diffRows.every((y) => y >= RM.BAR_Y && y < RM.BAR_Y + 3),
     "suppression touches only the indicator band, no other row");
}


/* NO TWO FRAMES OF A SLIDE MAY RENDER THE SAME PICTURE.
 *
 * Reported from the device as the page "going one pixel too far and snapping
 * back". It was not an overshoot: with the snap at half a pixel, the remaining
 * travel sits between 0.5px and 1.5px for two consecutive frames, Math.round
 * renders the incoming page ONE PIXEL SHORT of home in both, and then it jumps
 * that last pixel. Held-then-jumped is what reads as an overshoot.
 *
 * Asserted on the RENDERED OFFSET, not on the position: the position is
 * strictly monotone in both cases, so a position-based check passes while the
 * screen holds still. Mutating SNAP_PAGES back to 0.5/128 fails this. */
for (const [label, adv] of [["linear", advanceLinear], ["eased", advanceEased], ["shipping", advanceScroll]]) {
  for (const ms of [90, 160, 200, 280]) {
    let pos = 0, n = 0, repeats = 0, prev = null, seq = [];
    while (pos !== 1 && n < 200) {
      pos = adv(pos, 1, 16.7, ms); n++;
      /* what the composite actually draws: the incoming page x */
      const x = pos === 1 ? 0 : 128 - Math.round(pos * 128);
      if (prev !== null && x === prev) repeats++;
      prev = x; seq.push(x);
    }
    ok(repeats === 0, label + "@" + ms + "ms: every frame of the slide draws a DIFFERENT picture (" +
       repeats + " repeated, x = " + seq.join(" ") + ")");
    ok(seq[seq.length - 1] === 0, label + "@" + ms + "ms: the last frame is home, not one pixel short");
  }
}


/* A ZERO dt MUST NOT END THE SLIDE, and on this device dt is OFTEN zero.
 *
 * Date.now() here is quantized to ~11-12ms against a ~17ms tick, so two
 * consecutive ticks regularly read the SAME clock value. A step of zero
 * therefore means NO TIME HAS PASSED -- not "there is no progress left to
 * draw", which is what the sub-pixel termination means. Conflating them ended
 * the slide on the first frame of most page changes; measured on hardware as
 * dt=0ms with the position snapping to the target and zero composited frames.
 *
 * Driven through a QUANTIZED clock rather than a clean one, because a test
 * that hands the advance a tidy 17ms every time cannot see this at all -- and
 * did not: every other assertion in this file was green while the animation
 * was invisible on the panel. */
for (const [label, adv] of [["linear", advanceLinear], ["eased", advanceEased], ["shipping", advanceScroll]]) {
  ok(adv(0, 1, 0, 160) === 0,
     label + ": a dt of ZERO leaves the position alone (it must not settle)");

  /*
   * The real shape, taken from the hardware log rather than invented.
   *
   * The dt that matters is NOT tick-to-tick -- a 17ms tick always crosses a
   * 12ms quantum, so that gap is never zero and a probe built on it cannot see
   * this bug (my first one could not, and reported 0 zero-dt ticks).
   *
   * It is JOG-to-TICK: aimScroll stamps the clock when the page changes, and
   * the very next tick reads Date.now() a few milliseconds later -- inside the
   * SAME quantum, so dt is 0. That is the first frame of the slide, and
   * ending it there is ending the whole animation.
   */
  const QUANTUM = 12;
  const q = (t) => Math.floor(t / QUANTUM) * QUANTUM;
  let zeros = 0, worst = 0;
  for (let offsetIntoQuantum = 0; offsetIntoQuantum < QUANTUM; offsetIntoQuantum++) {
    let realT = 1000 + offsetIntoQuantum;
    const stamp = q(realT);            /* aimScroll, at jog time */
    let pos = 0, frames = 0, last = stamp;
    for (let i = 0; i < 400 && pos !== 1; i++) {
      realT += (i === 0) ? 4 : 17;     /* the first tick lands ~4ms after the jog */
      const clock = q(realT);
      const dt = clock - last;
      last = clock;
      if (i === 0 && dt === 0) zeros++;
      pos = adv(pos, 1, dt, 160);
      frames++;
    }
    if (worst === 0 || frames < worst) worst = frames;
  }
  ok(zeros > 0,
     label + ": the jog-to-tick gap really does land inside one clock quantum (" +
     zeros + " of " + QUANTUM + " phases give dt=0)");
  ok(worst >= 6,
     label + ": the slide survives a dt=0 first frame at EVERY clock phase -- " +
     "worst case " + worst + " frames, not 1");
}


/* A SHORT TICK IS NOT THE END OF THE SLIDE.
 *
 * The termination above asks whether a whole FRAME at this rate could move a
 * pixel. Judging the raw step instead breaks linear, whose step is dt/ms: a
 * 1ms tick moves 0.006 of a page, which is sub-pixel because barely any time
 * passed -- not because the motion is over. Measured, that ended a 160ms
 * linear slide on its first frame.
 *
 * The composite test caught this and this file did not, so it is pinned here
 * too: the two failures were in a page-render assertion whose connection to
 * the advance is several layers away, which is a slow way to find out. */
for (const [label, adv] of [["linear", advanceLinear], ["eased", advanceEased], ["shipping", advanceScroll]]) {
  for (const firstDt of [1, 2, 3]) {
    let pos = adv(0, 1, firstDt, 160);
    ok(pos !== 1,
       label + ": a " + firstDt + "ms first tick does not end the slide (a short tick " +
       "means little time passed, not that the motion is over)");
  }
}

process.exit(fail ? 1 : 0);
'
