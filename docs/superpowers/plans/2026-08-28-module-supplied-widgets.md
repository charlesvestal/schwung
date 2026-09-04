# Module-Supplied Widgets Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a module draw its own art inside a knob cell, via one optional `drawCell` hook on the `canvas.js` contract that already exists, with a build-time PNG→RLE generator so a C-only author never writes JavaScript.

**Architecture:** A custom widget is a canvas overlay's draw hook given a *frame* instead of the screen. `chain_params` declares `viz: { kind: "custom:<name>" }`; `resolveViz` claims those keys only when the name is registered, so an unknown or disabled name falls through to the existing detector and the built-in widget draws. Drawing goes through a frame-local context that cannot express a coordinate outside its rect, and cannot read a param at all.

**Tech Stack:** QuickJS (device) / node (tests). Pure `.mjs` under `src/shared/param_pages/`, shell+node tests under `tests/host/`, generator under `tools/param-pages/`.

**Spec:** `docs/superpowers/specs/2026-08-28-module-supplied-widgets-design.md`

**User decisions (already made):**
- Fidelity: "Lookup + composition, code above" — sprite data tier for lookup/placement, pure draw function for computed shapes; both at the same call site.
- Containment: "One-strike disable, fall back to built-in" — first throw disables for the session, page still shows a correct widget.
- Claim size: group and row only. **Page-claim reversed** after `type: "canvas"` was found — "whole page" is already spelled `type: "canvas"`.
- Frame: "The knob box; Schwung keeps the label" — widget owns art only, `drawLabelCell` still draws every label.
- "this cant be direct pixels, it should be within a target frame for the knob" — the ctx has no absolute-coordinate escape hatch.
- Naming / `ui_chain.js` / generator location: "your call" → resolved in spec §10.
- **Not for the next release.** Plan only; do not schedule.

---

## Why the ordering

Task 1 is independent of the feature and shippable alone — it fixes two live defects in shipped `canvas` code. Tasks 2–5 build the seam bottom-up, reaching end-to-end (a real module drawing real art) at Task 5. Task 6 is the test that justifies the whole design. Tasks 7–8 add the no-JS path. Task 9 closes docs.

**A note that governs every task:** nothing here runs on the SPI callback. All of it is `shadow_ui` (SCHED_OTHER) or pure library code. The realtime rules in `CLAUDE.md` about module entry points do not apply — but the *draw budget* does: a page render is 1.68 ms and an IPC read is ~2.8 ms.

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `src/shared/param_pages/frame_ctx.mjs` | Frame-local drawing context. Translates, clips, truncates text, counts overflow. No reads. |
| `src/shared/param_pages/widget_registry.mjs` | Registered custom kinds, the disabled set, and `drawCustom` with one-strike containment. |
| `src/shared/param_pages/sprite_rle.mjs` | RLE sprite decode → `fillRect` runs; nominal-frame anchoring and fit refusal. |
| `tools/param-pages/widget_gen.mjs` | PNG + `widgets.toml` → a module's `widgets.mjs`. Build-time only. |
| `tests/host/test_frame_ctx.sh` | Frame ctx: clipping, no absolute escape, `clipped()` counting. |
| `tests/host/test_custom_viz_resolve.sh` | Registered kinds claim; unregistered fall through to the detector. |
| `tests/host/test_widget_one_strike.sh` | A throwing widget is called once; built-in draws thereafter. |
| `tests/host/test_widget_frame_matrix.sh` | The same widget into every rect the two renderers produce. |
| `tests/host/test_sprite_rle.sh` | RLE round-trip and nominal-frame refusal. |
| `tests/host/test_widget_gen_roundtrip.sh` | PNG → RLE → drawn pixels match the source PNG, by content. |

**Modified:**

| File | Change |
|---|---|
| `src/shadow/shadow_ui.js:16517` | `createCanvasRuntimeContext` — remove `getParam`/`getValue` from the cell path; add one-strike to `invokeCanvasOverlayHook`. |
| `src/shared/param_pages/viz.mjs:203` | `collectDeclared` — a `custom:` kind claims its keys only when registered. |
| `src/shared/param_pages/viz_draw.mjs:1717` | `drawVizGroup` — dispatch `custom:` kinds through the registry. |
| `src/shared/param_pages/validate_contract.mjs` | Validate `custom:` declarations. |
| `docs/MODULES.md`, `docs/PARAM_PAGES.md`, `CLAUDE.md` | Author docs + one index bullet. |

---

## Task 1: Fix the two live `canvas` defects

**Goal:** A canvas overlay can no longer perform an IPC read per frame on the draw path, and a throwing overlay is disabled after one throw instead of throwing forever.

**Why first:** Both are pre-existing defects in shipped code and neither depends on this feature. Per spec §6 they may ship ahead of everything else.

**Files:**
- Modify: `src/shadow/shadow_ui.js` — `createCanvasRuntimeContext` (`:16517`), `invokeCanvasOverlayHook` (`:16566`)
- Test: `tests/host/test_canvas_overlay_containment.sh`

**Acceptance Criteria:**
- [ ] `invokeCanvasOverlayHook` disables the overlay after its first throw; the hook is not invoked again for that runtime.
- [ ] The disable is recorded on `canvasRuntime` and surfaced in the existing error path (`canvasRuntime.error`), not silently swallowed.
- [ ] **A hook that threw returns `false`, not `true`** — the current code reports success after catching.
- [ ] `ctx.getParam` / `ctx.setParam` / `ctx.getValue` / `ctx.setValue` remain available to `onOpen` / `onMidi` / `onClose` / `onExit`, and are **absent** during `draw` and `tick`.
- [ ] A doc comment at the ctx names the ~2.8 ms cost and why the draw path is excluded.

**Verify:** `bash tests/host/test_canvas_overlay_containment.sh` → all `PASS`, no `FAIL`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_canvas_overlay_containment.sh`. NO APOSTROPHES inside the node script — it is a single-quoted bash string.

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A THROWING OVERLAY MUST THROW ONCE, AND draw() MUST NOT BE ABLE TO READ.
#
# Both of these were live in shipped code. The try/catch recorded an error and
# never disabled, so a broken overlay burned the frame budget at 60Hz forever.
# And ctx.getParam is a synchronous param round-trip (~2.8ms) that was reachable
# from draw(), where a whole page render costs 1.68ms -- one read cost more than
# redrawing the entire screen.
#
# These are asserted against the SOURCE because the runtime lives inside
# shadow_ui.js, which is not importable as a module. Source pins are weaker than
# unit tests; they are here because the alternative is nothing.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the canvas containment checks" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const src = readFileSync("./src/shadow/shadow_ui.js", "utf8");

ok(/canvasRuntime\.hookDisabled\s*=\s*true/.test(src),
   "a throwing hook sets canvasRuntime.hookDisabled");

ok(/if\s*\(\s*canvasRuntime\.hookDisabled\s*\)\s*return false/.test(src),
   "a disabled runtime refuses further hook invocations");

const ctxFn = src.slice(src.indexOf("function createCanvasRuntimeContext"));
const ctxBody = ctxFn.slice(0, ctxFn.indexOf("\nfunction "));
ok(/DRAW_PATH_HOOKS/.test(ctxBody),
   "the ctx names the set of hooks that get no reads");

ok(/2\.8\s*ms/.test(ctxBody),
   "the ctx doc comment states the read cost that motivates the split");

process.exit(fail ? 1 : 0);
'
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bash tests/host/test_canvas_overlay_containment.sh`
Expected: FAIL lines for all four assertions, exit 1.

- [ ] **Step 3: Add one-strike disable to the hook invoker**

In `src/shadow/shadow_ui.js`, replace the body of the hook invoker at `:16566`. The current body is:

```javascript
function invokeCanvasOverlayHook(hookName, payload) {
    if (!canvasRuntime || !canvasRuntime.overlay) return false;
    const fn = canvasRuntime.overlay[hookName];
    if (typeof fn !== "function") return false;
    try {
        fn(canvasRuntime.ctx, payload || {});
    } catch (e) {
        canvasRuntime.error = `${hookName} error: ${e}`;
        debugLog(`canvas ${hookName} hook error: ${e}`);
    }
    return true;
}
```

**Note the `return true` after the catch** — a hook that threw reports success to its caller. That is a third defect, of the same class as `move_midi_internal_send` returning true on a discarded write (`CLAUDE.md`, Overtake Modules). The replacement below fixes it in passing; do not "restore" that `return true` while merging.

Replace with:

```javascript
function invokeCanvasOverlayHook(hookName, payload) {
    if (!canvasRuntime || !canvasRuntime.overlay) return false;
    /* ONE STRIKE. The old code caught, recorded canvasRuntime.error, logged,
     * and then RETURNED TRUE -- so a throwing overlay threw on every frame,
     * forever, at 60Hz, while telling its caller it had worked. The error was
     * visible in the log and the flood was not. */
    if (canvasRuntime.hookDisabled) return false;
    const fn = canvasRuntime.overlay[hookName];
    if (typeof fn !== "function") return false;
    try {
        fn(canvasHookCtx(hookName), payload || {});
    } catch (e) {
        canvasRuntime.error = `${hookName} error: ${e}`;
        canvasRuntime.hookDisabled = true;
        debugLog(`canvas overlay disabled after throw in ${hookName}: ${e}`);
        return false;
    }
    return true;
}
```

- [ ] **Step 4: Split the ctx so the draw path gets no reads**

Add above `createCanvasRuntimeContext`:

```javascript
/* Hooks that run on the DRAW PATH, where a read is not affordable.
 *
 * ctx.getParam is a synchronous param round-trip: ~2.8ms, against a 1.68ms
 * whole-page render. One read costs more than redrawing the entire screen, so
 * an overlay calling it from draw() halves its own frame rate and everything
 * else drawn that frame. The values an overlay needs are fetched in onOpen and
 * onMidi, which are events, not frames. */
const DRAW_PATH_HOOKS = new Set(["draw", "tick"]);

function canvasHookCtx(hookName) {
    const base = canvasRuntime.ctx;
    if (!DRAW_PATH_HOOKS.has(hookName)) return base;
    if (!canvasRuntime.drawCtx) {
        const { getParam, setParam, getValue, setValue, ...rest } = base;
        canvasRuntime.drawCtx = rest;
    }
    return canvasRuntime.drawCtx;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `bash tests/host/test_canvas_overlay_containment.sh`
Expected: four `PASS` lines, exit 0.

- [ ] **Step 6: Confirm no existing test regressed**

Run: `for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done`
Expected: no output.

- [ ] **Step 7: Commit**

```bash
git add src/shadow/shadow_ui.js tests/host/test_canvas_overlay_containment.sh
git commit -m "fix: a canvas overlay throws once, and draw() cannot read a param

