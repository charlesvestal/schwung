# Page Slide Transition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Jogging between param-pages knob-grid pages slides horizontally instead of cutting, and the incoming page arrives with its values already populated.

**Architecture:** A translating `ctx` proxy renders the outgoing and incoming pages at abutting x offsets in one frame — no clip rectangle, no C change, no offscreen buffer, because `js_display_set_pixel` already discards off-screen writes and the body's ink stops at row 54. `render()` in `page_controller.mjs` is refactored so it can draw an arbitrary page index, and a new pure module owns the proxy and the composite so the filming tool and the device share one implementation.

**Tech Stack:** Plain ES modules (`.mjs`) run under QuickJS on device and Node in tests; `tests/host/*.sh` shell tests driving the real controller through `tools/param-pages/harness.mjs`; `tools/param-pages/movie.mjs` + ffmpeg for GIFs.

**User decisions (already made):**
- ~~"Body only", then "header + body as one 128px unit"~~ — **REVISED 2026-08-27 after looking at the filmed frames.** Final: **the body slides 128px; the page NAME slides within its own ~58px header column over the same duration; the module title, bank bar and footer are FIXED.** The title is identical on every page of a module, so sliding an identical copy in and out is motion carrying no information. The name gets a shorter travel because it is right-aligned — a full-width travel leaves ~2 of the 5 frames with no page name on screen.
- **Motion: 90ms, eased** (`SLIDE_MS = 90`, `advanceScroll = advanceEased`), five real frames. Chosen from filmed GIFs over two rounds. Known and accepted: at this budget linear uses the time better (three both-pages frames vs one), because eased front-loads 52% of the travel into frame one.
- **`ms` must mean the SETTLE time.** The original `tau = ms/3` made it mean 95% of travel, so nominal 90 settled at 164. Divisor derived from `SNAP_PAGES`, not written as a literal.
- **DO NOT BUILD OR DEPLOY TO THE DEVICE** until explicitly told. Stated 2026-08-27: the human is using the hardware for other work. Task 8 is on hold; everything else completes without it.
- **Every page change slides** — plain jog, Shift+jog level steps, and section-picker jumps. Distance is always one screen width regardless of how many pages were crossed.
- **Chase, not queue** — a jog arriving mid-slide retargets the incoming page and continues from the current offset.
- **Warm neighbours while idle** — prefetch adjacent pages' uncached keys on spare rotation stops, not with a blocking read at jog time.
- Design review additions: bank-bar suppression is an explicit `bankBar: false` option, **not** a `pageCount: 1` lie; frame assertions go on the pixel buffer, not on draw calls.
- The user asked for **real PNGs and GIFs**, not text mockups, for the motion decision.

**Spec:** `docs/superpowers/specs/2026-08-27-page-slide-transition-design.md`

**Branch/worktree:** `page-slide-transition` at `../schwung-page-slide`. All paths below are relative to that worktree root.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/shared/param_pages/page_transition.mjs` | **New.** Pure: the translating ctx proxy, slide offsets over time, and the two-page composite. No state, no I/O, no device globals. Imported by both the controller and the filming tool so they cannot drift. |
| `src/shared/param_pages/render_page_movy.mjs` | **Modify.** Gains one option, `bankBar: false`, so a sliding pass can suppress the indicator. |
| `src/shared/param_pages/page_controller.mjs` | **Modify.** Extract `drawPage(ctx, index, opts)` out of `render()`; add `s.transition` and start it from `onJog`/`goToPage`; add the neighbour prefetch lane to `tick()`. |
| `tools/param-pages/movie.mjs` | **Modify.** Gains transition scenes filmed at the device's real tick cadence with a quantized clock. |
| `tests/host/test_page_transition_proxy.sh` | **New.** Per-method translation of the proxy; unknown methods omitted rather than passed through. |
| `tests/host/test_page_slide_composite.sh` | **New.** Drives the real controller: mid-slide frame differs from both endpoints; bank bar and footer rows byte-identical to the un-animated destination frame; chase retargets without the offset jumping backwards. |
| `tests/host/test_neighbour_prefetch.sh` | **New.** Read **counts** for the prefetch lane. |
| `CLAUDE.md`, `../schwung-catalog-site/manual.html` | **Modify.** Required by the repo's Release Checklist for user-visible behaviour. |

---

## Task 1: The transition module — proxy, offsets, composite

**Goal:** A pure module that can translate any draw context horizontally and composite two page draws into one frame.

**Files:**
- Create: `src/shared/param_pages/page_transition.mjs`
- Test: `tests/host/test_page_transition_proxy.sh`

**Acceptance Criteria:**
- [ ] `translateCtx(ctx, dx)` returns a proxy where every wrapped method's x coordinates are shifted by `dx` and y coordinates are untouched
- [ ] `translateCtx(ctx, 0)` returns the original ctx object identically (no wrapper allocated)
- [ ] A method present on `ctx` but absent from the wrap table is **omitted** from the proxy, never passed through untranslated
- [ ] `scrollFrame(pos)` splits a scroll position into `{ base, frac }` with `frac` in `[0, 1)`
- [ ] `slideOffsets(frac, width)` returns abutting offsets: `to - from` is exactly `width`, for every `frac`
- [ ] `advanceLinear` and `advanceEased` both move `pos` toward `target` and **snap exactly onto it** rather than approaching asymptotically
- [ ] Neither advance function ever overshoots or moves away from the target
- [ ] `drawSlide` calls `drawFrom` and `drawTo` each exactly once, then `drawChrome` once, in that order

**Design note — why a scroll POSITION, not a from/to pair.** The obvious model
is `{ fromIndex, toIndex, startMs }`. It cannot chase: when a jog retargets
mid-slide, the page that was arriving is somewhere out at `+80px`, and a fresh
`from → to` pair starts its outgoing page at `0` — so it **snaps up to 128px
backwards** on the retarget frame, which is the exact discontinuity chase
exists to prevent. A single continuous position eased toward a target index
chases by construction: the target changes, the position does not.

**Verify:** `bash tests/host/test_page_transition_proxy.sh` → every line `PASS:`, exit 0

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_page_transition_proxy.sh`:

```bash
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
```

- [ ] **Step 2: Run it and watch it fail**

Run: `bash tests/host/test_page_transition_proxy.sh`
Expected: FAIL — `Cannot find module .../page_transition.mjs`

- [ ] **Step 3: Write the module**

Create `src/shared/param_pages/page_transition.mjs`:

```javascript
/**
 * page_transition.mjs — sliding one page off the screen while the next slides on.
 *
 * WHY THIS NEEDS NO CLIPPING, which is the whole reason it is cheap.
 *
 * Every draw both renderers make goes through the injected ctx, and
 * js_display_set_pixel (src/host/js_display.c) already discards anything
 * outside the 128x64 buffer. Render the outgoing page at dx = -N and the
 * incoming at dx = 128 - N and they occupy [-N, 128-N) and [128-N, 256-N):
 * they ABUT EXACTLY and the screen bounds do the rest. No clip rect, no C
 * change, no offscreen framebuffer, no JS blit.
 *
 * Two properties hold that up and must not quietly stop being true:
 *   - the two pages never overlap (slideOffsets is what guarantees it, and
 *     tests/host/test_page_transition_proxy.sh pins it at every t);
 *   - the body's ink stops at row 54 (ROW1_Y 33 + LBL1_Y 48 + a 7-row label),
 *     with RULE_Y 55 belonging to the footer, which the sliding passes suppress.
 *
 * Pure: no state, no device globals, no clock. Time is passed in, exactly as
 * anim_state.mjs takes `now` -- which is what lets tools/param-pages/movie.mjs
 * film a transition deterministically instead of racing a real clock.
 */

/**
 * How each draw method is translated.
 *
 * A method NOT in this table is deliberately OMITTED from the proxy rather
 * than passed through. Both renderers treat `line` and `drawArc` as optional
 * and fall back to a JS Bresenham drawn through ctx.fillRect, which IS
 * translated -- so omission degrades to correct-but-slower, while an
 * untranslated passthrough would draw in the wrong place. Neither failure
 * raises, so the direction of the safe default matters.
 */
const WRAP = {
    fillRect:   (f, dx) => (x, y, w, h, v) => f(x + dx, y, w, h, v),
    print:      (f, dx) => (x, y, t, c) => f(x + dx, y, t, c),
    /* No coordinates: a measurement is the same wherever it is drawn. It must
     * still be forwarded -- a missing textWidth makes every centred string
     * lay out against undefined. */
    textWidth:  (f) => (t) => f(t),
    setPixel:   (f, dx) => (x, y, v) => f(x + dx, y, v),
    drawRect:   (f, dx) => (x, y, w, h, v) => f(x + dx, y, w, h, v),
    line:       (f, dx) => (x0, y0, x1, y1, v) => f(x0 + dx, y0, x1 + dx, y1, v),
    drawLine:   (f, dx) => (x0, y0, x1, y1, v) => f(x0 + dx, y0, x1 + dx, y1, v),
    fillCircle: (f, dx) => (cx, cy, r, v) => f(cx + dx, cy, r, v),
    drawArc:    (f, dx) => (cx, cy, r, a, s, v) => f(cx + dx, cy, r, a, s, v),
};

/** A view of `ctx` shifted `dx` pixels horizontally. */
export function translateCtx(ctx, dx) {
    /* Identity at zero, by object, so the ordinary no-transition path is
     * literally the un-proxied one and cannot drift from it. */
    if (!ctx || !dx) return ctx;
    const out = {};
    for (const name of Object.keys(WRAP)) {
        const fn = ctx[name];
        if (typeof fn === "function") out[name] = WRAP[name](fn.bind(ctx), dx);
    }
    return out;
}

/**
 * THE SLIDE IS A POSITION, NOT A FROM/TO PAIR — and that is what makes a fast
 * jog chase instead of stutter.
 *
 * The obvious model is `{ fromIndex, toIndex, startMs }`, and it cannot chase.
 * When a jog retargets mid-slide, the page that was arriving is somewhere out
 * at +80px; a fresh from/to pair puts its outgoing page at 0, so the picture
 * SNAPS up to a full screen width backwards on the retarget frame. That is the
 * exact discontinuity chase exists to prevent.
 *
 * So the state is a single continuous `pos`, in page units, easing toward a
 * target index. Retargeting changes the target; the position carries on from
 * wherever it is. `s.pageIndex` remains the logical page — the one input, LEDs
 * and the screen reader belong to — and `pos` is purely what is drawn.
 */

/** Which two pages are on screen at scroll position `pos`, and how far along. */
export function scrollFrame(pos) {
    /* The epsilon keeps a position that arithmetic left at 2.9999999 from
     * reading as base 2 with frac 0.9999 -- one frame of the previous page
     * flashing at the moment of arrival. */
    const base = Math.floor(pos + 1e-6);
    const frac = pos - base;
    return { base, frac: frac < 1e-6 ? 0 : frac };
}

/**
 * Where the two pages sit at `frac` of the way between them.
 *
 * The offsets always differ by exactly one `width` — that is the no-overlap
 * guarantee the whole approach rests on, and it is asserted across the range
 * rather than at the endpoints.
 *
 * There is no direction argument: travelling backwards is `pos` DECREASING,
 * and `frac` is still the fraction between `base` and `base + 1`.
 */
export function slideOffsets(frac, width) {
    const from = -Math.round(frac * width);
    return { from, to: from + width };
}

/** How close to the target counts as arrived: under a quarter of a pixel. */
const SNAP_PAGES = 0.002;

/**
 * Constant-velocity advance: one page per `ms`.
 *
 * Both advances SNAP EXACTLY onto the target rather than approaching it. An
 * eased chase is naturally asymptotic, and a position a thousandth of a page
 * from home would leave the transition running for ever — two page renders
 * every frame, on a screen that has visibly finished moving.
 */
export function advanceLinear(pos, target, dtMs, ms) {
    if (!(ms > 0)) return target;
    const d = target - pos;
    if (Math.abs(d) <= SNAP_PAGES) return target;
    const step = dtMs / ms;
    if (step >= Math.abs(d)) return target;
    return d > 0 ? pos + step : pos - step;
}

/**
 * Exponential advance: velocity proportional to the distance left.
 *
 * Frame-rate independent by construction (`1 - exp(-dt/tau)`), which matters
 * here because this device's clock is quantized to ~11-12ms and the tick
 * interval is not constant — a naive `pos += d * k` would move at a speed that
 * depended on how the clock happened to round.
 */
export function advanceEased(pos, target, dtMs, ms) {
    if (!(ms > 0)) return target;
    const d = target - pos;
    if (Math.abs(d) <= SNAP_PAGES) return target;
    /* tau chosen so the visible motion is over in about `ms`: three time
     * constants is 95% of the distance. */
    const next = pos + d * (1 - Math.exp(-dtMs / (ms / 3)));
    return Math.abs(target - next) <= SNAP_PAGES ? target : next;
}

/**
 * Draw one composite frame: both pages, then the fixed chrome over them.
 *
 * Chrome LAST because the bank bar and the footer do not travel -- the bar is
 * the page indicator and cannot be unreadable for the duration of the page
 * change it is reporting.
 */
export function drawSlide(ctx, { fromDx, toDx, drawFrom, drawTo, drawChrome }) {
    drawFrom(translateCtx(ctx, fromDx));
    drawTo(translateCtx(ctx, toDx));
    if (drawChrome) drawChrome(ctx);
}

/**
 * Duration and advance.
 *
 * Chosen from filmed GIFs at the device's real cadence, not from a preview at
 * a frame rate the hardware cannot produce -- this device's clock is quantized
 * to ~11-12ms and the grid ticks at ~55Hz, so a 128px slide is about a dozen
 * real frames of ~12px each. See tools/param-pages/movie.mjs.
 *
 * SLIDE_MS of 0 disables the slide: advanceScroll returns the target
 * immediately, the position is always an integer, and every frame is the
 * ordinary un-composited draw.
 */
export const SLIDE_MS = 200;
export const advanceScroll = advanceLinear;
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `bash tests/host/test_page_transition_proxy.sh`
Expected: every line `PASS:`, exit 0

- [ ] **Step 5: Commit**

```bash
chmod +x tests/host/test_page_transition_proxy.sh
git add src/shared/param_pages/page_transition.mjs tests/host/test_page_transition_proxy.sh
git commit -m "anim: the translating ctx proxy, and why it needs no clip rect

The two pages abut exactly and set_pixel already discards what falls off
the screen, so a slide costs two renders and no new infrastructure.

An unknown draw method is OMITTED from the proxy rather than passed
through: both renderers guard line/drawArc with a typeof check and fall
back through fillRect, so omission is correct-but-slower and passthrough
is wrong pixels. Neither raises, which is why the test is per-method."
```

---

## Task 2: Suppress the bank bar on a sliding pass

**Goal:** `renderPageMovy` can be asked not to draw the page indicator, so the sliding passes leave it to the fixed chrome.

**Files:**
- Modify: `src/shared/param_pages/render_page_movy.mjs:2316` (the unconditional `drawBankBar` call)
- Test: `tests/host/test_page_transition_proxy.sh` (extend — same subject, no new file)

**Acceptance Criteria:**
- [ ] `renderPageMovy(ctx, { …, bankBar: false })` draws nothing on the bank-bar row
- [ ] Omitting the option draws the bar exactly as before (byte-identical frame)
- [ ] `pageCount: 1` is **not** used to achieve suppression

**Verify:** `bash tests/host/test_page_transition_proxy.sh` → every line `PASS:`, exit 0

**Steps:**

- [ ] **Step 1: Find the bank bar row constant**

Run: `command grep -n "BAR_Y" src/shared/param_pages/render_page_movy.mjs | head -3`
Expected: a `const BAR_Y = 7` (or similar) plus its uses in `drawBankBar`. Note the value — the test asserts on that row.

- [ ] **Step 2: Add the failing assertions**

Append to the node script in `tests/host/test_page_transition_proxy.sh`, before `process.exit`:

```javascript
/* THE BANK BAR IS SUPPRESSED BY AN OPTION, NOT BY A LIE.
   drawBankBar returns early when pageCount <= 1, so passing pageCount: 1 would
   also blank it -- and would be telling the renderer the page set has one page
   in order to obtain a drawing side effect. Every other thing that reads
   pageCount (and anything that later will) would be reading a fiction. */
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

const withBar = shotPage({});
const noBar = shotPage({ bankBar: false });
ok(rowInk(withBar, RM.BAR_Y) > 0, "the bank bar draws ink by default");
ok(rowInk(noBar, RM.BAR_Y) === 0, "bankBar:false leaves the indicator row empty");
ok(Buffer.from(withBar.pixels).toString("base64") ===
   Buffer.from(shotPage({ bankBar: undefined })).toString("base64"),
   "omitting the option is byte-identical to before");
```

Note: this needs `BAR_Y` exported. If `command grep -n "export const BAR_Y" src/shared/param_pages/render_page_movy.mjs` finds nothing, add `export` to its declaration in Step 4.

- [ ] **Step 3: Run to verify it fails**

Run: `bash tests/host/test_page_transition_proxy.sh`
Expected: FAIL on `bankBar:false leaves the indicator row empty` (the option is ignored, so the row still has ink)

- [ ] **Step 4: Implement**

In `src/shared/param_pages/render_page_movy.mjs`, export the row constant and gate the call. Change:

```javascript
    drawBankBar(ctx, o.pageIndex | 0, Math.max(1, o.pageCount | 0), o.pageGroups);
