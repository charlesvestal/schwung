# Chain-editor knob card — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the chain editor's bare `Value: 0.62` overlay with a bordered card showing the turned knob's whole row, drawn with the real knob-grid widgets.

**Architecture:** Three layers. `render_page_movy.mjs` gets its cell geometry parameterised so a row can be drawn at an arbitrary origin and width (default unchanged, proven by test). A new pure module `knob_card.mjs` owns the card's frame and calls that row. `shadow_ui.js` supplies touch state, the row's values (read once on touch-down, never on the draw path) and the draw call.

**Tech Stack:** ES modules (`.mjs`) run by QuickJS on device and by node in tests; `tools/param-pages/harness.mjs` renders them headlessly into a 128x64 framebuffer through the device's own font atlas.

**User decisions (already made):**
- "pixel truth obviously" — mockups come from the real renderers via `tools/param-pages/preview.mjs`, not sketches.
- Card contents: **inset card, row re-laid to fit** (not full-bleed, not a single control).
- Lifecycle: **on touch, gone on release** (with a no-touch fallback).
- Global slot knobs (nothing selected): **header-only card**.
- Frame: **F5 + 1px gap (H2/H4)** — 2px solid border, 1px black gap, then the header band.
- Whole-panel takeover was explicitly rejected: *"i find the whole panel jarring."*
- PR strategy: one PR for the whole branch, no split at `427ae5cd`.

**Spec:** `docs/superpowers/specs/2026-08-20-chain-knob-card-design.md`

---

## File structure

| File | Responsibility |
|---|---|
| `src/shared/param_pages/render_page_movy.mjs` (modify) | Cell geometry becomes a parameter; the row is exported as `drawKnobStrip`. |
| `src/shared/param_pages/knob_card.mjs` (create) | The card: rect maths, gutter, border, gap, header band, strip. Pure. |
| `src/shadow/shadow_ui.js` (modify) | Touch state, row resolution, value cache, the draw call, announcement. |
| `tests/host/test_knob_card.sh` (create) | Card pixel tests + the frame invariant + the geometry-inertness invariant. |
| `tests/host/test_chain_knob_card_reads.sh` (create) | Drawing the card costs zero `getSlotParam`. |
| `tools/param-pages/preview_knob_card.mjs` (create) | Renders the card over a real chain diagram, so the frame can be judged without a deploy. |

## Environment notes (read before Task 1)

- **`rg` is not installed.** `tests/host/*.sh` has **32 failing tests on every
  branch** for this reason. Confirm the count is unchanged rather than chasing
  them. Some print `rg is required to run this test`, others `rg: command not
  found` — filter on both.
- **`grep` is a shell function that silently swallows output.** Use `command
  grep`. An empty result is not proof of absence.
- Test `.sh` files use single-quoted `node -e '...'` blocks — **no apostrophes
  or single quotes anywhere inside, including comments.** Use backticks.
- `.serena/project.yml` is dirty at session start. Leave it; never commit it.
- Deploy is `./scripts/build.sh && ./scripts/install.sh local --skip-modules
  --skip-confirmation`. **Never scp individual files.** Verify with `md5sum`
  against the local build — timestamps lie, incremental builds preserve mtimes.

---

### Task 1: Parameterise cell geometry in the Movy row renderer

**Goal:** `render_page_movy.mjs` can draw a knob row at any origin and cell width, and its default output is byte-identical to before.

**Files:**
- Modify: `src/shared/param_pages/render_page_movy.mjs` (`drawDivableMark` ~661, `drawKnobWidget` ~665, `drawLabelCell` ~733, `drawKnobRow` ~757)
- Test: `tests/host/test_knob_card.sh` (created here, extended in Task 2)

**Acceptance Criteria:**
- [ ] `drawKnobStrip` is exported and takes a geometry `{ x0, cellW }`.
- [ ] `GRID_GEOM` is exported as the default `{ x0: 0, cellW: CELL_W }`.
- [ ] `renderPageMovy` output is **pixel-identical** to the pre-change output for every fixture module — this is the invariant, asserted against a snapshot captured before the refactor.
- [ ] A non-default geometry demonstrably moves the cells (a strip at `x0: 6, cellW: 29` puts ink in different columns than the default).
- [ ] `fb.clipped() === 0` and `missingGlyphs` empty for both geometries.

**Verify:** `bash tests/host/test_knob_card.sh` → `PASS` lines, exit 0

**Steps:**

- [ ] **Step 1: Capture the pre-change snapshot**

This must happen BEFORE touching the renderer, or the invariant proves nothing.

```bash
node -e '
Promise.all([
  import("./tools/param-pages/harness.mjs"),
  import("./tools/param-pages/cases.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/render_page_movy.mjs"),
  import("./src/shared/param_pages/viz.mjs"),
  import("node:fs"),
]).then(([H, C, P, M, RM, V, fs]) => {
  const fx = JSON.parse(fs.readFileSync(C.FIXTURE, "utf8"));
  const out = {};
  for (const mod of fx.modules) {
    const r = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    r.pages.forEach((p, i) => {
      if (p.kind !== P.PAGE_KNOBS) return;
      const values = {};
      for (const k of (p.keys || [])) if (k) values[k] = 0.5;
      const fb = H.createFramebuffer();
      const { groups } = V.resolveViz({ keys: p.keys || [], metaIndex });
      RM.renderPageMovy(H.drawContext(fb), {
        page: p, metaIndex, values, title: mod.id,
        pageIndex: i, pageCount: r.pages.length, touched: 2, viz: groups,
        footer: [["MUTE", "DFLT"], ["SHFT", "FINE"]],
      });
      out[mod.id + ":" + i] = Buffer.from(fb.pixels).toString("base64");
    });
  }
  fs.writeFileSync("tests/fixtures/movy-geom-baseline.json", JSON.stringify(out, null, 1));
  console.log("baseline pages: " + Object.keys(out).length);
});'
```

Expected: `baseline pages: <N>` with N > 0, and `tests/fixtures/movy-geom-baseline.json` written.

- [ ] **Step 2: Write the failing test**

