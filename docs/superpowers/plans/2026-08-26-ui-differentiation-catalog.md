# UI Differentiation Catalog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a catalog of 130 UI design options (13 sets x 10) rendered to PNG, plus a local pairwise A/B comparator that produces a ranked preference dataset.

**Architecture:** Options are authored as data in `src/shared/param_pages/styles/`, each with a `kind` discriminator (`draw` / `font` / `motion`). A render tool composites them into isolated swatches and real module pages using the existing headless harness. A dependency-free node server serves a pairwise comparator and appends judgements to JSON as they land. A Bradley-Terry fit turns those judgements into per-set rankings.

**Tech Stack:** Node ESM (`.mjs`), no dependencies. Existing `tools/param-pages/harness.mjs` for the 1-bit framebuffer and PNG encoding. Bash + node for the host test, matching `tests/host/*.sh`.

**User decisions (already made):**
- Deliverable is a rendered option catalog to choose from; implementation of picks is a follow-up issue.
- Control set is everything except momentary/push buttons, plus font (13 sets).
- Options spread minimal -> radical on a shared axis.
- Widget-internal only; grid geometry does not move.
- Animation set covers value-change motion (not page transitions).
- Review is pairwise A/B, ~16 pairs/set, Bradley-Terry ranking.
- A/B page is a local node server writing JSON live; portability explicitly not needed.
- **Rounded corners are kept.** "I do like rounded corners on stuff tho that is pretty elektronny" — resolved as a convergent idiom (spec §1.3): the 1px notch is what the 1-bit constraint forces, not a house signature, so it is applied ACROSS options rather than isolated as one to accept or reject.

**Spec:** `docs/superpowers/specs/2026-08-26-ui-differentiation-catalog-design.md`

---

## A note on how the option tasks are specified

Tasks 5–10 each ask for ten option implementations, and this plan gives an
**axis table naming what each option is, plus worked examples of the exact
shape**, rather than 130 finished pixel-level bodies.

That is deliberate and it is where this plan departs from "show the complete
code in every step". The option bodies are the creative work being planned: a
plan cannot pre-specify what a "stipple arc" looks like pixel by pixel without
simply being the implementation. What the plan CAN pin, and does, is everything
around it — the exact signature each option must match, the box it must stay
inside, the assertion that catches it when it does not, the shared dither
predicates it must use rather than reinventing, and a worked example per set
showing all of that in one piece.

Where a step implements infrastructure rather than a design — the registry, the
dither module, the render tool, the server, the ranking fit — the code is
complete and literal.

---

## File Structure

| Path | Responsibility |
|---|---|
| `src/shared/param_pages/render_page_movy.mjs` | **Modified**: add `export` to 7 private fns. Nothing else. |
| `src/shared/param_pages/styles/index.mjs` | Set registry; single import point for tools and tests |
| `src/shared/param_pages/styles/dither.mjs` | Shared screen-space dither predicates used by many options |
| `src/shared/param_pages/styles/knob.mjs` | Set 1 — 10 arc-knob options |
| `src/shared/param_pages/styles/fader.mjs` | Set 2 — 10 level options |
| `src/shared/param_pages/styles/fills.mjs` | Set 3 — 10 footer/rule options |
| `src/shared/param_pages/styles/enum_square.mjs` | Set 4 |
| `src/shared/param_pages/styles/label_cell.mjs` | Set 5 |
| `src/shared/param_pages/styles/opaque_box.mjs` | Set 6 |
| `src/shared/param_pages/styles/viz_envelope.mjs` | Set 7 |
| `src/shared/param_pages/styles/viz_filter.mjs` | Set 8 |
| `src/shared/param_pages/styles/viz_lfo.mjs` | Set 9 |
| `src/shared/param_pages/styles/viz_sample.mjs` | Set 10 |
| `src/shared/param_pages/styles/viz_switch.mjs` | Set 11 |
| `src/shared/param_pages/styles/font/index.mjs` | Set 12 — 10 complete 4x5 glyph tables |
| `src/shared/param_pages/styles/anim.mjs` | Set 13 — 10 motion specs |
| `tools/param-pages/catalog.mjs` | Renders swatches, in-context pages, contact sheets |
| `tools/param-pages/ab_server.mjs` | Local comparator server, appends judgements |
| `tools/param-pages/rank.mjs` | Bradley-Terry fit over the judgement log |
| `tests/host/test_style_catalog.sh` | Structural invariants over all sets |

Sets are one file each because they are independently authored and independently reviewed. `dither.mjs` exists because the vimana-derived options in several sets share the same density ladder, and duplicating the predicates would let them drift apart.

---

## Task 1: Export the widget draw functions

**Goal:** Make the seven module-private widget functions importable so the catalog can render current-state baselines and substitute options into real pages.

**Files:**
- Modify: `src/shared/param_pages/render_page_movy.mjs` (lines 567, 778, 821, 1056, 1084, 1153, 1267)

**Acceptance Criteria:**
- [ ] `drawArcKnob`, `drawEnumSquare`, `drawLabelCell`, `drawOpaqueBox`, `drawKnobWidget`, `drawModDot`, `centeredText` are all exported
- [ ] No signature, body, or call-site changes
- [ ] Every existing param-pages test still passes

**Verify:** `bash tests/host/test_knob_card.sh && bash tests/host/test_param_pages_movy.sh && bash tests/host/test_enum_picker_chrome.sh && bash tests/host/test_chain_edit_read_budget.sh` -> all PASS

**Steps:**

- [ ] **Step 1: Add the export keyword to each of the seven declarations**

Change each of these lines in `src/shared/param_pages/render_page_movy.mjs`, adding only the leading `export`:

```js
export function centeredText(ctx, x0, span, y, text, color) {
export function drawModDot(ctx, kx, ky, normVal) {
export function drawArcKnob(ctx, kx, ky, normVal) {
export function drawEnumSquare(ctx, kx, ky, text) {
export function drawOpaqueBox(ctx, kx, ky, value, override) {
export function drawKnobWidget(ctx, g, col, rowY, meta, raw, modRaw, liveRaw, cellText, btnPhase) {
export function drawLabelCell(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) {
```

Do not touch `drawButton` (line 1006) — trigger buttons are out of scope.

- [ ] **Step 2: Confirm nothing else changed**

Run: `git diff --stat src/shared/param_pages/render_page_movy.mjs`
Expected: `1 file changed, 7 insertions(+), 7 deletions(-)`

Run: `git diff -U0 src/shared/param_pages/render_page_movy.mjs | command grep -c '^+export function'`
Expected: `7`

- [ ] **Step 3: Run the pinned param-pages tests**

Run: `bash tests/host/test_knob_card.sh`
Expected: all `PASS:` lines, exit 0. This one hashes a byte-identical baseline, so it is the strongest signal that nothing moved.

Run: `bash tests/host/test_param_pages_movy.sh && bash tests/host/test_enum_picker_chrome.sh && bash tests/host/test_chain_edit_read_budget.sh`
Expected: all PASS.

- [ ] **Step 4: Commit**

```bash
git add src/shared/param_pages/render_page_movy.mjs
git commit -m "movy: export the widget draw fns for the SCH-50 catalog

Additive only -- the export keyword and nothing else. The catalog needs
these to render the current widget as the baseline each option is compared
against, and to substitute an option into a real page. The alternative was
rendering a page and overdrawing the widget rects, which would drift
silently from what the page actually draws."
```

---

## Task 2: Option schema and the set registry

**Goal:** Define the option record shape, the three payload kinds, and a registry that tools and tests read, before any options exist.

**Files:**
- Create: `src/shared/param_pages/styles/index.mjs`
- Create: `tests/host/test_style_catalog.sh`

**Acceptance Criteria:**
- [ ] `SETS` exports an array of set descriptors, each `{ id, title, kind, unit, options }`
- [ ] `validateSet(set)` returns an array of human-readable problems, empty when valid
- [ ] Validation catches: wrong option count, duplicate ids, positions not covering 1..10, payload not matching `kind`
- [ ] Test passes with zero sets registered (it validates whatever exists)

**Verify:** `bash tests/host/test_style_catalog.sh` -> PASS

**Steps:**

- [ ] **Step 1: Write the registry with validation**

Create `src/shared/param_pages/styles/index.mjs`:

```js
/**
 * styles/index.mjs — the SCH-50 option catalog registry.
 *
 * Each SET is ten alternative treatments of one control, ordered on a shared
 * minimal -> radical axis so that "option 7" means a comparable distance in
 * every set. The axis is a HYPOTHESIS, not a conclusion: the pairwise
 * preference data collected by tools/param-pages/ab_server.mjs is allowed to
 * contradict it, and that disagreement is a finding rather than an error.
 *
 * Nothing here ships to the device. Production draw code imports nothing from
 * this directory.
 */

export const OPTIONS_PER_SET = 10;

/** Payload shapes. A set declares one; every option in it must match. */
export const KIND_DRAW = "draw";     /* draw(ctx, ...) — same signature as the fn it could replace */
export const KIND_FONT = "font";     /* { glyphs } — [advance, yOff, w, h, ...rowBits], CHARS order */
export const KIND_MOTION = "motion"; /* { behaviour, frames(from, to, n) } */

export const KINDS = [KIND_DRAW, KIND_FONT, KIND_MOTION];

/* Sets are registered here as they are authored. Order is the catalog order. */
export const SETS = [];

export function registerSet(set) {
    SETS.push(set);
    return set;
}

export function setById(id) {
    return SETS.find((s) => s.id === id) || null;
}

/**
 * Structural problems with a set, as readable strings. Empty array = valid.
 * Deliberately returns ALL problems rather than throwing on the first: when a
 * set is being authored, seeing every complaint at once is faster.
 */
export function validateSet(set) {
    const bad = [];
    const where = set && set.id ? set.id : "<no id>";

    if (!set || typeof set !== "object") return ["set is not an object"];
    if (!set.id) bad.push("set has no id");
    if (!set.title) bad.push(where + ": set has no title");
    if (!KINDS.includes(set.kind)) bad.push(where + ": kind must be one of " + KINDS.join("/"));
    if (!Array.isArray(set.options)) return bad.concat(where + ": options is not an array");

    if (set.options.length !== OPTIONS_PER_SET)
        bad.push(where + ": has " + set.options.length + " options, want " + OPTIONS_PER_SET);

    const seenId = new Set();
    const seenPos = new Set();
    for (const o of set.options) {
        const oid = o && o.id ? o.id : "<no id>";
        if (!o || typeof o !== "object") { bad.push(where + ": an option is not an object"); continue; }
        if (!o.id) bad.push(where + ": an option has no id");
        else if (seenId.has(o.id)) bad.push(where + ": duplicate option id " + o.id);
        else seenId.add(o.id);

        if (!o.name) bad.push(where + "/" + oid + ": no name");
        if (!o.note) bad.push(where + "/" + oid + ": no note (the catalog rationale line)");

        if (!Number.isInteger(o.position) || o.position < 1 || o.position > OPTIONS_PER_SET)
            bad.push(where + "/" + oid + ": position must be an integer 1.." + OPTIONS_PER_SET);
        else if (seenPos.has(o.position)) bad.push(where + ": duplicate position " + o.position);
        else seenPos.add(o.position);

        if (set.kind === KIND_DRAW && typeof o.draw !== "function")
            bad.push(where + "/" + oid + ": kind draw needs a draw function");
        if (set.kind === KIND_FONT && !Array.isArray(o.glyphs))
            bad.push(where + "/" + oid + ": kind font needs a glyphs array");
        if (set.kind === KIND_MOTION && (typeof o.frames !== "function" || !o.behaviour))
            bad.push(where + "/" + oid + ": kind motion needs frames() and behaviour");
    }

    if (set.options.length === OPTIONS_PER_SET && seenPos.size === OPTIONS_PER_SET) {
        for (let p = 1; p <= OPTIONS_PER_SET; p++)
            if (!seenPos.has(p)) bad.push(where + ": position " + p + " is missing");
    }
    return bad;
}

export function validateAll() {
    return SETS.flatMap(validateSet);
}
```

- [ ] **Step 2: Write the host test**

Create `tests/host/test_style_catalog.sh`. Note: the node script is a single-quoted bash string, so it must contain **no apostrophes anywhere**, including in comments and messages.

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Structural invariants over the SCH-50 style option catalog.
#
# The catalog is data, not behaviour, so what can go wrong is structural: a
# set that lost an option, two options claiming the same axis position, a
# font option that is a copy of the shipping font. None of that is visible in
# a rendered contact sheet, which is exactly why it is asserted here.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the style catalog tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/styles/index.mjs"),
]).then(async ([S]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };

  const problems = S.validateAll();
  if (problems.length) fail("structural problems:\n  " + problems.join("\n  "));
  console.log("PASS: " + S.SETS.length + " set(s) structurally valid");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
```

- [ ] **Step 3: Run it — passes vacuously with no sets yet**

Run: `chmod +x tests/host/test_style_catalog.sh && bash tests/host/test_style_catalog.sh`
Expected: `PASS: 0 set(s) structurally valid`

- [ ] **Step 4: Prove the validator actually catches things**

Run:
```bash
node -e 'import("./src/shared/param_pages/styles/index.mjs").then(S => {
  const p = S.validateSet({ id: "t", title: "T", kind: "draw", options: [] });
  console.log(p.length >= 1 ? "OK empty-set caught" : "BROKEN");
  const q = S.validateSet({ id: "t", title: "T", kind: "nope", options: [] });
  console.log(q.some(x => x.includes("kind")) ? "OK bad-kind caught" : "BROKEN");
})'
```
Expected: `OK empty-set caught` then `OK bad-kind caught`

- [ ] **Step 5: Commit**

```bash
git add src/shared/param_pages/styles/index.mjs tests/host/test_style_catalog.sh
git commit -m "styles: option registry and structural validation

Validation before any options exist, so an authoring mistake fails at the
test rather than becoming a silently wrong contact sheet. validateSet
returns every problem rather than throwing on the first -- when a set is
being written, seeing all the complaints at once is faster."
```

---

## Task 3: Shared dither predicates

**Goal:** One screen-space density ladder, shared by every option that needs a fill, so the vimana-derived options in different sets cannot drift apart.

**Files:**
- Create: `src/shared/param_pages/styles/dither.mjs`
- Modify: `tests/host/test_style_catalog.sh`

**Acceptance Criteria:**
- [ ] Exports `SOLID`, `DIAG_HEAVY`, `CHECKER`, `DIAG_LIGHT`, `DOTS` predicates taking absolute `(x, y)`
- [ ] Exports `fillDithered(ctx, x, y, w, h, pattern)` which only ever sets pixels, never clears
- [ ] Exports `fillTerrain(ctx, x, y, w, h, heights, pattern)` filling from the curve down to the bottom edge
- [ ] Predicates are screen-space: the same `(x,y)` gives the same answer regardless of the rect it is called with
- [ ] Measured densities over a 64x64 field are within 3 percentage points of nominal

**Verify:** `bash tests/host/test_style_catalog.sh` -> includes `PASS: dither densities`

**Steps:**

- [ ] **Step 1: Write the module**

Create `src/shared/param_pages/styles/dither.mjs`:

```js
/**
 * dither.mjs — the density ladder shared by every option that fills an area.
 *
 * On a 1-bit display, "colour" is pixel density. One ladder with fixed
 * meanings is what makes a set of options read as one design language rather
 * than as ten unrelated fills:
 *
 *   100%  SOLID       active, selected, primary
 *    75%  DIAG_HEAVY  emphasised secondary
 *    50%  CHECKER     muted, ghosted
 *    25%  DIAG_LIGHT  background, hint, range
 *    11%  DOTS(3)     reference grid, barely there
 *
 * Predicates take ABSOLUTE screen coordinates, not rect-relative ones. That is
 * deliberate: phase-continuity across adjacent shapes means two neighbouring
 * filled areas share one lattice instead of showing a seam, and a shape that
 * moves does not shimmer as its fill re-phases under it.
 *
 * Adapted from the density ladder in vimana2-rust
 * (crates/vimana-app/src/anim.rs, docs/plans/aesthetic-reference.md).
 */

export const SOLID = (_x, _y) => true;
export const CHECKER = (x, y) => ((x + y) % 2) === 0;
export const DIAG_LIGHT = (x, y) => ((x + y) % 4) === 0;
export const DIAG_HEAVY = (x, y) => ((x + y) % 4) !== 0;
export const DIAG_THIRD = (x, y) => ((x + y) % 3) === 0;
export const DOTS = (n = 3) => (x, y) => (x % n) === 0 && (y % n) === 0;

/** Nominal densities, for tests and for documenting an option. */
export const NOMINAL = { SOLID: 1, DIAG_HEAVY: 0.75, CHECKER: 0.5, DIAG_THIRD: 1 / 3, DIAG_LIGHT: 0.25, DOTS3: 1 / 9 };

/**
 * Fill a rect through a pattern. Only ever SETS pixels — a dithered fill
 * composites over whatever is beneath it rather than punching a hole in it.
 */
export function fillDithered(ctx, x, y, w, h, pattern) {
    for (let dy = 0; dy < h; dy++) {
        for (let dx = 0; dx < w; dx++) {
            const px = x + dx, py = y + dy;
            if (pattern(px, py)) ctx.fillRect(px, py, 1, 1, 1);
        }
    }
}

/**
 * Fill from a curve down to the bottom edge of the rect, so the shape reads as
 * mass rather than as a line. `heights` is one normalised 0..1 value per
 * column; short arrays hold their last value.
 *
 * `crest` draws a solid 1px line along the curve itself. Without it a light
 * pattern makes the boundary ambiguous, which is the whole reason the vimana
 * filter page strokes the crest solid over a 1-in-3 hatch.
 */
export function fillTerrain(ctx, x, y, w, h, heights, pattern, crest = true) {
    if (!heights || !heights.length) return;
    for (let i = 0; i < w; i++) {
        const hv = Math.max(0, Math.min(1, heights[Math.min(i, heights.length - 1)]));
        const curveY = y + h - Math.round(hv * h);
        const bottom = y + h - 1;
        if (crest && curveY <= bottom) ctx.fillRect(x + i, curveY, 1, 1, 1);
        for (let py = curveY + (crest ? 1 : 0); py <= bottom; py++) {
            if (pattern(x + i, py)) ctx.fillRect(x + i, py, 1, 1, 1);
        }
    }
}

/** A 50%-dotted rule — the separator idiom. Horizontal. */
export function dottedRule(ctx, x, y, w, phase = 0) {
    for (let i = 0; i < w; i++) if (((i + phase) % 2) === 0) ctx.fillRect(x + i, y, 1, 1, 1);
}

/** A dashed vertical rule. `dash` on, `gap` off. */
export function dashedVRule(ctx, x, y, h, dash = 1, gap = 1) {
    const cycle = dash + gap;
    for (let i = 0; i < h; i++) if ((i % cycle) < dash) ctx.fillRect(x, y + i, 1, 1, 1);
}