```

to:

```javascript
    /* A sliding page does not carry the indicator. The bar reports WHICH page
     * you are on, so travelling with the page it reports would make it
     * unreadable for the whole transition -- see page_transition.mjs.
     *
     * An explicit option, not `pageCount: 1`. That would also blank it (see
     * drawBankBar's early return) by telling the renderer the page set has one
     * page in order to obtain a drawing side effect. */
    if (o.bankBar !== false) {
        drawBankBar(ctx, o.pageIndex | 0, Math.max(1, o.pageCount | 0), o.pageGroups);
    }
```

And ensure the row constant is exported (find the `const BAR_Y = …` declaration and prefix it with `export`).

- [ ] **Step 5: Run the test to verify it passes**

Run: `bash tests/host/test_page_transition_proxy.sh`
Expected: every line `PASS:`, exit 0

- [ ] **Step 6: Check nothing else regressed**

Run: `for t in tests/host/test_movy*.sh tests/host/test_knob_card.sh tests/host/test_enum_peek.sh; do bash "$t" >/dev/null && echo "ok $t" || echo "FAIL $t"; done`
Expected: `ok` for each (skip any that does not exist)

- [ ] **Step 7: Commit**

```bash
git add src/shared/param_pages/render_page_movy.mjs tests/host/test_page_transition_proxy.sh
git commit -m "movy: bankBar:false, so a sliding page can leave the indicator behind

pageCount:1 would blank it too, via drawBankBar early return -- and would
be lying to the renderer about the page set to get a drawing side effect."
```

---

## Task 3: Film the transition, and pick duration and easing from GIFs

**Goal:** Real GIFs at the device's true cadence, from which the user chooses `SLIDE_MS` and `SLIDE_EASE`.

**Why this decision is still open:** it is the one question in the whole feature that cannot be answered in text. The user explicitly asked for "real pngs, and gifs" rather than mockups, and this repo's own `movie.mjs` header records that six of ten SCH-50 animation options were withdrawn precisely because a still frame strip renders duration as a frame count — "comparing two easings meant reading a number off a chart rather than feeling a motion."

**Files:**
- Modify: `tools/param-pages/movie.mjs`

**Acceptance Criteria:**
- [ ] A `slide` scene films a forward page change using `drawSlide` from `page_transition.mjs` — not a reimplementation of the composite
- [ ] Frames are sampled at the device's real tick cadence (~55Hz) with the clock quantized to the device's ~12ms quantum, not at a smooth continuous 30fps
- [ ] Variants are produced for at least 3 durations × 2 easings
- [ ] The scene does **not** assert `clipped() === 0`
- [ ] GIFs land in `catalog-out/movies/` and are shown to the user

**Verify:** `node tools/param-pages/movie.mjs --scene slide` → prints one line per variant naming a `.gif`, and the files exist

**Steps:**

- [ ] **Step 1: Add the scene**

In `tools/param-pages/movie.mjs`, add imports at the top alongside the existing ones:

```javascript
import { drawSlide, slideOffsets, scrollFrame, advanceLinear, advanceEased }
  from "../../src/shared/param_pages/page_transition.mjs";
```

Then add this function (a transition films two pages, so it does not fit the existing single-page `SCENES` shape — it gets its own renderer):

```javascript
/*
 * THE DEVICE CANNOT PRODUCE A SMOOTH 30FPS SLIDE, SO FILMING ONE IS THE WRONG
 * PROBE.
 *
 * Two hardware facts, both recorded in shadow_ui_param_pages.mjs: this
 * device's clock is quantized to roughly 11-12ms (proven there by 20
 * back-to-back Date.now() calls returning the identical value), and the grid
 * ticks at ~55Hz. A 128px slide over 200ms is therefore about a dozen real
 * frames of ~12px each, and the animation reads the clock through that
 * quantum. A film at a continuous 30fps would flatter the motion into
 * something the panel cannot show, and would report green on a slide that
 * stutters in the hand.
 *
 * So: sample at DEVICE_TICK_MS and quantize the clock the renderer is handed
 * to DEVICE_CLOCK_QUANTUM_MS. The GIF is then re-timed to the sampled
 * cadence for playback.
 */
const DEVICE_TICK_MS = 1000 / 55;
const DEVICE_CLOCK_QUANTUM_MS = 12;

const SLIDE_VARIANTS = [
    { name: "slide-160-linear", ms: 160, adv: advanceLinear },
    { name: "slide-200-linear", ms: 200, adv: advanceLinear },
    { name: "slide-280-linear", ms: 280, adv: advanceLinear },
    { name: "slide-160-eased", ms: 160, adv: advanceEased },
    { name: "slide-200-eased", ms: 200, adv: advanceEased },
    { name: "slide-280-eased", ms: 280, adv: advanceEased },
];

function renderSlideVariant(v) {
    const { j, metaIndex, groups } = loadPage({});
    const dir = path.join(OUT, v.name);
    fs.mkdirSync(dir, { recursive: true });
    for (const f of fs.readdirSync(dir)) fs.unlinkSync(path.join(dir, f));

    /* Two visibly different pages, so the motion is legible rather than one
     * identical grid sliding over another. */
    const pageA = j.page;
    const pageB = { ...j.page, name: "AMP", keys: j.page.keys.slice().reverse() };

    /* A beat of stillness at each end: motion is judged against rest, and a
     * GIF that loops straight from the last frame back to the first reads as
     * faster than the transition is. */
    const HOLD_MS = 260;
    const total = HOLD_MS + v.ms + HOLD_MS;
    const nFrames = Math.round(total / DEVICE_TICK_MS);

    const drawOne = (page, name) => (c) => RM.renderPageMovy(c, {
        page, metaIndex, values: j.values, title: j.title,
        pageIndex: j.pageIndex, pageCount: j.pageCount, touched: -1,
        viz: groups, pageLabel: name,
        /* Both suppressions the sliding passes make on device. */
        bankBar: false, footer: null,
        nowMs: 0, anim: createAnimState(),
    });

    /* Driven exactly as the controller drives it: a position advanced by the
     * elapsed time each tick, not a progress computed from a start stamp. A
     * film that computed t = (now - start)/ms would be testing a different
     * mechanism than the one that ships. */
    let pos = 0;
    let prevClock = 0;

    for (let i = 0; i < nFrames; i++) {
        const wall = i * DEVICE_TICK_MS;
        const clock = Math.floor(wall / DEVICE_CLOCK_QUANTUM_MS) * DEVICE_CLOCK_QUANTUM_MS;
        const dt = clock - prevClock;
        prevClock = clock;
        const target = clock < HOLD_MS ? 0 : 1;
        pos = v.adv(pos, target, dt, v.ms);

        const fb = createFramebuffer();
        const ctx = drawContext(fb);
        const { base, frac } = scrollFrame(pos);
        const pageAt = (b) => (b <= 0 ? pageA : pageB);
        const nameAt = (b) => (b <= 0 ? j.page.name : "AMP");

        if (frac === 0) {
            /* At rest: the ordinary un-proxied draw, chrome and all. */
            RM.renderPageMovy(ctx, {
                page: pageAt(base), metaIndex, values: j.values, title: j.title,
                pageIndex: j.pageIndex + base, pageCount: j.pageCount,
                touched: -1, viz: groups, pageLabel: nameAt(base),
                footer: j.footer, nowMs: clock, anim: createAnimState(),
            });
        } else {
            const off = slideOffsets(frac, 128);
            drawSlide(ctx, {
                fromDx: off.from, toDx: off.to,
                drawFrom: drawOne(pageAt(base), nameAt(base)),
                drawTo: drawOne(pageAt(base + 1), nameAt(base + 1)),
                drawChrome: (c) => {
                    /* The indicator reports the TARGET, which is where input
                     * already is -- it does not travel and it does not lag. */
                    RM.drawBankBar(c, j.pageIndex + target, j.pageCount, undefined);
                    if (j.footer) RM.drawFooter(c, j.footer);
                },
            });
        }
        /* NO clipped() ASSERTION HERE. A transition frame produces
         * out-of-bounds writes BY DESIGN -- that is exactly how the pages get
         * clipped at the screen edges without a clip rect. The probe measures
         * the wrong thing on these scenes and is deliberately not consulted. */
        fs.writeFileSync(path.join(dir, String(i).padStart(4, "0") + ".png"), fb.toPng(4));
    }

    const fps = Math.round(1000 / DEVICE_TICK_MS);
    const gif = path.join(OUT, v.name + ".gif");
    execFileSync("ffmpeg", ["-y", "-loglevel", "error", "-framerate", String(fps),
        "-i", path.join(dir, "%04d.png"),
        "-vf", "palettegen=max_colors=2:reserve_transparent=0:stats_mode=single",
        "-frames:v", "1", path.join(dir, "pal.png")]);
    execFileSync("ffmpeg", ["-y", "-loglevel", "error", "-framerate", String(fps),
        "-i", path.join(dir, "%04d.png"), "-i", path.join(dir, "pal.png"),
        "-lavfi", "paletteuse=dither=none", "-loop", "0", gif]);
    console.log(v.name.padEnd(20) + " " + nFrames + " frames, " + v.ms + "ms slide" +
        "  -> " + path.relative(ROOT, gif));
    return gif;
}
```

Wire it into `main()` — inside the `for (const n of names)` loop, before the `SCENES[n]` lookup:

```javascript
        if (n === "slide") { for (const v of SLIDE_VARIANTS) renderSlideVariant(v); continue; }