Create `tests/host/test_knob_card.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Geometry and frame tests for the chain-editor knob card.
#
# Two different KINDS of assertion live here on purpose.
#
# The card tests are BEHAVIOUR: does it draw the right thing. The baseline test
# is an INVARIANT: parameterising CELL_W must not disturb the knob grid, a
# screen that was tuned deliberately and whose regressions are invisible in
# review. Behaviour tests cannot express that, because the grid is not what
# they draw.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the knob card tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./tools/param-pages/harness.mjs"),
  import("./tools/param-pages/cases.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/render_page_movy.mjs"),
  import("./src/shared/param_pages/viz.mjs"),
  import("node:fs"),
]).then(([H, C, P, M, RM, V, fs]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync(C.FIXTURE, "utf8"));

  /* ---- 1. the geometry parameter exists and defaults to the grid ---- */
  if (typeof RM.drawKnobStrip !== "function") fail("drawKnobStrip is not exported");
  if (!RM.GRID_GEOM || RM.GRID_GEOM.x0 !== 0 || RM.GRID_GEOM.cellW !== RM.CELL_W)
    fail("GRID_GEOM must be {x0: 0, cellW: CELL_W}");
  console.log("PASS: geometry surface");

  /* ---- 2. INVARIANT: the default path is byte-identical ---- */
  const baseline = JSON.parse(fs.readFileSync("tests/fixtures/movy-geom-baseline.json", "utf8"));
  let checked = 0;
  for (const mod of fx.modules) {
    const r = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    r.pages.forEach((p, i) => {
      if (p.kind !== P.PAGE_KNOBS) return;
      const id = mod.id + ":" + i;
      if (!baseline[id]) return;
      const values = {};
      for (const k of (p.keys || [])) if (k) values[k] = 0.5;
      const fb = H.createFramebuffer();
      const g = V.resolveViz({ keys: p.keys || [], metaIndex }).groups;
      RM.renderPageMovy(H.drawContext(fb), {
        page: p, metaIndex, values, title: mod.id,
        pageIndex: i, pageCount: r.pages.length, touched: 2, viz: g,
        footer: [["MUTE", "DFLT"], ["SHFT", "FINE"]],
      });
      const now = Buffer.from(fb.pixels).toString("base64");
      if (now !== baseline[id])
        fail("renderPageMovy changed for " + id + " -- the CELL_W parameterisation " +
             "is NOT inert on the default path");
      checked++;
    });
  }
  if (checked === 0) fail("baseline covered no pages");
  console.log("PASS: default geometry inert across " + checked + " pages");

  /* ---- 3. a non-default geometry actually moves the cells ---- */
  {
    const params = [
      { key: "a", name: "Alpha", type: "float", min: 0, max: 1, step: 0.01 },
      { key: "b", name: "Beta",  type: "float", min: 0, max: 1, step: 0.01 },
      { key: "c", name: "Gamma", type: "float", min: 0, max: 1, step: 0.01 },
      { key: "d", name: "Delta", type: "float", min: 0, max: 1, step: 0.01 },
    ];
    const mi = M.buildMetaIndex({ hierarchy: null, chainParams: params });
    const page = { kind: "knobs", name: "T", keys: ["a", "b", "c", "d"], authored: true };
    const values = { a: 0.2, b: 0.4, c: 0.6, d: 0.8 };
    const draw = (geom) => {
      const fb = H.createFramebuffer();
      RM.drawKnobStrip(H.drawContext(fb), { page, metaIndex: mi, values, touched: -1 },
                       0, RM.ROW0_Y, RM.LBL0_Y, geom);
      return fb;
    };
    const a = draw(RM.GRID_GEOM);
    const b = draw({ x0: 6, cellW: 29 });
    if (Buffer.from(a.pixels).equals(Buffer.from(b.pixels)))
      fail("a 29px strip at x0=6 drew identically to the 32px grid strip -- the " +
           "geometry parameter is being ignored");
    if (a.clipped() !== 0 || b.clipped() !== 0) fail("strip drew outside the display");
    if (a.missingGlyphs.size || b.missingGlyphs.size) fail("strip used a glyph the atlas lacks");
    console.log("PASS: non-default geometry moves the cells");
  }

  console.log("OK");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
```

- [ ] **Step 3: Run the test, confirm it fails for the right reason**

Run: `bash tests/host/test_knob_card.sh`
Expected: `FAIL: drawKnobStrip is not exported`

- [ ] **Step 4: Add the geometry surface**

In `src/shared/param_pages/render_page_movy.mjs`, after `export const CELL_W = 32;`:

```javascript
/**
 * Where a row's cells sit.
 *
 * The grid draws four 32px cells from x=0, which is what every constant here
 * was written against. The chain editor's knob card (knob_card.mjs) draws the
 * SAME row inside a bordered box, so it needs a narrower cell at an offset
 * origin. Rather than a second copy of the row — which is how the two
 * renderers in this directory came to disagree about a pixel — the origin and
 * the width became parameters with the grid's own values as the default.
 *
 * Nothing else changes: the widgets, the fonts, the label budget and the
 * touched-cell inversion are all the same code, so a card cell and a grid cell
 * cannot drift apart.
 */
export const GRID_GEOM = Object.freeze({ x0: 0, cellW: CELL_W });
const cellLeft = (g, col) => g.x0 + col * g.cellW;
```

- [ ] **Step 5: Thread it through the four functions**

Replace `drawDivableMark`:

```javascript
function drawDivableMark(ctx, g, col, rowY) {
    drawBrackets(ctx, cellLeft(g, col) + 1, rowY, g.cellW - 2, BOX_H);
}
```

In `drawKnobWidget`, change the signature and the two x computations:

```javascript
function drawKnobWidget(ctx, g, col, rowY, meta, raw, modRaw, liveRaw, cellText) {
    const kx = cellLeft(g, col) + Math.floor((g.cellW - KW) / 2), ky = rowY;
```

and, inside the `KIND_ENUM` branch:

```javascript
        drawEnumSquare(ctx, cellLeft(g, col) + Math.floor((g.cellW - ENUM_W) / 2), ky, text);
```

In `drawLabelCell`, change the signature and the three uses:

```javascript
function drawLabelCell(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) {
    const cellX = cellLeft(g, col);
    const text = showValue ? displayValue : label;
    const tw = fontWidth4x5(text);
    const tx = centreX(cellX, g.cellW, tw);
    const ty = lblY + Math.floor((LBL_H - LBL_FONT_H) / 2);
    if (inverted) {
        ctx.fillRect(cellX, lblY, g.cellW, LBL_H, 1);
        fontPrint4x5(ctx, tx, ty, text, 0);
    } else {
        fontPrint4x5(ctx, tx, ty, text, 1);
    }
    if (modulated) {
        const wx = Math.max(cellX, tx - 6);
        drawWaveMark(ctx, wx, lblY + 1, inverted ? 0 : 1);
    }
}
```

- [ ] **Step 6: Rename the row to `drawKnobStrip` and export it**

Change the declaration:

```javascript
export function drawKnobStrip(ctx, o, row, rowY, lblY, geom) {
    const g = geom || GRID_GEOM;
    const { page, metaIndex, values, touched, modulated, viz, modValues } = o;
```

Inside it, four call sites change (everything else in the body is untouched).

**The viz loop needs its loop variable renamed.** It currently binds each group
to `g`, which is now the geometry — leave it and the group shadows the geometry
inside the loop and the rect comes out garbage. Rename the group to `grp`:

```javascript
    for (const grp of (viz || [])) {
        if (!grp || typeof grp.slotStart !== "number") continue;
        if (Math.floor(grp.slotStart / 4) !== row) continue;
        const localStart = grp.slotStart - slotBase;
        for (let s = localStart; s < localStart + grp.slotSpan && s < 4; s++) covered[s] = true;
        drawVizGroup(ctx, {
            x: cellLeft(g, localStart), y: rowY,
            w: grp.slotSpan * g.cellW, h: LBL0_Y - ROW0_Y,
        }, grp, liveValues, metaIndex);
    }
```

Then in the per-column loop:

```javascript
        const cellX = cellLeft(g, col);
```

```javascript
            drawKnobWidget(ctx, g, col, rowY, meta, raw,
                           modValues ? modValues[key] : undefined,
                           liveValues ? liveValues[key] : undefined,
                           cellText);
```

```javascript
        if (meta.divable) drawDivableMark(ctx, g, col, rowY);

        const labelWidth = Math.min(g.cellW - 2, fontWidth4x5("M".repeat(LABEL_CHARS)));
        const label = caps(shortenLabel(LBL_MEASURE, preAbbreviate(meta.label || meta.key), labelWidth));
        const display = fitDev(ctx,
            (cellText === null || cellText === undefined) ? displayValue(raw, meta) : String(cellText),
            g.cellW - 2);
        drawLabelCell(ctx, g, col, lblY, label, display, isTouched, isTouched,
                      modulated ? !!modulated(key) : false);
```

- [ ] **Step 7: Point `renderPageMovy` at the new name**

At the bottom of `renderPageMovy`:

```javascript
    drawKnobStrip(ctx, o, 0, ROW0_Y, LBL0_Y);
    drawKnobStrip(ctx, o, 1, ROW1_Y, LBL1_Y);
```