/** Knock the four corner pixels out of a box. The "rounded" idiom. */
export function notchCorners(ctx, x, y, w, h) {
    ctx.fillRect(x, y, 1, 1, 0);
    ctx.fillRect(x + w - 1, y, 1, 1, 0);
    ctx.fillRect(x, y + h - 1, 1, 1, 0);
    ctx.fillRect(x + w - 1, y + h - 1, 1, 1, 0);
}
```

- [ ] **Step 2: Add the density assertions to the test**

In `tests/host/test_style_catalog.sh`, change the import array and add a block before the final `console.log`. Replace the whole `node -e` script body with:

```bash
node -e '
Promise.all([
  import("./src/shared/param_pages/styles/index.mjs"),
  import("./src/shared/param_pages/styles/dither.mjs"),
]).then(async ([S, D]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };

  const problems = S.validateAll();
  if (problems.length) fail("structural problems:\n  " + problems.join("\n  "));
  console.log("PASS: " + S.SETS.length + " set(s) structurally valid");

  /* ---- dither densities ---- */
  const density = (pred) => {
    let on = 0;
    for (let y = 0; y < 64; y++) for (let x = 0; x < 64; x++) if (pred(x, y)) on++;
    return on / 4096;
  };
  const near = (got, want, tol, what) => {
    if (Math.abs(got - want) > tol) fail(what + " density " + got.toFixed(3) + ", want ~" + want);
  };
  near(density(D.SOLID), 1, 0.001, "SOLID");
  near(density(D.DIAG_HEAVY), 0.75, 0.03, "DIAG_HEAVY");
  near(density(D.CHECKER), 0.5, 0.03, "CHECKER");
  near(density(D.DIAG_THIRD), 1 / 3, 0.03, "DIAG_THIRD");
  near(density(D.DIAG_LIGHT), 0.25, 0.03, "DIAG_LIGHT");
  near(density(D.DOTS(3)), 1 / 9, 0.03, "DOTS3");

  /* ---- predicates are screen-space, not rect-relative ---- */
  for (const [name, pred] of [["CHECKER", D.CHECKER], ["DIAG_LIGHT", D.DIAG_LIGHT]]) {
    if (pred(10, 10) !== pred(10, 10)) fail(name + " is not pure");
    if (pred(0, 0) === pred(1, 0) === pred(2, 0)) fail(name + " looks constant along x");
  }
  console.log("PASS: dither densities");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
```

- [ ] **Step 3: Run the test**

Run: `bash tests/host/test_style_catalog.sh`
Expected:
```
PASS: 0 set(s) structurally valid
PASS: dither densities
```

- [ ] **Step 4: Commit**

```bash
git add src/shared/param_pages/styles/dither.mjs tests/host/test_style_catalog.sh
git commit -m "styles: shared screen-space dither ladder

Predicates take absolute coordinates, not rect-relative ones, so adjacent
filled areas share one lattice instead of showing a seam and a moving shape
does not shimmer as its fill re-phases. Densities are asserted because a
pattern that is subtly wrong is invisible in a contact sheet.

Ladder adapted from vimana2-rust crates/vimana-app/src/anim.rs."
```

---

## Task 4: Catalog render tool

**Goal:** Render any registered set to isolated swatches, in-context pages, and a contact sheet — before most sets exist, so authoring gets immediate visual feedback.

**Files:**
- Create: `tools/param-pages/catalog.mjs`
- Modify: `.gitignore` (already has `catalog-out/` from the spec commit — verify only)

**Acceptance Criteria:**
- [ ] `node tools/param-pages/catalog.mjs --list` prints registered sets and option counts
- [ ] `node tools/param-pages/catalog.mjs --set <id>` writes swatch + in-context + contact-sheet PNGs to `catalog-out/<id>/`
- [ ] `--all` does every registered set
- [ ] A `draw`-kind option is rendered both isolated at 4x and composited into a real module page at 1x
- [ ] The current shipping widget is rendered as `baseline.png` in every `draw` set
- [ ] Exits non-zero with a readable message if `validateAll()` reports problems

**Verify:** `node tools/param-pages/catalog.mjs --list` -> prints the registry without error

**Steps:**

- [ ] **Step 1: Write the tool**

Create `tools/param-pages/catalog.mjs`:

```js
/**
 * catalog.mjs — render the SCH-50 option catalog to PNG.
 *
 * Every option is rendered TWICE: an isolated swatch at 4x for comparing
 * construction, and composited into a real module page at 1x for judging it
 * where it will actually live. The in-context render is not optional. An arc
 * that reads cleanly on a blank field can collide with the modulation dot or
 * the label strip once it is in a 32px cell next to seven neighbours, and
 * reviewing widgets in isolation is precisely what has let real defects
 * through before.
 *
 * Output goes to catalog-out/, which is gitignored. Only the contact sheets
 * are committed, as the record of what was in the catalog.
 *
 * Node-only. Nothing here ships to the device.
 */

import fs from "node:fs";
import path from "node:path";
import { createFramebuffer, drawContext, SCREEN_WIDTH, SCREEN_HEIGHT } from "./harness.mjs";
import * as S from "../../src/shared/param_pages/styles/index.mjs";
import * as RM from "../../src/shared/param_pages/render_page_movy.mjs";

const OUT_ROOT = "catalog-out";

/* ---------------------------------------------------------------- helpers */

function ensureDir(p) { fs.mkdirSync(p, { recursive: true }); }

function writePng(fb, file, scale) {
    ensureDir(path.dirname(file));
    fs.writeFileSync(file, fb.toPng(scale));
}

/** A blank framebuffer plus its context. */
function surface(w = SCREEN_WIDTH, h = SCREEN_HEIGHT) {
    const fb = createFramebuffer(w, h);
    return { fb, ctx: drawContext(fb) };
}

/**
 * Compose several framebuffers into one taller sheet. Written here rather
 * than in the harness because it is a catalog concern -- the harness models a
 * 128x64 device and a contact sheet is not one.
 */
function stack(frames, gap = 2) {
    const w = Math.max(...frames.map((f) => f.width));
    const h = frames.reduce((a, f) => a + f.height + gap, 0) - gap;
    const out = createFramebuffer(w, h);
    let y = 0;
    for (const f of frames) {
        for (let fy = 0; fy < f.height; fy++)
            for (let fx = 0; fx < f.width; fx++)
                out.setPixel(fx, y + fy, f.pixels[fy * f.width + fx]);
        y += f.height + gap;
    }
    return out;
}
```

**The framebuffer has no `getPixel`.** Its surface is
`{ width, height, pixels, fillRect, print, textWidth, clearScreen, setPixel,
toAscii, toBlocks, toPng, missingGlyphs, clipped(), countLit() }`. Read pixels
directly out of the `pixels` Uint8Array at `y * width + x`.

```js

/* ------------------------------------------------------------- swatch/page */

/**
 * Isolated swatch: the widget alone, centred in a box the size of a grid cell,
 * with a caption. Rendered at 4x by the caller.
 */
function renderSwatch(opt, set) {
    const W = RM.CELL_W, H = RM.BOX_H + RM.LBL_H + 2;
    const { fb, ctx } = surface(W, H);
    if (set.kind === S.KIND_DRAW) {
        /* Widget origin inside the cell, matching drawKnobWidget centring. */
        const kx = Math.floor((RM.CELL_W - RM.KW) / 2);
        opt.draw(ctx, kx, 0, 0.62);
    }
    return fb;
}

/**
 * In-context: a real module page with this option substituted for the widget.
 * Drawn by rendering the page through the shipping renderer, then clearing and
 * redrawing each widget box with the option. The page around it -- header,
 * bank bar, labels, footer -- is exactly what the device draws.
 */
function renderInContext(opt, set, pageCase) {
    const { fb, ctx } = surface();
    RM.renderPageMovy(ctx, pageCase);
    if (set.kind !== S.KIND_DRAW) return fb;

    for (const [row, rowY] of [[0, RM.ROW0_Y], [1, RM.ROW1_Y]]) {
        for (let col = 0; col < 4; col++) {
            const cellX = col * RM.CELL_W;
            const kx = cellX + Math.floor((RM.CELL_W - RM.KW) / 2);
            ctx.fillRect(cellX, rowY, RM.CELL_W, RM.BOX_H, 0);
            /* A spread of values so one page shows the option across its range. */
            const v = (row * 4 + col + 1) / 9;
            opt.draw(ctx, kx, rowY, v);
        }
    }
    return fb;
}

/* -------------------------------------------------------------------- main */

function renderSet(set, pageCase) {
    const dir = path.join(OUT_ROOT, set.id);
    ensureDir(dir);
    const sheetFrames = [];

    if (set.kind === S.KIND_DRAW) {
        const base = surface(RM.CELL_W, RM.BOX_H + RM.LBL_H + 2);
        const bkx = Math.floor((RM.CELL_W - RM.KW) / 2);
        RM.drawArcKnob(base.ctx, bkx, 0, 0.62);
        writePng(base.fb, path.join(dir, "baseline.png"), 4);
        sheetFrames.push(base.fb);
    }

    for (const opt of [...set.options].sort((a, b) => a.position - b.position)) {
        const sw = renderSwatch(opt, set);
        writePng(sw, path.join(dir, `${String(opt.position).padStart(2, "0")}-${opt.id}-swatch.png`), 4);
        sheetFrames.push(sw);

        const pg = renderInContext(opt, set, pageCase);
        writePng(pg, path.join(dir, `${String(opt.position).padStart(2, "0")}-${opt.id}-page.png`), 4);
    }

    writePng(stack(sheetFrames), path.join(dir, "contact-sheet.png"), 4);
    console.log(`${set.id}: ${set.options.length} option(s) -> ${dir}/`);
}

function main() {
    const argv = process.argv.slice(2);
    const problems = S.validateAll();
    if (problems.length) {
        console.error("catalog: registry has structural problems:");
        for (const p of problems) console.error("  " + p);
        process.exit(1);
    }

    if (argv.includes("--list") || argv.length === 0) {
        if (!S.SETS.length) { console.log("catalog: no sets registered yet"); return; }
        for (const s of S.SETS) console.log(`${s.id.padEnd(16)} ${s.kind.padEnd(7)} ${s.options.length} option(s)  ${s.title}`);
        return;
    }

    /* A representative page for the in-context render. Uses the same fixture
     * the preview tool does, so what appears around the widget is real. */
    const pageCase = JSON.parse(fs.readFileSync("tools/param-pages/catalog-page.json", "utf8"));

    const only = argv.includes("--set") ? argv[argv.indexOf("--set") + 1] : null;
    const sets = only ? [S.setById(only)].filter(Boolean) : S.SETS;
    if (only && !sets.length) { console.error("catalog: no such set: " + only); process.exit(1); }
    for (const s of sets) renderSet(s, pageCase);
}

main();
```

- [ ] **Step 2: Capture a representative page fixture**

The in-context render needs a real `renderPageMovy` argument object. Capture one from the existing preview path rather than inventing it:

Run:
```bash
node -e 'Promise.all([
  import("./tools/param-pages/cases.mjs"),
  import("node:fs"),
]).then(([C, fs]) => {
  const fx = JSON.parse(fs.readFileSync(C.FIXTURE, "utf8"));
  console.log(Object.keys(fx).slice(0, 20).join("\n"));
})'
```
Expected: a list of module ids present in the contract fixture.

Then build `tools/param-pages/catalog-page.json` by dumping the object `preview.mjs` passes to `renderPageMovy` for one knob-grid page. Read `tools/param-pages/preview.mjs` to find where it constructs that object, add a temporary `JSON.stringify` of it, run the preview once for a module with a full 8-knob page, and save the result:

```bash
node tools/param-pages/preview.mjs braids --page 0 --layout movy > /dev/null
```

Save the captured object to `tools/param-pages/catalog-page.json`. Remove the temporary dump afterwards.

- [ ] **Step 3: Verify the tool runs against an empty registry**

Run: `node tools/param-pages/catalog.mjs --list`
Expected: `catalog: no sets registered yet`

- [ ] **Step 4: Confirm catalog-out is ignored**

Run: `command grep -n 'catalog-out' .gitignore`
Expected: one match.

- [ ] **Step 5: Commit**

```bash
git add tools/param-pages/catalog.mjs tools/param-pages/catalog-page.json
git commit -m "tools: catalog renderer -- swatch, in-context, contact sheet