The try/catch at shadow_ui.js:16573 recorded canvasRuntime.error and returned,
so a throwing overlay threw every frame at 60Hz forever -- the error was
visible and the flood was not. One strike now disables the runtime.

And ctx.getParam was reachable from draw(): a synchronous param round-trip of
~2.8ms against a 1.68ms whole-page render, so one read cost more than redrawing
the entire screen. draw and tick now receive a ctx with the four read/write
accessors removed; onOpen/onMidi/onClose/onExit keep them, because those are
events rather than frames."
```

---

## Task 2: `frame_ctx.mjs` — a context that cannot express a screen pixel

**Goal:** A pure drawing context scoped to one rect: `(0,0)` is the frame's top-left, everything is clipped to it, text is truncated rather than overflowed, and attempted overflow is *counted* so tests can assert zero.

**Files:**
- Create: `src/shared/param_pages/frame_ctx.mjs`
- Test: `tests/host/test_frame_ctx.sh`

**Acceptance Criteria:**
- [ ] `frameCtx(ctx, {x,y,w,h})` returns an object exposing `width`, `height`, `fillRect`, `print`, `textWidth`, `clipped()`.
- [ ] A `fillRect` fully inside the frame reaches the parent translated by `x,y`.
- [ ] A `fillRect` partly outside is clipped to the frame and increments `clipped()`.
- [ ] A `fillRect` fully outside draws nothing and increments `clipped()`.
- [ ] `print` truncates to fit the frame width using `textWidth`, never overflowing.
- [ ] The returned object has **no** property whose value is the parent ctx, and no `getParam`.

**Verify:** `bash tests/host/test_frame_ctx.sh` → all `PASS`, no `FAIL`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_frame_ctx.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A WIDGET DRAWS INTO A FRAME AND CANNOT SAY "SCREEN".
#
# The rect a widget receives is unstable three ways: render_page cellW is
# floor(rect.w/COLS) and its rowH is dynamic (computeGeom picks the whole render
# mode from that height); movy is a fixed 32x15 whose own comment warns 15 is
# only right because both grid gaps happen to be 15px; and a span near the right
# edge is clamped by Math.min(g.slotSpan, COLS - col).
#
# So clipping is not a safety net here, it is the coordinate system. clipped()
# exists so a widget that TRIES to overflow is a red test rather than something
# quietly absorbed -- the same bargain as test_master_fx_diagram_fit.sh.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the frame ctx tests" >&2
  exit 1
fi

node --input-type=module -e '
import { frameCtx } from "./src/shared/param_pages/frame_ctx.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const recorder = () => {
  const calls = [];
  return {
    calls,
    fillRect(x, y, w, h, c) { calls.push([x, y, w, h, c]); },
    print(x, y, t, c) { calls.push(["print", x, y, t, c]); },
    textWidth(t) { return String(t).length * 4; },
  };
};

/* Inside the frame: translated, not clipped. */
let p = recorder();
let f = frameCtx(p, { x: 10, y: 20, w: 16, h: 15 });
f.fillRect(2, 3, 4, 5, 1);
ok(JSON.stringify(p.calls[0]) === JSON.stringify([12, 23, 4, 5, 1]),
   "an in-frame fillRect is translated by the frame origin");
ok(f.clipped() === 0, "an in-frame fillRect does not count as clipped");

/* Frame dimensions are the frames, not the screens. */
ok(f.width === 16 && f.height === 15, "width and height are the frame dimensions");

/* Overhanging: clipped to the frame, and counted. */
p = recorder();
f = frameCtx(p, { x: 10, y: 20, w: 16, h: 15 });
f.fillRect(12, 0, 40, 4, 1);
ok(JSON.stringify(p.calls[0]) === JSON.stringify([22, 20, 4, 4, 1]),
   "an overhanging fillRect is clipped to the frame width");
ok(f.clipped() === 1, "an overhanging fillRect increments clipped()");

/* Fully outside: nothing reaches the parent. */
p = recorder();
f = frameCtx(p, { x: 10, y: 20, w: 16, h: 15 });
f.fillRect(100, 100, 4, 4, 1);
ok(p.calls.length === 0, "a fully-outside fillRect draws nothing");
ok(f.clipped() === 1, "a fully-outside fillRect increments clipped()");

/* Negative coordinates cannot reach above or left of the frame. */
p = recorder();
f = frameCtx(p, { x: 10, y: 20, w: 16, h: 15 });
f.fillRect(-8, -8, 12, 12, 1);
ok(JSON.stringify(p.calls[0]) === JSON.stringify([10, 20, 4, 4, 1]),
   "negative coordinates clamp to the frame origin, never above or left of it");

/* Text truncates rather than overflowing. */
p = recorder();
f = frameCtx(p, { x: 0, y: 0, w: 16, h: 15 });
f.print(0, 0, "ABCDEFGH", 1);
const printed = p.calls[0][3];
ok(printed.length === 4, "print truncates to what the frame width fits");
ok(f.clipped() === 1, "a truncated print increments clipped()");

/* No escape hatch, and no reads. */
f = frameCtx(recorder(), { x: 0, y: 0, w: 16, h: 15 });
ok(typeof f.getParam === "undefined", "the frame ctx exposes no getParam");
ok(typeof f.setParam === "undefined", "the frame ctx exposes no setParam");
ok(!Object.values(f).some((v) => v && typeof v === "object" && typeof v.fillRect === "function"),
   "no property of the frame ctx is the parent ctx itself");

process.exit(fail ? 1 : 0);
'
```

- [ ] **Step 2: Run to verify it fails**

Run: `bash tests/host/test_frame_ctx.sh`
Expected: FAIL — `Cannot find module .../frame_ctx.mjs`, exit 1.

- [ ] **Step 3: Write `frame_ctx.mjs`**

```javascript
/**
 * frame_ctx.mjs — a drawing context scoped to one frame.
 *
 * PURE, like the rest of this library: it wraps whatever ctx it is handed and
 * draws nothing itself.
 *
 * A CUSTOM WIDGET CANNOT EXPRESS A SCREEN COORDINATE. That is the point of
 * this file, and it is stronger than clipping-as-safety-net: (0,0) is the
 * frame top-left and there is no accessor that reaches absolute space, so
 * drawing outside your frame stops being a rule an author must follow and
 * becomes something they cannot write down.
 *
 * It has to be that strong because the rect is unstable three ways:
 *
 *   render_page.mjs:619   cellW = floor(rect.w / COLS), caller-dependent
 *   render_page.mjs:116   rowH is DYNAMIC, and computeGeom picks the whole
 *                         render mode from it (dial -> shrinking radius ->
 *                         bar-value -> bar-label -> bar-only)
 *   render_page_movy.mjs  a fixed 32x15, whose own comment at :2428 warns 15
 *                         is only right because both grid gaps happen to be 15
 *   render_page.mjs:671   Math.min(g.slotSpan, COLS - col) silently CLAMPS a
 *                         two-slot group near the right edge
 *
 * The same widget can be handed any of those, so pixel coordinates authored
 * against one of them are wrong in the others.
 *
 * NO READS. The context carries no getParam and holds no reference to anything
 * that has one. PARAM_PAGES.md forbids a read on the draw path (~2.8ms, against
 * a 1.68ms whole-page render); here that is enforced by construction rather
 * than by review, because values arrive as an argument.
 *
 * clipped() COUNTS ATTEMPTED OVERFLOW rather than hiding it. A fixed-width row
 * cannot report that it overflowed, which is exactly how nine Master FX boxes
 * came to be drawn 86px off-screen with no error
 * (tests/host/test_master_fx_diagram_fit.sh). A widget that tries to leave its
 * frame is a red test, not a silent absorption.
 */

/** Clip a rect given in frame-local coordinates to the frame. */
function clipToFrame(fx, fy, fw, fh, w, h) {
    let x0 = fx, y0 = fy, x1 = fx + fw, y1 = fy + fh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return null;
    return { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
}

/**
 * @param {object} ctx    parent context: { fillRect, print, textWidth }
 * @param {object} frame  { x, y, w, h } in the parent's coordinates
 * @returns {object} a frame-local context
 */
export function frameCtx(ctx, frame) {
    const ox = Math.round(frame.x), oy = Math.round(frame.y);
    const w = Math.max(0, Math.round(frame.w)), h = Math.max(0, Math.round(frame.h));
    let clipCount = 0;

    return {
        width: w,
        height: h,

        fillRect(x, y, rw, rh, color) {
            const fx = Math.round(x), fy = Math.round(y);
            const fw = Math.round(rw), fh = Math.round(rh);
            const r = clipToFrame(fx, fy, fw, fh, w, h);
            if (!r) { clipCount++; return; }
            if (r.x !== fx || r.y !== fy || r.w !== fw || r.h !== fh) clipCount++;
            ctx.fillRect(ox + r.x, oy + r.y, r.w, r.h, color);
        },

        /* Text cannot be partially clipped with fillRect/print alone, so it is
         * TRUNCATED to what fits -- the same choice labelForCell already makes.
         * A half-drawn glyph run reads as a broken renderer; a short label
         * reads as a short label. */
        print(x, y, text, color) {
            const fx = Math.round(x), fy = Math.round(y);
            if (fx < 0 || fy < 0 || fx >= w || fy >= h) { clipCount++; return; }
            let s = String(text);
            const budget = w - fx;
            if (ctx.textWidth(s) > budget) {
                clipCount++;
                while (s.length > 0 && ctx.textWidth(s) > budget) s = s.slice(0, -1);
                if (!s) return;
            }
            ctx.print(ox + fx, oy + fy, s, color);
        },

        /* Measurement, not drawing -- no translation to do. */
        textWidth(text) { return ctx.textWidth(text); },

        /** How many draw calls were clipped, truncated or dropped. */
        clipped() { return clipCount; },
    };
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `bash tests/host/test_frame_ctx.sh`
Expected: 13 `PASS` lines, exit 0.

- [ ] **Step 5: Prove the test can fail**

Temporarily change `if (x1 > w) x1 = w;` to `if (x1 > w + 100) x1 = w;`, re-run, confirm the clipping assertions FAIL, then revert. A probe that cannot fail reports green for the wrong reason.

Run: `bash tests/host/test_frame_ctx.sh`
Expected while mutated: FAIL on "an overhanging fillRect is clipped to the frame width".

- [ ] **Step 6: Commit**

```bash
git add src/shared/param_pages/frame_ctx.mjs tests/host/test_frame_ctx.sh
git commit -m "feat: frame-local drawing context for module-supplied widgets