- [ ] **Step 8: Run the test**

Run: `bash tests/host/test_knob_card.sh`
Expected: three `PASS:` lines then `OK`, exit 0

- [ ] **Step 9: Run the whole movy suite for collateral damage**

Run: `bash tests/host/test_param_pages_movy.sh && bash tests/host/test_param_pages_render.sh && bash tests/host/test_param_pages_viz.sh`
Expected: exit 0 from all three

- [ ] **Step 10: Commit**

```bash
git add src/shared/param_pages/render_page_movy.mjs tests/host/test_knob_card.sh tests/fixtures/movy-geom-baseline.json
git commit -m "movy: a row can be drawn at any origin and cell width

The chain editor wants the SAME row inside a bordered card, which is
narrower than the screen. Copying the row is how the two renderers in
this directory would come to disagree about a pixel, so the origin and
the cell width became parameters instead, defaulting to the grid.

The default path being inert is the load-bearing claim and it has its
own test: a base64 snapshot of every fixture page, captured before the
refactor. Behaviour tests could not have expressed it -- the grid is
not what the card draws.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Kqae7GKuzfSXCbNTDzpg56"
```

---

### Task 2: The card renderer

**Goal:** A pure `knob_card.mjs` that draws the frame and the strip, with the black gap between border and header band asserted on the pixel buffer.

**Files:**
- Create: `src/shared/param_pages/knob_card.mjs`
- Modify: `tests/host/test_knob_card.sh` (append sections 4 and 5)

**Acceptance Criteria:**
- [ ] `knobCardRect(true)` is `{ x: 3, y: 12, w: 122, h: 38 }`; `knobCardRect(false)` is `{ x: 3, y: 24, w: 122, h: 15 }`.
- [ ] The gap column/row immediately inside the 2px border is black on every interior row/column, for **both** heights and with the outermost cell touched (the case that fills a label band to the cell edge).
- [ ] Card content is 116px wide; cells are 29px.
- [ ] `fb.clipped() === 0` and `missingGlyphs` empty for float, int, enum, opaque and header-only cards.
- [ ] A name too long for the band is truncated; the **value** never is.

**Verify:** `bash tests/host/test_knob_card.sh` → `PASS` lines through section 5, exit 0

**Steps:**

- [ ] **Step 1: Write the failing tests**

Append to `tests/host/test_knob_card.sh`, inside the existing `.then(...)` body,
just before `console.log("OK");`:

```javascript
  /* ---- 4. the card rect ---- */
  const KC = await import("./src/shared/param_pages/knob_card.mjs");
  {
    const full = KC.knobCardRect(true), short = KC.knobCardRect(false);
    const eq = (a, b, what) => { if (a !== b) fail(what + ": expected " + b + ", got " + a); };
    eq(full.x, 3, "full.x"); eq(full.y, 12, "full.y"); eq(full.w, 122, "full.w"); eq(full.h, 38, "full.h");
    eq(short.x, 3, "short.x"); eq(short.y, 24, "short.y"); eq(short.w, 122, "short.w"); eq(short.h, 15, "short.h");
    if (full.y + full.h > RM.RULE_Y) fail("full card overlaps the footer rule");
    if (full.y < RM.HEADER_H + 1) fail("full card overlaps the screen header");
    console.log("PASS: card rect");
  }

  /* ---- 5. INVARIANT: a black gap separates the border from the band ----
   *
   * The border is white and so is the inverted header band. Where they touch,
   * the border stops existing: a short card without the gap reads as one fat
   * stripe across sliced-off diagram boxes, with no left, right or top. This
   * is invisible in code review, so it is asserted on the pixels. */
  {
    const params = [
      { key: "a", name: "Alpha", type: "float", min: 0, max: 1, step: 0.01 },
      { key: "b", name: "Beta",  type: "float", min: 0, max: 1, step: 0.01 },
      { key: "c", name: "Gamma", type: "float", min: 0, max: 1, step: 0.01 },
      { key: "d", name: "Delta", type: "enum",  options: ["Hall", "Room", "Plate"] },
      { key: "e", name: "Eps",   type: "int",   min: 0, max: 127 },
      { key: "f", name: "File",  type: "filepath" },
    ];
    const mi = M.buildMetaIndex({ hierarchy: null, chainParams: params });
    const keys = ["a", "b", "c", "d", "e", "f", null, null];
    const values = { a: 0.2, b: 0.4, c: 0.6, d: 2, e: 64, f: "/x/y/kick.wav" };

    const draw = (o) => {
      const fb = H.createFramebuffer();
      KC.drawKnobCard(H.drawContext(fb), o);
      return fb;
    };
    const px = (fb, x, y) => fb.pixels[y * fb.width + x];

    const checkGap = (fb, r, what) => {
      const gx0 = r.x + KC.BORDER_W, gx1 = r.x + r.w - 1 - KC.BORDER_W;
      const gy0 = r.y + KC.BORDER_W, gy1 = r.y + r.h - 1 - KC.BORDER_W;
      for (let y = gy0; y <= gy1; y++) {
        if (px(fb, gx0, y)) fail(what + ": left gap column lit at y=" + y);
        if (px(fb, gx1, y)) fail(what + ": right gap column lit at y=" + y);
      }
      for (let x = gx0; x <= gx1; x++) {
        if (px(fb, x, gy0)) fail(what + ": top gap row lit at x=" + x);
        if (px(fb, x, gy1)) fail(what + ": bottom gap row lit at x=" + x);
      }
      /* and the border itself must actually BE there */
      for (let i = 0; i < KC.BORDER_W; i++) {
        if (!px(fb, r.x + i, r.y + Math.floor(r.h / 2))) fail(what + ": left border missing");
        if (!px(fb, r.x + r.w - 1 - i, r.y + Math.floor(r.h / 2))) fail(what + ": right border missing");
      }
    };

    /* every knob touched in turn -- cols 0 and 3 fill their label band to the
     * cell edge, which is the case that eats a border without the gap */
    for (let k = 0; k < 6; k++) {
      const fb = draw({ name: "ALPHA", value: "0.62", row: k >> 2, touched: k,
                        page: { kind: "knobs", keys }, metaIndex: mi, values });
      checkGap(fb, KC.knobCardRect(true), "full card, knob " + k);
      if (fb.clipped() !== 0) fail("card drew outside the display, knob " + k);
      if (fb.missingGlyphs.size) fail("card used a glyph the atlas lacks, knob " + k);
    }
    const sfb = draw({ name: "S1: CUTOFF", value: "72" });
    checkGap(sfb, KC.knobCardRect(false), "short card");
    if (sfb.clipped() !== 0) fail("short card drew outside the display");
    console.log("PASS: frame invariant, gap holds against every cell");
  }

  /* ---- 6. the NAME loses a collision, never the value ---- */
  {
    const fb = H.createFramebuffer();
    const ctx = H.drawContext(fb);
    KC.drawKnobCard(ctx, { name: "A Ludicrously Long Parameter Name", value: "12345" });
    const r = KC.knobCardRect(false);
    const vw = ctx.textWidth("12345");
    /* The band is white and the glyphs are knocked out of it, so the value
     * being present means UNLIT pixels in the columns it should occupy. */
    let knocked = 0;
    for (let x = r.x + r.w - 3 - vw; x < r.x + r.w - 3; x++)
      for (let y = r.y + 3; y < r.y + 3 + 9; y++) if (!fb.pixels[y * fb.width + x]) knocked++;
    if (knocked === 0) fail("value was squeezed out of the header band by a long name");
    if (fb.clipped() !== 0) fail("long name drew outside the display");
    console.log("PASS: long name truncates, value survives");
  }
```

