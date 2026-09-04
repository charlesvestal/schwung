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

`shadow_ui.js:16517` (`createCanvasRuntimeContext`) and its surroundings implement:

- script path resolved from `module.json` via `canvas_script`, supporting
  `file.js#overlay_name` specs, with `canvas_overlay` / `canvas_target` /
  `overlay` aliases;
- overlay object resolution against `globalThis.canvas_overlay` or
  `globalThis.canvas_overlays`;
- lifecycle hooks `onOpen`, `onMidi`, `tick`, `draw`, `onClose`, `onExit`,
  invoked as `fn(canvasRuntime.ctx, payload)` at `:16566`;
- a per-hook `try/catch` recording `canvasRuntime.error` at `:16573`;
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

`viz.mjs:13` documents this slot as knowingly empty:

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
drawCell(ctx, frame, values, meta)
```

Declaring it makes that parameter's cell render the module's own art **in the
grid**. Clicking the cell still dives into the same overlay's fullscreen `draw`.
One file, one author mental model, two scales.

`frame` is the knob box's own coordinate space, not a screen rect — see §3, which
is the constraint the rest of this design hangs off.

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
groups already follow: contiguous, within a single row (`viz.mjs:76` — a graphic
cannot span the header gap), and reflowed by `alignGroupsToRows`, which already
moves 24 fleet pages to keep that true. No new layout rules to design or test.

---

## 3. The frame-local context

**A custom widget never draws direct pixels. It draws into a frame, and it cannot
express a coordinate outside that frame.**

This is stronger than clipping, and it has to be, because the rect a widget receives
is unstable in three independent ways:

- `render_page.mjs:619` — `cellW = floor(rect.w / COLS)`, dependent on the caller's
  rect; and `:116` — `rowH = floor(gridH / ROWS)` is **dynamic**. `computeGeom` then
  selects the entire render mode from that height: `dial` at full height, then a
  shrinking radius, then `bar-value`, `bar-label`, `bar-only`.
- `render_page_movy.mjs` — `CELL_W = 32` fixed, height `LBL0_Y - ROW0_Y` = **15**,
  with the comment at `:2428` warning that 15 is only correct because both of that
  grid's gaps happen to be 15px.
- `render_page.mjs:671` — `w: cellW * Math.min(g.slotSpan, COLS - col)`. A two-slot
  group near the right edge is silently **clamped** to less width than it declared.

So the same widget can be handed 32×15, a taller dial-mode row, a squashed bar-only
row, or a clamped span. Pixel coordinates are wrong under all of it.

### The frame is the knob box, not the cell

Movy already draws this distinction: `KW = 17` (`:606`) is the knob box inside a
`CELL_W = 32` cell, and the label is drawn separately by `drawLabelCell`. **A custom
widget owns the art area; Schwung keeps drawing the label.** Labels therefore stay
consistent across a page even when half its cells are custom, and the widget gets
the more stable of the two frames.

### The context

- `(0, 0)` is the widget's own top-left. `width` / `height` are the frame's.
  There is **no escape hatch to absolute space** — clipping stops being a safety
  net and becomes the coordinate system.
- `fillRect` / `drawRect` / `setPixel` / `drawLine` / `print` operate in that
  space. Drawing outside your frame stops being a rule authors must follow and
  becomes something they cannot express.
- **`getParam` and `getValue` are absent.** Values arrive via the `values`
  argument. This makes `PARAM_PAGES.md`'s hardest rule — *nothing reads on the
  draw path* — enforced by construction rather than by review.
- `state` is retained; `now()` is retained for animation, matching
  `viz_draw.mjs`'s existing optional `nowMs` convention.

### The two tiers adapt differently, and this is the load-bearing part

**Code tier** receives the frame's real `width`/`height` and must adapt to them —
exactly what built-in viz widgets already do, since they are handed a rect and
compute against it.

**Sprite tier cannot do that.** 1-bit art on a 128×64 mono display cannot be
fractionally scaled; it dithers into mush. So a sprite widget declares the
**nominal frame it was drawn for**, and Schwung anchors it 1:1 inside the real
frame, integer-scaling only on an exact fit. **If the nominal frame does not fit,
fall back to the built-in widget rather than scaling it.** Never fractional. The
generator enforces this at build time, on the author's machine.

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
profiled, and the reason `PARAM_PAGES.md` says not to optimise draw calls.

A knob box is `KW = 17` wide (`render_page_movy.mjs:677`) by roughly 15 tall, so a
sprite filling one is ~255 pixels. Blitted per pixel that is 255 calls ≈ **125 µs**,
against a 1.68 ms page render — and a page can hold **eight** of them, which is
1 ms, over half the render, before anything else is drawn. Run-length encoding the
rows (a 1-bit sprite is typically 2–4 runs per row) gives roughly 15 × 3 = 45 calls
≈ **22 µs**, so a full page of custom widgets costs ~180 µs instead of ~1 ms.

The per-sprite figure is survivable; **the full-page figure is what makes RLE
non-optional**, and it is the one to check against, because a module that ships one
custom widget will ship eight. It is still only `fillRect` calls, so it stays inside
the ctx and stays host-testable.

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
(`shadow_ui.js:16550`). An overlay calling it inside `draw` pays a ~2.8 ms IPC
round-trip *per frame* — an IPC read costs more than redrawing the entire screen
(1.68 ms). Removing it from the rect-scoped ctx is part of the design above; the
fullscreen path needs the hazard documented and a values-in path offered.

**The `try/catch` at `:16573` records the error, logs, and then returns `true`.**
So a throwing overlay throws every frame, forever, flooding the log and burning
the frame budget — *and tells its caller it succeeded*. That second half is the
same defect class as `move_midi_internal_send` returning true on a discarded
write (`CLAUDE.md`, Overtake Modules): the failure is reported as success at the
boundary, so no caller can react to it. One-strike disable applies to both paths.

These may reasonably ship ahead of the widget seam as their own change.

---

## 7. Integration points

Named against real functions, not invented ones.

- **`viz.mjs` — `resolveViz({ keys, metaIndex, overrides, ignoreRows })` (`:881`).**
  Custom kinds resolve here, returning groups whose `kind` carries a custom
  identifier. Precedence is the existing chain, with custom widgets filling the
  documented-but-empty "module layout file" slot.
- **`viz_draw.mjs` — `drawVizGroup(ctx, rect, group, values, metaIndex, anim, nowMs, baseValues)` (`:1717`).**
  Dispatch is `const fn = DRAW[group.kind]`. Extending that registry is the whole
  draw-path change; everything downstream — cell coverage, one-mark-per-graphic,
  `alignGroupsToRows`, `vizDiveTarget` — then works unmodified.
- **Call sites** are `render_page.mjs:668` and `render_page_movy.mjs:2436`. Both
  already pass a rect; neither needs to change.
- **`validate_contract.mjs`** gains validation for the custom-kind declaration.
- **`tools/param-pages/widget_sheet.mjs`** — the widget reference in
  `docs/MODULES.md` is generated between markers and pinned by
  `tests/host/test_widget_sheet.sh`, which also fails on an orphaned image. A new
  widget kind that does not appear in the generated sheet is a test failure, not an
  oversight.

---

## 8. Testing

- Unit tests for the frame-local ctx: a widget drawing outside its frame must
  produce **no pixels** outside it. Mutate the clip to prove the test can fail.
- **Frame-instability matrix.** Draw the same widget into every rect the two
  renderers can actually produce — movy's 32×15, each of `computeGeom`'s modes
  (`dial` at full radius, reduced radius, `bar-value`, `bar-label`, `bar-only`),
  and a span clamped by `Math.min(g.slotSpan, COLS - col)`. A widget must be legible
  or cleanly absent in all of them, never overflowing and never half-drawn. This is
  the axis the design exists to handle, so it gets asserted directly rather than
  inferred from a single-size snapshot passing.
- Sprite fallback: a nominal frame larger than the real frame must draw the
  **built-in** widget, not a scaled sprite. Assert no fractional scaling path exists.
- Unit test for one-strike disable: a widget that throws is called **once**, and the
  fallback widget draws.
- Unit test that the frame ctx exposes no read: asserting `getParam === undefined`
  is weak; assert instead that a widget attempting a read cannot obtain one.
- Label ownership: a page mixing custom and built-in cells must draw **all** labels
  through `drawLabelCell`, with no custom widget able to suppress or replace one.
- A snapshot in `tests/fixtures/snapshots/param_pages_viz.txt` covering a custom
  group, regenerated deliberately.
- Generator round-trip: PNG → RLE → drawn pixels must match the source PNG.
  **Compare by content, not bytes.**
- An example module exercising both `drawCell` and the fullscreen `draw` from one
  `canvas.js`.
- **Unknown `custom:` name falls through to the detector** (§10.1) — the same
  assertion covers a typo, a failed load, and an older host reading a newer module.
  Assert the *drawn result* is the detector's widget, not merely that no throw
  occurred.

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

## 10. Resolved

### 10.1 Naming — reuse `viz`, namespaced `custom:<name>`

`viz` is an **object**, not a string — `viz.mjs:213` returns early on anything that
is not one, and the declared form is `{ group, role, kind, span }`:

```json
{ "key": "attack", "name": "Attack", "type": "float",
  "viz": { "group": "amp", "role": "attack" } }
