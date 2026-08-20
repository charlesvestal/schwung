# Chain-editor knob card — design, 2026-08-20

Residual 2.1 from `docs/plans/2026-08-20-chain-handoff.md`.

> *"fix the overlay from the chain editor, so those knobs have a better view:
> either show the actual control (or row) or the whole UI panel, or something,
> so it's not just the value - we have so much more we can show."*

Turning a knob in the chain editor raises `menu_layout.mjs`'s 120x28 box, which
says a name and `Value: 0.62`. The renderers to do better already exist and the
chain editor uses none of them for knob feedback.

---

## 1. What it is

In `VIEWS.CHAIN_EDIT`, **touching** a knob raises a bordered card over the
diagram. The card carries an inverted header band — parameter name left,
formatted value right — and beneath it **the four cells of that knob's row**,
drawn with the real `render_page_movy` widgets. The touched cell inverts and
shows its value, exactly as it does on the knob grid. An enum gets the enum
square and a viz group still resolves across its span. Let go and the diagram is
back.

The per-cell modulation marks are **omitted for the neighbours**, and that is a
budget decision, not an oversight: `isHierarchyParamModulated` is one to three
IPC reads per key, so marking all four cells would cost up to twelve reads —
~34 ms of input latency — for a 4x2 tilde. The **touched** knob keeps its mark,
because `showKnobOverlay` already pays for that read today to put a `~` on the
title.

Two heights:

- **full card** — a component is selected, so there are eight declared knobs
  and the touched one has a row to sit in.
- **short card** — nothing is selected, so the knobs drive the slot's global
  mappings. Those serve a name and a value but no type/min/max, so there is no
  control to draw: header band only.

The whole-panel alternative was built and rejected as jarring; a bare value
strip was rejected as not answering the ask.

## 2. Geometry

The frame is **2px solid border, 1px black gap, then the header band**, with a
2px black gutter cleared outside the border.

The gap is load-bearing, not decoration. The header band is white and so is the
border, so where they touch the border stops existing — a short card with no gap
reads as one fat stripe laid across sliced-off diagram boxes, with no left,
right or top. The gap is the only thing the band cannot eat. Any future change
to this frame has to keep a black row between any white border and any white
fill inside it.

```
gutter (clear)   x 1..126          y 10..51   (full)   y 22..40 (short)
border   2px     x 3..124          h 38       (full)   h 15     (short)
interior         x 5..122
gap      1px
content          x 6..121  (116 wide)
```

Rows, top to bottom: `border 2 + gap 1 + header 9 + gap 1 + row 22 + gap 1 +
border 2 = 38`. Centred in the body band (y 8..54 — 47 rows between the screen
header and `RULE_Y`), so the full card is y 12..49. The short card drops the row
and its gap: `2 + 1 + 9 + 1 + 2 = 15`, centred at y 24..38.

Content is 116 wide, so the row's four cells are **29px**, not the grid's 32.
Everything in a cell still fits: the arc knob is 17 (`KW`), the enum square 20
(`ENUM_W`), and a label is budgeted in characters at `LABEL_CHARS = 4`, about
23px of `font4x5`.

Header band text uses the device `print` (5x7), as today's overlay does, with
the **name** truncated when name and value would collide — never the value.

## 3. Three pieces

### 3.1 `render_page_movy.mjs` — parameterise the cell geometry

Row drawing derives every x from a module-level `CELL_W = 32`, at eleven sites
across `drawKnobWidget`, `drawLabelCell` and `drawKnobRow` (all of the form
`col * CELL_W`). Thread a geometry `{ x0, cellW }` through those three,
defaulting to `{ 0, CELL_W }`, and export the row entry point as
`drawKnobStrip(ctx, o, { x0, cellW, rowY, lblY, row })`.

`renderPageMovy` keeps calling it with the defaults and **its output must be
byte-identical afterwards**. That is an invariant with its own test (§6), not a
hope: this is a screen that was tuned deliberately and recently.

### 3.2 `src/shared/param_pages/knob_card.mjs` — new, pure

```
export const CARD_X, CARD_W, BORDER_W, GAP_W, HEADER_H, GUTTER
export function knobCardRect(hasStrip) -> { x, y, w, h }
export function drawKnobCard(ctx, o)
```