Note the `await import` in section 4 — change the arrow function to `async` in the
existing `.then((...) => {` so `await` is legal: `.then(async ([H, C, P, M, RM, V, fs]) => {`.

- [ ] **Step 2: Run the test, confirm it fails**

Run: `bash tests/host/test_knob_card.sh`
Expected: `FAIL:` mentioning `knob_card.mjs` (module not found)

- [ ] **Step 3: Write `src/shared/param_pages/knob_card.mjs`**

```javascript
/**
 * knob_card.mjs — the chain editor's knob feedback card.
 *
 * Turning a knob in the chain editor used to answer with a name and
 * `Value: 0.62` in a centred box, on a screen that already owns renderers for
 * labelled cells, arc knobs, enum squares and viz groups. This draws a
 * bordered card over the diagram instead, carrying the parameter name and
 * value in an inverted band and, beneath it, the four cells of that knob's row
 * — the SAME `drawKnobStrip` the knob grid uses, at a narrower cell.
 *
 * Pure, like everything else in this directory: takes a draw context, draws,
 * and touches no parameter, no device global and no state. That is what lets
 * the whole card be rendered into tools/param-pages/harness.mjs and inspected
 * pixel by pixel — which is the only way to catch the failure it actually has.
 *
 * THE GAP IS LOAD-BEARING. The border is white and so is the header band. Where
 * they touch, the border stops existing: a short card with no gap reads as one
 * fat stripe laid across sliced-off diagram boxes, with no left, right or top.
 * One black row between them is the whole fix, and it is asserted on the pixel
 * buffer in tests/host/test_knob_card.sh because it is invisible in review.
 * Any future change to this frame has to keep a black row between any white
 * border and any white fill inside it.
 */

import { drawKnobStrip, ROW0_Y, LBL0_Y, LBL_H, RULE_Y, HEADER_H }
    from "./render_page_movy.mjs";

/** Inset from the screen edges. The card is a modal, not a band. */
export const CARD_X = 3;
export const CARD_W = 122;
/** 2px reads as a frame at this size where 1px reads as a hairline. */
export const BORDER_W = 2;
/** The black row that keeps the band from eating the border. See above. */
export const GAP_W = 1;
/** Cleared outside the border, so the card lifts off the diagram. */
export const GUTTER = 2;
/** The 5x7 device font plus one clear row above and below. */
export const HEADER_BAND_H = 9;

const INSET = BORDER_W + GAP_W;
const ROW_H = (LBL0_Y + LBL_H) - ROW0_Y;
const LBL_DY = LBL0_Y - ROW0_Y;
/** The band between the screen header and the footer rule. */
const BODY_TOP = HEADER_H + 1;
const BODY_BOT = RULE_Y;

/**
 * Where the card sits. Centred in the body band so both heights look
 * deliberate rather than anchored to whichever edge was convenient.
 */
export function knobCardRect(hasStrip) {
    const h = hasStrip
        ? INSET * 2 + HEADER_BAND_H + GAP_W + ROW_H
        : INSET * 2 + HEADER_BAND_H;
    const y = BODY_TOP + Math.floor(((BODY_BOT - BODY_TOP) - h) / 2);
    return { x: CARD_X, y, w: CARD_W, h };
}

/** Content width inside the border and the gap. */
export function knobCardContentW() { return CARD_W - INSET * 2; }

/**
 * Name left, value right, both knocked out of a white band.
 *
 * The NAME loses a collision. The value is the thing being read — a truncated
 * value is a wrong reading, where a truncated name is still recognisable.
 */
function drawCardHeader(ctx, x, y, w, name, value) {
    ctx.fillRect(x, y, w, HEADER_BAND_H, 1);
    const val = String(value === null || value === undefined ? "" : value);
    const vw = val ? ctx.textWidth(val) : 0;
    let nm = String(name === null || name === undefined ? "" : name);
    const nameMax = w - 6 - vw;
    while (nm.length > 1 && ctx.textWidth(nm) > nameMax) nm = nm.slice(0, -1);
    ctx.print(x + 2, y + 1, nm, 0);
    if (val) ctx.print(x + w - 2 - vw, y + 1, val, 0);
}

/**
 * @param {object} ctx  fillRect/print/textWidth, plus the native line/arc
 *                      primitives when the caller has them
 * @param {object} o    { name, value } always; for the full card also
 *                      { page, metaIndex, values, touched, row, viz, modulated }
 *                      — the same shapes renderPageMovy takes
 * @returns {object}    the rect it drew into
 */
export function drawKnobCard(ctx, o) {
    const keys = o && o.page && o.page.keys;
    const hasStrip = !!(keys && keys.some(Boolean));
    const r = knobCardRect(hasStrip);

    ctx.fillRect(r.x - GUTTER, r.y - GUTTER, r.w + GUTTER * 2, r.h + GUTTER * 2, 0);

    ctx.fillRect(r.x, r.y, r.w, BORDER_W, 1);
    ctx.fillRect(r.x, r.y + r.h - BORDER_W, r.w, BORDER_W, 1);
    ctx.fillRect(r.x, r.y, BORDER_W, r.h, 1);
    ctx.fillRect(r.x + r.w - BORDER_W, r.y, BORDER_W, r.h, 1);

    /* The interior, cleared. This is both the card being opaque and the gap
     * being cut — see the module doc. */
    ctx.fillRect(r.x + BORDER_W, r.y + BORDER_W,
                 r.w - BORDER_W * 2, r.h - BORDER_W * 2, 0);

    const cx = r.x + INSET;
    const cw = knobCardContentW();
    drawCardHeader(ctx, cx, r.y + INSET, cw, o.name, o.value);
    if (!hasStrip) return r;

    const rowY = r.y + INSET + HEADER_BAND_H + GAP_W;
    drawKnobStrip(ctx, o, (o.row | 0), rowY, rowY + LBL_DY,
                  { x0: cx, cellW: Math.floor(cw / 4) });
    return r;
}
```

- [ ] **Step 4: Run the test**

Run: `bash tests/host/test_knob_card.sh`
Expected: `PASS: card rect`, `PASS: frame invariant, gap holds against every cell`, `PASS: long name truncates, value survives`, then `OK`

- [ ] **Step 5: Commit**

```bash
git add src/shared/param_pages/knob_card.mjs tests/host/test_knob_card.sh
git commit -m "chain: a knob card, not a value box

The card carries the parameter name and value in an inverted band and,
under it, the four cells of that knob row -- the same drawKnobStrip the
knob grid uses, at a 29px cell.

The frame is 2px border, 1px black gap, then the band, and the gap is
the whole design. Border and band are both white, so where they touch
the border stops existing: a short card without it reads as one stripe
across sliced-off diagram boxes, with no left, right or top. Asserted
on the pixel buffer with the outermost cell touched, which is the case
that fills a label band to the cell edge and eats a border.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Kqae7GKuzfSXCbNTDzpg56"
```

---

### Task 3: Preview the card without a deploy

**Goal:** `node tools/param-pages/preview_knob_card.mjs <module-id> --knob N` renders the card over a real chain diagram, so the frame can be judged from a PNG.

**Files:**
- Create: `tools/param-pages/preview_knob_card.mjs`

**Acceptance Criteria:**
- [ ] Prints half-block art by default; `--png DIR --scale 4` writes a PNG.
- [ ] Background is the real `chain_diagram.mjs` output plus the real `drawHeader`/`drawFooter`, so the card is judged against what it actually covers.
- [ ] `--knob N` selects the touched knob; `--short` renders the header-only card.
- [ ] Reports `clipped=<n>`, so an overflowing card is visible without squinting.