```

and add `slide` to the `--list` output by printing it after the `SCENES` loop:

```javascript
        console.log("slide".padEnd(9) + "page slide transition — " +
                    SLIDE_VARIANTS.length + " duration/easing variants");
```

- [ ] **Step 2: Run it**

Run: `node tools/param-pages/movie.mjs --scene slide`
Expected: six lines, each naming a `catalog-out/movies/slide-*.gif`

If `ffmpeg` is missing, install it (`brew install ffmpeg`) — the existing scenes need it too.

- [ ] **Step 3: Look at the frames before showing anything**

Run: `ls catalog-out/movies/slide-200-linear/ | head -5`
Then open a mid-transition frame and confirm two pages are visible side by side with the bank bar and footer sitting still. A frame showing only one page means the offsets or the suppression are wrong — fix before proceeding.

- [ ] **Step 4: Show the user and get the decision**

Send the six GIFs with `SendUserFile` and ask with `AskUserQuestion`:

- Question: "Which slide duration and advance?"
- Options must state, per variant: the duration in ms, whether it is constant-velocity (`linear`) or decelerating (`eased`), and **the real frame count** (`round(ms / 18.2)`) so the choice is made knowing 160ms is about 9 frames of ~14px each. State that nothing else about the feature changes with this choice — it sets two constants in `page_transition.mjs`, and both advances chase identically.

- [ ] **Step 5: Apply the decision**

Edit `SLIDE_MS` and the `advanceScroll` alias in `src/shared/param_pages/page_transition.mjs` to the chosen values (`advanceScroll = advanceLinear` or `= advanceEased`).

- [ ] **Step 6: Commit**

```bash
git add tools/param-pages/movie.mjs src/shared/param_pages/page_transition.mjs
git commit -m "tools: film the page slide at the device cadence, not at 30fps

The clock is quantized to ~12ms and the grid ticks at ~55Hz, so a 128px
slide is about a dozen frames of ~12px. A smooth 30fps film shows a
motion the panel cannot produce, and would report green on a slide that
stutters in the hand.

Duration and easing chosen from the GIFs."
```

---

## Task 4: Make `render()` able to draw an arbitrary page index

**Goal:** Extract `drawPage(ctx, index, opts)` from `render()`'s MOVY/LIST branch, with no behaviour change, so the outgoing page can be drawn once it is no longer current.

**Files:**
- Modify: `src/shared/param_pages/page_controller.mjs:2728-2901` (the `LAYOUT_MOVY || LAYOUT_LIST` branch of `render`), `:2709` (`drawKnobsAsList`), `:2937` (`vizGroups`)
- Test: `tests/host/test_page_slide_composite.sh` (created here, extended in Task 5)

**Acceptance Criteria:**
- [ ] `drawPage(ctx, index, { title, footer, chrome })` renders the page at `index`, not the current one, for all four page kinds (`PAGE_KNOBS`, `PAGE_MENU`, `PAGE_ITEMS`, `PAGE_PRESET`)
- [ ] With `chrome: false` it draws neither the bank bar nor the footer
- [ ] With no transition running, the rendered frame is byte-identical to the frame before this refactor
- [ ] `vizGroups` is parameterised by index and still caches (no re-detect per frame)

**Verify:** `bash tests/host/test_page_slide_composite.sh` → every line `PASS:`, exit 0

**Steps:**

- [ ] **Step 1: Capture a baseline frame before touching anything**

Create `tests/host/test_page_slide_composite.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE SLIDE, DRIVEN THROUGH THE REAL CONTROLLER.
#
# The renderer tests hand their inputs in directly, so they prove the renderer
# and never the wiring -- which is how every widget animation shipped inert
# (see tests/host/test_anim_wiring.sh). This drives createController and
# asserts on the PIXEL BUFFER, because on a 1-bit screen a stroke, a dither and
# a highlight all light the same pixel and a draw-call assertion can pass while
# the picture is wrong.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the page slide test" >&2
  exit 1
fi

node --input-type=module -e '
import { createController, LAYOUT_MOVY }
  from "./src/shared/param_pages/page_controller.mjs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import { BAR_Y } from "./src/shared/param_pages/render_page_movy.mjs";
import { RULE_Y } from "./src/shared/list_geometry.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* Enough params for three knob pages, so there is a page to slide to and a
   third to chase onto. */
const KEYS = [];
for (let i = 0; i < 24; i++) KEYS.push("p" + i);
const CHAIN_PARAMS = KEYS.map((k, i) => ({
  key: k, name: "P" + i, type: "float", min: 0, max: 1, step: 0.01,
}));
const HIER = { modes: null, levels: { root: { label: "T", knobs: KEYS,
  params: CHAIN_PARAMS.map((p) => ({ key: p.key })) } } };

let clock = 1000;
let reads = 0;
const store = {};
for (const k of KEYS) store[k] = "0.5";
const mkCtl = () => createController({
  getParam: (k) => {
    reads++;
    const b = String(k).replace(/^[^:]+:/, "");
    if (b === "ui_hierarchy") return JSON.stringify(HIER);
    if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
    return b in store ? store[b] : "";
  },
  setParam: () => {},
  announce: () => {},
  now: () => clock,
});

const ctl = mkCtl();
ctl.setLayout(LAYOUT_MOVY);
ctl.load({ prefix: "synth" });
for (let i = 0; i < 60; i++) { clock += 18; ctl.tick(); }

ok(ctl.state.pages.length >= 3, "the fixture plans at least three pages");

const shot = (opts) => {
  const fb = createFramebuffer();
  ctl.render(drawContext(fb), opts || { title: "T", footer: [["CLK", "OPEN"]] });
  return fb;
};
const key = (fb) => Buffer.from(fb.pixels).toString("base64");
const band = (fb, y0, y1) => Buffer.from(fb.pixels.slice(y0 * 128, y1 * 128)).toString("base64");

/* drawPage must be able to draw a page that is NOT the current one -- that is
   the whole reason for the extraction, and nothing else in the file needed it. */
ok(typeof ctl.drawPage === "function", "the controller exposes drawPage");
const cur = ctl.state.pageIndex;
const fbCur = createFramebuffer();
ctl.drawPage(drawContext(fbCur), cur, { title: "T", footer: [["CLK", "OPEN"]] });
const fbOther = createFramebuffer();
ctl.drawPage(drawContext(fbOther), cur + 1, { title: "T", footer: [["CLK", "OPEN"]] });
ok(key(fbCur) !== key(fbOther),
   "drawPage at a different index draws a different page, not the current one");
ok(key(fbCur) === key(shot()),
   "drawPage at the current index reproduces the ordinary render exactly");

/* chrome:false is what a sliding pass uses. */
const fbNoChrome = createFramebuffer();
ctl.drawPage(drawContext(fbNoChrome), cur, { title: "T", footer: [["CLK", "OPEN"]], chrome: false });
let barInk = 0, footInk = 0;
for (let x = 0; x < 128; x++) if (fbNoChrome.pixels[BAR_Y * 128 + x]) barInk++;
for (let y = RULE_Y; y < 64; y++)
  for (let x = 0; x < 128; x++) if (fbNoChrome.pixels[y * 128 + x]) footInk++;
ok(barInk === 0, "chrome:false draws no bank bar");
ok(footInk === 0, "chrome:false draws no footer");

process.exit(fail ? 1 : 0);
'
```

- [ ] **Step 2: Run to verify it fails**

Run: `bash tests/host/test_page_slide_composite.sh`
Expected: FAIL on `the controller exposes drawPage`

- [ ] **Step 3: Extract `drawPage`**

In `src/shared/param_pages/page_controller.mjs`:

**3a.** Parameterise `vizGroups` by index. Change the existing function to:

```javascript
    let vizCache = null;
    function vizGroupsFor(index) {
        const p = s.pages[index];
        if (!p || p.kind !== PAGE_KNOBS || !s.metaIndex) return [];
        const cacheKey = `${s.fingerprint}#${index}`;
        if (vizCache && vizCache.key === cacheKey) return vizCache.groups;
        const { groups } = resolveViz({ keys: p.keys, metaIndex: s.metaIndex, overrides: vizOverrides });
        vizCache = { key: cacheKey, groups };
        return groups;
    }
    /* The current page's graphics. Every other consumer (the peek, the extra
     * keys, the LED lane) asks about the page the user is ON. */
    function vizGroups() { return vizGroupsFor(s.pageIndex); }
