# Module-supplied widgets — design

**Date:** 2026-08-28
**Status:** design approved, not scheduled (explicitly *not* for the next release)
**Sub-project 1 of 2.** Sub-project 2 ("document and harden the takeover paths") is
deliberately deferred and scoped by what this leaves uncovered.

---

## 1. What already exists

The starting request was "let modules supply custom widgets, and let them supply a
fully custom UI with knob interactions". Reading the code first turned up that the
second half already exists — **twice** — and that the landscape is four mechanisms,
not one gap.

| Mechanism | Scope | Input it receives | What the author ships |
|---|---|---|---|
| `viz` in `chain_params` | one graphic, in-grid, row-bounded | none — knobs still drive the params underneath | an enum string, or nothing (a detector guesses) |
| `type: "canvas"` param | **fullscreen**, entered by clicking a cell | `onMidi` — all knobs, pads, jog | `canvas.js` + a `chain_params` entry |
| `ui_chain.js` | replaces the whole component editor | all MIDI except Back | `ui_chain.js` |
| overtake module | the whole device | all MIDI, internal + external | `ui.js` |

### `canvas` is a complete runtime, and it is the good one

`shadow_ui.js:15186` (`createCanvasRuntimeContext`) and its surroundings implement:

- script path resolved from `module.json` via `canvas_script`, supporting
  `file.js#overlay_name` specs, with `canvas_overlay` / `canvas_target` /
  `overlay` aliases;
- overlay object resolution against `globalThis.canvas_overlay` or
  `globalThis.canvas_overlays`;
- lifecycle hooks `onOpen`, `onMidi`, `tick`, `draw`, `onClose`, `onExit`,
  invoked as `fn(canvasRuntime.ctx, payload)` at `:15236`;
- a per-hook `try/catch` recording `canvasRuntime.error` at `:15242`;
- a drawing context exposing `width`, `height`, `state`, `clear`, `setPixel`,
  `drawRect`, `fillRect`, `drawLine`, `print`, `now`, `random`, `getValue`,
  `setValue`, `getParam`, `setParam`, `sourcePath`.

Unlike `ui_chain.js`, canvas **composes**: you dive into it from a cell and come
back, the grid stays intact around it, and it is declared in `module.json` — so a
C-only author's build step just ships one more file.

### The gap, stated precisely

**A canvas overlay can only be fullscreen.** There is no way for a module to draw
its own art *in a cell*. Between "pick one of eight built-in `viz` kinds" and "take
over the entire screen" there is nothing.

`viz.mjs:19` documents this slot as knowingly empty:

> `module chain_params viz → module layout file → host override → detector → none`
> …"module layout file" is the still-open, undesigned mechanism… zero fleet modules
> ship one today, so there is nothing to read yet. The slot is left as a documented
> no-op rather than invented here.

---

## 2. Design

> **A custom widget is a canvas overlay's draw hook given a rect instead of the
> screen.**

`canvas.js` gains one optional hook:

```javascript
drawCell(ctx, rect, values, meta)
```

Declaring it makes that parameter's cell render the module's own art **in the
grid**. Clicking the cell still dives into the same overlay's fullscreen `draw`.
One file, one author mental model, two scales.

This is not a fourth mechanism. Script loading, overlay resolution, error
surfacing, the `try/catch` and the ctx shape are all already built; this adds a
hook and a rect-scoped context.

### Why not the alternatives

Three approaches were considered.

**Widget pack (data + code as two runtime formats)** — a `widgets/` directory with
`widgets.json`, `sprites.rle` and an optional `draw.mjs`. Rejected: two runtime
formats to spec, validate, version and document, when one hook on an existing
contract does the same job. Its one real advantage — inert, install-time-validatable
data — buys less than it appears to, because modules already ship arbitrary
JavaScript into this exact QuickJS context (`ui_chain.js`, `ui.js`, `canvas.js`)
with no guard at all. This introduces no new trust category; it is a
*better-guarded* version of a door already standing open.