**Verify:** `node tools/param-pages/preview_knob_card.mjs cloudseed --knob 2` → half-block art with `clipped=0`

**Steps:**

- [ ] **Step 1: Write the previewer**

```javascript
#!/usr/bin/env node
/**
 * preview_knob_card.mjs — render the chain editor's knob card over a real
 * chain diagram, as half-block art or a PNG. No hardware required: text goes
 * through the device font atlas, so this is what the OLED shows.
 *
 *   node tools/param-pages/preview_knob_card.mjs cloudseed --knob 2
 *   node tools/param-pages/preview_knob_card.mjs obxd --knob 5 --png /tmp/out --scale 4
 *   node tools/param-pages/preview_knob_card.mjs --short
 *
 * The frame exists to survive being drawn over the diagram, so the diagram is
 * not optional scenery here — a card judged against a blank screen is a card
 * judged against the one background it never has.
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createFramebuffer, drawContext } from "./harness.mjs";
import { FIXTURE } from "./cases.mjs";
import { planPages, PAGE_KNOBS } from "../../src/shared/param_pages/page_plan.mjs";
import { buildMetaIndex } from "../../src/shared/param_pages/param_meta.mjs";
import { resolveViz } from "../../src/shared/param_pages/viz.mjs";
import { drawHeader, drawFooter, RULE_Y } from "../../src/shared/param_pages/render_page_movy.mjs";
import { drawKnobCard } from "../../src/shared/param_pages/knob_card.mjs";
import { drawChainDiagram, DEFAULT_Y, BOX_H } from "../../src/shared/chain_diagram.mjs";

const argv = process.argv.slice(2);
const flag = (n, d = null) => {
    const i = argv.indexOf("--" + n);
    return i >= 0 ? (argv[i + 1] && !argv[i + 1].startsWith("--") ? argv[i + 1] : true) : d;
};
const modId = argv.find((a) => !a.startsWith("--")) || null;
const knob = parseInt(flag("knob", "2"), 10) || 0;
const short = !!flag("short");
const pngDir = flag("png");
const scale = parseInt(flag("scale", "4"), 10) || 4;

/* A representative chain: two MIDI FX, a synth, three FX, FX 1 selected. */
const comps = [
    { key: "patch", kind: "patch", label: "Patch" },
    { key: "add_midi", kind: "add", section: "midiFx", label: "+" },
    { key: "midiFx", kind: "module", section: "midiFx", label: "MIDI FX 1" },
    { key: "midi_fx2", kind: "module", section: "midiFx", label: "MIDI FX 2" },
    { key: "synth", kind: "synth", label: "Synth" },
    { key: "fx1", kind: "module", section: "fx", label: "FX 1" },
    { key: "fx2", kind: "module", section: "fx", label: "FX 2" },
    { key: "fx3", kind: "module", section: "fx", label: "FX 3" },
    { key: "add_fx", kind: "add", section: "fx", label: "+" },
    { key: "settings", kind: "settings", label: "Settings" },
];
const ABBREV = { patch: "PA", add_midi: "+", midiFx: "AR", midi_fx2: "CH",
                 synth: "OB", fx1: "RV", fx2: "DL", fx3: "CH", add_fx: "+", settings: "*" };

const fb = createFramebuffer();
const ctx = drawContext(fb);

drawHeader(ctx, "Slot 1", "OB-Xd", false);
{
    const GAP = 1, BH = Math.floor((RULE_Y - DEFAULT_Y - 3 * GAP) / 4);
    for (let s = 0; s < 4; s++) {
        const iy = DEFAULT_Y + s * (BH + GAP);
        if (s === 0) ctx.fillRect(0, iy, 4, BH, 1);
        else {
            ctx.fillRect(0, iy, 4, 1, 1); ctx.fillRect(0, iy + BH - 1, 4, 1, 1);
            ctx.fillRect(0, iy, 1, BH, 1); ctx.fillRect(3, iy, 1, BH, 1);
        }
    }
}
drawChainDiagram(ctx, comps, 5, { abbrev: (c) => ABBREV[c.key] || "--" });
{
    const ly = DEFAULT_Y + BOX_H + 3;
    const centre = (y, s) => ctx.print(Math.floor((128 - s.length * 5) / 2), y, s, 1);
    centre(ly, "FX 1"); centre(ly + 11, "CloudSeed");
}
drawFooter(ctx, [["MUTE", "DFLT"], ["SHFT", "FINE"]]);

if (short) {
    drawKnobCard(ctx, { name: "S1: CUTOFF", value: "72" });
} else {
    const fx = JSON.parse(fs.readFileSync(FIXTURE, "utf8"));
    const mod = fx.modules.find((m) => m.id === modId) || fx.modules[0];
    const { pages } = planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const page = pages.find((p) => p.kind === PAGE_KNOBS && (p.keys || []).some(Boolean));
    if (!page) { console.error("no knob page in " + mod.id); process.exit(1); }
    const metaIndex = buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const values = {};
    for (const k of page.keys) if (k) {
        const m = metaIndex.getOrGuess(k);
        const min = typeof m.min === "number" ? m.min : 0;
        const max = typeof m.max === "number" ? m.max : 1;
        values[k] = min + (max - min) * 0.62;
    }
    const { groups } = resolveViz({ keys: page.keys, metaIndex });
    const meta = metaIndex.getOrGuess(page.keys[knob] || page.keys.find(Boolean));
    drawKnobCard(ctx, {
        name: String(meta.label || meta.key).toUpperCase(),
        value: String(values[page.keys[knob]] !== undefined
            ? Number(values[page.keys[knob]]).toFixed(2) : "--"),
        page, metaIndex, values, touched: knob, row: knob >> 2, viz: groups,
    });
    console.log(mod.id + " knob " + knob + ": " + (page.keys[knob] || "(empty)"));
}

if (pngDir && pngDir !== true) {
    fs.mkdirSync(pngDir, { recursive: true });
    const file = path.join(pngDir, "knob-card" + (short ? "-short" : "-" + knob) + ".png");
    fs.writeFileSync(file, fb.toPng(scale));
    console.log(file + "  clipped=" + fb.clipped());
} else {
    console.log(fb.toBlocks());
    console.log("clipped=" + fb.clipped());
}
```

- [ ] **Step 2: Run it for every knob and both heights**

```bash
for k in 0 1 2 3 4 5 6 7; do
  node tools/param-pages/preview_knob_card.mjs obxd --knob $k | tail -2
done
node tools/param-pages/preview_knob_card.mjs --short | tail -1
```

Expected: `clipped=0` from all nine runs.

- [ ] **Step 3: Commit**

```bash
git add tools/param-pages/preview_knob_card.mjs
git commit -m "preview: the knob card, over the diagram it has to survive

The frame exists to hold up against the chain diagram, so a preview
against a blank screen judges it against the one background it never
has. Draws the real diagram, the real header and footer, then the card.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Kqae7GKuzfSXCbNTDzpg56"
```

---

### Task 4: Wire the card into the chain editor

**Goal:** Touching a knob in `VIEWS.CHAIN_EDIT` raises the card; releasing drops it; no IPC happens on the draw path.

**Files:**
- Modify: `src/shadow/shadow_ui.js` — imports (~line 51), state (~1094), `setView` (818), `showKnobOverlay` (10280), `refreshPendingKnobOverlay` (14284), `drawChainEdit` (14336, 14351, 14499), note handlers (~17590, ~17636)

