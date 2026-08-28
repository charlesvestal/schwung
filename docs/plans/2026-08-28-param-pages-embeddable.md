# Offering the knob grid to modules

**Status:** design, in progress on `worktree-param-pages-embeddable`.

The goal: a module should be able to use Schwung's parameter UI instead of
writing its own, and a *tool* module — a sequencer — should be able to drive
those parameters from its own sequencer while showing them in its own chrome.

Movy is the first consumer and the reason the design is shaped the way it is.

## Movy is not a module missing a UI. It is the module ours was copied from.

The obvious framing — "modules like Movy can just embed Schwung's pages without
making their own UI" — is backwards for Movy specifically, and getting that
wrong would set the seam in the wrong place.

`src/shared/param_pages/` already credits schwung-movy's `hierarchy-walk.ts` for
the level-graph walk, the metadata inference fallback and the segmented page
indicator. Movy has a mature UI; we reimplemented its look as
`render_page_movy.mjs`. What the two repos actually have is ~5,000 lines of the
same derivation maintained twice:

| Movy | Schwung |
| --- | --- |
| `model/hierarchy-walk.ts`, `model/hierarchy.ts` | `page_plan.mjs` |
| `model/param-build.ts`, `model/enum-class.ts` | `param_meta.mjs` |
| `model/page-layout.ts`, `model/viewmodel.ts` | `page_plan.mjs` / `render_page.mjs` |
| `model/{filter,lfo,eq,cut,wav}-viz.ts`, `model/envelope.ts` | `viz.mjs` |
| `renderer/{knob,envelope,lfo-wave,eq-curve,filter-curve,cut-curve,wav-form}.ts` | `viz_draw.mjs` |
| `model/wav-peaks.ts`, `renderer/knob-leds.ts` | `wav_peaks.mjs`, `knob_leds.mjs` |
| `model/meta-retry.ts` | `page_controller.mjs`'s placeholder/contract retry |

**That last row is the proof the duplication already costs money.** Both repos
independently discovered that osirus publishes `rom_index: ["(loading)"]` and
`preset_count: 0` while it scans its ROM, and both independently wrote a retry
for it — Movy in `meta-retry.ts`, us at `page_controller.mjs:601` and
`page_plan.mjs:411`, citing the same module by name. Nobody was copying anybody.
The same bug was found twice, debugged twice and fixed twice, because the
derivation lives in two places.

So the thing worth sharing is the DERIVATION, and the thing worth keeping
separate is the PIXELS.

## Three layers, and a consumer takes as many as it wants

```
1. contract + controller   the walk, metadata, viz resolution, read scheduling,
   (shared)                write/announce throttles, retries, knob feel
        |
        v
2. VIEW MODEL              header left/right, bank bar, footer hints, per-cell
   (the published API)     { label, value, normalized, style, options, marks },
        |                  resolved viz groups
        v
3. renderer                render_page_movy.mjs draws that view model
   (optional)
```

- A module with **no UI** takes all three and gets today's grid for free. This
  already works — it is what the shadow UI does.
- **Movy** takes 1 and 2 and keeps its own 3.
- A tool that wants our drawing under its own header takes 3 with a `rect` and a
  band selection (below).

### Why the view model is the seam, and not the renderer

The first plan here was a full grid swap: Movy deletes `src/renderer/` and calls
`renderPageMovy`. It maximises dedup and it is the wrong trade.

- **It moves Movy's pixels.** Movy gates on pixelmatch screenshots
  (`browser-test/screenshot.mjs`). `render_page_movy.mjs` is a reimplementation
  of Movy's look, not a byte-identical copy, so a swap means a visual delta and
  a rebaseline the author has to accept.
- **It couples Movy's appearance to our release cadence** — every restyle we
  ship drags Movy's UI with it, forever. That is a maintenance burden we would
  be imposing on someone else's repo.
- **The renderer is the one layer where divergence is legitimate.** Movy wants
  its own look. Nothing goes wrong when two projects draw the same values
  differently; things go wrong when they *derive* them differently.

The view model gets the whole interaction model — the staggered read cursor, the
`setParam` write throttle during knob motion, the announce throttle, the
contract tri-state, the placeholder retry, precision mode, Mute-reset — and
leaves the pixels alone.

**Both projects independently converged on this exact shape**, which is the best
evidence available that it is the right join. Movy's `src/types/viewmodel.ts`
`ParamVM` against what we already compute:

| Movy `ParamVM` | Schwung |
| --- | --- |
| `shortName` | `labelForCell` |
| `fullName` | `meta.label \|\| meta.key` |
| `displayValue`, `normalizedValue` | `displayValue()`, `normalizedOf` |
| `renderStyle` | the cascade in `drawKnobWidget` (extracted — see below) |
| `options`, `enumIndex`, `isLongEnum` | `param_meta` enum handling |
| `modulated` | the `~` mark (`isModulatedCached`) |
| `EnvelopeVM`/`LfoVizVM`/`FilterVizVM`/`EqVizVM`/`CutVizVM`/`WavVizVM` | `resolveViz` groups |
| `bankIndex`/`bankCount`/`bankGroups` | `pageIndex`/`pageCount`/`pageGroups` |
| `headerOverride`, `rows[][]` | `title`/`pageLabel`, `page.keys` |
| `automated`, `automatable`, `assigned` | *Movy-only — this is `decorations`* |