**Everything through `chain_params`** — embed the widget spec in the JSON a module
already returns. Rejected outright: `chain_params` is served by `get_param` **on
the SPI callback**, at roughly one 2.8 ms round-trip per read. Sprite data is
kilobytes. This would push a widget atlas through the realtime param channel.

**A page-claim** (a module owning a whole knob page, all 8 knobs and click/dive,
while Schwung keeps header, footer, bank bar, LFO and a11y) — initially approved,
then **reversed** once `canvas` was found. Canvas already provides
fullscreen-with-knobs with a footer and a return path. A page-claim would be a
second, worse answer to a solved problem. **"Whole page" is spelled
`type: "canvas"`.**

### Claim sizes

**Group and row only.** A custom widget lives under exactly the rules built-in viz
groups already follow: contiguous, within a single row (`viz.mjs:66` — a graphic
cannot span the header gap), and reflowed by `alignGroupsToRows`, which already
moves 24 fleet pages to keep that true. No new layout rules to design or test.

---

## 3. The rect-scoped context

The `drawCell` hook receives a context that is the canvas ctx **translated,
clipped, and stripped of reads**:

- `fillRect` / `drawRect` / `setPixel` / `drawLine` / `print` offset into the
  group's rect and are bounded by it. Drawing outside your rect stops being a rule
  authors must follow and becomes something they *cannot express* — roughly fifteen
  lines over the same method names.
- `width` / `height` are the rect's, not the screen's.
- **`getParam` and `getValue` are absent.** Values are handed in via the `values`
  argument. This makes `PARAM_PAGES.md`'s hardest rule — *nothing reads on the
  draw path* — enforced by construction rather than by review.
- `state` is retained; `now()` is retained for animation, matching
  `viz_draw.mjs`'s existing optional `nowMs` convention.

Staying inside this contract is also what keeps a custom widget **host-testable**.
The entire 14k-line `param_pages/` library unit-tests on a Mac with no device
precisely because it is written against `ctx = { fillRect, print, textWidth }`
(`render_page.mjs:9`). Anything reaching for the global `set_pixel` / `fill_rect`
bindings leaves that, permanently.

---

## 4. The sprite tier (no-JS authors)

The declarative path is a **build-time generator**, not a runtime format.

A C-only author writes `widgets.toml` next to their PNGs and runs
`schwung-widget-gen`, which emits a `drawCell` implementation plus run-length data,
committed as part of their build output. They never write JavaScript — exactly as
they never write ARM assembly to get a `dsp.so`.

The declarative tier still exists; it runs on the author's Mac instead of on the
user's device. A malformed sprite config therefore fails during *their* build
rather than at *someone else's* install. Nothing new ships to the device: no
parser, no runtime validator, no second file format to version.

### The format falls out of the cost math, and it is not PNG

A QuickJS binding costs roughly 490 ns — measured previously when the draw path was
profiled, and the reason `PARAM_PAGES.md` says not to optimise draw calls. A 32×16
sprite blitted per pixel is 512 calls
≈ **250 µs** — about 15% of the 1.68 ms page render, for one sprite. Run-length
encoding the rows (a 1-bit sprite is typically 2–4 runs per row) gives roughly
16 × 3 = 48 calls ≈ **24 µs**. Ten times cheaper, and it is only `fillRect` calls,
so it stays inside the ctx and stays host-testable.

**`draw_image` is not the substrate.** `js_display.c:205` is stb_image-backed and
registered in the shadow UI process (`shadow_ui.c:2819`), but it is unusable here
for three independent reasons: it `stbi_load`s **from disk on every call** (a PNG
decode per frame at 60 Hz); it writes **absolute screen coordinates** straight into
the global buffer, so it cannot be clipped to a rect or participate in the page
slide transition; and it is OR-only (`buffer[..] = 1` when lit, never clearing), so
it cannot erase.

---

## 5. Containment

**One-strike disable, falling back to the built-in widget.**

`try/catch` per group. The first throw disables *that widget* for the session, logs
it, and the cell falls back to whatever `resolveViz`'s detector or default would
have drawn — so the page still shows something **correct**, just not custom. The
author sees it in `debug.log`; the user sees a working page.