Every option renders twice. The in-context page is the one that matters: an
arc that reads cleanly on a blank field can collide with the modulation dot
or the label strip once it is in a 32px cell beside seven neighbours, and
judging widgets in isolation is what has let real defects through before.

Every draw set also renders the shipping widget as baseline.png, so there is
always something to compare against rather than a vague memory of it."
```

---

## Task 5: Set 1 — arc knob, ten options

**Goal:** The first real set, authored end to end, establishing the pattern every later set follows.

**Files:**
- Create: `src/shared/param_pages/styles/knob.mjs`
- Modify: `src/shared/param_pages/styles/index.mjs` (import + register)

**Acceptance Criteria:**
- [ ] Ten options, positions 1..10, ordered minimal -> radical
- [ ] Every `draw` has signature `(ctx, kx, ky, normVal)`, matching `drawArcKnob`
- [ ] Every option renders inside `KW` (17) x `BOX_H` (15) with zero clipped pixels
- [ ] Positions 8-10 include at least one non-rotary treatment
- [ ] `bash tests/host/test_style_catalog.sh` passes
- [ ] `node tools/param-pages/catalog.mjs --set knob` writes 21 PNGs plus a contact sheet

**Verify:** `bash tests/host/test_style_catalog.sh && node tools/param-pages/catalog.mjs --set knob` -> PASS, then `knob: 10 option(s)`

**Steps:**

- [ ] **Step 1: Write the ten options**

Create `src/shared/param_pages/styles/knob.mjs`. The axis assignment:

| Pos | id | Treatment |
|---|---|---|
| 1 | `thin-arc` | Current geometry, 1px arc, shorter pointer |
| 2 | `open-gap` | Arc with a gap at the value, pointer removed |
| 3 | `inset-rim` | Arc inset 1px, solid 2px pointer to rim |
| 4 | `tick-ladder` | 12 radial ticks, lit up to value |
| 5 | `stipple-arc` | Arc as 2px stamps at fixed count, spacing encodes value |
| 6 | `segment-ring` | 8 discrete segments, filled to value |
| 7 | `bar-in-box` | Horizontal bar inside a notched box |
| 8 | `dotted-track` | Dotted rail, solid 3x3 thumb (vimana idiom) |
| 9 | `terrain-cell` | Cell filled from bottom to value with DIAG_THIRD, solid crest |
| 10 | `numeric-slab` | No graphic; large value glyphs on a dithered ground |

Each option is written as:

```js
import { KIND_DRAW, registerSet } from "./index.mjs";
import { DIAG_THIRD, DOTS, dottedRule, fillTerrain, notchCorners } from "./dither.mjs";
import { KW, BOX_H } from "../render_page_movy.mjs";

/* Widget box is KW (17) wide by BOX_H (15) tall, origin (kx, ky). Every
 * option MUST stay inside it -- one row of overflow lands on the label row
 * below, which the grid does not repaint. */

const CX = (kx) => kx + Math.floor(KW / 2);
const CY = (ky) => ky + Math.floor(BOX_H / 2);

/* pos 1 -- current silhouette, lighter hand. */
function thinArc(ctx, kx, ky, v) {
    const cx = CX(kx), cy = CY(ky), r = 6;
    ctx.drawArc(cx, cy, r, 230, 260, 1);
    const a = (230 + 260 * Math.max(0, Math.min(1, v))) * Math.PI / 180;
    const px = cx + Math.round(Math.sin(a) * (r - 3));
    const py = cy - Math.round(Math.cos(a) * (r - 3));
    ctx.line(cx, cy, px, py, 1);
}

/* pos 8 -- vimana: dotted rail, solid thumb. No rotary metaphor at all. */
function dottedTrack(ctx, kx, ky, v) {
    const y = ky + Math.floor(BOX_H / 2) - 1;
    const w = KW - 2;
    dottedRule(ctx, kx + 1, y + 1, w);
    const t = kx + 1 + Math.round(Math.max(0, Math.min(1, v)) * (w - 3));
    ctx.fillRect(t, y, 3, 3, 1);
}

/* pos 9 -- the cell itself is the readout: terrain fill to the value. */
function terrainCell(ctx, kx, ky, v) {
    const w = KW - 2, h = BOX_H - 2;
    const heights = new Array(w).fill(Math.max(0, Math.min(1, v)));
    fillTerrain(ctx, kx + 1, ky + 1, w, h, heights, DIAG_THIRD, true);
    notchCorners(ctx, kx, ky, KW, BOX_H);
}

/* ... the remaining seven, each with the same shape ... */

export default registerSet({
    id: "knob",
    title: "Arc knob",
    kind: KIND_DRAW,
    unit: "drawArcKnob(ctx, kx, ky, normVal)",
    options: [
        { id: "thin-arc", name: "Thin arc", position: 1, draw: thinArc,
          note: "Current geometry with a 1px arc and a shorter pointer. The lightest possible move." },
        { id: "dotted-track", name: "Dotted track", position: 8, draw: dottedTrack,
          note: "Drops the rotary metaphor for a dotted rail and a solid thumb. vimana has no dial on its 1-bit display at all." },
        { id: "terrain-cell", name: "Terrain cell", position: 9, draw: terrainCell,
          note: "The cell is the readout: filled from the bottom with a 1-in-3 hatch under a solid crest." },
        /* ... the remaining seven ... */
    ],
});
```

Author all ten before running the catalog. Every `draw` must clamp `v` to 0..1 and must not draw outside `kx..kx+KW-1`, `ky..ky+BOX_H-1`.

- [ ] **Step 2: Register the set**

Each set file exports a named `register()` and `index.mjs` **calls** it at the
bottom of the module:

```js
/* in knob.mjs */
export function register() {
    return registerSet({ id: "knob", title: "Arc knob", kind: KIND_DRAW, /* ... */ });
}

/* at the bottom of index.mjs */
import { register as registerKnob } from "./knob.mjs";
registerKnob();
```

**Self-registration on import does not work here, and the reason is worth
knowing because both obvious spellings fail.** The set file imports
`registerSet` and the `KIND_*` constants from `index.mjs`, so the two modules
are a cycle.

- `import "./knob.mjs";` at the bottom — **import declarations are hoisted**, so
  bottom placement does not delay evaluation. The set runs before `index.mjs`'s
  body and finds `SETS` and `KIND_DRAW` in the temporal dead zone:
  `ReferenceError` at import time, nothing registered. Only `registerSet`
  survives, because a function declaration hoists too — which is exactly what
  makes this easy to talk yourself into.
- `await import("./knob.mjs");` — **deadlocks.** The set statically imports
  `index.mjs`, so its evaluation waits on ours while ours waits on its. Node
  reports "Detected unsettled top-level await" and exits 13.

A named export called after the body has run has neither problem: the set file
only *declares* functions at evaluation time, so the hoisted import of it is
harmless, and by the time `register()` runs every binding above is initialised.

- [ ] **Step 3: Add the clipping assertion to the test**

In `tests/host/test_style_catalog.sh`, add before the final success log, inside the `.then`:

```js
  /* ---- every draw option stays inside its widget box ---- */
  const H = await import("./tools/param-pages/harness.mjs");
  const RM = await import("./src/shared/param_pages/render_page_movy.mjs");
  for (const set of S.SETS) {
    if (set.kind !== S.KIND_DRAW) continue;
    for (const o of set.options) {
      for (const v of [0, 0.5, 1]) {
        const fb = H.createFramebuffer(RM.KW, RM.BOX_H);
        const ctx = H.drawContext(fb);
        o.draw(ctx, 0, 0, v);
        if (fb.clipped() !== 0)
          fail(set.id + "/" + o.id + " at v=" + v + " drew " + fb.clipped() + " pixel(s) outside its box");
      }
    }
  }
  console.log("PASS: draw options stay in their boxes");