## What has to be built

### `describePage()` — assembly, not new derivation

Most of the view model is already exposed piecemeal on the controller:
`knobRows()`, `knobListEntries()`, `vizGroups()`, `metaAt`, `keyAt`,
`isModulatedCached`, `pageLabel`, `pageIndex`, `pages`, `presetName`,
`diveTargetAt`, `metaIndex`. `describePage()` gathers them into one published
shape.

That it is assembly is the point: **nothing on the existing draw path changes,
so there is no pixel risk on the Schwung side.** The invariant to hold is that
the default render output stays byte-identical, and the snapshot tests are what
prove it.

Two rules it inherits and must not break:

- **No device reads.** Values come from `s.values`, filled by the staggered read
  cursor, exactly as the grid's do. A param read is ~2.8 ms against a 1.68 ms
  whole-page render, so reading here would cost more than the screen.
- **One reading of a value.** `displayValue` is reached by import, not
  reproduced — the same rule `knobRowValue` already follows, and for the same
  reason: a second formatter is invisible until someone declares a `unit`.

### `widgetKindFor()` — extract the rule, don't restate it

`renderStyle` is the one field that is a decision rather than a lookup. Today it
lives inline in `drawKnobWidget` as a cascade — `KIND_OPAQUE` → opaque box,
`meta.writeOnly` → button, `KIND_ENUM` → enum square, else knob — with viz
`covered[]` deciding upstream whether the cell is drawn at all.

Extract that cascade into a pure predicate and have **the renderer call it too**.
A copy of the rule in the view model that the draw does not consult is a second
definition, and this repo has paid for that shape before (`isDoor` is exported
from the controller for precisely this reason: "Ask the controller; do not
restate the kinds").

### `decorations` in the Movy layout

`page_controller.mjs:3338` currently refuses `decorations` and `rect` under
`LAYOUT_MOVY`. Movy is the p-lock consumer, so this is on the critical path for
layer 3 — though **not** for Movy itself if it renders the view model, since
`automated`/`assigned` are then just VM fields it draws its own way.

Note the existing rule to preserve: graphics stand down while decorations are
active, because a picture replacing several cells hides which of them is locked.

### Bands + a region ctx (layer 3 only)

For a tool that wants *our* drawing under *its* chrome. `renderPageMovy`
composes `drawHeader` / `drawBankBar` / `drawKnobRow`×2 / `drawFooter`, all
already separate exports, so band selection is small at that level. The
complication is that the geometry is module-level absolutes (`ROW0_Y = 9`,
`ROW1_Y = 33`) and a foreign header is not our height.

Rather than thread an origin through 2,500 lines: **translate and clip the
injected draw context**. Rule 5 of `README.md` — the draw context is injected,
`{ fillRect, print, textWidth }` — is what makes a decorator possible, and it
keeps the change to the composition root.

Clipping is not optional. The preview harness already reports pixels drawn
outside the display; a body drawn into a rect shorter than it needs must be
clipped rather than scribbling over the caller's chrome.

## Staging

Phase 0 is the whole Schwung side and is independently useful — it is a new
published API plus one extracted predicate, with no behaviour change.

0. **schwung** — `describePage()`, `widgetKindFor()`, decorations under
   `LAYOUT_MOVY`, bands + region ctx, a version marker on the shared lib.
   Snapshot tests prove the default path is byte-identical.
1. **movy fork** — build a `ViewModel` from `describePage()` behind a flag,
   keeping Movy's existing model alongside, and diff the two view models. This
   is the go/no-go evidence and it is cheap: comparing view models is a data
   diff, not a pixel diff.
2. **adjudicate** — for each difference decide whether Schwung or Movy is right.
   Fixes land in the shared lib. Expect traffic in both directions; Movy's
   `meta-retry` placeholder handling and ours are not identical.
3. **delete** Movy's duplicated derivation, PR with the diff report attached.
4. **document** `param_pages` as a supported embedding API in `docs/MODULES.md`,
   with the three layers and which one a given module wants.

## The versioning question this opens

Movy already imports `constants.mjs` and `input_filter.mjs` from
`/data/UserData/schwung/shared/` by absolute path, with
`external: ['/data/UserData/schwung/*']` in its esbuild config — so the
mechanism exists and the precedent is set. But `param_pages` is a far larger and
faster-moving surface than a constants file, and a module that imports it is
pinned to whatever host is installed on the device.

The view model needs a declared version and a compatibility rule before any
module outside this repo depends on it. Unresolved; it does not block Phase 0,
and it blocks Phase 3.

## Social note

Phase 3 is a large PR against someone else's repo that changes their
maintenance burden. Sound megadake out before writing it — the technical case
being good is not the same as it being welcome.