```

**3b.** Parameterise `drawKnobsAsList`:

```javascript
    function drawKnobsAsList(ctx, index, title, footer, chrome) {
        const mp = s.pages[index];
        drawHeaderMovy(ctx, title || "", pageLabel(mp), false);
        if (chrome) drawBankBar(ctx, index | 0, Math.max(1, s.pages.length), pageGroups());
        const bottom = footer ? RULE_Y : 64;
        const entered = menuEntered() && index === s.pageIndex;
        drawPageChromeList(ctx,
            { x: MENU_LIST_X, y: MENU_LIST_Y, w: MENU_LIST_W, h: bottom - MENU_LIST_Y },
            knobListEntries(mp),
            entered ? knobRowIndex(mp) : -1,
            { editMode: entered && s.knobEditing });
        if (!entered) {
            drawBrackets(ctx, MENU_FRAME_X, MENU_FRAME_Y, MENU_FRAME_W,
                         bottom - MENU_FRAME_Y - MENU_FRAME_BOTTOM_INSET,
                         MENU_BRACKET_LEN);
        }
        if (chrome && footer) drawFooter(ctx, footer);
    }
```

**3c.** Extract the body of the `LAYOUT_MOVY || LAYOUT_LIST` branch into `drawPage`. Mechanically, for every line currently inside that branch:

| was | becomes |
|---|---|
| `page()` | `s.pages[index]` |
| `s.pageIndex` (as a draw argument) | `index` |
| `menuEntered()` | `menuEntered() && index === s.pageIndex` |
| `pageLabel()` | `pageLabel(s.pages[index])` |
| `vizGroups()` | `vizGroupsFor(index)` |
| `drawBankBar(...)` | `if (chrome) drawBankBar(...)` |
| `if (footer) drawFooter(...)` | `if (chrome && footer) drawFooter(...)` |
| `renderPageMovy(ctx, {…})` | add `bankBar: chrome` and pass `footer: chrome ? footer : null` |

The signature and the hint/picker handling:

```javascript
    /**
     * Draw ONE page, by index, into the given ctx.
     *
     * Parameterised by index rather than reading s.pageIndex because the page
     * slide has to draw the page you are LEAVING, which by then is no longer
     * current (see page_transition.mjs). Per-page state -- the menu cursor, the
     * items list, the preset browser -- is already keyed by page name in `s`,
     * so drawing a non-current index needs no new state.
     *
     * `chrome` is false on a sliding pass: the bank bar is the page INDICATOR
     * and does not travel with the page it indicates, and the footer stays put
     * with it. Both are drawn afterwards by the caller, unproxied.
     *
     * Overlays -- the hint panel, the section picker, the knob card, the enum
     * peek -- are NOT drawn here. They belong to the screen, not to a page, and
     * they must never slide.
     */
    function drawPage(ctx, index, { title, footer, chrome = true } = {}) {
        const mp = s.pages[index];
        if (knobsAsList(mp)) { drawKnobsAsList(ctx, index, title, footer, chrome); return; }
        if (mp && mp.kind === PAGE_ITEMS) { /* …existing ITEMS block, transformed… */ return; }
        if (mp && mp.kind === PAGE_PRESET) { /* …existing PRESET block, transformed… */ return; }
        if (mp && mp.kind === PAGE_MENU) { /* …existing MENU block, transformed… */ return; }
        renderPageMovy(ctx, {
            /* …existing options, with: */
            page: mp, pageIndex: index,
            viz: vizEnabled ? vizGroupsFor(index) : [],
            pageLabel: pageLabel(mp),
            bankBar: chrome,
            footer: chrome ? footer : null,
            /* …everything else unchanged… */
        });
    }
```

**3d.** `render()`'s MOVY/LIST branch becomes:

```javascript
        if (s.layout === LAYOUT_MOVY || s.layout === LAYOUT_LIST) {
            if (s.hintLines) {
                drawPage(ctx, s.pageIndex, { title, footer });
                renderHint(ctx, { rect, lines: s.hintLines.lines, title: s.hintLines.title });
                return;
            }
            if (s.pickerOpen) {
                /* …existing picker block, unchanged… */
                return;
            }
            drawPage(ctx, s.pageIndex, { title, footer });
            return;
        }
```

**3e.** Expose it. In the returned object near line 3023, add `drawPage` to the exported list.

- [ ] **Step 4: Run the test to verify it passes**

Run: `bash tests/host/test_page_slide_composite.sh`
Expected: every line `PASS:`, exit 0 — in particular `drawPage at the current index reproduces the ordinary render exactly`, which is what makes this a pure refactor.

- [ ] **Step 5: Run the whole host suite for regressions**

Run: `make -C tests/host test && for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 && echo "ok $(basename $t)" || echo "FAIL $(basename $t)"; done`
Expected: no `FAIL` lines other than the ~13 that fail on every branch because ripgrep is not installed locally (see the repo's known-failures note). Compare against `git stash && …` on a clean tree if unsure which those are.

- [ ] **Step 6: Commit**

```bash
git add src/shared/param_pages/page_controller.mjs tests/host/test_page_slide_composite.sh
git commit -m "grid: render() can draw an arbitrary page index

Pure refactor -- drawPage at the current index is byte-identical to the
old render, which the test asserts. The slide needs to draw the page you
are LEAVING, and by then it is no longer s.pageIndex."
```

---

## Task 4b: Split drawHeader — fixed title, sliding name

**Added 2026-08-27**, after the filmed frames showed the module title sliding
out and a byte-identical copy sliding back in. Full task text lives in the
harness task list (`Task 4b`); the short of it:

`drawHeader` (`render_page_movy.mjs:723`) draws the title and the name
together and **measures the right side first, giving the left the remainder**
— so the split DEPENDS on the name, and two different names mid-transition
would move the title. Decompose it into title-only and name-only draws
sharing ONE split computation, and pin that split to the DESTINATION name.

`drawPage` gains `header: false` so a sliding pass leaves rows 0–6 untouched.
The composite then draws the two names at column-relative offsets and the
fixed title over them — the title's opaque redraw is what clips the outgoing
name's left edge.

The name travels its COLUMN width (~58px), not 128px. At 90ms a full-width
travel leaves roughly two of the five frames with no page name at all.

Blocks Task 5.

---

## Task 5: Run the slide

**Goal:** A page change starts a transition; the frame composites two pages with fixed chrome; a jog mid-slide chases.

**Files:**
- Modify: `src/shared/param_pages/page_controller.mjs` (`onJog:1957-2028`, `goToPage:2050-2072`, `render`, state init)
- Test: `tests/host/test_page_slide_composite.sh` (extend)

**Acceptance Criteria:**
- [ ] A page index change from `onJog` or `goToPage` leaves `s.scrollPos` behind the new `s.pageIndex`, so a slide is in progress
- [ ] A mid-slide frame differs from both the outgoing and the incoming settled frames
- [ ] The bank-bar row and the footer rows in a mid-slide frame are byte-identical to the settled destination frame
- [ ] The scroll settles exactly onto the target and the settled frame equals the plain destination frame
- [ ] A jog during a slide retargets **without `s.scrollPos` moving backwards** (the chase discontinuity)
- [ ] A jump of more than one page still slides exactly one screen width
- [ ] `SLIDE_MS = 0` disables the animation entirely (frames cut, as today)

**Verify:** `bash tests/host/test_page_slide_composite.sh` → every line `PASS:`, exit 0

**Steps:**

- [ ] **Step 1: Add the failing assertions**

Append to the node script in `tests/host/test_page_slide_composite.sh`, before `process.exit`:

```javascript
/* --- the slide itself --------------------------------------------------- */

const settledAt = (idx) => {
  const fb = createFramebuffer();
  ctl.drawPage(drawContext(fb), idx, { title: "T", footer: [["CLK", "OPEN"]] });
  return fb;
};

const from = ctl.state.pageIndex;
const before = key(settledAt(from));

clock += 18;
ctl.onJog(1);
const to = ctl.state.pageIndex;
ok(to !== from, "the jog moved the page index");
ok(ctl.state.scrollPos !== to,
   "the scroll position lags the page index -- that gap IS the slide");

