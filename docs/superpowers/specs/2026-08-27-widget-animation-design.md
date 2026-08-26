# Widget animation — a design per widget

**Status:** design, nothing implemented. Successor to the SCH-50 animation set,
which was cut to four options and then deferred entirely.

## Why the catalog could not answer this

SCH-50 authored ten value-change motions and rendered them as frame strips. Six
were withdrawn before judging because a still strip renders *duration* as a
frame count — comparing `ease-4` with `ease-8` meant reading a number off a
chart, not feeling a motion. The remaining four got one judgement between them.

The conclusion was not "motion is unwanted". It was that **a catalog of
trajectories is the wrong unit**. Motion belongs to a widget, not to a value:
what should move when an enum changes is not what should move when a switch
flips, and neither is a curve morphing. So this document is organised by widget,
each with the thing that actually moves, what it costs, and how it fails.

## The one architectural cost, paid once

`render_page_movy.mjs` is **stateless**. Every frame is a snapshot computed from
the current values; there is no per-key store, so nothing can know what a value
was a moment ago or how long ago it changed.

Every item below needs the same thing: **a per-key frame store** — previous
value, and the tick it changed on. That is one change, and once it exists each
animation is cheap. It should be built once, deliberately, rather than smuggled
in with whichever animation lands first.

Two constraints it must respect:

- **The draw path is measured and tight.** A whole page render is ~1.68ms and an
  IPC read is ~2.8ms — a read costs more than redrawing the screen. The store
  must be fed from values the page ALREADY reads, never by reading more.
- **An idle page currently costs zero draws.** Anything that animates makes an
  idle page draw for the duration of every change. That is the real price, and
  it is why "instant" was in the catalog as the control.

---

## Enum square — the frame morphs to the value

**What moves:** the square's WIDTH. It is fixed at `ENUM_W` (28) today, sized
for the widest four characters, so `TRI` sits in a box built for `MONO` with
5px of dead air each side. Instead size the frame to the value plus its margin,
and animate between sizes when the value changes.

**Why it is the strongest of these.** It is the only one where the motion
carries information that is otherwise absent: a row of enums whose boxes differ
in width tells you how long each value is before you read any of them — exactly
the argument that won `half-strip` for the label band. The motion is a
by-product of a static improvement, which means it degrades well: if the
animation is ever switched off, the variable-width box is still better than the
fixed one.

**Geometry.** Interior = text + 2 (margin) + 2 (frame). Floor it at a width that
holds two characters so a short value still reads as a box rather than a slot,
and cap it at `ENUM_W` so it never grows past what the cell holds.

**How it fails:** every enum on the row animating at once when a page changes.
Only animate a change made ON THIS PAGE by a knob turn — a value that arrived
because the page itself changed should be drawn at its new size immediately.

---

## Waveform silhouette — morph between shapes

**What moves:** the curve itself, interpolated from the old shape to the new
over a handful of frames.

`drawWaveCell` already computes its `yAt` per column from `lfoShapeSample`.
Morphing is sampling BOTH shapes and blending: `yAt(x) = lerp(oldY, newY, t)`.
Nothing else changes — the stroke, the CHECKER mass and the parity assertions
all follow the same closure, which is what stops the fill and the line
disagreeing.

**Why it is worth having.** An LFO shape list is stepped through with a knob,
and the shapes are similar in silhouette — sine, triangle and swishy differ by a
few pixels at 13 rows. A morph makes the CHANGE legible even when the two states
are not easily told apart, which is the case a static render cannot help with.

**How it fails:** a morph between two very different shapes passes through
intermediate curves that are neither, so mid-flight it can read as a third
shape. Keep it short — 3 to 4 frames — so no intermediate is on screen long
enough to be read as a value.

**Prior art in the tree:** vimana's `AnimCurve` morphs 128 heights and captures
the CURRENT interpolated values when retargeted mid-flight, so a fast scroll
does not pop. Worth copying that specifically.

---

## Switch — the slug travels, and the flip bursts

**What moves:** two things, and they are separable.

1. **The slug travels** between its seats over 2–3 frames instead of jumping.
   The track is only 16px wide now, so the travel is ~7px — enough to read as
   movement, short enough not to feel slow.
2. **A burst on the flip**, the same 8-ray figure `drawButton` already draws:
   rays radiating from the slug's new seat, expanding over ~300ms.

**Reuse, do not reimplement.** `drawButton`'s burst is authored, tuned and
already carries the timing constants (`BTN_FLASH_MS`, `BTN_RAY_TRAVEL`). Lift it
to a shared helper and let both call it, or the two will drift and a switch
burst will stop looking like a button burst for no reason anyone can name.

**How it fails:** the switch is 16×9 and a burst wants room around it. Rays that
reach past the cell will clip against the neighbour, and the harness counts
clipped pixels but the device silently discards them. Bound the ray travel by
the CELL, not by the widget.

**Worth questioning:** a burst is a lot of ceremony for a two-state value that
already changes its whole ink on flip. The travel alone may be enough. Build the
travel first and judge the burst against it rather than shipping both together.

---

## Arc knob — nothing

Deliberately.

The pointer already moves continuously with the value; it IS the animation. A
knob is turned in a stream of small deltas, not discrete jumps, so easing means
the pointer lags the encoder — the one place in this UI where a control must
feel directly connected. `arc-short` won its set as the incumbent geometry with
one pixel changed, and the finding there was that the knob is not where this UI
needs work.

The only motion worth considering is on a value that changes **without** a turn —
a modulation source driving the parameter — and that already animates, because
the modulation dot is redrawn per frame from the live value.

---

## Label band — nothing beyond what `half-strip` already does

The strip is sized to the value, so it already grows and shrinks as the value
changes. Whether that reads as useful feedback or as fidget is the open question
recorded in the decisions doc, and it is a question about the STATIC design. Do
not add motion on top of an effect that has not been judged yet.

---

## Momentary button — it is now the tallest widget on the row

Not an animation note, but it came out of the same look.

Measured on a real page beside the widgets that just shrank:

| widget | size |
|---|---|
| switch | 16 x 9 |
| momentary button | **15 x 13** |
| enum square | 28 x 15 |
| arc knob | 17 x 15 |

The button is NARROWER than the switch and **4 rows taller**. Its height is
`BTN_RY` (3, cap) + `BTN_DEPTH` + `BTN_RY` (3, base arc), so the depth is what
would have to give. Against a flat 9px switch it reads as the heavy object on
the row — which it did not, when the switch was 24x11.

It was excluded from the SCH-50 catalog by the issue's own terms ("excluding
momentary buttons"). That exclusion was reasonable when nothing else had moved;
it is worth revisiting now that everything around it has.

---

## Suggested order

1. **The frame store.** Nothing else can start, and it should be built for its
   own sake with the draw-cost constraint in view.
2. **Enum square width.** Static improvement first, animation second — it is
   the one that is better even without motion.
3. **Switch travel.** Cheap once the store exists; judge the burst separately.
4. **Waveform morph.** Most visually interesting, least load-bearing.

Each of these is judgeable on hardware in a way the frame strips were not, which
is the whole reason for deferring them to here.