```

- [ ] **Step 4: Run the test**

Run: `bash tests/host/test_style_catalog.sh`
Expected:
```
PASS: 1 set(s) structurally valid
PASS: dither densities
PASS: draw options stay in their boxes
```

If any option clips, fix the option — do not widen the assertion. One row of overflow lands on the label row, which the grid does not repaint, so it would appear as a stray artefact on the device.

- [ ] **Step 5: Render and LOOK at the output**

Run: `node tools/param-pages/catalog.mjs --set knob`
Expected: `knob: 10 option(s) -> catalog-out/knob/`

Then open `catalog-out/knob/contact-sheet.png` and every `*-page.png` and actually look at them. Text art and passing tests do not substitute for this: seven real defects have previously survived three rounds of review that skipped it. Specifically check that no option collides with the modulation dot or the label strip in the in-context render.

- [ ] **Step 6: Commit**

```bash
git add src/shared/param_pages/styles/knob.mjs src/shared/param_pages/styles/index.mjs tests/host/test_style_catalog.sh
git commit -m "styles: set 1 -- ten arc-knob options

Ordered minimal -> radical. Positions 8-10 abandon the rotary metaphor
entirely, which is what vimana does on its 1-bit display: it has no dial
widget and renders knob parameters as linear cells.

Clipping is asserted at v=0, 0.5 and 1 rather than eyeballed. One row of
overflow lands on the label row, which the grid does not repaint, so it
would show up as a stray artefact on hardware and nowhere else."
```

---

## Task 6: Sets 2, 3, 6, 11 — fader, fills, opaque box, switch

**Goal:** The remaining independent `draw` sets, following the pattern Task 5 established.

**Files:**
- Create: `src/shared/param_pages/styles/fader.mjs`
- Create: `src/shared/param_pages/styles/fills.mjs`
- Create: `src/shared/param_pages/styles/opaque_box.mjs`
- Create: `src/shared/param_pages/styles/viz_switch.mjs`
- Modify: `src/shared/param_pages/styles/index.mjs`

**Acceptance Criteria:**
- [ ] Four sets, ten options each, positions 1..10, ordered minimal -> radical
- [ ] `fader` options match `drawFader(ctx, rect, key, values, metaIndex)` and render within `VIZ_ROWS` (13)
- [ ] `fills` options draw the footer band and rule within `FOOTER_Y` (56) to 63 and `RULE_Y` (55)
- [ ] `opaque_box` options match `drawOpaqueBox(ctx, kx, ky, value, override)`
- [ ] `viz_switch` options match `drawSwitch(ctx, rect, key, values, metaIndex)`
- [ ] All four render with zero clipped pixels
- [ ] Contact sheets generated and visually reviewed

**Verify:** `bash tests/host/test_style_catalog.sh && node tools/param-pages/catalog.mjs --all` -> PASS, 5 sets rendered

**Steps:**

- [ ] **Step 1: Write `fader.mjs`**

Signature is `(ctx, rect, key, values, metaIndex)` where `rect` is `{x, y, w, h}` and `h` is `VIZ_ROWS` (13). Axis:

| Pos | id | Treatment |
|---|---|---|
| 1 | `thin-rails` | Current, 1px fill column instead of 3px |
| 2 | `no-rails` | Fill column only, rails removed |
| 3 | `capped` | Current plus a 1px cap line at the top of the fill |
| 4 | `ghost-track` | Unfilled portion shown at CHECKER density |
| 5 | `notched` | Fill column with a notch every 4 rows |
| 6 | `outline-fill` | Outlined column, interior at DIAG_HEAVY |
| 7 | `stepped` | Fill quantised to 6 discrete blocks |
| 8 | `dotted-thumb` | vimana: dotted rail, solid 3x3 thumb, no fill |
| 9 | `terrain` | Filled to value with DIAG_THIRD under a solid crest |
| 10 | `horizontal` | Rotated to a horizontal bar, label above |

- [ ] **Step 2: Write `fills.mjs`**

These draw the bottom band. Signature `(ctx, hints)` matching `drawFooter`. Axis:

| Pos | id | Treatment |
|---|---|---|
| 1 | `thin-rule` | Current, 1px solid rule |
| 2 | `no-rule` | Rule removed, spacing only |
| 3 | `dotted-rule` | 50% dotted rule (vimana idiom) |
| 4 | `dashed-rule` | 2-on-1-off dashed rule |
| 5 | `double-rule` | Two 1px rules with a 1px gap |
| 6 | `inverted-band` | Whole footer inverted, corners notched, hints in black |
| 7 | `dither-band` | Footer ground at DIAG_LIGHT, hints knocked out |
| 8 | `tab-band` | Inverted band that extrudes upward under the active cell |
| 9 | `tethered` | Dotted rule with a 5-3-1 triangle under the active cell |
| 10 | `terrain-band` | Footer ground as a terrain fill rising toward the active hint |

- [ ] **Step 3: Write `opaque_box.mjs` and `viz_switch.mjs`**

`drawOpaqueBox` currently has no frame of its own — the divable brackets are its frame, which `test_knob_card.sh` pins. Any option that removes the brackets must supply its own frame, and the note must say so.

`drawSwitch` is a 26px tabulated circle sprite ported pixel-for-pixel from Movy. That makes it one of the more directly-derived widgets, so the note on position 1 should record that the current sprite is the thing being moved away from.

- [ ] **Step 4: Register all four**

Follow the `register()` pattern from Task 5 Step 2 — **not** a side-effect
import, which throws a `ReferenceError` from the temporal dead zone. At the
bottom of `index.mjs`:

```js
import { register as registerKnob } from "./knob.mjs";
import { register as registerFader } from "./fader.mjs";
import { register as registerFills } from "./fills.mjs";
import { register as registerOpaqueBox } from "./opaque_box.mjs";
import { register as registerVizSwitch } from "./viz_switch.mjs";

registerKnob();
registerFader();
registerFills();
registerOpaqueBox();
registerVizSwitch();
```

Call order is catalog order.

- [ ] **Step 5: Extend the clipping assertion to non-widget kinds**

The Task 5 assertion assumes `(ctx, kx, ky, v)`. Generalise it: each set declares a `probe(ctx, drawFn)` that calls the option with appropriate arguments and a `probeSize` of `{w, h}`. Add to each set descriptor:

```js
probeSize: { w: 128, h: 13 },
probe: (ctx, draw) => draw(ctx, { x: 0, y: 0, w: 128, h: 13 }, "level", { level: 0.5 }, {}),
```

Update the test block from Task 5 Step 3 to use `set.probe` when present, falling back to the `(ctx, 0, 0, v)` form.

- [ ] **Step 6: Run, render, and LOOK**

Run: `bash tests/host/test_style_catalog.sh && node tools/param-pages/catalog.mjs --all`
Expected: all PASS, then five `<id>: 10 option(s)` lines.

Open every contact sheet and every in-context page. Check specifically that `fills` options do not collide with the bank bar at row 7 — a menu page owns that row, which is why the enum picker list starts at y=9 rather than `MENU_LIST_Y`.

- [ ] **Step 7: Commit**

```bash
git add src/shared/param_pages/styles/ tests/host/test_style_catalog.sh
git commit -m "styles: sets 2, 3, 6, 11 -- fader, fills, opaque box, switch

The switch sprite is a pixel-for-pixel Movy port, so its position-1 note
records that the current sprite is the thing being moved away from rather
than a neutral starting point.

Opaque box options that drop the divable brackets must supply their own
frame: drawOpaqueBox has no frame of its own and the brackets ARE it, which
test_knob_card.sh pins on the pixel buffer."
```

---

## Task 7: Set 12 — font, ten complete 4x5 sets

**Goal:** Ten complete replacement letterform sets, so the nine copied Elektron glyphs and the forty-seven authored to match them are all replaced together.

**Files:**
- Create: `src/shared/param_pages/styles/font/index.mjs`
- Create: `src/shared/param_pages/styles/font/<variant>.mjs` (10 files)
- Modify: `src/shared/param_pages/styles/index.mjs`
- Modify: `tests/host/test_style_catalog.sh`

**Acceptance Criteria:**
- [ ] Ten glyph tables, each covering the full `CHARS` string from `font4x5.mjs`
- [ ] Format is `[advance, yOff, w, h, ...rowBits]`, bit0 = leftmost, identical to `font5x3.mjs`
- [ ] **No glyph entry is byte-identical to `font4x5.mjs`** for the nine Elektron letters `A D E I L M P T U`
- [ ] Every set renders `AMPLITUDE` inside a 32px cell without clipping
- [ ] Test asserts the non-identity property

**Verify:** `bash tests/host/test_style_catalog.sh` -> includes `PASS: font sets differ from font4x5`

**Steps:**

- [ ] **Step 1: Read the current font to get CHARS and the encoding**

Run: `sed -n '40,46p' src/shared/param_pages/font4x5.mjs`
Expected: the `CHARS` constant and the row-bit legend.

`CHARS` is:
```
 !"'()+,-./:0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ%<>=?*#&_\^