**Acceptance Criteria:**
- [ ] Touching knob N with a component selected shows the card with that knob's row, its cell inverted.
- [ ] Releasing hides it. A turn with no touch shows it and it decays after ~700 ms.
- [ ] With no component selected, the short (header-only) card shows the global mapping's name and value.
- [ ] The screen reader still announces, and only on content change.
- [ ] Leaving `CHAIN_EDIT` closes the card.
- [ ] Every other view keeps the old centred box (`HIERARCHY_EDITOR` especially).

**Verify:** `bash tests/host/test_chain_edit_read_budget.sh && bash tests/host/test_param_pages_wiring.sh` → exit 0; then Task 5's test

**Steps:**

- [ ] **Step 1: Import the card**

After the existing `render_page_movy` import block (~line 51):

```javascript
import { drawKnobCard } from '/data/UserData/schwung/shared/param_pages/knob_card.mjs';
import { buildMetaIndex } from '/data/UserData/schwung/shared/param_pages/param_meta.mjs';
import { resolveViz } from '/data/UserData/schwung/shared/param_pages/viz.mjs';
```

- [ ] **Step 2: Add the state and its accessors**

After the knob-context cache block (~line 1100):

```javascript
/*
 * The chain editor's knob card (shared/param_pages/knob_card.mjs).
 *
 * Raised by TOUCH, not by turn: resting a finger tells you what the knob does
 * before you move it, and it is the same signal the knob grid already follows.
 * A turn with no touch raises it too and decays, because a cap sensor that
 * misses must not be able to strand the feature.
 */
const KNOB_CARD_DECAY_MS = 700;
const knobTouched = new Array(NUM_KNOBS).fill(false);
let knobCardKnob = -1;       /* physical knob the card follows, or -1 */
let knobCardExpiry = 0;      /* ms deadline; 0 means held, so no deadline */
let knobCardKeys = null;     /* param key per physical knob, or null */
let knobCardMeta = null;     /* metaIndex for the focused component */
let knobCardValues = null;   /* raw values, keyed by param key */
let knobCardViz = null;
let knobCardModKey = null;   /* the ONE key known to be modulated (see below) */
let knobCardName = "";
let knobCardValue = "";

function knobCardClose() {
    if (knobCardKnob < 0) return;
    knobCardKnob = -1;
    knobCardExpiry = 0;
    knobCardKeys = null;
    knobCardMeta = null;
    knobCardValues = null;
    knobCardViz = null;
    knobCardModKey = null;
    needsRedraw = true;
}

function knobCardActive() {
    if (knobCardKnob < 0) return false;
    if (knobCardExpiry && Date.now() > knobCardExpiry) { knobCardClose(); return false; }
    return true;
}

/*
 * Everything the card needs, resolved ONCE on touch-down.
 *
 * The reads happen here, on an input event, and never on the draw path: an IPC
 * round trip is ~2.8ms against a 1.68ms whole-page render, so four of them is a
 * quarter of a frame budget. Four is also the whole bill — one per key in the
 * touched knob ROW. The turned knob is updated by local arithmetic afterwards
 * (knobCardShow), so the card costs nothing per frame while it is up.
 *
 * The neighbours therefore do not animate under modulation. That is the trade:
 * animating them means four reads EVERY frame to move a pointer nobody is
 * looking at.
 */
function knobCardOpen(knobIndex) {
    knobCardKnob = knobIndex;
    knobCardKeys = null;
    knobCardMeta = null;
    knobCardValues = null;
    knobCardViz = null;
    knobCardModKey = null;

    const comps = slotChainComponents(selectedSlot);
    const comp = selectedChainComponent >= 0 ? comps[selectedChainComponent] : null;
    if (!comp || !isChainModuleKey(comp.key)) return;  /* short card */

    const hierarchy = getComponentHierarchy(selectedSlot, comp.key);
    const chainParams = getComponentChainParams(selectedSlot, comp.key);
    if (!hierarchy || !chainParams || !chainParams.length) return;

    const keys = new Array(NUM_KNOBS).fill(null);
    for (let i = 0; i < NUM_KNOBS; i++) {
        const kc = getKnobContext(i);
        keys[i] = (kc && kc.key) ? kc.key : null;
    }
    if (!keys.some(Boolean)) return;

    knobCardMeta = buildMetaIndex({ hierarchy, chainParams });
    knobCardKeys = keys;
    knobCardViz = resolveViz({ keys, metaIndex: knobCardMeta }).groups;

    const prefix = getComponentParamPrefix(comp.key);
    const base = (knobIndex >> 2) * 4;
    const values = {};
    for (let c = 0; c < 4; c++) {
        const k = keys[base + c];
        if (!k) continue;
        const raw = getSlotParam(selectedSlot, `${prefix}:${k}`);
        /* An unserved key reads back as "", not an error — do not let that
         * masquerade as a value of zero. */
        values[k] = (raw === null || raw === undefined) ? "" : raw;
    }
    knobCardValues = values;
}

/*
 * The chain editor answers a knob with the CARD; every other view keeps the
 * centred name/value box. Both announce, so the screen reader does not care
 * which is up.
 */
function showKnobFeedback(knobIndex, name, value, raw) {
    if (view !== VIEWS.CHAIN_EDIT) { showOverlay(name, value); return; }

    if (knobCardKnob !== knobIndex) knobCardOpen(knobIndex);
    /* Held keeps it up with no deadline; a turn with no touch gets a decay. */
    knobCardExpiry = knobTouched[knobIndex] ? 0 : Date.now() + KNOB_CARD_DECAY_MS;

    if (knobCardValues && knobCardKeys && knobCardKeys[knobIndex] &&
        raw !== undefined && raw !== null) {
        knobCardValues[knobCardKeys[knobIndex]] = raw;
    }
    /* Only the TOUCHED key's modulation is known, because that read is one
     * showKnobOverlay already pays for. Marking the neighbours would cost up
     * to three more reads each. */
    knobCardModKey = (name && name.endsWith("~") && knobCardKeys)
        ? knobCardKeys[knobIndex] : null;

    const changed = (knobCardName !== name || knobCardValue !== value);
    knobCardName = name;
    knobCardValue = value;
    if (changed) announceParameter(name, value);
    needsRedraw = true;
}
```

- [ ] **Step 3: Route the three `showOverlay` calls in `showKnobOverlay`**

In `showKnobOverlay` (~10286, ~10289, ~10326), replace:

```javascript
            showOverlay(ctx.title, "No Module Selected");
```
with
```javascript
            showKnobFeedback(knobIndex, ctx.title, "No Module Selected");
```

```javascript
            showOverlay(`Knob ${knobIndex + 1}`, "not mapped");
```
with
```javascript
            showKnobFeedback(knobIndex, `Knob ${knobIndex + 1}`, "not mapped");
```

```javascript
            showOverlay(title, displayVal);
```
with
```javascript
            showKnobFeedback(knobIndex, title, displayVal,
                             value !== undefined ? value : undefined);
```

Then do the same for every remaining `showOverlay` call that has a knob index in
scope — in `adjustKnobAndShow` and its tick-time partner, at approximately lines
10348, 10361, 10441, 10443, 10446, 10448, 10450, 10505, 10540, 10542, 10567,
10580, 10585, 10590 and 10635. Find them with:

```bash
command grep -n "showOverlay(" src/shadow/shadow_ui.js
```

**Two knob-index variables are in play and they are not interchangeable.** In
`adjustKnobAndShow` and the touch path it is `knobIndex`; in the throttled
tick-time path (~10441–10450) it is `pendingHierKnobIndex`. Use whichever is in
scope at each site. So the two forms are:

```javascript
            showKnobFeedback(knobIndex, ctx.title, formatMetaOptionValue(ctx.meta, newVal), newVal);
```