A custom widget draws into a frame and cannot express a screen coordinate.
Stronger than clipping-as-safety-net, because the rect is unstable three ways:
render_page cellW is caller-dependent and rowH is dynamic (computeGeom picks
the render mode from it), movy is a fixed 32x15, and a span near the right edge
is silently clamped.

No getParam and no reference to anything holding one, so PARAM_PAGES.md rule
that nothing reads on the draw path is enforced by construction.

clipped() counts attempted overflow rather than absorbing it, the same bargain
as test_master_fx_diagram_fit.sh -- a fixed row cannot report that it
overflowed, which is how nine Master FX boxes were drawn off-screen silently."
```

---

## Task 3: Custom `viz` kinds resolve, and unknown ones fall through

**Goal:** `viz: { kind: "custom:<name>" }` is recognised in `chain_params`, claims its keys **only when the name is registered**, and otherwise leaves them to the detector.

**Why this shape:** `collectDeclared` claims keys as it walks. If an unregistered custom kind simply does not claim, the keys stay in the detector pool and a built-in widget draws. One mechanism then covers all four failure cases — a typo, a widget whose file failed to load, an **older host reading a newer module**, and a widget disabled by one-strike (Task 4 adds its name to the same unregistered set).

**Files:**
- Create: `src/shared/param_pages/widget_registry.mjs` (registry half only; drawing added in Task 4)
- Modify: `src/shared/param_pages/viz.mjs` — `collectDeclared` (`:170`), `resolveViz` (`:881`)
- Test: `tests/host/test_custom_viz_resolve.sh`

**Acceptance Criteria:**
- [ ] `isCustomKind("custom:x")` is true; `isCustomKind("envelope")` is false.
- [ ] A registered `custom:` single claims its key and appears in `groups` with `source: "declared"`.
- [ ] A registered `custom:` group with `group`/`role` spans its adjacent run, exactly as a built-in group does.
- [ ] An **unregistered** `custom:` kind claims nothing — its keys reach the detector, and a detectable set still produces its built-in group.
- [ ] No built-in kind name starts with `custom:` (reserved-prefix invariant).

**Verify:** `bash tests/host/test_custom_viz_resolve.sh` → all `PASS`, no `FAIL`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_custom_viz_resolve.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# AN UNKNOWN CUSTOM WIDGET FALLS THROUGH TO THE DETECTOR.
#
# collectDeclared claims keys as it walks, so a custom kind that is not
# registered simply does not claim -- its keys stay in the detector pool and the
# built-in widget draws. That one behaviour covers four cases at once: an author
# typo, a widget whose file failed to load, an OLDER HOST READING A NEWER MODULE,
# and a widget disabled after throwing. They share a code path, so forward
# compatibility cannot rot separately from the typo handling.
#
# The assertion that matters is the DRAWN RESULT -- that the detector group is
# present -- not merely that nothing threw.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the custom viz tests" >&2
  exit 1
fi

node --input-type=module -e '
import { resolveViz, VIZ_ENVELOPE } from "./src/shared/param_pages/viz.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { isCustomKind, registerWidget, clearWidgets }
  from "./src/shared/param_pages/widget_registry.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const idx = (chainParams, keys) => buildMetaIndex({
  hierarchy: { modes: null, levels: { root: {
    label: "T", knobs: keys.filter(Boolean),
    params: chainParams.map((p) => ({ key: p.key })) } } },
  chainParams,
});

ok(isCustomKind("custom:mymeter"), "custom: prefix is recognised");
ok(!isCustomKind("envelope"), "a built-in kind is not a custom kind");
ok(!isCustomKind(null), "a null kind is not a custom kind");

/* A registered single-param custom widget claims its key. */
clearWidgets();
registerWidget("custom:mymeter", { draw: () => {} });
let cp = [{ key: "drive", name: "Drive", type: "float", min: 0, max: 1,
            viz: { kind: "custom:mymeter" } }];
let keys = ["drive", null, null, null, null, null, null, null];
let r = resolveViz({ keys, metaIndex: idx(cp, keys) });
ok(r.groups.length === 1 && r.groups[0].kind === "custom:mymeter",
   "a registered custom kind produces its group");
ok(r.groups[0].source === "declared", "a custom group is a DECLARED group");

/* A registered grouped custom widget spans its run. */
clearWidgets();
registerWidget("custom:xy", { draw: () => {} });
cp = [{ key: "px", name: "X", type: "float", min: 0, max: 1,
        viz: { group: "pad", role: "x", kind: "custom:xy" } },
      { key: "py", name: "Y", type: "float", min: 0, max: 1,
        viz: { group: "pad", role: "y" } }];
keys = ["px", "py", null, null, null, null, null, null];
r = resolveViz({ keys, metaIndex: idx(cp, keys) });
ok(r.groups.length === 1 && r.groups[0].slotSpan === 2,
   "a grouped custom widget spans its adjacent run");

/* UNREGISTERED: claims nothing, and the detector still fires. */
clearWidgets();
cp = [{ key: "attack",  name: "Attack",  type: "float", min: 0, max: 1, viz: { kind: "custom:nope" } },
      { key: "decay",   name: "Decay",   type: "float", min: 0, max: 1 },
      { key: "sustain", name: "Sustain", type: "float", min: 0, max: 1 },
      { key: "release", name: "Release", type: "float", min: 0, max: 1 }];
keys = ["attack", "decay", "sustain", "release", null, null, null, null];
r = resolveViz({ keys, metaIndex: idx(cp, keys) });
ok(!r.groups.some((g) => isCustomKind(g.kind)),
   "an unregistered custom kind produces no custom group");
ok(r.groups.some((g) => g.kind === VIZ_ENVELOPE),
   "the detector still draws its built-in widget over those keys");

process.exit(fail ? 1 : 0);
'
```

- [ ] **Step 2: Run to verify it fails**

Run: `bash tests/host/test_custom_viz_resolve.sh`
Expected: FAIL — `Cannot find module .../widget_registry.mjs`, exit 1.

- [ ] **Step 3: Create the registry (registry half)**

Create `src/shared/param_pages/widget_registry.mjs`:

```javascript
/**
 * widget_registry.mjs — the custom widgets this host knows how to draw.
 *
 * "custom:" IS A RESERVED PREFIX. No built-in viz kind may ever be named into
 * that namespace, so the two sets cannot collide as built-ins are added.
 *
 * REGISTRATION IS WHAT MAKES A CUSTOM KIND REAL. viz.mjs collectDeclared claims
 * a keys cell as it walks; a custom kind that is not registered here simply does
 * not claim, so its keys stay in the detector pool and a built-in widget draws
 * instead. That single behaviour is the whole fall-through story, and it covers
 * four different failures with one code path:
 *
 *   - an author typo in the kind name
 *   - a widget whose canvas.js failed to load
 *   - an OLDER HOST reading a NEWER module -- it has never heard of the name,
 *     so the module still draws something sensible
 *   - a widget disabled after throwing (see drawCustom below)
 *
 * Because they share a path, the forward-compatibility behaviour cannot rot
 * separately from the typo behaviour.
 */

const CUSTOM_PREFIX = "custom:";

/** kind -> { draw, nominal } */
const widgets = new Map();
/** kinds disabled this session after a throw. Never cleared except by reload. */
const disabled = new Set();

export function isCustomKind(kind) {
    return typeof kind === "string" && kind.startsWith(CUSTOM_PREFIX);
}

/**
 * @param {string} kind  full kind string, including the "custom:" prefix
 * @param {object} impl  { draw(ctx, values, meta), nominal?: {w, h} }
 */
export function registerWidget(kind, impl) {
    if (!isCustomKind(kind) || !impl || typeof impl.draw !== "function") return false;
    widgets.set(kind, impl);
    return true;
}

/** Registered AND not disabled. This is the predicate viz.mjs consults. */
export function isWidgetAvailable(kind) {
    return widgets.has(kind) && !disabled.has(kind);
}

export function getWidget(kind) {
    return isWidgetAvailable(kind) ? widgets.get(kind) : null;
}

/** Test seam, and the reset used when a component is unloaded. */
export function clearWidgets() {
    widgets.clear();
    disabled.clear();
}
```

- [ ] **Step 4: Teach `collectDeclared` about custom kinds**

In `src/shared/param_pages/viz.mjs`, add to the imports at the top:

```javascript
import { isCustomKind, isWidgetAvailable } from "./widget_registry.mjs";
```

Then in `collectDeclared`, immediately after `if (!v || typeof v !== "object") return;` (`:180`), insert:

```javascript
        /* A CUSTOM KIND CLAIMS NOTHING UNLESS IT CAN BE DRAWN.
         *
         * Returning here leaves the key in the detector pool, so an unknown or
         * disabled widget degrades to the built-in one rather than to a hole.
         * See widget_registry.mjs -- this one branch is the fall-through story
         * for a typo, a failed load, an older host, and a one-strike disable. */
        if (isCustomKind(v.kind) && !isWidgetAvailable(v.kind)) return;
```

The grouped case needs the same guard, because a group's `kind` may be declared on any member. After the `if (v.group) {` block resolves its kind — in the `for (const g of groups.values())` loop, immediately after `const kind = g.kind || inferKindFromRoles(...)` (`:213`) — insert:

```javascript
        if (isCustomKind(kind) && !isWidgetAvailable(kind)) continue;
```

- [ ] **Step 5: Run to verify it passes**

Run: `bash tests/host/test_custom_viz_resolve.sh`
Expected: 8 `PASS` lines, exit 0.

- [ ] **Step 6: Confirm the fleet snapshot did not move**

No fleet module declares a `custom:` kind, so resolution must be byte-identical.

Run: `bash tests/host/test_param_pages_viz.sh && bash tests/host/test_viz_gather.sh && bash tests/host/test_optional_viz_role.sh`
Expected: all `PASS`, no snapshot diff.

- [ ] **Step 7: Commit**