```

Every variant must supply an entry for each character in that order.

- [ ] **Step 2: Author the ten variants**

Each variant is a construction rule applied consistently, not a set of hand-drawn letters. Axis:

| Pos | id | Construction |
|---|---|---|
| 1 | `humanist` | 4-wide caps, open apertures, 1px stems |
| 2 | `square` | Uniform 4x5 box construction, flat terminals |
| 3 | `narrow` | 3-wide body with 4px advance, tall counters |
| 4 | `rounded` | 4-wide with corner pixels dropped on O C G D |
| 5 | `slab` | 4-wide with 1px serifs on stem terminals |
| 6 | `stencil` | 4-wide with a 1px break in every enclosed counter |
| 7 | `condensed-caps` | 3-wide throughout, 4px advance, no descenders |
| 8 | `dot-matrix` | 4x5 on a visible lattice, every glyph on odd pixels |
| 9 | `geometric` | Circles and straight lines only, no diagonal strokes |
| 10 | `wide` | 5-wide body, 6px advance, generous counters |

For each, write the glyph table with the same inline row-bit comments the current font uses, e.g.:

```js
/* Row bit values, 4-wide: col0=1 col1=2 col2=4 col3=8
 *   ....=0  #...=1  .#..=2  ##..=3  ..#.=4  #.#.=5  .##.=6  ###.=7
 *   ...#=8  #..#=9  .#.#=10 ##.#=11 ..##=12 #.##=13 .###=14 ####=15 */
export const GLYPHS = [
    [3, 0, 0, 5],                       /* ' ' */
    /* ... */
    [5, 0, 4, 5, 6, 9, 15, 9, 9],       /* A */
    /* ... */
];
```

**The nine letters `A D E I L M P T U` must differ from `font4x5.mjs`.** That file documents them as read straight off Elektron's screen, and replacing them is the single most concrete thing this issue does.

- [ ] **Step 3: Write the font set registry**

Create `src/shared/param_pages/styles/font/index.mjs` importing each variant and registering the set with `kind: KIND_FONT` and `glyphs` payloads.

- [ ] **Step 4: Add the non-identity assertion**

In `tests/host/test_style_catalog.sh`, inside the `.then`:

```js
  /* ---- font options must not reproduce the shipping Elektron glyphs ---- */
  const F4 = await import("./src/shared/param_pages/font4x5.mjs");
  const fontSet = S.SETS.find((s) => s.kind === S.KIND_FONT);
  if (fontSet) {
    const CH = F4.CHARS || " ";
    const ELEKTRON = "ADEILMPTU";
    for (const o of fontSet.options) {
      if (o.glyphs.length !== CH.length)
        fail(o.id + ": " + o.glyphs.length + " glyphs, want " + CH.length);
      for (const letter of ELEKTRON) {
        const i = CH.indexOf(letter);
        if (i < 0) continue;
        const a = JSON.stringify(o.glyphs[i]);
        const b = JSON.stringify(F4.GLYPHS_FOR_TEST ? F4.GLYPHS_FOR_TEST[i] : null);
        if (b !== "null" && a === b)
          fail(o.id + ": glyph " + letter + " is byte-identical to font4x5, which is the thing being replaced");
      }
    }
    console.log("PASS: font sets differ from font4x5");
  }
```

If `font4x5.mjs` does not export its glyph table, add a test-only export `export const GLYPHS_FOR_TEST = G;` at the end of that file — additive, no behaviour change, and the assertion is worthless without it.

- [ ] **Step 5: Run and render**

Run: `bash tests/host/test_style_catalog.sh`
Expected: includes `PASS: font sets differ from font4x5`

Render specimen sheets showing `AMPLITUDE`, `SINE`, `SAW`, `MAIN`, `ATTACK` in each variant at 4x and look at them. `font4x5.mjs` records that the 3-wide font it replaced rendered `MAIN` as `MAIK`, `SINE` as `SIKE` and `SAW` as `SAU` — those exact strings are the legibility regression test, so check them by eye in every variant.

- [ ] **Step 6: Commit**

```bash
git add src/shared/param_pages/styles/font/ src/shared/param_pages/font4x5.mjs tests/host/test_style_catalog.sh
git commit -m "styles: set 12 -- ten complete 4x5 replacement fonts

font4x5.mjs says of itself that nine glyphs are Elektron letterforms read
straight off their screen, and that the rest were authored to match their
weight and construction. Replacing only the nine would leave a font whose
whole system still derives from theirs, so each option is a complete set
built from its own construction rule.

The non-identity of those nine is asserted rather than assumed. Legibility
is checked against MAIN/SINE/SAW by eye, which is where the previous 3-wide
font failed -- it rendered them MAIK, SIKE and SAU."
```

---

## Task 8: Sets 4, 5 — enum square and label cell

**Goal:** The two text-bearing sets, authored after font so they can be sized against the real letterform options.

**Files:**
- Create: `src/shared/param_pages/styles/enum_square.mjs`
- Create: `src/shared/param_pages/styles/label_cell.mjs`
- Modify: `src/shared/param_pages/styles/index.mjs`

**Acceptance Criteria:**
- [ ] Ten options each, positions 1..10
- [ ] `enum_square` matches `drawEnumSquare(ctx, kx, ky, text)` and fits `ENUM_W` (20) x `BOX_H` (15)
- [ ] `label_cell` matches `drawLabelCell(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated)` and fits `CELL_W` (32) x `LBL_H` (7)
- [ ] Each set's note records which font variants it fits and which it does not
- [ ] Zero clipped pixels with the widest realistic text

**Verify:** `bash tests/host/test_style_catalog.sh && node tools/param-pages/catalog.mjs --all` -> PASS

**Steps:**

- [ ] **Step 1: Write `enum_square.mjs`**

| Pos | id | Treatment |
|---|---|---|
| 1 | `thin-frame` | Current 1px frame, tighter padding |
| 2 | `no-frame` | Text only, no frame |
| 3 | `underline` | Text with a 1px rule beneath instead of a box |
| 4 | `bracket` | Corner brackets only, no full frame |
| 5 | `inverted` | Filled ground, text knocked out |
| 6 | `heavy-frame` | 2px frame, notched corners |
| 7 | `dotted-frame` | 50% dotted frame, notched corners |
| 8 | `tab` | Frame open at the bottom, reading as a tab |
| 9 | `dither-ground` | DIAG_LIGHT ground behind text, no frame |
| 10 | `slab` | Text on a solid slab wider than the box, notched corners |

**Corner notches are applied wherever an option has a filled or framed box**,
per spec §1.3 — they are a convergent 1-bit idiom being kept, not a variable
under test. No option exists purely to add or remove them.

- [ ] **Step 2: Write `label_cell.mjs`**

The touched-value state is the inverted white strip, which is one of the more Elektron-reading elements. Options must cover both the resting label and the touched value.

| Pos | id | Treatment |
|---|---|---|
| 1 | `thin-strip` | Current, 1px shorter strip |
| 2 | `underline-value` | Value underlined rather than inverted |
| 3 | `boxed-value` | Value in a 1px box, not inverted |
| 4 | `bracket-value` | Value flanked by corner brackets |
| 5 | `dotted-strip` | Strip ground at CHECKER, value in black |
| 6 | `double-strip` | Inverted strip with a 1px black inset border |
| 7 | `half-strip` | Strip only as wide as the value |
| 8 | `tethered` | Value strip with a 5-3-1 triangle pointing at its widget |
| 9 | `caret` | Small caret glyph replaces inversion entirely |
| 10 | `swap` | Label and value swap position rather than the cell inverting |

- [ ] **Step 3: Record the cross-set constraint**

Each option's `note` must state the narrowest font variant it survives. Measure rather than guess:

```bash
node -e 'Promise.all([
  import("./src/shared/param_pages/styles/font/index.mjs"),
  import("./src/shared/param_pages/styles/index.mjs"),
]).then(([, S]) => {
  const f = S.SETS.find(s => s.kind === "font");
  for (const o of f.options) {
    const adv = (t) => [...t].reduce((a, c) => a + (o.glyphs[0] ? 0 : 0), 0);
    console.log(o.id, "AMPLITUDE advance:", adv("AMPLITUDE"));
  }
})'
```

Replace the stub `adv` with a real advance sum over the variant glyph table, then record the result per option.

- [ ] **Step 4: Run, render, and LOOK**

Run: `bash tests/host/test_style_catalog.sh && node tools/param-pages/catalog.mjs --all`

Open the in-context pages. The label cell sits directly under the widget box with no gutter to spare — `BOX_H` (15) then `LBL_H` (7) — so an option that grows the strip by a pixel eats into the widget above it. Check that specifically.

- [ ] **Step 5: Commit**

```bash
git add src/shared/param_pages/styles/enum_square.mjs src/shared/param_pages/styles/label_cell.mjs src/shared/param_pages/styles/index.mjs
git commit -m "styles: sets 4 and 5 -- enum square and label cell

Authored after the font set so each option could be sized against real
letterform variants rather than against the shipping font it is meant to
replace. Every note records the narrowest variant that option survives,
measured rather than estimated.