This matches how `page_controller.mjs` already handles an unresolved contract: keep
something correct on screen, never let a failure become a picture.

Note that a module-supplied draw function **does not run on the SPI callback** — it
runs in the `shadow_ui` process at SCHED_OTHER. Every war story in `CLAUDE.md` about
module entry points *being* the SPI callback (`create_instance`, `set_param`,
`render_block`) does not apply. This is the one place a module can be given code
execution without a realtime-safety argument.

---

## 6. Two live defects in `canvas`, fixed as part of this

Both are pre-existing, in shipped code, and both are the same defects this design
avoids for the new path. Fixing only the new path would leave the old one
idiomatic-but-wrong.

**`ctx.getParam()` is a synchronous param read available on the draw path**
(`shadow_ui.js:15219`). An overlay calling it inside `draw` pays a ~2.8 ms IPC
round-trip *per frame* — an IPC read costs more than redrawing the entire screen
(1.68 ms). Removing it from the rect-scoped ctx is part of the design above; the
fullscreen path needs the hazard documented and a values-in path offered.

**The `try/catch` at `:15242` records the error but never disables.** A throwing
overlay therefore throws every frame, forever, flooding the log and burning the
frame budget. One-strike disable applies to both paths.

These may reasonably ship ahead of the widget seam as their own change.

---

## 7. Integration points

Named against real functions, not invented ones.

- **`viz.mjs` — `resolveViz({ keys, metaIndex, overrides, ignoreRows })` (`:881`).**
  Custom kinds resolve here, returning groups whose `kind` carries a custom
  identifier. Precedence is the existing chain, with custom widgets filling the
  documented-but-empty "module layout file" slot.
- **`viz_draw.mjs` — `drawVizGroup(ctx, rect, group, values, metaIndex, anim, nowMs, baseValues)` (`:1687`).**
  Dispatch is `const fn = DRAW[group.kind]`. Extending that registry is the whole
  draw-path change; everything downstream — cell coverage, one-mark-per-graphic,
  `alignGroupsToRows`, `vizDiveTarget` — then works unmodified.
- **Call sites** are `render_page.mjs:668` and `render_page_movy.mjs:2131`. Both
  already pass a rect; neither needs to change.
- **`validate_contract.mjs`** gains validation for the custom-kind declaration.
- **`tools/param-pages/widget_sheet.mjs`** — the widget reference in
  `docs/MODULES.md` is generated between markers and pinned by
  `tests/host/test_widget_sheet.sh`, which also fails on an orphaned image. A new
  widget kind that does not appear in the generated sheet is a test failure, not an
  oversight.

---

## 8. Testing

- Unit tests for the rect-scoped ctx: a widget drawing outside its rect must
  produce **no pixels** outside it. Mutate the clip to prove the test can fail.
- Unit test for one-strike disable: a widget that throws is called **once**, and the
  fallback widget draws.
- Unit test that the rect ctx exposes no read: asserting `getParam === undefined`
  is weak; assert instead that a widget attempting a read cannot obtain one.
- A snapshot in `tests/fixtures/snapshots/param_pages_viz.txt` covering a custom
  group, regenerated deliberately.
- Generator round-trip: PNG → RLE → drawn pixels must match the source PNG.
  **Compare by content, not bytes.**
- An example module exercising both `drawCell` and the fullscreen `draw` from one
  `canvas.js`.

---

## 9. Out of scope

- Page-claim (reversed — see §2).
- Any change to `ui_chain.js` or overtake modules.
- Master FX. Consistent with the trailing-pages precedent, scope is the four chain
  slots' real components, excluded in one helper so a new call site cannot silently
  opt it in.
- Consolidating the four mechanisms into one contract. Considered and deferred;
  it is a larger project and this design does not foreclose it.

---

## 10. Open questions

- How a custom kind is *named* and referenced from `chain_params` — reusing the
  existing `viz` field with a namespaced value is the obvious candidate but has
  not been settled.
- Whether `drawCell` should be offered on `ui_chain.js` modules too, or remain
  canvas-only.
- Whether the generator lives in this repo (alongside `tools/param-pages/`) or
  ships as a standalone tool module authors install.