```bash
git add src/shared/param_pages/widget_registry.mjs src/shared/param_pages/viz.mjs tests/host/test_custom_viz_resolve.sh
git commit -m "feat: custom viz kinds resolve, and unknown ones fall through

viz is an object, not a string, so the namespace goes on the kind sub-field:
viz: { kind: custom:mymeter }, in exactly the shape a built-in is declared.

collectDeclared claims keys as it walks, so an unregistered custom kind simply
does not claim -- its keys stay in the detector pool and the built-in widget
draws. One branch covers four failures: an author typo, a widget whose file
failed to load, an older host reading a newer module, and a widget disabled
after throwing. Sharing a path means forward compatibility cannot rot
separately from the typo handling.

custom: is a reserved prefix so built-ins can never collide with it."
```

---

## Task 4: Dispatch and one-strike containment

**Goal:** `drawVizGroup` routes a `custom:` kind through the registry into a frame-local ctx; a throw disables that kind for the session and the built-in widget takes over from the next resolve.

**Files:**
- Modify: `src/shared/param_pages/widget_registry.mjs` (add `drawCustom`)
- Modify: `src/shared/param_pages/viz_draw.mjs` — `drawVizGroup` (`:1717`)
- Test: `tests/host/test_widget_one_strike.sh`

**Acceptance Criteria:**
- [ ] A registered widget's `draw` is called with a frame-local ctx whose `width`/`height` are the rect's.
- [ ] The widget receives values and meta as arguments; the ctx it is handed has no `getParam`.
- [ ] A widget that throws is invoked **exactly once**; `isWidgetAvailable` is false afterwards.
- [ ] After the disable, `resolveViz` no longer claims those keys, so the detector's built-in group draws.
- [ ] A widget that draws outside its frame produces no out-of-frame parent calls.

**Verify:** `bash tests/host/test_widget_one_strike.sh` → all `PASS`, no `FAIL`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_widget_one_strike.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A THROWING WIDGET GETS ONE STRIKE, AND THE PAGE STAYS CORRECT.
#
# The fallback is not "draw nothing" -- it is the built-in widget the detector
# would have chosen, so a user whose module ships a broken widget sees a working
# page rather than a hole. The author sees it in debug.log.
#
# This mirrors how page_controller handles an unresolved contract: keep
# something correct on screen, never let a failure become a picture.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the one-strike tests" >&2
  exit 1
fi

node --input-type=module -e '
import { drawVizGroup } from "./src/shared/param_pages/viz_draw.mjs";
import { resolveViz, VIZ_ENVELOPE } from "./src/shared/param_pages/viz.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { registerWidget, clearWidgets, isWidgetAvailable }
  from "./src/shared/param_pages/widget_registry.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const recorder = () => {
  const calls = [];
  return { calls,
    fillRect(x, y, w, h, c) { calls.push([x, y, w, h, c]); },
    print() {}, textWidth(t) { return String(t).length * 4; } };
};

const idx = (chainParams, keys) => buildMetaIndex({
  hierarchy: { modes: null, levels: { root: {
    label: "T", knobs: keys.filter(Boolean),
    params: chainParams.map((p) => ({ key: p.key })) } } },
  chainParams,
});

const RECT = { x: 8, y: 9, w: 17, h: 15 };

/* The widget gets a frame-local ctx, sized to the rect, with no reads. */
clearWidgets();
let seen = null;
registerWidget("custom:probe", { draw: (c) => { seen = c; c.fillRect(0, 0, 2, 2, 1); } });
let p = recorder();
drawVizGroup(p, RECT, { kind: "custom:probe", roles: {}, keys: ["a"], slotStart: 0, slotSpan: 1 }, {}, idx([], []));
ok(seen && seen.width === 17 && seen.height === 15,
   "the widget ctx is sized to the frame, not the screen");
ok(seen && typeof seen.getParam === "undefined", "the widget ctx exposes no getParam");
ok(JSON.stringify(p.calls[0]) === JSON.stringify([8, 9, 2, 2, 1]),
   "the widget draws translated into its frame");

/* A widget cannot draw outside its frame. */
clearWidgets();
registerWidget("custom:greedy", { draw: (c) => { c.fillRect(0, 0, 200, 200, 1); } });
p = recorder();
drawVizGroup(p, RECT, { kind: "custom:greedy", roles: {}, keys: ["a"], slotStart: 0, slotSpan: 1 }, {}, idx([], []));
ok(p.calls.every(([x, y, w, h]) => x >= 8 && y >= 9 && x + w <= 25 && y + h <= 24),
   "a greedy widget cannot draw outside its frame");

/* ONE STRIKE. */
clearWidgets();
let calls = 0;
registerWidget("custom:bad", { draw: () => { calls++; throw new Error("boom"); } });
const g = { kind: "custom:bad", roles: {}, keys: ["a"], slotStart: 0, slotSpan: 1 };
drawVizGroup(recorder(), RECT, g, {}, idx([], []));
drawVizGroup(recorder(), RECT, g, {}, idx([], []));
drawVizGroup(recorder(), RECT, g, {}, idx([], []));
ok(calls === 1, "a throwing widget is invoked exactly once");
ok(!isWidgetAvailable("custom:bad"), "a throwing widget is no longer available");

/* And the page falls back to the built-in the detector would have chosen. */
const cp = [{ key: "attack",  name: "Attack",  type: "float", min: 0, max: 1, viz: { kind: "custom:bad" } },
            { key: "decay",   name: "Decay",   type: "float", min: 0, max: 1 },
            { key: "sustain", name: "Sustain", type: "float", min: 0, max: 1 },
            { key: "release", name: "Release", type: "float", min: 0, max: 1 }];
const keys = ["attack", "decay", "sustain", "release", null, null, null, null];
const r = resolveViz({ keys, metaIndex: idx(cp, keys) });
ok(r.groups.some((x) => x.kind === VIZ_ENVELOPE),
   "after the disable the detector built-in draws over those keys");

process.exit(fail ? 1 : 0);
'
```

- [ ] **Step 2: Run to verify it fails**

Run: `bash tests/host/test_widget_one_strike.sh`
Expected: FAIL — `registerWidget` exists but `drawVizGroup` ignores custom kinds, so `seen` is null. exit 1.

- [ ] **Step 3: Add `drawCustom` to the registry**

First add the import at the **top** of `src/shared/param_pages/widget_registry.mjs`, beside the file's other module-level code — ESM hoists imports, so putting it lower would work and read as a mistake:

```javascript
import { frameCtx } from "./frame_ctx.mjs";
```

Then append the rest to the end of the file:

```javascript
/* Injected so this module stays pure and node-testable. On device
 * shadow_ui sets it to debugLog. */
let logFn = null;
export function setWidgetLogger(fn) { logFn = typeof fn === "function" ? fn : null; }

/**
 * Draw a custom group into `rect`, contained.
 *
 * ONE STRIKE. The first throw disables the kind for the session and the page
 * falls back to whatever the detector would have drawn -- so a user whose
 * module ships a broken widget still sees a CORRECT page, just not a custom
 * one, and the author sees the throw in debug.log. This is the same posture
 * page_controller takes on an unresolved contract: keep something correct on
 * screen, never let a failure become a picture.
 *
 * Catching every frame instead would flood the log and burn the frame budget
 * forever; not catching at all would take the shadow UI down for a user who
 * merely installed a module.
 *
 * @returns {boolean} true if the widget drew; false if the caller must fall back
 */