```javascript
                            showKnobFeedback(pendingHierKnobIndex, ctx.title,
                                             formatMetaOptionValue(ctx.meta, cached), cached);
```

Pass the raw value as the fourth argument wherever one is already in scope
(`newVal`, `cached`, `currentVal`, `value`); omit it where the call is showing a
message rather than a value (`"No Module Selected"`, `"not mapped"`,
`"Triggered"`).

**Leave the two `showOverlay("Zoom", label)` calls alone** (~10483, ~17499,
~17615). Zoom is a multi-marker role that only exists in `HIERARCHY_EDITOR`, so
routing it through the card would be dead code that implies the card handles a
view it does not.

- [ ] **Step 4: Route the global-mapping overlay**

In `refreshPendingKnobOverlay` (~14317, ~14320), replace:

```javascript
        showOverlay(displayName, mapping.value);
    } else {
        showOverlay(`Knob ${pendingKnobIndex + 1}`, "not mapped");
    }
```

with (note `pendingKnobIndex` is cleared at the end of the function, so capture it):

```javascript
        showKnobFeedback(pendingKnobIndex, displayName, mapping.value);
    } else {
        showKnobFeedback(pendingKnobIndex, `Knob ${pendingKnobIndex + 1}`, "not mapped");
    }
```

- [ ] **Step 5: Track touch, and close on release**

In the note-on branch that handles knob touch (~17590), immediately after
`const knobIndex = d1 - MoveKnob1Touch;` is computed, add:

```javascript
            knobTouched[knobIndex] = true;
```

In the note-off branch (~17638), after `const knobIndex = d1 - MoveKnob1Touch;`:

```javascript
            knobTouched[knobIndex] = false;
            /* Let go and the diagram is back. A card raised by a TURN has a
             * decay deadline instead and is not this gesture's to close. */
            if (knobCardKnob === knobIndex && !knobCardExpiry) knobCardClose();
```

- [ ] **Step 6: Close the card on any view change**

In `setView` (818):

```javascript
function setView(newView, customLabel) {
    if (view === newView) return;  /* No change */
    /* The card belongs to the chain editor and to one knob gesture; it must
     * not survive a screen change. */
    knobCardClose();
    view = newView;
    needsRedraw = true;
}
```

- [ ] **Step 7: Give `drawChainEdit` the native primitives and draw the card**

Change the `movy` context (~14351) so the arc knob takes the C path rather than
the JS fallback — same set `shadow_ui_param_pages.mjs` supplies:

```javascript
    const movy = {
        fillRect: fill_rect, print, textWidth: text_width, setPixel: set_pixel,
        line: typeof draw_line === "function" ? draw_line : undefined,
        fillCircle: typeof fill_circle === "function" ? fill_circle : undefined,
        drawCircle: typeof draw_circle === "function" ? draw_circle : undefined,
        drawArc: typeof draw_arc === "function" ? draw_arc : undefined,
    };
```

At the very end of `drawChainEdit`, after the `drawMovyFooter(...)` call (~14501):

```javascript
    /*
     * The card last, over everything — it is a modal. Every value it draws was
     * read on touch-down, so this costs no IPC. See knobCardOpen.
     */
    if (knobCardActive()) {
        drawKnobCard(movy, {
            name: knobCardName,
            value: knobCardValue,
            row: knobCardKnob >> 2,
            touched: knobCardKnob,
            page: knobCardKeys ? { kind: "knobs", keys: knobCardKeys } : null,
            metaIndex: knobCardMeta,
            values: knobCardValues,
            viz: knobCardViz,
            modulated: knobCardModKey ? ((k) => k === knobCardModKey) : null,
        });
    }
```

- [ ] **Step 8: Confirm `NUM_KNOBS` and `announceParameter` are in scope**

Run: `command grep -n "NUM_KNOBS\s*=\|announceParameter" src/shadow/shadow_ui.js | head`
Expected: a definition for `NUM_KNOBS` and an import or definition of
`announceParameter`. If `announceParameter` is not imported, add it to the
existing `screen_reader.mjs` import.

- [ ] **Step 9: Run the suites that touch this file**

Run: `bash tests/host/test_chain_edit_read_budget.sh && bash tests/host/test_param_pages_wiring.sh && bash tests/host/test_chain_gestures.sh`
Expected: exit 0 from all three

- [ ] **Step 10: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "chain: the knob card replaces the value box in the editor

Touch raises it, release drops it; a turn with no touch raises it and
decays, so a cap sensor that misses cannot strand the feature. With no
component selected the global mappings serve a name and a value but no
type, so there is nothing to draw a control from -- that gets the short
header-only card rather than a second kind of overlay.

Every value is read once on touch-down and never on the draw path. A
read is ~2.8ms against a 1.68ms whole-page render, so animating the
three neighbours would cost a quarter of the frame budget to move
pointers nobody is looking at.

Other views keep the centred box; the hierarchy editor especially,
which shares this knob-handling code.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Kqae7GKuzfSXCbNTDzpg56"
```

---

### Task 5: Prove the card costs no IPC per frame

**Goal:** A test that fails if drawing the card ever reads a parameter.

**Files:**
- Create: `tests/host/test_chain_knob_card_reads.sh`

**Acceptance Criteria:**
- [ ] Rendering the card 60 times issues **zero** `getSlotParam`-shaped calls.
- [ ] The test derives the card's draw surface from `knob_card.mjs` itself rather than from a copy, so a new read added inside it is caught.

**Verify:** `bash tests/host/test_chain_knob_card_reads.sh` → `PASS`, exit 0

**Steps:**

- [ ] **Step 1: Write the test**

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The knob card must cost NOTHING per frame.
#
# The chain editor sits at 60Hz and an IPC round trip is ~2.8ms against a
# 1.68ms whole-page render, so a single read added to the draw path costs more
# than redrawing the entire screen. Every value the card shows is read once on
# touch-down (knobCardOpen in shadow_ui.js); this pins the other half of that
# claim -- that the RENDERER reads nothing at all.
#
# Two assertions, because they fail differently. The static one catches a read
# helper being imported into knob_card.mjs. The dynamic one catches a read
# reaching the draw path through the ctx or the data.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the knob card read-budget test" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./tools/param-pages/harness.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/knob_card.mjs"),
  import("node:fs"),
]).then(([H, M, KC, fs]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };

  /* ---- 1. static: the module must not reach for a param at all ---- */
  const src = fs.readFileSync("src/shared/param_pages/knob_card.mjs", "utf8");
  for (const banned of ["getSlotParam", "shadow_get_param", "get_param",
                        "setSlotParam", "shadow_set_param"]) {
    if (src.indexOf(banned) >= 0)
      fail("knob_card.mjs references " + banned + " -- the card renderer is pure " +
           "and reads nothing; the values come from knobCardOpen");
  }
  console.log("PASS: knob_card.mjs holds no param I/O");

  /* ---- 2. dynamic: draw it 60 times with a ctx that counts everything ---- */
  const params = [
    { key: "a", name: "Alpha", type: "float", min: 0, max: 1, step: 0.01 },
    { key: "b", name: "Beta",  type: "float", min: 0, max: 1, step: 0.01 },
    { key: "c", name: "Gamma", type: "float", min: 0, max: 1, step: 0.01 },
    { key: "d", name: "Delta", type: "enum",  options: ["Hall", "Room", "Plate"] },
  ];
  const mi = M.buildMetaIndex({ hierarchy: null, chainParams: params });
  const keys = ["a", "b", "c", "d", null, null, null, null];
  const values = { a: 0.2, b: 0.4, c: 0.6, d: 1 };

  let reads = 0;
  const fb = H.createFramebuffer();
  const base = H.drawContext(fb);
  const ctx = {};
  for (const k of Object.keys(base)) ctx[k] = base[k];
  /* Anything the card might reach for that would be a round trip on device. */
  for (const name of ["getSlotParam", "getParam", "read"]) {
    ctx[name] = () => { reads++; return ""; };
  }
  /* A values object that screams if the renderer asks for a key it was not
   * handed -- that is what a lazy read on the draw path would look like. */
  const guarded = new Proxy(values, {
    get(t, p) {
      if (typeof p === "string" && !(p in t) && p !== "then") reads++;
      return t[p];
    },
  });

  for (let i = 0; i < 60; i++) {
    KC.drawKnobCard(ctx, {
      name: "ALPHA", value: "0.62", row: 0, touched: i % 4,
      page: { kind: "knobs", keys }, metaIndex: mi, values: guarded,
    });
  }
  if (reads !== 0) fail("the card issued " + reads + " reads across 60 frames -- " +
                        "at ~2.8ms each that is more than redrawing the screen");
  if (fb.clipped() !== 0) fail("card drew outside the display");
  console.log("PASS: 60 frames, zero reads");
  console.log("OK");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
```