```

So the namespace goes on the **`kind` sub-field**, and a custom widget is declared
in exactly the shape a built-in one already is — single-param or grouped:

```json
{ "key": "drive", "name": "Drive", "type": "float",
  "viz": { "kind": "custom:mymeter" } }

{ "key": "x", "viz": { "group": "pad", "role": "x", "kind": "custom:xy" } },
{ "key": "y", "viz": { "group": "pad", "role": "y" } }
```

Not a new `widget` field.

One field means one question — *what picture is this?* — with one answer. A
separate field would let a param declare both a built-in `viz` and a custom widget
and leave the precedence between them undefined at the declaration site, which is
the sort of thing that gets resolved differently by two call sites a year apart.

It also lands in the right precedence slot. `viz.mjs:13`'s chain is *declared viz →
module layout file → host override → detector → none*, and `custom:` is the author
saying what this is, which is the **declared** slot. The "module layout file" slot
stays empty; this design does not build one.

**An unrecognised `custom:` name falls through to the detector**, exactly as any
unknown `viz` value does today. That single behaviour covers three cases at once —
an author's typo, a widget whose file failed to load, and an **older host reading a
newer module** — and because they share one code path, the forward-compatibility
story cannot rot separately from the typo story. A module that ships a custom
widget therefore still draws something sensible on a host that has never heard of
it.

`custom:` is a **reserved prefix**: no built-in kind may ever be named into that
namespace. Cheap to pin, and it keeps the two sets from colliding as built-ins are
added.

### 10.2 `ui_chain.js` modules — no, and the reason is structural

Not a policy choice. `enterComponentEdit` (`shadow_ui.js:13322`) tries the
**hierarchy editor first**, and only falls through to `loadModuleUi` →
`VIEWS.COMPONENT_EDIT` when the component publishes **no `ui_hierarchy`**.

So a module with a hierarchy never reaches its `ui_chain.js` at all, and a module
without one has no knob grid — hence no cells, hence nothing for `drawCell` to draw
into. The two paths are mutually exclusive by construction. Offering `drawCell` to
`ui_chain.js` modules would be offering it to a view that has no cells.

Worth carrying into sub-project 2: `ui_chain.js` is reached by **fewer** modules
than its prominence suggests — it is the fallback for the hierarchy-less, not a
peer of the grid. That further supports deferring it.

### 10.3 The generator lives in this repo

`tools/param-pages/`, beside `widget_sheet.mjs`, with a round-trip test under
`tests/host/`.

The argument is **lockstep, not convenience.** The generator's output format is
consumed by the runtime; if a runtime change breaks the format, that has to fail in
*this* repo's CI, not silently in a module author's build weeks later. `build.sh`
silently skipping the link sidecar is the fresh scar here — a build step that can
drift without failing defeats every bisect that follows it.

Distribution to authors is a separate and much easier problem: it is one `.mjs` run
under node, shippable in the release tarball alongside `shared/` if that turns out
to be wanted. Solving distribution by splitting the repo would trade an easy problem
for a version-skew one.

---

## 11. Still open

Nothing blocking. Items deliberately left to implementation:

- The exact `widgets.toml` schema for the sprite generator.
- Whether `drawCell` receives modulation base values (`viz_draw.mjs`'s
  `baseValues`) for a modulated param, or only the live value.