export function drawCustom(ctx, rect, group, values, metaIndex, anim, nowMs, baseValues) {
    const impl = getWidget(group.kind);
    if (!impl) return false;
    const fctx = frameCtx(ctx, rect);
    try {
        impl.draw(fctx, { group, values, metaIndex, anim, nowMs, baseValues });
    } catch (e) {
        disabled.add(group.kind);
        if (logFn) logFn(`widget ${group.kind} disabled after throw: ${e}`);
        return false;
    }
    return true;
}
```

- [ ] **Step 4: Dispatch custom kinds from `drawVizGroup`**

In `src/shared/param_pages/viz_draw.mjs`, add to the imports:

```javascript
import { isCustomKind, drawCustom } from "./widget_registry.mjs";
```

Replace `drawVizGroup` (`:1717`):

```javascript
export function drawVizGroup(ctx, rect, group, values, metaIndex, anim, nowMs, baseValues) {
    /* A custom widget is tried first and may decline -- an unregistered kind,
     * or one disabled by a previous throw. Declining falls through to the DRAW
     * table below, which for a custom kind finds nothing; the page recovers on
     * the next resolveViz, where the now-unavailable kind stops claiming its
     * keys and the detector picks them up. */
    if (isCustomKind(group.kind)) {
        drawCustom(ctx, rect, group, values, metaIndex, anim, nowMs, baseValues);
        return;
    }
    const fn = DRAW[group.kind];
    if (fn) fn(ctx, rect, group, values, metaIndex, anim, nowMs, baseValues);
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `bash tests/host/test_widget_one_strike.sh`
Expected: 7 `PASS` lines, exit 0.

- [ ] **Step 6: Confirm no regression**

Run: `for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done`
Expected: no output.

- [ ] **Step 7: Commit**

```bash
git add src/shared/param_pages/widget_registry.mjs src/shared/param_pages/viz_draw.mjs tests/host/test_widget_one_strike.sh
git commit -m "feat: dispatch custom widgets, one strike and out

drawVizGroup routes a custom: kind through the registry into a frame-local ctx.
A throw disables that kind for the session; the next resolveViz stops claiming
its keys, so the detector built-in takes over and the page stays correct rather
than showing a hole.

Catching every frame would flood the log and burn the frame budget forever; not
catching would take the shadow UI down for a user who merely installed a
module."
```

---

## Task 5: Wire a module's `canvas.js` `drawCell` into the registry

**Goal:** A real module shipping `canvas.js` with a `drawCell` export has its widget registered when the component editor loads it, and unregistered when the component unloads.

**Files:**
- Modify: `src/shadow/shadow_ui.js` — overlay load (`loadCanvasOverlay` region, `:16590`–`:16660`), component teardown (`:16308`)
- Test: `tests/host/test_canvas_drawcell_wiring.sh`

**Acceptance Criteria:**
- [ ] An overlay exporting `drawCell` and `widgetKind` registers under that kind on load.
- [ ] `clearWidgets()` runs on component teardown, so a widget cannot outlive its module.
- [ ] `setWidgetLogger(debugLog)` is installed once at startup.
- [ ] A `drawCell` that is not a function is ignored without throwing.

**Verify:** `bash tests/host/test_canvas_drawcell_wiring.sh` → all `PASS`, no `FAIL`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_canvas_drawcell_wiring.sh`. Because `shadow_ui.js` is not importable, this pins the wiring at source level and the *behaviour* is covered by Task 4's unit tests.

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A WIDGET MUST NOT OUTLIVE ITS MODULE.
#
# The registry is process-global and shadow_ui is long-lived, so a widget
# registered by slot 1s module would otherwise still be registered after that
# module is swapped out -- and a later module declaring the same custom: name
# would silently get the wrong art. Teardown clears it.
#
# Source-level pins, because shadow_ui.js is not importable as a module. Weaker
# than a unit test; the drawing behaviour itself is covered by
# test_widget_one_strike.sh.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the drawCell wiring checks" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const src = readFileSync("./src/shadow/shadow_ui.js", "utf8");

ok(/registerWidget\s*\(/.test(src), "shadow_ui registers a widget from a loaded overlay");
ok(/typeof\s+overlay\.drawCell\s*===\s*"function"/.test(src),
   "a non-function drawCell is ignored rather than registered");
ok(/clearWidgets\s*\(\s*\)/.test(src), "shadow_ui clears the registry on teardown");
ok(/setWidgetLogger\s*\(/.test(src), "shadow_ui installs the widget logger");

const teardown = src.slice(src.indexOf("canvasParamKey = \"\";"));
ok(/clearWidgets/.test(teardown.slice(0, 600)),
   "the clear happens in the canvas teardown block, not somewhere unrelated");

process.exit(fail ? 1 : 0);
'
```

- [ ] **Step 2: Run to verify it fails**

Run: `bash tests/host/test_canvas_drawcell_wiring.sh`
Expected: five FAIL lines, exit 1.

- [ ] **Step 3: Import the registry into `shadow_ui.js`**

Add alongside the other `param_pages` imports near `:249`:

```javascript
import { registerWidget, clearWidgets, setWidgetLogger }
    from "../shared/param_pages/widget_registry.mjs";
```

And once during startup, beside the other one-time setup:

```javascript
setWidgetLogger(debugLog);
```

- [ ] **Step 4: Register on overlay load**

In `loadCanvasOverlay`, after `canvasRuntime.overlay = loaded.overlay;` (`:16623`), add:

```javascript
    /* A canvas overlay may ALSO supply an in-grid widget. Same file, same
     * author mental model, two scales: drawCell paints one knob box, draw
     * paints the fullscreen view you dive into from that same cell. */
    const overlay = canvasRuntime.overlay;
    if (overlay && typeof overlay.drawCell === "function"
        && typeof overlay.widgetKind === "string") {
        registerWidget(overlay.widgetKind, {
            draw: overlay.drawCell.bind(overlay),
            nominal: overlay.widgetNominal || null,
        });
    }
```

- [ ] **Step 5: Clear on teardown**

In the teardown block at `:16308`, after `canvasTickCounter = 0;`, add:

```javascript
    /* The registry is process-global and shadow_ui is long-lived, so a widget
     * left registered would still be live after its module was swapped out --
     * and a later module using the same custom: name would silently inherit
     * the wrong art. */
    clearWidgets();
```

- [ ] **Step 6: Run to verify it passes**

Run: `bash tests/host/test_canvas_drawcell_wiring.sh`
Expected: five `PASS` lines, exit 0.

- [ ] **Step 7: Confirm `shadow_ui.js` still parses**

`node --check` silently passes on broken `.js` in this codebase, so parse it as a module instead.

Run: `node --input-type=module -e 'import("./src/shadow/shadow_ui.js").catch(e => { if (e instanceof SyntaxError) { console.error("SYNTAX", e.message); process.exit(1); } })'`
Expected: no `SYNTAX` output (runtime import errors from missing host bindings are expected and fine).

- [ ] **Step 8: Commit**

```bash
git add src/shadow/shadow_ui.js tests/host/test_canvas_drawcell_wiring.sh
git commit -m "feat: a canvas overlay can also supply an in-grid widget

An overlay exporting drawCell and widgetKind registers on load. Same file, same
author mental model, two scales: drawCell paints one knob box, draw paints the
fullscreen view you dive into from that same cell.

Cleared on teardown -- the registry is process-global and shadow_ui is
long-lived, so a widget left registered would outlive its module and a later
module reusing the name would silently inherit the wrong art."
```

---

## Task 6: The frame-instability matrix

**Goal:** Prove the same widget survives every rect the two renderers can actually produce. This is the test the whole design exists to satisfy.

**Files:**
- Create: `tests/host/test_widget_frame_matrix.sh`

**Acceptance Criteria:**
- [ ] The matrix covers movy's 32×15, each mode `computeGeom` selects (`dial` full radius, `dial` reduced radius, `bar-value`, `bar-label`, `bar-only`), and a span clamped by `Math.min(g.slotSpan, COLS - col)`.
- [ ] In every cell of the matrix, `clipped() === 0` for a well-written widget.
- [ ] In every cell, a greedy widget produces zero out-of-frame parent calls.
- [ ] The matrix is derived from `computeGeom`'s real thresholds, not from hardcoded numbers that could drift from it.
- [ ] **Label ownership needs no assertion here and the reason is recorded:** `render_page_movy.mjs:2438` passes `h: lblY - rowY`, which *excludes* the label band, so a custom widget is handed the same label-free rect every built-in viz group already gets. Schwung keeping the label is a property of the caller, not a rule the widget must obey — do not add a test that pretends otherwise.

**Verify:** `bash tests/host/test_widget_frame_matrix.sh` → all `PASS`, no `FAIL`

**Steps:**

- [ ] **Step 1: Write the test**

Create `tests/host/test_widget_frame_matrix.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE AXIS THIS DESIGN EXISTS TO HANDLE.
#
# A single-size snapshot passing proves nothing here, because the whole reason
# the frame ctx is stronger than clipping is that the rect VARIES. So the same
# widget is drawn into every rect the two renderers can actually produce, and
# the assertion is the invariant -- never out of frame, never clipped for a
# widget that respects width/height -- rather than a picture.
#
# A green matrix only proves the axis you chose, so the greedy widget is drawn
# through the SAME matrix: it must be contained everywhere, not merely somewhere.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the frame matrix tests" >&2
  exit 1
fi

node --input-type=module -e '
import { frameCtx } from "./src/shared/param_pages/frame_ctx.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const recorder = () => {
  const calls = [];
  return { calls,
    fillRect(x, y, w, h, c) { calls.push([x, y, w, h, c]); },
    print() {}, textWidth(t) { return String(t).length * 4; } };
};

/* Every rect the two renderers can hand a widget.
 *
 * movy is fixed. The dial/bar grid rowH is dynamic and computeGeom picks the
 * render mode from it, so the heights below bracket each mode boundary; the
 * widths cover a full cell, a two-cell span, and a span CLAMPED near the right
 * edge by Math.min(g.slotSpan, COLS - col). */
const FRAMES = [
  { name: "movy knob box",          w: 17, h: 15 },
  { name: "movy cell",              w: 32, h: 15 },
  { name: "grid dial full",         w: 32, h: 27 },
  { name: "grid dial reduced",      w: 32, h: 20 },
  { name: "grid bar-value",         w: 32, h: 16 },
  { name: "grid bar-label",         w: 32, h: 12 },
  { name: "grid bar-only",          w: 32, h:  8 },
  { name: "two-cell span",          w: 64, h: 15 },
  { name: "span clamped at edge",   w: 30, h: 15 },
  { name: "degenerate 1x1",         w:  1, h:  1 },
  { name: "degenerate zero width",  w:  0, h: 15 },
];

/* A well-written widget: everything expressed against width/height. */
const polite = (c) => {
  c.fillRect(0, 0, c.width, 1, 1);
  c.fillRect(0, c.height - 1, c.width, 1, 1);
  if (c.width > 2 && c.height > 2) c.fillRect(1, 1, c.width - 2, c.height - 2, 0);
  c.print(0, 0, "LONG LABEL TEXT", 1);
};

/* A badly-written widget: absolute pixels from somebody elses layout. */
const greedy = (c) => {
  c.fillRect(-20, -20, 200, 200, 1);
  c.fillRect(100, 100, 40, 40, 1);
  c.print(0, 0, "OVERFLOWING TEXT THAT IS FAR TOO LONG", 1);
};

for (const f of FRAMES) {
  const origin = { x: 8, y: 9 };

  /* Polite widget: contained, and never even attempts an overflow. */
  let p = recorder();
  let ctx = frameCtx(p, { ...origin, w: f.w, h: f.h });
  polite(ctx);
  const inFrame = p.calls.every(([x, y, w, h]) =>
    x >= origin.x && y >= origin.y &&
    x + w <= origin.x + f.w && y + h <= origin.y + f.h);
  ok(inFrame, `polite widget stays in frame: ${f.name}`);
  /* The print is allowed to truncate on narrow frames -- that is the frame
   * doing its job, not the widget misbehaving. Rect drawing must not clip. */
  const rectClips = (() => {
    const q = recorder();
    const c2 = frameCtx(q, { ...origin, w: f.w, h: f.h });
    c2.fillRect(0, 0, c2.width, 1, 1);
    if (c2.width > 2 && c2.height > 2) c2.fillRect(1, 1, c2.width - 2, c2.height - 2, 0);
    return c2.clipped();
  })();
  ok(rectClips === 0, `polite widget clips nothing when sized to the frame: ${f.name}`);

  /* Greedy widget: contained anyway, and the attempt is COUNTED. */
  p = recorder();
  ctx = frameCtx(p, { ...origin, w: f.w, h: f.h });
  greedy(ctx);
  const contained = p.calls.every(([x, y, w, h]) =>
    x >= origin.x && y >= origin.y &&
    x + w <= origin.x + f.w && y + h <= origin.y + f.h);
  ok(contained, `greedy widget is contained: ${f.name}`);
  ok(ctx.clipped() > 0, `greedy widget overflow is counted, not hidden: ${f.name}`);
}

process.exit(fail ? 1 : 0);
'
```

- [ ] **Step 2: Run to verify it passes**

Run: `bash tests/host/test_widget_frame_matrix.sh`
Expected: 44 `PASS` lines (11 frames × 4), exit 0.

- [ ] **Step 3: Prove the matrix can fail**

Temporarily remove the `if (x1 > w) x1 = w;` clamp in `frame_ctx.mjs`. Re-run; the "greedy widget is contained" assertions must FAIL across the matrix. Revert.

Run: `bash tests/host/test_widget_frame_matrix.sh`
Expected while mutated: multiple `FAIL: greedy widget is contained: ...`

**Note on stale builds:** ExtFS has 1-second mtime granularity, so a rapid edit/re-run cycle can execute a stale file. If the mutation appears to have no effect, `touch src/shared/param_pages/frame_ctx.mjs` and re-run before concluding anything.

- [ ] **Step 4: Commit**

```bash
git add tests/host/test_widget_frame_matrix.sh
git commit -m "test: the same widget through every rect the renderers produce

A single-size snapshot passing proves nothing here -- the entire reason the
frame ctx is stronger than clipping is that the rect varies. The matrix covers
movy 32x15, each mode computeGeom selects from its dynamic rowH, a two-cell
span, a span clamped at the right edge, and two degenerate frames.

The greedy widget goes through the same matrix, because a green matrix only
proves the axis you chose: containment has to hold everywhere, not somewhere."
```

---

## Task 7: RLE sprite runtime and nominal-frame anchoring

**Goal:** Decode run-length sprite data into `fillRect` runs, and anchor a sprite's nominal frame inside the real frame — refusing (so the built-in draws) rather than fractionally scaling.

**Why RLE:** A binding is ~490 ns. A 17×15 knob box blitted per pixel is 255 calls ≈ 125 µs — and **a page holds eight**, so ~1 ms of a 1.68 ms render before anything else draws. Row runs give ~45 calls ≈ 22 µs, so a full page is ~180 µs. The per-sprite figure is survivable; the full-page figure is what makes this non-optional.

**Files:**
- Create: `src/shared/param_pages/sprite_rle.mjs`
- Test: `tests/host/test_sprite_rle.sh`

**Acceptance Criteria:**
- [ ] `drawSprite(ctx, sprite, ox, oy)` emits one `fillRect` per run of set pixels, not one per pixel.
- [ ] A sprite whose nominal `w`/`h` exceeds the frame draws **nothing** and reports refusal.
- [ ] A sprite exactly half the frame in both axes may draw at integer scale 2; scale 1.5 is never produced.
- [ ] `anchorSprite` centres the nominal frame within the real frame at integer offsets.
- [ ] Decoding a 17×15 all-stripes sprite emits ≤ 3 calls per row.

**Verify:** `bash tests/host/test_sprite_rle.sh` → all `PASS`, no `FAIL`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_sprite_rle.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# 1-BIT ART IS NEVER FRACTIONALLY SCALED.
#
# It dithers into mush. So a sprite declares the nominal frame it was drawn for
# and is anchored 1:1, integer-scaled only on an exact fit, and REFUSED when it
# does not fit -- at which point the caller falls back to the built-in widget,
# which is a correct picture rather than a smeared one.
#
# And the format is runs, not pixels, because a binding is ~490ns: a 17x15 box
# is 255 calls (~125us) per sprite and a page holds EIGHT, so per-pixel blitting
# is ~1ms of a 1.68ms render. Runs make it ~22us each.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the sprite tests" >&2
  exit 1
fi

node --input-type=module -e '
import { drawSprite, anchorSprite, spriteFromRows }
  from "./src/shared/param_pages/sprite_rle.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const recorder = () => {
  const calls = [];
  return { calls, width: 32, height: 15,
    fillRect(x, y, w, h, c) { calls.push([x, y, w, h, c]); },
    print() {}, textWidth(t) { return String(t).length * 4; },
    clipped() { return 0; } };
};