/* Mid-flight. Advance the scroll the way the device does: through tick(). */
for (let i = 0; i < 5; i++) { clock += 18; ctl.tick(); }
const mid = shot();
const after = key(settledAt(to));
ok(ctl.state.scrollPos !== to, "still mid-slide five ticks in");
ok(key(mid) !== before && key(mid) !== after,
   "a mid-slide frame differs from BOTH endpoints -- with the composite " +
   "missing, every frame after a jog is already the destination");

/* The chrome does not travel. Compared as bands, on the pixel buffer. */
const settledTo = settledAt(to);
ok(band(mid, BAR_Y, BAR_Y + 2) === band(settledTo, BAR_Y, BAR_Y + 2),
   "the bank bar row is identical to the settled destination mid-slide");
ok(band(mid, RULE_Y, 64) === band(settledTo, RULE_Y, 64),
   "the footer rows are identical to the settled destination mid-slide");

/* And it ends -- EXACTLY on the target, not near it. An eased chase is
   asymptotic by nature, and a position a thousandth of a page from home
   leaves two renders per frame running on a screen that has stopped. */
for (let i = 0; i < 60; i++) { clock += 18; ctl.tick(); }
ok(ctl.state.scrollPos === to, "the scroll lands exactly on the target");
ok(key(shot()) === after, "the settled frame is the plain destination page");

/* CHASE, NOT QUEUE, and the assertion is on the POSITION.
   A from/to pair cannot do this: retargeting starts its outgoing page at 0
   while the page actually on screen is out at +80px, so the picture snaps up
   to a whole screen width BACKWARDS on the retarget frame. Forward-only
   movement is the property that fails when someone reintroduces that model. */
clock += 18;
ctl.onJog(1);
for (let i = 0; i < 4; i++) { clock += 18; ctl.tick(); }
const posBefore = ctl.state.scrollPos;
ctl.onJog(1);
ok(ctl.state.scrollPos >= posBefore,
   "a jog mid-slide never moves the scroll position backwards (" +
   posBefore + " -> " + ctl.state.scrollPos + ")");
let monotone = true, prev = ctl.state.scrollPos;
for (let i = 0; i < 40; i++) {
  clock += 18; ctl.tick();
  if (ctl.state.scrollPos < prev) monotone = false;
  prev = ctl.state.scrollPos;
}
ok(monotone, "and it keeps travelling forward until it settles");

/* A MULTI-PAGE JUMP IS STILL ONE SCREEN WIDTH.
   Without the teleport the picker would scroll through every page it crossed,
   at whatever speed nine pages in 200ms is. */
