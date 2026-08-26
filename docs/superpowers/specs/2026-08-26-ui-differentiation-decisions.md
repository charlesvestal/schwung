# SCH-50 — the decisions

Charles's picks, control by control, recorded as he made them. The Bradley-Terry
rankings in `2026-08-26-ui-differentiation-preferences.json` are an input to
these, not the same thing: at ~17 judgements over 10 options **no option in any
set separated from its leader at 95% confidence**, so the numbers are a
directional read and the reasoning below is the decision.

Where a pick disagrees with the fitted ranking, that is recorded rather than
smoothed over.

---

## Enum square — `thin-frame` (option 01)

**Fitted rank:** 4th of 10 (`dotted-frame` 1st, `dither-ground` 2nd).
**Decision:** the incumbent-adjacent option — the current 1px frame with
tighter padding.

Effectively a no-change vote for this control. Defensible on the merits: an
enum square is a framed box containing a word, which is about as generic as a
widget gets, so there may be nothing here that carries a house style worth
changing.

### Grammar constraint discovered while deciding this

Enums **are divable but deliberately unmarked**, and the split is load-bearing
(`param_meta.mjs:242-245`):

```js
meta.divable_mark = opaqueType;
meta.divable = (opaqueType || listableEnum) && !meta.readOnly && !meta.writeOnly;
```

Every enum with declared options opens a picker on jog-click. But the corner
brackets key on `divable_mark`, which is opaque types ONLY — roughly 135 enums
against 5 opaque params, so bracketing every enum marks every cell on every
page, which is the same as marking none. An enum's affordance is the footer,
which flips to `CLK OPEN` off `divable`.

**Consequence: option 04 `bracket` is ineligible, independent of taste.** Corner
brackets already mean "opaque value, dive in", and `drawOpaqueBox` has no frame
of its own — the brackets ARE its frame, pinned on the pixel buffer by
`tests/host/test_knob_card.sh`. An enum wearing them either reads as an opaque
cell or dilutes the mark across the whole fleet. It ranked 8th, so it was never
a contender; it is recorded here as ruled out by grammar rather than by
preference, because 8th place is a weaker reason than the real one.

**Cross-set check:** whatever the enum square wears must stay distinct from the
opaque cell. `thin-frame` (framed) against the opaque winner `bracket-underline`
(bracketed) keeps the two grammars separate. The pairing that would NOT have
worked is a framed enum against a framed opaque cell — e.g. `dotted-frame` with
`frame-cell`.

---

## Fader — `outline-fill` (option 06)

**Fitted rank:** 1st of 10, 4–0. The strongest fader result.
**Decision:** taken, with a known collision accepted.

A notched box with a `DIAG_HEAVY` interior over dashed rails.

### The accepted collision

`outline-fill` borrows the page's box vocabulary, so a fader and an enum square
stop being separable by silhouette — both are framed boxes. Its own note
predicted this before any judging; picking `thin-frame` for the enum square
(above) made it live rather than hypothetical, and the in-context render of an
osirus page shows four framed faders beside a framed `MODE`/`POLY` cell.

**Accepted deliberately.** The escape was available and declined: `dither-ground`
was the fitted #2 enum square and carries no frame at all, which would have
dissolved the collision at the cost of the enum decision. Charles kept both.

Consequence for implementation: on a page mixing continuous faders with enums,
the two are distinguishable by CONTENT, not by shape. If that turns out badly on
hardware, the cheap fix is the enum square, not the fader.

### Rejected alternative

`stepped` (option 07, fitted 5th) was the other candidate and stays distinct
from everything on the page. It was declined for what its note already recorded:
six 2-row blocks over the full range means **five detents in six move nothing on
screen**, which is fine for a level grabbed roughly and bad for anything nudged.

---

## Footer band — `no-rule` (option 02) — LOCKED

**Fitted rank:** 2nd of 10 (`dotted-rule` 1st, by a margin well inside the
interval).
**Decision:** locked during judging and confirmed afterwards.

The rule is removed entirely; the hints sit in the space with nothing above
them.

Confirmed explicitly because option 03 is `dotted-rule` — which HAS a rule, just
a light one — and "the one without the rule" pointed at 02 while the number
pointed at 03. The description won.

Consistent with the pattern across the other picks: **texture inside widgets,
minimal chrome around them.** This is the most minimal option in the set, chosen
for the structural element, while the fader and viz picks all take fills.

---

## Label band — `half-strip` (option 07) — LOCKED

**Fitted rank:** 4th of 10, on only 7 judgements — the thinnest set in the
catalog.
**Decision:** locked during judging, confirmed afterwards, with a reason.

*"I like the rounded edge in the selection."* That is `notchCorners` on the
inverted strip — the convergent 1-bit idiom recorded in the spec as a keeper
(§1.3), chosen here on its merits rather than as a default.

The strip is sized to the value plus two pixels each side rather than spanning
the cell, so a row of held knobs becomes blocks of different widths and `ON`
stops claiming the same 32px as `-24.0`.

**Open question its own note raises and a still cannot answer:** the shape MOVES
as the value changes, so turning through 9 → 10 → 11 makes the block grow and
shrink under your finger. Useful feedback or visual noise. Worth watching on
hardware.

**Font constraint:** budget 26px — overflows `wide` and `dot-matrix`. Neither is
the font pick (see below), so this is currently satisfied.