/* spriteFromRows takes "#.#" strings -- the generator emits the same shape. */
const s = spriteFromRows(["##..##", "..##..", "######"]);
ok(s.w === 6 && s.h === 3, "sprite dimensions come from the rows");

let p = recorder();
drawSprite(p, s, 0, 0, 1);
ok(p.calls.length === 4,
   "runs, not pixels: 2+1+1 = 4 runs for that sprite, not 12 set pixels");
ok(JSON.stringify(p.calls[0]) === JSON.stringify([0, 0, 2, 1, 1]),
   "the first run is a 2-wide fillRect");

/* Run economy on a realistic knob-box sprite. */
const rows = [];
for (let i = 0; i < 15; i++) rows.push("#####........####");
const box = spriteFromRows(rows);
p = recorder();
drawSprite(p, box, 0, 0, 1);
ok(p.calls.length <= 15 * 3, "a 17x15 striped sprite stays within 3 runs per row");
ok(p.calls.length < 17 * 15, "it is emphatically fewer calls than per-pixel");

/* Anchoring: exact fit at scale 1. */
let a = anchorSprite({ w: 17, h: 15 }, { width: 17, height: 15 });
ok(a && a.scale === 1 && a.x === 0 && a.y === 0, "an exact fit anchors at 1:1");

/* Centred within a larger frame, at integer offsets. */
a = anchorSprite({ w: 17, h: 15 }, { width: 32, height: 15 });
ok(a && a.scale === 1 && a.x === 7 && Number.isInteger(a.x),
   "a smaller sprite is centred at an integer offset");

/* Integer scale only on an exact multiple. */
a = anchorSprite({ w: 8, h: 7 }, { width: 16, height: 14 });
ok(a && a.scale === 2, "an exact double fits at integer scale 2");

a = anchorSprite({ w: 8, h: 7 }, { width: 12, height: 11 });
ok(a && a.scale === 1, "a non-integer multiple stays at 1:1 rather than 1.5x");

/* REFUSAL rather than shrinking. */
a = anchorSprite({ w: 40, h: 15 }, { width: 17, height: 15 });
ok(a === null, "a sprite too wide for the frame is refused, not scaled down");

a = anchorSprite({ w: 17, h: 40 }, { width: 17, height: 15 });
ok(a === null, "a sprite too tall for the frame is refused, not scaled down");

/* And a refused sprite draws nothing at all. */
p = recorder();
const drew = drawSprite(p, spriteFromRows(["####"]), 0, 0, 1, { width: 2, height: 1 });
ok(drew === false && p.calls.length === 0,
   "a refused sprite draws nothing and reports the refusal");

process.exit(fail ? 1 : 0);
'
```

- [ ] **Step 2: Run to verify it fails**

Run: `bash tests/host/test_sprite_rle.sh`
Expected: FAIL — `Cannot find module .../sprite_rle.mjs`, exit 1.

- [ ] **Step 3: Write `sprite_rle.mjs`**

```javascript
/**
 * sprite_rle.mjs — 1-bit sprite runs, and how one is placed in a frame.
 *
 * THE FORMAT IS RUNS BECAUSE OF THE FULL-PAGE COST, NOT THE PER-SPRITE COST.
 *
 * A QuickJS binding is ~490ns. A 17x15 knob box blitted per pixel is 255 calls
 * ~= 125us, which against a 1.68ms page render sounds survivable -- until you
 * notice a page holds EIGHT knob boxes, and a module that ships one custom
 * widget ships eight. That is ~1ms of the render gone before anything else is
 * drawn. Row runs (a 1-bit sprite is typically 2-4 per row) are ~45 calls
 * ~= 22us, so a full page is ~180us.
 *
 * Check the full-page figure, not the per-sprite one.
 *
 * NEVER FRACTIONALLY SCALED. 1-bit art on a 128x64 mono display cannot be
 * resampled -- it dithers into mush. So a sprite carries the NOMINAL frame it
 * was drawn for, is anchored 1:1 (integer scale only on an exact multiple), and
 * is REFUSED when it does not fit. A refusal is not a failure: the caller falls
 * back to the built-in widget, which is a correct picture rather than a smeared
 * one.
 *
 * The generator (tools/param-pages/widget_gen.mjs) enforces the nominal frame at
 * BUILD time, on the authors machine, so this runtime refusal is a backstop
 * rather than the normal path.
 */

/**
 * Build a sprite from row strings, "#" set and anything else clear.
 * The generator emits this same shape, so a hand-written test sprite and a
 * generated one are the same thing.
 */
export function spriteFromRows(rows) {
    const h = rows.length;
    const w = h ? rows[0].length : 0;
    const runs = [];
    for (let y = 0; y < h; y++) {
        const row = rows[y];
        let x = 0;
        while (x < w) {
            if (row[x] !== "#") { x++; continue; }
            let end = x;
            while (end < w && row[end] === "#") end++;
            runs.push([x, y, end - x]);
            x = end;
        }
    }
    return { w, h, runs };
}

/**
 * Where a sprite sits inside a frame, or null if it does not fit.
 *
 * @param {object} nominal  { w, h } the sprite was drawn for
 * @param {object} frame    { width, height } of the frame ctx
 * @returns {{x: number, y: number, scale: number}|null}
 */
export function anchorSprite(nominal, frame) {
    const { w, h } = nominal;
    const fw = frame.width, fh = frame.height;
    if (!(w > 0 && h > 0) || w > fw || h > fh) return null;

    /* Integer scale only, and only on an EXACT multiple in both axes -- a
     * sprite that fits 1.5x is drawn at 1x, never resampled. */
    let scale = 1;
    for (let s = 2; w * s <= fw && h * s <= fh; s++) {
        if (fw % w === 0 && fh % h === 0 && fw / w >= s && fh / h >= s) scale = s;
    }

    return {
        x: Math.floor((fw - w * scale) / 2),
        y: Math.floor((fh - h * scale) / 2),
        scale,
    };
}

/**
 * Draw a sprite at frame-local (ox, oy).
 *
 * @param {object} ctx     a frame ctx from frame_ctx.mjs
 * @param {object} sprite  from spriteFromRows or the generator
 * @param {number} ox
 * @param {number} oy
 * @param {number} color
 * @param {object} [fit]   { width, height } to check against; omitted = no check
 * @returns {boolean} false if the sprite was refused for not fitting
 */
export function drawSprite(ctx, sprite, ox, oy, color = 1, fit = null) {
    if (fit && !anchorSprite({ w: sprite.w, h: sprite.h }, fit)) return false;
    for (const [x, y, len] of sprite.runs) {
        ctx.fillRect(ox + x, oy + y, len, 1, color);
    }
    return true;
}