The label cell has no gutter above it -- BOX_H then LBL_H, nothing between
-- so an option that grows the strip eats the widget."
```

---

## Task 9: Sets 7-10 — envelope, filter, LFO, sample

**Goal:** The four curve visualisations, varying treatment only, with the inherent-shape constraint mechanically enforced.

**Files:**
- Create: `src/shared/param_pages/styles/viz_envelope.mjs`
- Create: `src/shared/param_pages/styles/viz_filter.mjs`
- Create: `src/shared/param_pages/styles/viz_lfo.mjs`
- Create: `src/shared/param_pages/styles/viz_sample.mjs`
- Modify: `src/shared/param_pages/styles/index.mjs`
- Modify: `tests/host/test_style_catalog.sh`

**Acceptance Criteria:**
- [ ] Ten options each, positions 1..10
- [ ] Every option takes a **precomputed height array** and renders it — no option computes its own curve
- [ ] Test asserts all four sets produce identical curve geometry from an identical height array, differing only in fill
- [ ] All render within `VIZ_ROWS` (13) with zero clipped pixels

**Verify:** `bash tests/host/test_style_catalog.sh` -> includes `PASS: viz options share one curve`

**Steps:**

- [ ] **Step 1: Define the shared treatment vocabulary**

All four sets use the same ten treatments, so a pick is coherent across them:

| Pos | id | Treatment |
|---|---|---|
| 1 | `thin-stroke` | Current polyline, no fill |
| 2 | `no-endpoints` | Stroke with endpoint dots removed |
| 3 | `baseline` | Stroke plus a solid baseline rule |
| 4 | `dotted-baseline` | Stroke plus a 50% dotted baseline |
| 5 | `ghost-fill` | Stroke over a CHECKER fill to the baseline |
| 6 | `light-fill` | Stroke over a DIAG_LIGHT fill |
| 7 | `hatch-fill` | Stroke over a DIAG_THIRD fill |
| 8 | `terrain` | Solid crest over DIAG_THIRD, filled to the bottom edge (vimana) |
| 9 | `solid-mass` | Solid fill to the bottom, no separate crest |
| 10 | `outline-only` | Filled silhouette outlined, interior cleared |

- [ ] **Step 2: Write the four modules with a shared implementation**

Because the ten treatments are identical across sets, write them once:

```js
/* viz_treatments.mjs — the ten treatments, shared by sets 7-10.
 *
 * Each takes a PRECOMPUTED height array. No treatment computes a curve: the
 * shape is the maths and is not this issue's to change. Only how the shape is
 * drawn varies, which is what makes a filter option incapable of
 * misrepresenting the filter. */
export function makeTreatments() { /* returns the ten {id, name, position, note, draw} */ }
```

Then each set module imports it and registers with its own `id`, `title` and probe.

- [ ] **Step 3: Add the shared-curve assertion**

In `tests/host/test_style_catalog.sh`:

```js
  /* ---- viz options render the SAME curve, differing only in fill ---- */
  const VIZ = ["viz-envelope", "viz-filter", "viz-lfo", "viz-sample"];
  const heights = Array.from({ length: 100 }, (_, i) => 0.5 + 0.4 * Math.sin(i / 8));
  const crestOf = (set, opt) => {
    const fb = H.createFramebuffer(100, 13);
    const ctx = H.drawContext(fb);
    set.probe(ctx, opt.draw, heights);
    /* topmost lit pixel per column -- the curve, independent of any fill */
    const top = [];
    for (let x = 0; x < 100; x++) {
      let t = -1;
      for (let y = 0; y < 13; y++) if (fb.pixels[y * fb.width + x]) { t = y; break; }
      top.push(t);
    }
    return top.join(",");
  };
  const present = VIZ.map((id) => S.setById(id)).filter(Boolean);
  if (present.length > 1) {
    for (const pos of [1, 5, 8]) {
      const sigs = present.map((s) => crestOf(s, s.options.find((o) => o.position === pos)));
      if (new Set(sigs).size !== 1)
        fail("viz sets disagree on the curve at position " + pos + " -- a treatment is changing the shape");
    }
    console.log("PASS: viz options share one curve");
  }
```

- [ ] **Step 4: Run, render, and LOOK**

Run: `bash tests/host/test_style_catalog.sh && node tools/param-pages/catalog.mjs --all`

Open the contact sheets. Check that `terrain` and `solid-mass` do not make the 13-row band read as a solid block at high values — that is the failure mode for a fill in a short band.

- [ ] **Step 5: Commit**

```bash
git add src/shared/param_pages/styles/viz_*.mjs src/shared/param_pages/styles/index.mjs tests/host/test_style_catalog.sh
git commit -m "styles: sets 7-10 -- envelope, filter, LFO, sample treatments

Ten treatments shared across all four sets, so a pick is coherent between
them. Every treatment takes a precomputed height array and none computes a
curve: the shape is a picture of the maths and is not this issue to change.

That constraint is asserted, not trusted -- the test compares the topmost
lit pixel per column across all four sets at three axis positions, so a
treatment that bends the curve fails rather than shipping a filter graph
that misrepresents the filter."
```

---

## Task 10: Set 13 — value-change motion

**Goal:** Ten motion behaviours rendered as frame strips, since the render layer is stateless and cannot animate live.

**Files:**
- Create: `src/shared/param_pages/styles/anim.mjs`
- Modify: `src/shared/param_pages/styles/index.mjs`
- Modify: `tools/param-pages/catalog.mjs` (frame-strip rendering)

**Acceptance Criteria:**
- [ ] Ten options with `behaviour` prose and `frames(from, to, n)` returning `n` states
- [ ] `frames` is pure and deterministic — same inputs give the same output
- [ ] Every option starts at `from` and ends within 0.01 of `to`
- [ ] Catalog renders each as a horizontal strip of 12 frames
- [ ] Test asserts the start/end property

**Verify:** `bash tests/host/test_style_catalog.sh` -> includes `PASS: motion options settle`

**Steps:**

- [ ] **Step 1: Write the ten motion specs**

| Pos | id | Behaviour |
|---|---|---|
| 1 | `instant` | No motion — current behaviour, the control |
| 2 | `ease-2` | 2-frame linear catch-up |
| 3 | `ease-4` | 4-frame ease-out |
| 4 | `ease-8` | 8-frame ease-out, visibly lagging |
| 5 | `overshoot` | Ease-out past the target, settling back |
| 6 | `spring` | Damped oscillation, `1 - exp(-t*d) * cos(t*f)` |
| 7 | `flash` | Value snaps; the cell inverts for 2 frames |
| 8 | `trail` | Value snaps; the previous position decays over 4 frames |
| 9 | `dissolve` | Previous position fades Solid -> Checker -> DiagLight -> gone |
| 10 | `tick` | Value snaps in quantised steps regardless of turn speed |

Each as:

```js
{ id: "spring", name: "Spring", position: 6,
  behaviour: "Damped oscillation. Overshoots by ~8% and settles in about 10 frames. Reads as physical rather than as a transition.",
  frames: (from, to, n) => Array.from({ length: n }, (_, i) => {
      const t = i / (n - 1);
      if (i === n - 1) return to;
      const d = 5, f = 12;
      const e = 1 - Math.exp(-t * d) * Math.cos(t * f);
      return from + (to - from) * e;
  }),
}
```

The `i === n - 1` clamp is load-bearing: a spring that never exactly reaches its target leaves the last frame off by a fraction, which the settle assertion catches and which would show on the device as a value that never quite lands.

- [ ] **Step 2: Add frame-strip rendering to the catalog tool**

In `tools/param-pages/catalog.mjs`, add before `renderSet`:

```js
/** A motion option as a horizontal strip of N widget renders. */
function renderStrip(opt, frames = 12) {
    const cellW = RM.CELL_W, cellH = RM.BOX_H;
    const { fb, ctx } = surface(cellW * frames, cellH);
    const vs = opt.frames(0.15, 0.85, frames);
    for (let i = 0; i < frames; i++) {
        const kx = i * cellW + Math.floor((cellW - RM.KW) / 2);
        RM.drawArcKnob(ctx, kx, 0, vs[i]);
    }
    return fb;
}
```

and branch on `set.kind === S.KIND_MOTION` inside `renderSet` to call it.

- [ ] **Step 3: Add the settle assertion**

```js
  const motion = S.SETS.find((s) => s.kind === S.KIND_MOTION);
  if (motion) {
    for (const o of motion.options) {
      const f = o.frames(0.2, 0.8, 12);
      if (f.length !== 12) fail(o.id + ": frames(0.2, 0.8, 12) gave " + f.length + " frames");
      if (Math.abs(f[0] - 0.2) > 1e-9) fail(o.id + ": does not start at from");
      if (Math.abs(f[11] - 0.8) > 0.01) fail(o.id + ": settles at " + f[11] + ", want 0.8");
      const again = o.frames(0.2, 0.8, 12);
      if (JSON.stringify(f) !== JSON.stringify(again)) fail(o.id + ": frames is not deterministic");
    }
    console.log("PASS: motion options settle");
  }
```

- [ ] **Step 4: Run, render, and LOOK**

Run: `bash tests/host/test_style_catalog.sh && node tools/param-pages/catalog.mjs --set anim`

Open the strips. Motion judged from a strip is imperfect — that limitation is stated in the spec and is why implementing real motion is a follow-up rather than part of this issue.

- [ ] **Step 5: Commit**

```bash
git add src/shared/param_pages/styles/anim.mjs src/shared/param_pages/styles/index.mjs tools/param-pages/catalog.mjs
git commit -m "styles: set 13 -- ten value-change motion behaviours

The render layer is stateless -- every frame is a snapshot with no
per-widget store to ease a value through -- so each option is a behaviour
spec plus a rendered frame strip rather than live motion. Giving the
renderer frame state is an architectural change and belongs to the
implementation follow-up.

Every option is asserted to start at from, settle within 0.01 of to, and be
deterministic. The spring needs an explicit final-frame clamp: an
oscillator that only approaches its target leaves a value that never quite
lands, which is invisible in a strip and obvious on hardware."
```

---

## Task 11: A/B comparator server

**Goal:** A dependency-free local server serving pairwise comparisons and appending each judgement to disk as it lands.

**Files:**
- Create: `tools/param-pages/ab_server.mjs`

**Acceptance Criteria:**
- [ ] `node tools/param-pages/ab_server.mjs` serves on `127.0.0.1:7788` with no dependencies
- [ ] Serves in-context PNGs from `catalog-out/` for the two options under comparison
- [ ] Keyboard: left arrow picks left, right arrow picks right, space skips
- [ ] Each judgement appends one JSON line to `catalog-out/preferences.json` immediately
- [ ] Pair selection weights toward options with the fewest judgements so far
- [ ] Serves only sets that are registered and rendered; reports which are missing
- [ ] Progress shown as judgements-so-far against the ~16-per-set target

**Verify:** Start the server, open `http://127.0.0.1:7788`, make three judgements, then `wc -l catalog-out/preferences.json` -> 3