---

## Opaque cell — `door-open` (option 08)

**Fitted rank:** 2nd of 10, 4–0. `bracket-underline` was 1st by a margin well
inside the interval.
**Decision:** taken for the chevron.

### What an opaque cell actually is

Worth recording because it is rarer than it looks. `param_meta.mjs` defines
opaque as **"a knob cannot drive this at all"** — filepaths, strings, canvases,
plus `wav_position`. That is roughly **five params in the entire fleet**, against
~135 enums. Sample paths, file names, granny's waveform scrubber. Opening an
editor is the only thing to do with one.

So this decision affects very few cells, which is part of why dropping the
brackets is tolerable here.

### The reasoning

Charles picked out the affordance MARKER in three different options — the
ellipsis in 05, the chevron in 08, the lattice in 03 — rather than any frame
treatment. For a cell whose whole job is to be a door, marking the door
explicitly is arguably the right instinct.

`door-open` is a notched frame with its right edge cut away and a chevron in the
gap. It is the only option that says which DIRECTION the door goes, and the
broken edge means it can never be confused with an enum square however the
widths fall — which matters now, because the enum square pick (`thin-frame`) and
the fader pick (`outline-fill`) are both framed boxes.

### Accepted costs

- **It drops the divable brackets** and supplies its own frame. Safe —
  `drawOpaqueBox` has no frame of its own and the brackets ARE it
  (`test_knob_card.sh` pins this), so an option that removes them must replace
  them, and this one does. The consequence is that opaque cells stop being
  marked the way the codebase marks divable things. At ~5 params, acceptable.
- **A 1px pictogram either reads instantly or reads as debris**, and its note
  says plainly that this is not decidable from code or from a 4x render. This
  is the single pick in the catalog most likely to be reversed on hardware.

### Rejected, with reasons worth keeping

- **05 `frame-header`** (ellipsis) — states the affordance most explicitly, but
  spends four of fifteen rows on the lid, so the value sits low and it is the
  heaviest cell on the page.
- **03 `bracket-lattice`** — the only one of the three that KEEPS the brackets,
  so it stays consistent with how everything else divable is marked. Rejected on
  its own note: `DOTS(2)` is a lattice rather than a hatch, so it shows its
  phase — whether a cell's dots land on its corners depends where the cell sits,
  and four cells in a row will not agree.

---

## Envelope, filter, LFO — `ghost-fill` (option 05)

**Fitted rank:** 1st in ALL FOUR curve sets. The single strongest result in the
catalog — four sets judged independently, same winner each time.
**Decision:** taken for envelope, filter and LFO.

A stroke over a `CHECKER` fill to the baseline.

The four sets share one treatment implementation by construction (the same
function objects are registered in each), so choosing one treatment for three of
them costs nothing and keeps the page coherent.

**The current treatment, `thin-stroke`, ranked 10th / 8th / 7th / 8th across the
four.** The thing shipping today was near-bottom everywhere it appeared. For an
issue about differentiating from Elektron, that is the useful finding: the
current look is not merely derivative, it is not liked.

---

## Sample — `solid-mass` or `terrain` (UNDECIDED)

**Fitted rank:** `ghost-fill` 1st, `solid-mass` 2nd (up 7 places), `terrain` 6th.
**Status:** open — Charles named two candidates and did not settle.

Sample is the one curve set where the axis means something different, and the
option notes already record why:

- **`solid-mass` is what SHIPS for sample.** `drawSample` is solid today, so
  position 9 is the incumbent here and position 1 is the radical option — the
  axis is inverted relative to every other set. Choosing it is a no-change vote.
- **`terrain` came 2nd in envelope, filter and sample but 8th in LFO**, because
  the LFO's zero line runs through the middle rather than along the bottom, so
  "mass below the curve" means something different there. Sample is mirrored
  about a centre line too, which is why its terrain note warns that a QUIET
  sample puts seven of thirteen rows under hatch and the cell renders as a slab.

**Recommendation when this is picked up:** `terrain` differentiates and
`solid-mass` does not, but `terrain`'s failure case is the common one for
samples — quiet material — and it is the heavier of the two. Worth rendering
both against a real quiet sample before deciding.

---

## Switch — `pill-inverted` (option 03)

**Fitted rank:** 1st of 10, 5–0.
**Decision:** taken, and it resolves a tension worth recording.

`movy-sprite` — the pixel-for-pixel Movy port, and the most directly-derived
widget in the fleet — ranked **2nd**, also 5–0, separated from the winner by
0.05 log-strength. On the one control where the resemblance is most concrete,
the incumbent is genuinely well-liked.

The switch set is also the ONLY set where the authored minimal → radical axis
predicted preference (Spearman +0.68). Everywhere else it was near zero or
negative.

`pill-inverted` being the winner means the differentiation here is free: it is
independently authored, it beat the port on its own merits, and no legibility
was traded away to get there. Had the port won outright, this would have been a
real cost to weigh against the legal read.

**Note from authoring:** options 2 and 3 needed a fix before they worked at all
— the slug sat 1px from the wall and 8-connected to it, so OFF read as "the left
half of this box is thick" rather than as a slug in a track, and both seats drew
the same picture. Inset to 3px with an 8x7 slug. That fix is why this option is
legible enough to win.