for (let i = 0; i < 60; i++) { clock += 18; ctl.tick(); }
const jumpFrom = ctl.state.pageIndex;
const far = Math.min(ctl.state.pages.length - 1, jumpFrom + 3);
if (far - jumpFrom >= 2) {
  clock += 18;
  ctl.goToPage(far, { remember: false });
  ok(Math.abs(far - ctl.state.scrollPos) <= 1 + 1e-6,
     "a jump of " + (far - jumpFrom) + " pages still slides exactly one width");
} else {
  ok(true, "fixture too short to test a multi-page jump (skipped)");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `bash tests/host/test_page_slide_composite.sh`
Expected: FAIL on `the scroll position lags the page index -- that gap IS the slide`

- [ ] **Step 3: Add the scroll position**

In `src/shared/param_pages/page_controller.mjs`, add the import near the other `param_pages` imports:

```javascript
import { drawSlide, slideOffsets, scrollFrame, advanceScroll, SLIDE_MS }
    from "./page_transition.mjs";
```

Initialise the fields where the rest of `s` is initialised (alongside `s.peek`, `s.touched`):

```javascript
        /*
         * THE SLIDE IS A POSITION, NOT A FROM/TO PAIR.
         *
         * `pageIndex` is the LOGICAL page — what input, the knob LEDs and the
         * screen reader belong to, updated the instant the jog lands.
         * `scrollPos` is what is DRAWN, in page units, and it eases toward it.
         * When they are equal nothing is moving.
         *
         * A from/to pair cannot chase: retargeting mid-slide starts its
         * outgoing page at 0 while the page actually on screen is out at
         * +80px, so the picture snaps up to a screen width backwards. Here the
         * target changes and the position simply carries on.
         */
        scrollPos: 0,
        scrollLastMs: 0,
```

Add the retarget helper, next to `goToPage`:

```javascript
    /**
     * The page index moved; aim the scroll at it.
     *
     * Distance is always ONE screen width, however many pages were crossed —
     * a section-picker jump of nine pages is still one page arriving, not nine
     * pages flickering past at nine times the speed. That is what the teleport
     * is for: put the position one page away from the target and let the
     * ordinary advance cover it.
     *
     * Nothing else is needed for chase. The position is continuous, so a jog
     * during a slide changes only where it is heading.
     */
    function aimScroll(toIndex) {
        if (!(SLIDE_MS > 0)) { s.scrollPos = toIndex; return; }
        const d = toIndex - s.scrollPos;
        if (d > 1) s.scrollPos = toIndex - 1;
        else if (d < -1) s.scrollPos = toIndex + 1;
        s.scrollLastMs = now();
    }
```

Call it from `onJog`, inside the `if (s.pageIndex !== before)` block, as the first statement:

```javascript
        if (s.pageIndex !== before) {
            aimScroll(s.pageIndex);
            s.cursor = 0;
```

And from `goToPage`, immediately after `s.pageIndex` is assigned:

```javascript
        s.pageIndex = remember ? restoreSection(target) : target;
        aimScroll(s.pageIndex);
```

Also set `s.scrollPos = s.pageIndex` wherever the page set is (re)loaded, so a
newly planned page set does not slide in from nowhere. Find the assignment of
`s.pageIndex` in the load path and add it beside.

- [ ] **Step 4: Advance the position in `tick()`**

At the very TOP of `tick()`, before `flushDueWrites()` — **above every early
return**, because `tick()` returns early on preset and items pages and a slide
that stopped advancing there would freeze mid-travel:

```javascript
        /* The slide advances on the CLOCK, not per tick: this device's tick
         * interval is not constant and its clock is quantized to ~11-12ms, so
         * a fixed step per tick would move at a speed that depends on how the
         * rounding fell. */
        if (s.scrollPos !== s.pageIndex) {
            const t = now();
            const dt = Math.max(0, t - (s.scrollLastMs || t));
            s.scrollLastMs = t;
            s.scrollPos = advanceScroll(s.scrollPos, s.pageIndex, dt, SLIDE_MS);
        }
```

- [ ] **Step 5: Composite in `render()`**

Replace the final `drawPage(ctx, s.pageIndex, { title, footer });` in the MOVY/LIST branch with:

```javascript
            const { base, frac } = scrollFrame(s.scrollPos);
            if (frac !== 0 && s.pages[base] && s.pages[base + 1]) {
                const off = slideOffsets(frac, SCREEN_WIDTH);
                drawSlide(ctx, {
                    fromDx: off.from, toDx: off.to,
                    /* Body only. `header: false` because the header does not
                     * travel with the page — see Task 4b: the module title is
                     * identical on every page and only the NAME changes. */
                    drawFrom: (c) => drawPage(c, base, { title, footer, chrome: false, header: false }),
                    drawTo: (c) => drawPage(c, base + 1, { title, footer, chrome: false, header: false }),
                    /*
                     * The fixed chrome, drawn LAST and unproxied.
                     *
                     * ORDER IS LOAD-BEARING: the two page names first, then
                     * the module title OVER them. The title's opaque redraw is
                     * what clips the outgoing name's left edge — there is no
                     * clip rectangle anywhere in this feature.
                     *
                     * The name travels its COLUMN width, not the screen width:
                     * it is right-aligned, and at 90ms a full-width travel
                     * leaves two of the five frames with no page name at all.
                     *
                     * The indicator reports s.pageIndex — where input already
                     * is — rather than the scroll position, so it never lags
                     * the gesture it is reporting.
                     */
                    drawChrome: (c) => {
                        const nameOff = slideOffsets(frac, headerNameTravel(title, toName));
                        drawHeaderName(translateCtx(c, nameOff.from), title, toName, fromName);
                        drawHeaderName(translateCtx(c, nameOff.to), title, toName, toName);
                        drawHeaderTitle(c, title, toName);
                        drawBankBar(c, s.pageIndex | 0, Math.max(1, s.pages.length), pageGroups());
                        if (footer) drawFooter(c, footer);
                    },
                });
                return;
            }
            drawPage(ctx, s.pageIndex, { title, footer });
            return;
```

Import `SCREEN_WIDTH` from `../list_geometry.mjs` if it is not already imported in this file.

Note the `s.pages[base + 1]` guard: it is what keeps a scroll position left
stranded past the end of a shortened page set from indexing off the array.

- [ ] **Step 6: Run the test to verify it passes**

Run: `bash tests/host/test_page_slide_composite.sh`
Expected: every line `PASS:`, exit 0

- [ ] **Step 7: Confirm the disable path**

Run:
```bash
node --input-type=module -e '
import * as T from "./src/shared/param_pages/page_transition.mjs";
const ok = T.advanceLinear(0, 1, 18, 0) === 1 && T.advanceEased(0, 1, 18, 0) === 1;
console.log(ok ? "PASS: zero duration arrives immediately" : "FAIL");
'
```
Expected: `PASS: zero duration arrives immediately` — with `SLIDE_MS = 0`, `aimScroll` sets the position to the target outright, `frac` is always 0, and `render` never composites.

- [ ] **Step 8: Regression sweep**

Run: `for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $(basename $t)"; done`
Expected: only the known ripgrep-related failures.

- [ ] **Step 9: Commit**

```bash
git add src/shared/param_pages/page_controller.mjs tests/host/test_page_slide_composite.sh
git commit -m "grid: pages slide, and a fast jog chases rather than queues

The slide is a POSITION eased toward pageIndex, not a from/to pair. A
pair cannot chase: retargeting starts its outgoing page at 0 while the
page on screen is out at +80px, so the picture snaps a screen width
backwards on the retarget frame.

The bank bar and the footer are drawn last and unproxied, and the bar
reports pageIndex rather than the scroll position -- the indicator must
not lag the gesture it is reporting."
```

---

## Task 6: Warm the neighbours

**Goal:** Adjacent pages' uncached values are read on spare rotation stops, so the incoming page arrives populated.

**Files:**
- Modify: `src/shared/param_pages/page_controller.mjs:1225-1250` (the read rotation in `tick()`)
- Test: `tests/host/test_neighbour_prefetch.sh`

**Acceptance Criteria:**
- [ ] After settling on a page, keys of pages ±1 that are absent from `s.values` get read
- [ ] Once the neighbours are warm, the lane issues **no** further reads — asserted as a read count over many ticks, not as "the values are present"
- [ ] The lane does not run during the first full pass after a page change
- [ ] The lane does not run while any key is inside its settle window (a knob is being turned)
- [ ] Jogging to a warmed page issues zero reads at jog time

**Verify:** `bash tests/host/test_neighbour_prefetch.sh` → every line `PASS:`, exit 0

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_neighbour_prefetch.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE PREFETCH IS ASSERTED AS A READ COUNT, NOT AS "THE VALUES ARE THERE".
#
# "The incoming page has its values" passes just as well with a lane that reads
# every tick forever -- and a parameter read is ~2.8ms against a ~18ms tick, so
# a lane that never goes quiet costs more than the whole page render it is
# decorating. What must be true is that it warms the neighbours and then STOPS.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the prefetch test" >&2
  exit 1
fi

node --input-type=module -e '
import { createController, LAYOUT_MOVY }
  from "./src/shared/param_pages/page_controller.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const KEYS = [];
for (let i = 0; i < 24; i++) KEYS.push("p" + i);
const CHAIN_PARAMS = KEYS.map((k, i) => ({
  key: k, name: "P" + i, type: "float", min: 0, max: 1, step: 0.01,
}));
const HIER = { modes: null, levels: { root: { label: "T", knobs: KEYS,
  params: CHAIN_PARAMS.map((p) => ({ key: p.key })) } } };

const readsOf = [];
let clock = 1000;
const ctl = createController({
  getParam: (k) => {
    const b = String(k).replace(/^[^:]+:/, "");
    readsOf.push(b);
    if (b === "ui_hierarchy") return JSON.stringify(HIER);
    if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
    return KEYS.indexOf(b) >= 0 ? "0.5" : "";
  },
  setParam: () => {},
  announce: () => {},
  now: () => clock,
});
ctl.setLayout(LAYOUT_MOVY);
ctl.load({ prefix: "synth" });

const pages = ctl.state.pages;
ok(pages.length >= 3, "the fixture plans at least three pages");
const nextKeys = pages[ctl.state.pageIndex + 1].keys.filter(Boolean);

/* Run long enough for the current page AND its neighbours to warm. */
for (let i = 0; i < 200; i++) { clock += 18; ctl.tick(); }

const warmed = nextKeys.filter((k) => k in ctl.state.values).length;
ok(warmed === nextKeys.length,
   "every key of the NEXT page is cached without ever having visited it");

/* And now it must go quiet. Count reads over a long idle stretch and require
   that none of them names a neighbour key. */
readsOf.length = 0;
for (let i = 0; i < 200; i++) { clock += 18; ctl.tick(); }
const strayNeighbour = readsOf.filter((k) => nextKeys.indexOf(k) >= 0 &&
  (ctl.state.pages[ctl.state.pageIndex].keys || []).indexOf(k) < 0).length;
ok(strayNeighbour === 0,
   "once warm, the lane issues NO further neighbour reads (" + strayNeighbour + " seen)");

/* Arriving costs nothing. */
readsOf.length = 0;
clock += 18;
ctl.onJog(1);
ok(readsOf.length === 0, "the jog itself issues no reads");

/* The first pass after arriving belongs to the page you are ON. */
const arrivedKeys = ctl.state.pages[ctl.state.pageIndex].keys.filter(Boolean);
readsOf.length = 0;
for (let i = 0; i < arrivedKeys.length; i++) { clock += 18; ctl.tick(); }
const ownShare = readsOf.filter((k) => arrivedKeys.indexOf(k) >= 0).length;
ok(ownShare === readsOf.length,
   "the first full pass after a page change reads only the arrived page");

process.exit(fail ? 1 : 0);
'
```

- [ ] **Step 2: Run to verify it fails**

Run: `bash tests/host/test_neighbour_prefetch.sh`
Expected: FAIL on `every key of the NEXT page is cached without ever having visited it`

- [ ] **Step 3: Implement the lane**

In `tick()`, replace the `stops`/`at` computation:

```javascript
        const extraKeys = vizEnabled ? vizExtraKeys() : [];
        const stops = p.keys.length + 1 + extraKeys.length;
        const at = s.cursor % stops;
        s.cursor = (s.cursor + 1) % stops;
```

with:

```javascript
        const extraKeys = vizEnabled ? vizExtraKeys() : [];
        /*
         * THE NEIGHBOUR LANE — why the incoming page arrives populated.
         *
         * A page change is a slide now, and a page whose cells fill in one by
         * one while it travels is exactly what the slide was added to avoid.
         * The rotation already reads one parameter per tick; this spends a
         * stop on a key belonging to page +/-1 that is not yet cached.
         *
         * Bounded by construction: only UNCACHED keys, only the two adjacent
         * pages, so it goes quiet on its own and stays quiet. A lane that kept
         * reading would cost ~2.8ms per tick -- more than the 1.68ms whole-page
         * render it decorates -- which is why the test counts reads rather than
         * checking that the values are present.
         *
         * The stop is CONDITIONAL, so a warm neighbourhood costs nothing at
         * all. That makes `stops` change by one as the lane opens and closes,
         * which shifts `at` by one for a tick. Harmless: the rotation is a
         * refresh loop, not a sequence with meaning -- a key merely gets its
         * turn one tick early or late.
         */
        const warmKey = neighbourPrefetchKey(p);
        const stops = p.keys.length + 1 + extraKeys.length + (warmKey ? 1 : 0);
        const at = s.cursor % stops;
        s.cursor = (s.cursor + 1) % stops;

        if (warmKey && at === stops - 1) {
            const v = getParam(fullKey(warmKey));
            /* The tri-state, same as everywhere: a read that did not complete
             * must not be cached as a value. Leaving it absent simply means
             * the lane tries it again. */
            if (v !== null && v !== undefined && v !== "") s.values[warmKey] = v;
            return null;
        }
```

Add the helper above `tick()`:

```javascript
    /**
     * One uncached key belonging to an adjacent page, or null when both are warm.
     *
     * Held off for one full pass after a page change: the page you have just
     * ARRIVED on is the one whose values are on screen, and it must not have to
     * share the rotation with a page nobody is looking at yet.
     *
     * Held off entirely while anything is settling — a settle window means a
     * knob is under a finger, and that key's own refresh is what the rotation
     * is for.
     */
    function neighbourPrefetchKey(cur) {
        if (s.tickCount < (s.prefetchHoldUntil || 0)) return null;
        for (const k in s.settleUntil) {
            if ((s.settleUntil[k] || 0) > s.tickCount) return null;
        }
        for (const d of [1, -1]) {
            const q = s.pages[s.pageIndex + d];
            if (!q || q.kind !== PAGE_KNOBS || !q.keys) continue;
            for (const k of q.keys) {
                if (!k) continue;
                if (cur.keys.indexOf(k) >= 0) continue;
                if (!(k in s.values)) return k;
            }
        }
        return null;
    }
```

Arm the hold on every page change. In `onJog`'s `if (s.pageIndex !== before)` block and in `goToPage` after the index assignment, add:

```javascript
            /* One full pass for the page you arrived on before warming
             * anything else. A page of 8 knobs is 9 stops, ~0.16s. */
            s.prefetchHoldUntil = s.tickCount + 12;
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `bash tests/host/test_neighbour_prefetch.sh`
Expected: every line `PASS:`, exit 0

- [ ] **Step 5: Confirm the slide test still passes**

Run: `bash tests/host/test_page_slide_composite.sh`
Expected: every line `PASS:`, exit 0

- [ ] **Step 6: Commit**

```bash
chmod +x tests/host/test_neighbour_prefetch.sh
git add src/shared/param_pages/page_controller.mjs tests/host/test_neighbour_prefetch.sh
git commit -m "grid: warm the neighbouring pages on spare rotation stops

A page whose cells fill in while it slides is what the slide was added to
avoid. Only uncached keys, only the two adjacent pages, so the lane goes
quiet and stays quiet -- asserted as a read COUNT, because a lane that
never stops passes any test that merely checks the values are there."
```

---

## Task 7: Documentation

**Goal:** The repo's Release Checklist items for user-visible behaviour are done.

**Files:**
- Modify: `CLAUDE.md` (a subsection under the param-pages / knob-grid material, beside "Widget animation, and the wiring that carries it")
- Modify: `../schwung-catalog-site/manual.html`

**Acceptance Criteria:**
- [ ] `CLAUDE.md` records: what slides and what does not and why; that the mechanism needs no clip rect and why; that `clipped()` is not a correctness probe on transition frames; chase-not-queue; and the neighbour lane's read-count invariant
- [ ] `manual.html` mentions the slide in user-facing terms
- [ ] Neither claims hardware verification that has not happened

**Verify:** `command grep -n "page_transition" CLAUDE.md` → at least one hit; `command grep -in "slide" ../schwung-catalog-site/manual.html` → at least one hit

**Steps:**

- [ ] **Step 1: Write the CLAUDE.md section**

Add after the "Widget animation, and the wiring that carries it" section:

```markdown
### Pages SLIDE, and the slide needs no clip rectangle

A page change (jog, Shift+jog, or a section-picker jump) slides horizontally:
rows 0–54 travel as one 128px unit while the **bank bar (row 7) and the footer
stay put** — the bar is the page INDICATOR and cannot be unreadable for the
duration of the page change it reports.

`src/shared/param_pages/page_transition.mjs` is the whole mechanism and it is
pure. Both renderers draw through the injected `ctx`, and `js_display_set_pixel`
already discards anything off-screen, so rendering the outgoing page at
`dx = -N` and the incoming at `dx = 128 - N` puts them at `[-N, 128-N)` and
`[128-N, 256-N)`: **they abut exactly** and the screen bounds do the clipping.
No clip rect, no C change, no offscreen buffer, no JS blit. Cost is two page
renders (~3.4ms) against a ~18ms tick.

**An unknown draw method is OMITTED from the proxy, never passed through.**
Both renderers guard `line`/`drawArc` with a `typeof` check and fall back to a
JS Bresenham drawn through `fillRect`, which IS translated — so a forgotten
method degrades to correct-but-slower, while an untranslated passthrough draws
in the wrong place. Neither raises, which is why
`tests/host/test_page_transition_proxy.sh` asserts translation **per method**
rather than presence.

**`fb.clipped()` is not a correctness probe on a transition frame.** A
transition produces out-of-bounds writes by design — that is how the pages get
clipped at the edges — so those scenes deliberately do not consult it, and
correspondingly lose it as a check against real overflow.

`render()` draws by INDEX (`drawPage(ctx, index, { title, footer, chrome })`),
because the slide has to draw the page you are LEAVING and by then it is no
longer `s.pageIndex`. Per-page state is already keyed by page name, so a
non-current index needs no new state. `chrome: false` suppresses the bank bar
(via `renderPageMovy`'s `bankBar: false`, **not** a `pageCount: 1` lie) and the
footer; both are drawn afterwards, unproxied.

**The slide is a POSITION, not a from/to pair, and that is what makes a fast
jog chase.** `s.pageIndex` is the logical page — what input, the knob LEDs and
the screen reader belong to, updated the instant the jog lands — and
`s.scrollPos` is what is drawn, easing toward it. A retarget changes only where
it is heading. A `{ fromIndex, toIndex, startMs }` pair **cannot** do this:
retargeting starts its outgoing page at 0 while the page actually on screen is
out at +80px, so the picture snaps up to a screen width BACKWARDS on the
retarget frame — the exact discontinuity chase exists to prevent. The test
asserts forward-only movement of the position, which is what fails if that
model comes back.

Two consequences: a jump of more than one page **teleports** the position to one
page away first, so a nine-page picker jump is still one screen width of travel
rather than nine pages flickering past; and both advances **snap exactly** onto
the target, because an eased chase is asymptotic by nature and a position a
thousandth of a page from home would leave two renders per frame running on a
screen that has visibly stopped.

Duration and advance were chosen from filmed GIFs, not from a preview:
`node tools/param-pages/movie.mjs --scene slide` films at the device's real
cadence with the clock quantized to its ~12ms quantum, because a smooth 30fps
film shows a motion the panel cannot produce.

**The incoming page arrives populated because the neighbours are warmed while
idle.** The `tick()` rotation spends a conditional stop on an uncached key of
page ±1 (`neighbourPrefetchKey`), held off for one full pass after a page change
and entirely while anything is settling. Only uncached keys and only two pages,
so it goes quiet and stays quiet — `tests/host/test_neighbour_prefetch.sh`
asserts that as a read **count**, because "the values are there" passes just as
well with a lane that reads every tick forever, at ~2.8ms a read against a
1.68ms whole-page render.

Rejected: a blocking prefetch at jog time — up to eight uncached keys is ~22ms
of dead time on a page's first visit, a visible hitch on the exact gesture this
feature exists to smooth. Stale values are accepted deliberately: the ordinary
rotation corrects them within ~150ms of arrival, and a plausible stale number
beats a blank.
```

- [ ] **Step 2: Update the manual**

In `../schwung-catalog-site/manual.html`, find the section describing the knob grid / paging and add one user-facing sentence, e.g.:

```html
<p>Pages slide horizontally as you jog between them, and arrive with their
values already filled in. Spin the encoder quickly and the motion keeps up
rather than queueing.</p>
```

- [ ] **Step 3: Verify**

Run: `command grep -n "page_transition" CLAUDE.md && command grep -in "slide" ../schwung-catalog-site/manual.html | head -3`
Expected: hits in both

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: the page slide, and why it needs no clip rectangle

Also records the two probes that stop working on a transition frame:
clipped() counts out-of-bounds writes by design, and a prefetch test that
checks the values are present passes with a lane that never goes quiet."
```

Then in the catalog-site repo: `git -C ../schwung-catalog-site add manual.html && git -C ../schwung-catalog-site commit -m "manual: pages slide between one another"`

---

## Task 8: Hardware verification

**Goal:** Confirm on a real Move that the slide looks right and the values arrive populated.

**Files:** none

**Acceptance Criteria:**
- [ ] Deployed and the grid still renders
- [ ] A jog slides; the bank bar and footer do not move
- [ ] A fast spin keeps up rather than lagging
- [ ] An unvisited page arrives with values, not blanks or zeros
- [ ] `param_pages fps:` in `debug.log` has not dropped materially

**Verify:** visual, on device, plus the fps line in `debug.log`

**Steps:**

- [ ] **Step 1: Ask before deploying**

Deployment restarts the service under whatever the user is doing. Ask, then run `./scripts/build.sh` and `./scripts/install.sh local --skip-modules --skip-confirmation` — never `scp` individual files.

- [ ] **Step 2: Arm the fps log**

```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log | command grep param_pages"
```
Expected: `param_pages fps: NN draws / NN ticks / ~1000ms` with draws ≈ ticks and NN in the mid-50s, same as before the change.

- [ ] **Step 3: Exercise it**

Open a module's knob grid. Jog forward and back; Shift+jog; jump from the section picker; spin fast. Then jog onto a page never visited this session and check the cells carry values immediately.

- [ ] **Step 4: Disarm**

```bash
ssh ableton@move.local "rm -f /data/UserData/schwung/debug_log_on"
```
Leaving it armed has previously caused the dropouts it was being used to hunt.

- [ ] **Step 5: Open the PR**

`main` is branch-protected and all three CI checks are required.

```bash
git push -u origin page-slide-transition
gh pr create --title "Pages slide between one another, and arrive populated" --body "..."
```

Note: `gh pr merge` reports a failure it did not have when `main` is checked out in a worktree — confirm with `gh pr view <n> --json state,mergeCommit` rather than the exit status, and delete the remote branch yourself.