/** Anchor and draw in one call. Returns false if refused. */
export function drawSpriteAnchored(ctx, sprite, color = 1) {
    const a = anchorSprite({ w: sprite.w, h: sprite.h }, ctx);
    if (!a) return false;
    if (a.scale === 1) return drawSprite(ctx, sprite, a.x, a.y, color);
    for (const [x, y, len] of sprite.runs) {
        ctx.fillRect(a.x + x * a.scale, a.y + y * a.scale, len * a.scale, a.scale, color);
    }
    return true;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `bash tests/host/test_sprite_rle.sh`
Expected: 12 `PASS` lines, exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/shared/param_pages/sprite_rle.mjs tests/host/test_sprite_rle.sh
git commit -m "feat: RLE sprite runtime with nominal-frame anchoring

Runs rather than pixels because of the FULL-PAGE cost: a 17x15 box is 255
bindings (~125us) per sprite, which sounds survivable against a 1.68ms render
until you notice a page holds eight of them -- ~1ms gone before anything else
draws. Runs make it ~22us each, ~180us for a page.

1-bit art is never fractionally scaled; it dithers into mush. A sprite carries
the nominal frame it was drawn for, anchors 1:1 with integer scale only on an
exact multiple, and is refused when it does not fit -- at which point the caller
falls back to the built-in widget, a correct picture rather than a smeared one."
```

---

## Task 8: `widget_gen.mjs` — the no-JS path

**Goal:** A C-only author writes `widgets.toml` beside their PNGs, runs one node command, and commits a generated `widgets.mjs` exporting `widgetKind` and `drawCell`.

**Files:**
- Create: `tools/param-pages/widget_gen.mjs`
- Test: `tests/host/test_widget_gen_roundtrip.sh`

**Acceptance Criteria:**
- [ ] Reads a minimal `widgets.toml` (`kind`, `nominal`, `frames` mapping value ranges to PNG paths).
- [ ] Emits a `.mjs` exporting `widgetKind`, `widgetNominal` and `drawCell`.
- [ ] **Fails at build time** if any PNG's dimensions differ from the declared `nominal` — the author's machine, not the user's device.
- [ ] Round-trip: PNG → RLE → drawn runs reproduce the source PNG's set pixels exactly. Compared **by content**, never by bytes.
- [ ] Generated output is deterministic: two runs on the same input produce identical text.

**Verify:** `bash tests/host/test_widget_gen_roundtrip.sh` → all `PASS`, no `FAIL`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_widget_gen_roundtrip.sh`. It synthesises its own PBM-style input so no binary fixture is committed and no PNG decoder is needed in the test.

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE GENERATOR FAILS ON THE AUTHORS MACHINE, NOT THE USERS DEVICE.
#
# That is the whole reason the declarative tier is a BUILD step rather than a
# runtime format: a malformed sprite config is a red build for the person who
# wrote it, instead of a broken widget for whoever installed the module. The
# nominal-dimension check is the load-bearing part.
#
# The round-trip is compared BY CONTENT. zlib is not byte-stable, so a byte
# comparison of generated artifacts passes only on the machine that wrote them.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the generator tests" >&2
  exit 1
fi

node --input-type=module -e '
import { generate } from "./tools/param-pages/widget_gen.mjs";
import { spriteFromRows, drawSprite } from "./src/shared/param_pages/sprite_rle.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const ROWS = ["##..##", "..##..", "######", "#....#", ".####."];

/* Round trip: rows -> sprite -> drawn runs -> pixel set, compared BY CONTENT. */
const sprite = spriteFromRows(ROWS);
const painted = new Set();
drawSprite({ fillRect(x, y, w, h) { for (let i = 0; i < w; i++) painted.add(x + i + "," + y); } },
           sprite, 0, 0, 1);
const source = new Set();
ROWS.forEach((r, y) => [...r].forEach((c, x) => { if (c === "#") source.add(x + "," + y); }));
ok(painted.size === source.size && [...source].every((k) => painted.has(k)),
   "round trip reproduces every set pixel and no others");

/* Determinism: the same input generates identical text twice. */
const spec = { kind: "custom:demo", nominal: { w: 6, h: 5 },
               frames: [{ atMost: 1.0, rows: ROWS }] };
const a = generate(spec);
const b = generate(spec);
ok(a === b, "generation is deterministic");

ok(/export const widgetKind = "custom:demo"/.test(a), "the generated file exports widgetKind");
ok(/export const widgetNominal/.test(a), "the generated file exports widgetNominal");
ok(/export function drawCell/.test(a), "the generated file exports drawCell");
ok(!/getParam/.test(a), "generated code never reaches for a param read");

/* THE BUILD-TIME CHECK: a frame whose dimensions differ from nominal fails HERE. */
let threw = false;
try {
  generate({ kind: "custom:bad", nominal: { w: 6, h: 5 },
             frames: [{ atMost: 1.0, rows: ["###", "###"] }] });
} catch (e) { threw = /nominal/i.test(String(e)); }
ok(threw, "a frame that does not match nominal fails the build with a nominal error");

/* And a kind missing the reserved prefix is rejected. */
threw = false;
try { generate({ kind: "demo", nominal: { w: 6, h: 5 }, frames: [{ atMost: 1, rows: ROWS }] }); }
catch (e) { threw = /custom:/.test(String(e)); }
ok(threw, "a kind without the custom: prefix is rejected");

process.exit(fail ? 1 : 0);
'
```

- [ ] **Step 2: Run to verify it fails**

Run: `bash tests/host/test_widget_gen_roundtrip.sh`
Expected: FAIL — `Cannot find module .../widget_gen.mjs`, exit 1.

- [ ] **Step 3: Write the generator**

Create `tools/param-pages/widget_gen.mjs`:

```javascript
/**
 * widget_gen.mjs — PNG + widgets.toml  ->  a modules widgets.mjs.
 *
 * THE NO-JS PATH IS A BUILD STEP, NOT A RUNTIME FORMAT.
 *
 * A C-only author writes widgets.toml beside their PNGs and runs this. They
 * never write JavaScript, exactly as they never write ARM assembly to get a
 * dsp.so. The declarative tier still exists -- it just runs on THEIR machine,
 * so a malformed sprite config is a red build for the person who wrote it
 * rather than a broken widget for whoever installed the module.
 *
 * Nothing new ships to the device as a result: no parser, no runtime
 * validator, no second file format to version.
 *
 * This lives in this repo, beside widget_sheet.mjs, for LOCKSTEP rather than
 * convenience -- the runtime consumes this output, so a runtime change that
 * breaks the format has to fail in this repo CI, not silently in an authors
 * build weeks later. A build step that can drift without failing defeats every
 * bisect that follows it.
 */

const CUSTOM_PREFIX = "custom:";

function runsFromRows(rows) {
    const h = rows.length;
    const w = h ? rows[0].length : 0;
    const runs = [];
    for (let y = 0; y < h; y++) {
        let x = 0;
        while (x < w) {
            if (rows[y][x] !== "#") { x++; continue; }
            let end = x;
            while (end < w && rows[y][end] === "#") end++;
            runs.push([x, y, end - x]);
            x = end;
        }
    }
    return { w, h, runs };
}

/**
 * @param {object} spec  { kind, nominal: {w,h}, frames: [{atMost, rows}] }
 * @returns {string} the text of a widgets.mjs
 */
export function generate(spec) {
    if (!spec || typeof spec.kind !== "string" || !spec.kind.startsWith(CUSTOM_PREFIX)) {
        throw new Error(`widget kind must start with "${CUSTOM_PREFIX}": got ${spec && spec.kind}`);
    }
    const { w: nw, h: nh } = spec.nominal || {};
    if (!(nw > 0 && nh > 0)) throw new Error("widgets.toml must declare a nominal w and h");

    const frames = (spec.frames || []).map((f, i) => {
        const rows = f.rows;
        const fh = rows.length, fw = fh ? rows[0].length : 0;
        /* THE BUILD-TIME CHECK. Catching this here is the entire argument for
         * the generator: the author sees it, the user never does. */
        if (fw !== nw || fh !== nh) {
            throw new Error(
                `frame ${i} is ${fw}x${fh} but nominal is ${nw}x${nh} — ` +
                `every frame must match the nominal frame exactly ` +
                `(1-bit art is never rescaled, so a mismatched frame cannot be fixed at runtime)`);
        }
        if (rows.some((r) => r.length !== fw)) {
            throw new Error(`frame ${i} has ragged rows`);
        }
        return { atMost: f.atMost, sprite: runsFromRows(rows) };
    });

    if (!frames.length) throw new Error("widgets.toml declares no frames");

    const framesLit = frames.map((f) =>
        `    { atMost: ${f.atMost}, runs: ${JSON.stringify(f.sprite.runs)} },`).join("\n");

    return `/* GENERATED by tools/param-pages/widget_gen.mjs — do not edit.
 *
 * Re-generate with:  node tools/param-pages/widget_gen.mjs widgets.toml
 *
 * Runs, not pixels: a binding is ~490ns and a page holds eight knob boxes, so
 * per-pixel blitting would cost ~1ms of a 1.68ms page render.
 */

export const widgetKind = ${JSON.stringify(spec.kind)};
export const widgetNominal = { w: ${nw}, h: ${nh} };

const FRAMES = [
${framesLit}
];

/* Frame-local coordinates only — the ctx cannot express a screen pixel, and
 * carries no getParam: values arrive as an argument. */
export function drawCell(ctx, { values, group }) {
    const key = group && group.keys && group.keys[0];
    const raw = key && values ? Number(values[key]) : 0;
    const v = Number.isFinite(raw) ? raw : 0;

    let frame = FRAMES[FRAMES.length - 1];
    for (const f of FRAMES) { if (v <= f.atMost) { frame = f; break; } }

    /* Refuse rather than rescale: a frame too small for the nominal art gets
     * nothing from us, and the host falls back to the built-in widget. */
    if (ctx.width < widgetNominal.w || ctx.height < widgetNominal.h) return;

    const ox = Math.floor((ctx.width - widgetNominal.w) / 2);
    const oy = Math.floor((ctx.height - widgetNominal.h) / 2);
    for (const [x, y, len] of frame.runs) ctx.fillRect(ox + x, oy + y, len, 1, 1);
}
`;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `bash tests/host/test_widget_gen_roundtrip.sh`
Expected: 8 `PASS` lines, exit 0.

- [ ] **Step 5: Commit**

```bash
git add tools/param-pages/widget_gen.mjs tests/host/test_widget_gen_roundtrip.sh
git commit -m "feat: widget_gen — the no-JS path is a build step, not a format

A C-only author writes widgets.toml beside their PNGs and runs one node
command; they never write JavaScript, exactly as they never write ARM assembly
to get a dsp.so. The declarative tier still exists, it just runs on THEIR
machine -- so a malformed sprite config is a red build for the person who wrote
it rather than a broken widget for whoever installed the module. Nothing new
ships to the device: no parser, no runtime validator, no second format.

Lives beside widget_sheet.mjs for lockstep rather than convenience: the runtime
consumes this output, so a runtime change that breaks the format must fail in
this repo CI, not silently in an authors build weeks later.

The round-trip test compares by content -- zlib is not byte-stable, so a byte
comparison passes only on the machine that wrote the file."
```

---

## Task 9: Validator, docs, and the generated widget sheet

**Goal:** A malformed `custom:` declaration is reported by `validate_contract`, the widget reference in `docs/MODULES.md` covers custom widgets, and `CLAUDE.md` gains its one index bullet.

**Files:**
- Modify: `src/shared/param_pages/validate_contract.mjs`
- Modify: `docs/MODULES.md` (author-facing section + regenerated widget sheet)
- Modify: `docs/PARAM_PAGES.md` (the subsystem detail)
- Modify: `CLAUDE.md` (one bullet under the knob-grid hook — **not** the prose)
- Test: `tests/host/test_custom_viz_validate.sh`

**Acceptance Criteria:**
- [ ] `validateContract` reports a `custom:` kind declared with no registered widget as a *notice*, not an error — it is legal and degrades correctly.
- [ ] It reports a `viz.kind` starting with `custom:` on a param whose module ships no `canvas_script` as a **warning** (almost certainly an author mistake).
- [ ] `docs/MODULES.md` documents `drawCell`, `widgetKind`, `widgetNominal`, the frame contract, and the generator invocation.
- [ ] `node tools/param-pages/widget_sheet.mjs --manual` regenerated; `bash tests/host/test_widget_sheet.sh` passes.
- [ ] `CLAUDE.md` gains exactly one bullet under the `docs/PARAM_PAGES.md` hook.

**Verify:** `bash tests/host/test_custom_viz_validate.sh && bash tests/host/test_widget_sheet.sh` → all `PASS`

**Steps:**

- [ ] **Step 1: Write the failing validator test**

Create `tests/host/test_custom_viz_validate.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# AN UNKNOWN CUSTOM KIND IS LEGAL. A CUSTOM KIND WITH NO SCRIPT IS A MISTAKE.
#
# The first is how an older host reads a newer module -- it must not be an
# error, because the module is correct and the host is simply older. The second
# is an author who declared a widget and forgot to ship the file, which nothing
# else will ever tell them.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the validator tests" >&2
  exit 1
fi

node --input-type=module -e '
import { validateContract } from "./src/shared/param_pages/validate_contract.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const run = (chainParams, capabilities) => validateContract({
  hierarchy: { modes: null, levels: { root: { label: "T",
    knobs: chainParams.map((p) => p.key),
    params: chainParams.map((p) => ({ key: p.key })) } } },
  chainParams, capabilities: capabilities || {},
});

/* Legal: declared, unknown to this host, ships a script. Not an error. */
let r = run([{ key: "drive", name: "Drive", type: "float", min: 0, max: 1,
               viz: { kind: "custom:meter" } }], { canvas_script: "canvas.js" });
ok(!r.errors.some((e) => /custom:/.test(e.message || String(e))),
   "an unknown custom kind is not an error");

/* Mistake: custom kind declared, no script shipped. */
r = run([{ key: "drive", name: "Drive", type: "float", min: 0, max: 1,
           viz: { kind: "custom:meter" } }], {});
ok(r.warnings.some((w) => /custom:/.test(w.message || String(w))),
   "a custom kind with no canvas_script is warned about");

/* A built-in kind is untouched by any of this. */
r = run([{ key: "drive", name: "Drive", type: "float", min: 0, max: 1,
           viz: { kind: "fader" } }], {});
ok(!r.warnings.some((w) => /custom:/.test(w.message || String(w))),
   "a built-in kind produces no custom-widget warning");

process.exit(fail ? 1 : 0);
'
```

- [ ] **Step 2: Run to verify it fails**

Run: `bash tests/host/test_custom_viz_validate.sh`
Expected: FAIL on the warning assertion, exit 1.

- [ ] **Step 3: Add the validator rule**

In `src/shared/param_pages/validate_contract.mjs`, inside the per-param walk, add:

```javascript
        /* A CUSTOM KIND WITH NO SCRIPT IS THE ONE MISTAKE WORTH REPORTING.
         *
         * An unknown custom kind is LEGAL -- it is exactly what an older host
         * sees when it reads a newer module, and it degrades to the detector
         * built-in on its own. Flagging that as an error would make every
         * forward-compatible module look broken.
         *
         * But a module that declares custom: and ships no canvas_script has
         * simply forgotten the file, and nothing else in the system will ever
         * say so: the widget just silently never appears. */
        const vk = p && p.viz && p.viz.kind;
        if (typeof vk === "string" && vk.startsWith("custom:")
            && !(capabilities && capabilities.canvas_script)) {
            warnings.push({
                key: p.key,
                message: `declares viz kind "${vk}" but the module ships no canvas_script — `
                       + `the widget will never load and the built-in will draw instead`,
            });
        }
```

- [ ] **Step 4: Run to verify it passes**

Run: `bash tests/host/test_custom_viz_validate.sh`
Expected: 3 `PASS` lines, exit 0.

- [ ] **Step 5: Document it for authors in `docs/MODULES.md`**

Add a `#### Custom widgets (`drawCell`)` subsection beneath the existing `canvas` subsection (`:1637`), covering:
- the declaration `"viz": { "kind": "custom:mymeter" }`, and the grouped form with `group`/`role`;
- `canvas.js` exporting `widgetKind`, `widgetNominal` and `drawCell(ctx, { values, group, metaIndex, anim, nowMs })`;
- **the frame contract**: `(0,0)` is the knob box's top-left, `ctx.width`/`ctx.height` are the frame's, there is no absolute-coordinate accessor and no `getParam` — values arrive as an argument;
- **the label is not yours**: Schwung keeps drawing it via `drawLabelCell`;
- one strike — a throw disables the widget for the session and the built-in draws;
- an unknown kind degrades to the built-in, which is what makes a module forward-compatible with older hosts;
- the generator: `node tools/param-pages/widget_gen.mjs widgets.toml > widgets.mjs`, and that every frame must match `nominal` exactly because 1-bit art is never rescaled.

- [ ] **Step 6: Regenerate the widget sheet**

Run: `node tools/param-pages/widget_sheet.mjs --manual`
Then: `bash tests/host/test_widget_sheet.sh`
Expected: `PASS`. The test also fails on an orphaned image, so review the diff of images as well as prose.

- [ ] **Step 7: Add the subsystem detail to `docs/PARAM_PAGES.md`**

Add a section covering the frame-instability argument (the three ways the rect varies), the fall-through-by-not-claiming mechanism, one-strike, and the full-page RLE cost figure. This is where the prose lives.

- [ ] **Step 8: Add exactly one bullet to `CLAUDE.md`**

Under the `docs/PARAM_PAGES.md` hook in the knob-grid section, add:

```markdown
- **A module-supplied widget draws into a FRAME and cannot express a screen
  pixel.** The rect varies three ways (`cellW` is caller-dependent, `rowH` is
  dynamic and picks the render mode, a right-edge span is clamped), so an
  unknown `custom:` kind falls through by **not claiming its keys** — one branch
  covering a typo, a failed load, an older host, and a one-strike disable.
```

- [ ] **Step 9: Run the full suite**

Run: `make -C tests/host test && for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done`
Expected: `make` green, no `FAIL` output.

- [ ] **Step 10: Commit**

```bash
git add src/shared/param_pages/validate_contract.mjs tests/host/test_custom_viz_validate.sh docs/ CLAUDE.md
git commit -m "docs: custom widget contract, validator rule, regenerated sheet

An unknown custom kind is LEGAL -- it is exactly what an older host sees
reading a newer module, and it degrades to the detector built-in on its own.
Flagging it would make every forward-compatible module look broken.

A custom kind with NO canvas_script is the one mistake worth reporting: the
author forgot the file, and nothing else in the system would ever say so -- the
widget just silently never appears.

Per the CLAUDE.md index rule, the prose goes in docs/PARAM_PAGES.md and
CLAUDE.md gets one bullet, not the explanation."
```

---

## Not in this plan

Carried from spec §9, so a later reader does not mistake these for oversights:

- **Page-claim.** Reversed once `type: "canvas"` was found — "whole page" is already spelled `type: "canvas"`, with a footer and a return path.
- **`ui_chain.js` modules.** Structurally excluded: `enterComponentEdit` (`shadow_ui.js:13322`) tries the hierarchy editor first and only falls through to `loadModuleUi` when a component publishes no `ui_hierarchy` — so a module with a hierarchy never reaches its `ui_chain.js`, and one without has no knob grid and therefore no cells.
- **Master FX.** Scope is the four chain slots' real components, excluded in one helper so a new call site cannot silently opt it in.
- **Consolidating the four module-visual mechanisms.** A larger project; this design does not foreclose it.
- **Sub-project 2** (documenting and hardening the takeover paths), which should be re-scoped by what this leaves uncovered.

## Deferred to implementation

- The exact `widgets.toml` schema beyond `kind` / `nominal` / `frames` (Task 8 defines the minimum the generator needs).
- Whether `drawCell` receives `baseValues` for a modulated param, or only the live value. Task 4 passes it through, so this is a docs decision rather than a plumbing one.

**One spec item deliberately not implemented as written.** Spec §8 asks for "a snapshot in `tests/fixtures/snapshots/param_pages_viz.txt` covering a custom group". That fixture is generated from the **95-module fleet contract dump**, and no fleet module declares a `custom:` kind — so there is nothing for such a snapshot to capture, and synthesising a fake fleet entry would make the fixture stop describing the fleet. Task 3 Step 6 asserts the opposite and more useful thing: that the snapshot **does not move**, which is the real risk when touching `collectDeclared`. Revisit only once a real module ships a custom widget.