`o` carries `{ name, value }` and, for the full card, `{ keys, metaIndex,
values, touchedCol, viz, modulated, modValues }` — the same shapes
`renderPageMovy` already takes. No param I/O, no state, no screen ownership;
draws through the injected ctx only, the same contract as every other module in
this directory. That is what lets it render headlessly into
`tools/param-pages/harness.mjs`.

### 3.3 `shadow_ui.js` — the wiring

The only part that needs the shadow UI:

- **Touch state.** Capacitive knob touch arrives as note-on/note-off on notes
  0–7 (`MoveKnob1Touch`..`MoveKnob8Touch` in `shared/constants.mjs`), and
  `shadow_ui.js` already handles both edges around line 17590/17636. Track which
  knob is held; last touch wins.
- **Row resolution.** `getKnobContext(i)` already resolves each knob's key and
  meta from `cachedKnobContexts`; the card needs the four in the touched knob's
  row.
- **Value cache.** See §4.
- **Draw.** One call at the end of `drawChainEdit`, after `drawMovyFooter`.
- **Announce.** `showOverlay` calls `announceParameter` on content change; that
  call moves with the card, keeping the same "only when content changed" guard
  so it does not spam D-Bus per frame.

The `showOverlay` call is diverted **only** when `view === VIEWS.CHAIN_EDIT` —
the same knob-handling code serves `HIERARCHY_EDITOR`, which keeps today's
overlay.

## 4. Read budget

The rule from the handoff: an IPC read is ~2.8 ms and a whole page render is
1.68 ms, so a read costs more than redrawing the screen.

**On touch-down**, read the (up to) four row values once — ~11 ms, one-off, on
an input event and off the draw path. After that the turned knob's value is
updated by local JS math, which is the existing `knobValueCache` pattern, and
the neighbours hold still. **Zero IPC per frame while the card is up.**

The cost: a neighbour being driven by an LFO will not animate while you hold a
knob. That is the right trade — the alternative is four reads a frame, 11 ms out
of a 16 ms budget, to animate a value nobody is looking at.

## 5. Lifecycle and edges

- **Raise on touch, drop on release.** No timer in the normal case.
- **Turn with no touch** (a knob whose cap sensor missed) raises the card anyway
  and decays after a short pause, so a flaky sensor cannot strand the feature.
- **No module loaded, or the touched knob is past the end of the level's
  `knobs`** — short card reading `not mapped`, the same words as today.
- **Scope is `CHAIN_EDIT`.** Master FX and the hierarchy editor keep today's
  overlay; residual 2.2 revisits Master FX.

## 6. Testing

Three of the four bugs in this branch's history shipped with a green suite, each
correct along the axis being tested. So the invariants are asserted separately
from the behaviour:

- **Pixel (behaviour).** Card fits its rect, `fb.clipped() === 0`,
  `missingGlyphs` empty — across float, int, enum, opaque, modulated, viz-group,
  and the short card.
- **Frame (invariant).** There is a black row between the border and any white
  fill inside it, asserted on the pixel buffer rather than by eye. This is the
  bug the design exists to prevent, and it is invisible in code review.
- **Inertness (invariant).** `renderPageMovy` with default geometry is unchanged
  against the existing snapshots. The `CELL_W` parameterisation must be provably
  inert on the default path.
- **Read budget.** Drawing the card costs **zero** `getSlotParam` calls, in the
  style of `tests/host/test_chain_edit_read_budget.sh`.
- **Hardware.** All eight knobs on a synth, an audio FX, a MIDI FX, and with
  nothing selected; plus the screen reader still announcing on change.

## 7. Risks

- **The `CELL_W` threading touches a shared renderer.** Mitigated by the
  inertness test, which is the reason it exists.
- **An unserved param key reads back as `""`, not an error** — the handoff notes
  this has already caused two silent bugs. The value cache must distinguish
  "not yet read" from "read as empty".

## 8. Preview

`tools/param-pages/preview.mjs` renders the real widgets through the device font
atlas. The mockups this design was chosen from were generated that way and the
card should be previewable the same way, so the frame can be judged without a
deploy.