**Steps:**

- [ ] **Step 1: Write the server**

Create `tools/param-pages/ab_server.mjs` using only `node:http`, `node:fs`, `node:path`. Structure:

- `GET /` — the comparator page (inline HTML/CSS/JS, no external requests)
- `GET /api/pair?set=<id>` — returns `{ set, a: {id, name, note, img}, b: {...} }`
- `GET /img/<set>/<file>.png` — serves from `catalog-out/`
- `POST /api/judge` — body `{ set, a, b, winner }` where `winner` is `a`, `b` or `skip`; appends a JSONL line and returns the next pair
- `GET /api/progress` — per-set judgement counts

Judgement line format:

```json
{"ts":"2026-08-26T12:00:00.000Z","set":"knob","a":"thin-arc","b":"dotted-track","winner":"b"}
```

Pair selection: pick the set with the fewest judgements relative to target, then within it pick the two options with the fewest appearances, breaking ties randomly. This spends time where it resolves the most rather than uniformly.

**Do not use `Math.random()` as the only mechanism** — a purely random pairing over 16 draws leaves some options unseen. Weight by appearance count first, randomise only among ties.

- [ ] **Step 2: Write the comparator page**

Two in-context PNGs side by side at 4x, option names hidden until after the judgement (so the name and note cannot bias the choice), keyboard-driven, with a progress line. Show the axis position only in the post-judgement reveal.

- [ ] **Step 3: Verify persistence is immediate**

Run the server in the background, make three judgements, then:

Run: `wc -l < catalog-out/preferences.json`
Expected: `3`

Kill the server mid-session and restart it. Expected: previous judgements still counted in `/api/progress`. Appending per judgement rather than on shutdown is the point — a crash must not lose the session.

- [ ] **Step 4: Commit**

```bash
git add tools/param-pages/ab_server.mjs
git commit -m "tools: pairwise A/B comparator, local, no deps

Each judgement is appended to disk as it lands rather than flushed at the
end, so killing the server mid-session loses nothing.

Option names and notes are hidden until after the choice. The axis position
is an authored hypothesis and showing it beforehand would bias the data
meant to test it.

Pairing weights toward options with the fewest appearances rather than
picking uniformly at random -- over 16 draws, uniform random leaves some
options unseen."
```

---

## Task 12: Bradley-Terry ranking

**Goal:** Turn the judgement log into a per-set ranking with confidence, tolerant of partial data.

**Files:**
- Create: `tools/param-pages/rank.mjs`
- Modify: `tests/host/test_style_catalog.sh`

**Acceptance Criteria:**
- [ ] `node tools/param-pages/rank.mjs` prints a ranked table per set
- [ ] Fits Bradley-Terry strengths by iterative MM, converging or stopping at 500 iterations
- [ ] Skips are counted and reported, not fed into the fit
- [ ] Reports a confidence interval per option
- [ ] Handles a set with zero judgements without crashing
- [ ] Reports where the fitted ranking disagrees with the authored axis order
- [ ] Unit test on a synthetic log with a known answer

**Verify:** `bash tests/host/test_style_catalog.sh` -> includes `PASS: bradley-terry recovers a known ranking`

**Steps:**

- [ ] **Step 1: Write the fit**

Standard MM update: with `w_i` wins for option `i` and `n_ij` comparisons between `i` and `j`,

```
p_i <- w_i / sum_j ( n_ij / (p_i + p_j) )
```

normalised each iteration, iterated to convergence. Add a small prior (one half-win against a phantom average opponent) so an option that lost every comparison gets a finite strength rather than zero.

Report strengths on a log scale so differences are readable, plus an approximate interval from the inverse Fisher information.

- [ ] **Step 2: Report axis disagreement**

The whole point of collecting data is that the authored minimal -> radical order might be wrong. Print, per set, the Spearman correlation between authored position and fitted rank, and list the options that moved more than three places. That disagreement is a finding and belongs in the output rather than being left for someone to notice.

- [ ] **Step 3: Add the synthetic-recovery test**

```js
  /* ---- Bradley-Terry recovers a ranking it was given ---- */
  const R = await import("./tools/param-pages/rank.mjs");
  const log = [];
  /* Option k beats option j whenever k > j, with 10% noise. Deterministic
   * pseudo-random so the test cannot flake. */
  let seed = 12345;
  const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
  for (let i = 0; i < 400; i++) {
    const a = Math.floor(rnd() * 5), b = Math.floor(rnd() * 5);
    if (a === b) continue;
    const better = a > b ? "a" : "b";
    const worse = better === "a" ? "b" : "a";
    log.push({ set: "t", a: "o" + a, b: "o" + b, winner: rnd() < 0.9 ? better : worse });
  }
  const fitted = R.fit(log.filter((l) => l.set === "t"));
  const order = fitted.map((f) => f.id);
  if (order[0] !== "o4" || order[order.length - 1] !== "o0")
    fail("bradley-terry did not recover the planted order, got " + order.join(" > "));
  console.log("PASS: bradley-terry recovers a known ranking");
```

- [ ] **Step 4: Run**

Run: `bash tests/host/test_style_catalog.sh`
Expected: includes `PASS: bradley-terry recovers a known ranking`

- [ ] **Step 5: Commit**

```bash
git add tools/param-pages/rank.mjs tests/host/test_style_catalog.sh
git commit -m "tools: Bradley-Terry ranking over the judgement log

Skips are counted and reported but not fed into the fit -- a skip means the
pair was indistinguishable, which is information about the SET rather than
evidence for either option.

A half-win prior against a phantom opponent keeps an option that lost every
comparison at a finite strength instead of zero.

Reports where the fitted order disagrees with the authored minimal ->
radical axis. The axis is a hypothesis and the disagreement is the finding,
so it goes in the output rather than waiting to be noticed."
```

---

## Task 13: Run the A/B session and commit the dataset

**Goal:** Collect ~200 judgements, fit the ranking, and write the results into the spec.

**Files:**
- Create: `docs/superpowers/specs/2026-08-26-ui-differentiation-preferences.json`
- Modify: `docs/superpowers/specs/2026-08-26-ui-differentiation-catalog-design.md`
- Create: `docs/superpowers/specs/contact-sheets/<set>.png` (13 files)

**Acceptance Criteria:**
- [ ] All 13 sets rendered, contact sheets copied into the spec directory
- [ ] Judgement log has at least 10 judgements per set
- [ ] `rank.mjs` output pasted into the spec, one ranked table per set
- [ ] Axis-disagreement findings written up in prose
- [ ] Dataset committed

**Verify:** `node tools/param-pages/rank.mjs` -> a ranked table for all 13 sets with no set reporting zero judgements

**Steps:**

- [ ] **Step 1: Render everything**

Run: `node tools/param-pages/catalog.mjs --all`
Expected: 13 lines, one per set.

- [ ] **Step 2: Hand the session to the user**

Start the server:

```bash
node tools/param-pages/ab_server.mjs
```

Tell the user the URL and the controls, and that judgements save as they go so they can stop and resume freely. **Do not make the judgements yourself** — the dataset is a record of the user's preferences and inventing it would make the entire exercise worthless.

- [ ] **Step 3: Fit and report**

Run: `node tools/param-pages/rank.mjs`
Expected: 13 ranked tables.

- [ ] **Step 4: Write results into the spec**

Add a section per set: contact sheet, ranked table, the top two options with their notes, and any axis disagreement. State the number of judgements and skips for each so a thin set is visible as thin rather than presented with false confidence.

- [ ] **Step 5: Copy the dataset and sheets into the spec directory**

```bash
mkdir -p docs/superpowers/specs/contact-sheets
cp catalog-out/*/contact-sheet.png docs/superpowers/specs/contact-sheets/
cp catalog-out/preferences.json docs/superpowers/specs/2026-08-26-ui-differentiation-preferences.json
```

Rename each sheet to its set id during the copy.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/specs/
git commit -m "spec: catalog results -- 13 ranked sets and the preference dataset

Judgement and skip counts are reported per set so a thin set reads as thin
rather than being presented with false confidence.

Where the fitted ranking disagrees with the authored minimal -> radical
axis, the disagreement is written up rather than smoothed over: the axis
was a hypothesis and the data is what tests it."
```

---

## Task 14: Open the PR

**Goal:** Get the catalog and dataset onto a reviewable PR against main.

**Files:** none

**Acceptance Criteria:**
- [ ] All three CI checks green (`host-tests`, `go`, `cross-compile`)
- [ ] PR body summarises the assessment, links the spec, and states the font finding
- [ ] PR states plainly that nothing ships and picks are a follow-up

**Verify:** `gh pr checks` -> all green

**Steps:**

- [ ] **Step 1: Run the full host suite**

Run: `make -C tests/host test && for t in tests/host/*.sh; do bash "$t" || echo "FAILED: $t"; done`
Expected: no `FAILED:` lines.

Note: `rg` is a shell function in this environment that swallows output, so tests using it can fail locally on any branch. If a test fails, check whether it also fails on `main` before treating it as a regression.

- [ ] **Step 2: Push and open the PR**

```bash
git push -u origin charlesv/sch-50-differentiate-the-ui-and-icons-from-elektron
```

PR body must lead with the font finding — it is the part of the assessment most likely to change what someone does next, and it was not in the issue.

- [ ] **Step 3: Wait for CI**

Run: `gh pr checks --watch`
Expected: `host-tests`, `go`, `cross-compile` all pass.

---

## Deferred decision

**Whether to implement any picked option** is deliberately not in this plan. The spec scopes SCH-50 to the catalog and dataset, and the user chose that scope explicitly ("Rendered option catalog to choose from"). Implementation needs the ranking to exist first, and for the animation set it needs an architectural change (renderer frame state) that should be its own issue.

Raise it after Task 13, when there is data to act on.