- [ ] **Step 2: Run it**

Run: `bash tests/host/test_chain_knob_card_reads.sh`
Expected: `PASS: knob_card.mjs holds no param I/O`, `PASS: 60 frames, zero reads`, `OK`

- [ ] **Step 3: Verify it can actually fail**

Temporarily add `const _ = values["nonexistent_key"];` inside `drawKnobCard`, re-run, confirm `FAIL`, then remove it.

Run: `bash tests/host/test_chain_knob_card_reads.sh`
Expected: `FAIL: the card issued 60 reads across 60 frames ...`

- [ ] **Step 4: Commit**

```bash
git add tests/host/test_chain_knob_card_reads.sh
git commit -m "test: the knob card costs nothing per frame

An IPC read is ~2.8ms against a 1.68ms whole-page render, so one read
on the draw path costs more than redrawing the screen. Values are read
once on touch-down; this pins the other half -- that the renderer reads
nothing. Static (no param helper in the module) and dynamic (60 frames
through a ctx and a values proxy that count every reach), because the
two fail differently.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Kqae7GKuzfSXCbNTDzpg56"
```

---

### Task 6: Full suite, deploy, hardware check, docs

**Goal:** The change is verified on device and the documentation matches it.

**Files:**
- Modify: `CLAUDE.md` (Signal Chain Module section)
- Modify: `../schwung-catalog-site/manual.html`
- Modify: `src/shared/help_content.json`

**Acceptance Criteria:**
- [ ] `tests/host/*.sh` failure count is **32**, unchanged from the branch baseline.
- [ ] Built and deployed; `md5sum` of the deployed `shared/param_pages/knob_card.mjs` matches the local build.
- [ ] On hardware: all eight knobs on a synth, an audio FX and a MIDI FX raise the card with the right row; release drops it.
- [ ] On hardware: with nothing selected, the short card shows the global mapping.
- [ ] On hardware: the screen reader still announces the parameter on change.
- [ ] `CLAUDE.md`, `manual.html` and `help_content.json` describe the card.

**Verify:** `for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done | wc -l` → `32`

**Steps:**

- [ ] **Step 1: Establish the baseline failure count on a clean tree**

```bash
git stash list >/dev/null
for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done | tee /tmp/after.txt | wc -l
```

Expected: `32`. If it is higher, diff `/tmp/after.txt` against the pre-existing
`rg`-missing set — every failure must print either `rg is required to run this
test` or `rg: command not found`.

- [ ] **Step 2: Build and deploy**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Expected: build succeeds, install reports the service restarted.

- [ ] **Step 3: Verify the deploy actually landed**

Timestamps lie — incremental builds preserve mtimes.

```bash
md5sum src/shared/param_pages/knob_card.mjs
ssh ableton@move.local "md5sum /data/UserData/schwung/shared/param_pages/knob_card.mjs"
```

Expected: identical hashes.

- [ ] **Step 4: Hardware check**

Open the chain editor (Shift+Vol+Track 1). For a slot with a synth, an audio FX
and a MIDI FX in turn:

1. Select the component, touch each of knobs 1–8. The card appears with that
   knob's row; the touched cell is inverted and shows its value; the header
   shows the full name and value.
2. Release. The diagram returns.
3. Turn a knob without resting a finger first. The card appears and disappears
   after roughly three quarters of a second.
4. Back out so nothing is selected, turn a knob. The short header-only card
   appears with the slot mapping name.
5. With the screen reader on, confirm the parameter is still announced, and that
   holding a knob still does not repeat the announcement every frame.

- [ ] **Step 5: Update `CLAUDE.md`**

In the Signal Chain Module section, after the "Chain shape edits are a
PERMUTATION" subsection, add:

```markdown
### Chain editor knob feedback is a CARD

Turning or touching a knob in the chain editor raises a bordered card
(`src/shared/param_pages/knob_card.mjs`) showing the four cells of that knob's
row, drawn with the knob grid's own widgets via `drawKnobStrip` at a 29px cell.
Touch raises it, release drops it; a turn with no touch decays after ~700ms.
With no component selected the global slot mappings have no type metadata, so
that case gets a header-only card.

**The 1px black gap between the border and the header band is load-bearing.**
Both are white, so where they touch the border stops existing and the card
reads as a stripe across the diagram. `tests/host/test_knob_card.sh` asserts
the gap on the pixel buffer with the outermost cell touched, which is the case
that fills a label band to the cell edge.

**Every value is read on touch-down, never on the draw path**
(`knobCardOpen` in `shadow_ui.js`) — a read is ~2.8ms against a 1.68ms whole
page render. `tests/host/test_chain_knob_card_reads.sh` pins it. The
consequence is that a modulated NEIGHBOUR does not animate while a knob is
held; only the touched knob carries a modulation mark, because that read is one
`showKnobOverlay` already pays for.

`render_page_movy.mjs`'s cell geometry is a parameter (`GRID_GEOM`,
`drawKnobStrip`) so the card and the grid share one row renderer. The default
path is pinned byte-identical against `tests/fixtures/movy-geom-baseline.json`.
```

- [ ] **Step 6: Update the user manual and help content**

In `../schwung-catalog-site/manual.html`, in the Signal Chain section, describe
the card in user terms: touching a knob in the chain editor shows that knob and
its three neighbours with their names and values; let go to see the chain again.

In `src/shared/help_content.json`, find the chain-editor entry and add a line to
the same effect.

- [ ] **Step 7: Commit**

```bash
git add CLAUDE.md src/shared/help_content.json
git commit -m "docs: the chain editor answers a knob with a card

Records the two things that are invisible in the code: the black gap is
what keeps the border from being eaten by the header band, and the
values are read on touch-down rather than per frame, which is why a
modulated neighbour does not animate.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Kqae7GKuzfSXCbNTDzpg56"
git -C ../schwung-catalog-site add manual.html
git -C ../schwung-catalog-site commit -m "manual: chain editor knob card"
```

---

## Notes for whoever executes this

- **Re-run the WHOLE test file after any fix, not just the new case.** On this
  branch a mutation went from killed to survived because a new guard masked an
  older check.
- **An unserved param key reads back as `""`, not an error.** That has caused
  two separate silent bugs here already. Suspect it whenever something "does
  nothing".
- Task 1's baseline snapshot must be captured **before** the refactor. If you
  reach Task 1 Step 4 without `tests/fixtures/movy-geom-baseline.json`
  existing, stop and go back — a snapshot taken afterwards proves nothing.
