# UI differentiation catalog (SCH-50)

**Status:** design accepted 2026-08-26. Catalog built; the derived assets this
document identifies have since been REPLACED — see
`2026-08-26-ui-differentiation-decisions.md`. **This is a historical record of
the assessment, not a description of the current code.** Every quotation from
the source tree below is quoted as it stood *before* that work; the files no
longer read that way.
**Branch:** `charlesv/sch-50-differentiate-the-ui-and-icons-from-elektron`
**Issue:** SCH-50 — *Differentiate the UI and icons from Elektron*

This issue produces a **rendered option catalog and a ranked preference
dataset**, not a change to the shipping UI. Nothing in
`src/shared/param_pages/` that the device draws today is modified.
Implementing the preferred options is a separate follow-up.

---

## 1. Assessment

The issue asks to separate what is inherent to the product from what can be
differentiated. The code splits three ways, not two, and the middle category is
the one that matters and the one the issue does not mention.

### 1.1 Inherent — the shape is the maths

Filter curves, ADSR envelopes, LFO waveforms, sample waveforms. These are
pictures of what the DSP is doing. An ADSR that does not look like an ADSR is
wrong, and the shape of an exponential decay is not anyone's property.

They are still differentiable in **treatment** — stroke weight, whether the
mass under the curve is filled and how, where the baseline sits, how endpoints
are marked. That is a real axis: a stroked outline and a terrain fill of the
same curve read as different products.

**Constraint:** viz options vary treatment only. The curve maths is untouched.
This is enforced mechanically (§3.4), not by discipline.

### 1.2 Derived assets — the concrete exposure

Not in the issue's list, and the most consequential finding of the assessment.

> **Resolved.** Both items below have been replaced: `font4x5.mjs` now carries
> the metric-matched redraw (all 57 characters, no form carried forward), and
> the movy vertical rhythm was re-cut against our own panel. The quotations
> that follow are the *evidence as it stood*, retained because the finding is
> what justified the work. They no longer describe the source tree.

**`src/shared/param_pages/font4x5.mjs`** — nine glyphs (`A D E I L M P T U`)
are Elektron letterforms. The file says so itself:

> Measured off Elektron's own UI rather than guessed. Recovering their 128x64
> screen from a 4x screenshot and segmenting the header text gives: [...]
>
> Letterforms for A M I T U D E P L are Elektron's, read straight off the
> screen. The rest are authored to match their weight and construction.

Each of the nine carries an inline `/* (Elektron) */` annotation in the glyph
table. The remaining letters are not independent either — they were authored to
match the nine, so the resemblance is systemic rather than confined to the
copied bitmaps.

**`src/shared/param_pages/render_page_movy.mjs`** — the vertical band rhythm
(`:388`) and the arc geometry (`:726`–`:744`, `ARC_START_DEG = 230`,
`ARC_SWEEP_DEG = 260`) are documented as measured off Elektron's screen,
"recovered from a 4x capture".

This is reproduction of specific expression rather than stylistic similarity.
It is a different order of concern from a knob that merely looks Elektron-ish.

**This document is an engineering assessment, not legal advice.** The
distinction drawn here — copied asset versus evoked style — is offered so a
lawyer has something concrete to look at, not as a conclusion about liability.

**Consequence for scope:** restyling knobs addresses the visible half while the
concrete half ships unchanged. This is why font is in the catalog (§2) despite
not being in the issue's bullet list.

### 1.3 Convergent idioms — resemblance that is not evidence

Some elements look Elektron-ish because **the constraint admits one sensible
answer**, and everyone working at 128x64 and 1 bit arrives at it independently.

The clearest case is the **1px corner notch**. Elektron uses it. vimana uses it
and calls it out as its signature move — *"on a 1-bit display this single
missing pixel transforms a harsh box into something that feels designed."* It
recurs across 1-bit UIs generally. That is not imitation; it is what happens
when you have one pixel and two colours and a filled box reads as harsh. There
is no second way to round a corner at this resolution.

Others in the same category: a dotted rule as a light separator (a solid rule
at 1 bit is too loud, and 50% is the only lighter option that stays even), an
inverted bar as the selection state (there is no third colour), a downward
triangle as a pointer.

**These are kept, deliberately.** Removing a functional idiom because a
competitor also arrived at it makes the product worse and improves nothing:
convergent solutions are weak grounds for a claim precisely because the
constraint, not the designer, produced them. It is a different kind of thing
from nine glyph bitmaps read off a screenshot.

Practical consequence for the catalog: corner notches appear **across** options
in several sets rather than being isolated as one "notched" option to accept or
reject. The choice being offered is what the notch is applied to, not whether
the product is allowed to have rounded corners.

### 1.4 Stylistic resemblance — the issue's list

Arc knob, fader, bottom fills, label strips, animations. Independently authored
code that evokes a house look. Real, and the bulk of the catalog.

### 1.5 Documented residual

The catalog is **widget-internal only** (§3.2, and see §1.3 for what is
deliberately kept): options change what is drawn
inside a cell, never where cells are. Therefore **the recovered band geometry
survives this issue unchanged** — `ROW0_Y=10`, `LBL0_Y=25`, `ROW1_Y=33`,
`LBL1_Y=48`, `HEADER_H=7`, `BOX_H=15`, `LBL_H=7`, `RULE_Y=55`, `FOOTER_Y=56`,
`FOOTER_H=8`, `CELL_W=32`, `KW=17`, `ENUM_W=20`, `VIZ_ROWS=13`.

This is a deliberate scope decision, recorded so it is not mistaken for an
oversight. Re-cutting the grid is a separate piece of work with a real test
cost (§3.5).

---

## 2. Catalog structure

**Thirteen sets, ten options each — 130 options.**

| # | Set | Unit |
|---|-----|------|
| 1 | Arc knob | widget draw fn |
| 2 | Fader / level | widget draw fn |
| 3 | Bottom-area fills | footer rule + hint band |
| 4 | Enum square | widget draw fn |
| 5 | Label cell | label + touched value strip |
| 6 | Opaque box | widget draw fn |
| 7 | Envelope viz | treatment only |
| 8 | Filter curve viz | treatment only |
| 9 | LFO viz | treatment only |
| 10 | Sample viz | treatment only |
| 11 | Switch viz | widget draw fn |
| 12 | **Font** | complete 4x5 replacement set |
| 13 | **Value-change animation** | behaviour spec + frame strip |

Momentary / trigger buttons are excluded per the issue.

### 2.1 The minimal → radical axis

Every set's ten positions are ordered and the ordering means the same thing in
each set, so "option 7" is a comparable distance in all thirteen. This is what
makes a consistent cross-set pick possible.

- **1–3 — re-cut.** Silhouette survives, construction changes. Stroke weight,
  pointer length, inset, rim treatment. An existing user would not be startled.
- **4–7 — restated.** Same information, different idiom. Arc becomes stipple,
  tick ladder, segmented ring, open-gap indicator. Same control, visibly not
  the same house.
- **8–10 — replaced.** Different visual language, including non-rotary answers
  for the knob. This is where the vimana vocabulary lands (§2.2).

### 2.2 Reference: vimana2-rust

The issue cites `~/github/vimana2r-rust`. The repo is
`/Users/charlesvestal/github/vimana2-rust`, and it has **two** renderers. The
relevant one is **not** `crates/vimana-port-app/src/gfx/knob.rs` — that is a
640x480 RGBA/GL path. The reference is
**`crates/vimana-app/src/display/`**, an actual 128x64 1-bit UI, with a written
design language at `docs/plans/aesthetic-reference.md` (repo root).

Transferable vocabulary:

- **Dither ladder with fixed semantics** — Solid / DiagHeavy 75% / Checker 50%
  / DiagLight 25% / Dots ~12%, mapping to active / emphasised / muted / hint /
  reference. Predicates are evaluated in **screen space**, not rect-relative,
  so patterns stay phase-continuous across adjacent shapes and do not shimmer
  when a shape moves.
- **50% dotted rules** for every separator, border and slider track. The single
  most characteristic element of that UI.
- **1px corner notches** on every box. *"On a 1-bit display this single missing
  pixel transforms a harsh box into something that feels designed."*
- **Dotted track + solid 3x3 thumb** for continuous values; hollow-vs-filled
  3x3 dots for discrete.
- **Fill = selected, outline = editing** as a two-state affordance needing no
  extra glyphs.
- **5-3-1 pixel triangles** tethering a footer back to its owning cell.
- **Terrain fill** — solid 1px crest line over an `(x+y)%3` diagonal hatch,
  filled down to the bottom edge, so the shape reads as mass rather than line.
  This is the "bottom-area fill" idiom the issue points at.

Also worth noting: vimana **has no rotary dial on its 1-bit display at all**.
Knob parameters render as linear cells. Options 8–10 of set 1 explore what that
looks like in our grid.

### 2.3 Two sets whose unit needs stating

**Font (set 12).** Replacing only the nine copied glyphs leaves forty-seven
letters authored to match them. Each option is therefore a **complete 4x5
replacement set** derived from its own construction rules. Ten complete fonts
is the largest single piece of work in the catalog. Glyph format stays
`[advance, yOff, w, h, ...rowBits]` with bit0 = leftmost, identical to
`font5x3.mjs`, so any option blits through the existing path unchanged.

**Animation (set 13).** The render layer is stateless — every frame is a
snapshot and there is no per-widget store to ease a value through. Each option
is therefore a **behaviour spec plus a rendered frame strip** showing the
reaction to a value step. Motion is judged from the strip. Giving the renderer
frame state is an architectural change and belongs to the follow-up
implementation issue, not to a catalog.

---

## 3. Construction

### 3.1 Style module

`src/shared/param_pages/styles/` — one file per set (`knob.mjs`, `fader.mjs`,
`fills.mjs`, `enum_square.mjs`, `label_cell.mjs`, `opaque_box.mjs`,
`viz_envelope.mjs`, `viz_filter.mjs`, `viz_lfo.mjs`, `viz_sample.mjs`,
`viz_switch.mjs`, `font/`, `anim.mjs`).

Each file exports an ordered array of ten:

```js
{ id, name, position, note, kind, ...payload }
```

`position` is the 1–10 rank on the axis. `note` is the one-line rationale that
appears in the catalog. `kind` selects the payload shape, of which there are
three:

| `kind` | Sets | Payload |
|---|---|---|
| `draw` | 1–11 | `draw(ctx, ...)` — **same signature as the draw function it could replace** |
| `font` | 12 | `glyphs` — a `[advance, yOff, w, h, ...rowBits]` table, `CHARS` order, identical encoding to `font5x3.mjs` |
| `motion` | 13 | `frames(valueFrom, valueTo, n)` → array of value/offset states, plus a prose `behaviour` |

The `draw` kind takes the existing `ctx` (`fillRect`, `line`, `print`,
`drawArc`) and no tool-only contract. That is what makes a winning option a
drop-in rather than a rewrite, and it means the catalog renders the real thing
rather than an approximation. `font` and `motion` are data and cannot be
drop-in draw functions — see §2.3 for why each is shaped that way.

### 3.2 Geometry envelope

**Widget-internal only.** Options draw inside the existing cells and may not
move them. Consequences: nothing pinned moves (§3.5), the catalog composites
into real pages immediately, and any pick is a low-risk swap. Cost: the
recorded residual in §1.4.

### 3.3 Catalog tool

`tools/param-pages/catalog.mjs`, built on the existing harness
(`createFramebuffer` → `drawContext` → `toPng(scale, invert)`).

Per set it renders:

1. **Swatch grid** — ten options side by side at 4x on a blank field, for
   comparing construction.
2. **In-context page** — each option composited into a real module page at true
   size, with seven neighbours, a modulation dot present, and a touched label
   strip present.
3. **Contact sheet** — the two above stacked and labelled, one PNG per set.

Animation renders a horizontal frame strip in place of the single page.

**The in-context render is not optional.** Reviewing isolated widgets is
precisely what previously let seven real defects through while text art passed
three rounds. An arc that reads cleanly on a blank field can collide with the
modulation dot or the label strip once it is in a 32px cell.

### 3.4 Enforcing the inherent-shape constraint

The five viz sets all render from an **identical height array** supplied by the
tool. A viz option physically cannot alter the curve, only its treatment. This
is asserted in the test below rather than left to authorial discipline.

### 3.5 Test surface

Production draw code changes only by the seven added `export` keywords (§3.6),
so **nothing pinned moves**:
`movy-geom-baseline.txt`, `test_enum_picker_chrome.sh`,
`test_chain_edit_read_budget.sh`, `test_knob_card.sh`,
`test_master_fx_diagram_fit.sh` all stay green unmodified.

One new test, `tests/host/test_style_catalog.sh`:

- every set exports exactly ten options
- every option has a unique `id` and a `position` covering 1..10 exactly once
- every option declares a `kind`, and its payload matches that kind
- `kind: "draw"` — arity matches the function it shadows
- `kind: "font"` — glyph table covers the full `CHARS` set, every entry
  well-formed, and **no entry is byte-identical to `font4x5.mjs`**, which is
  the whole point of the set
- the five viz sets render from the identical height array

Note for whoever writes it: the node script inside a `tests/host/*.sh` is a
single-quoted bash string, so **no apostrophes** in any comment or message
inside it.

### 3.6 Fidelity, and the one production change

**`drawArc` is not a risk.** An earlier draft of this spec claimed the harness
renders arcs through a JS fallback that could diverge from the device. That is
wrong. `drawContext` in `harness.mjs` implements `drawArc` as a deliberate
replication of `js_display_draw_arc` — the same union of one pixel per row and
one per column at a distance-rounded radius, carrying the same comment about
why neither a fillCircle difference nor a midpoint walk is correct. The harness
header states the intent plainly: *"Previews are therefore pixel-identical to
the OLED, not an approximation — which is the whole point: layout decisions
made here hold on hardware."* Arc-based options need no special device check
beyond what any option needs.

**One additive production change is required.** The widget draw functions are
module-private:

| Exported | Not exported |
|---|---|
| `drawKnobRow`, `drawFooter`, `renderPageMovy`, `drawHeader`, `drawBankBar`, `drawBrackets` | `drawArcKnob`, `drawEnumSquare`, `drawLabelCell`, `drawOpaqueBox`, `drawKnobWidget`, `drawModDot`, `centeredText` |

The catalog needs them for two things: rendering the **current** widget as the
baseline every option is compared against, and substituting an option into a
real page. The alternative — rendering a full page then clearing and
overdrawing the widget rects — is fragile and would silently drift from what
the page actually draws.

So the plan adds the `export` keyword to those seven functions and nothing
else. No behaviour change, no signature change, no call-site change; every
pinned test is blind to it. This is a deliberate, bounded exception to
"production untouched", recorded here rather than slipped in.

### 3.7 Output policy

130 options x 2 renders is ~260 PNGs — too much binary for the repo.

- **Committed:** the spec, the thirteen contact sheets, `styles/`, the catalog
  tool, the A/B server, the ranking script, the new test, and
  **`preferences.json` once judging is done** — the dataset is the point of the
  exercise and belongs in the repo.
- **Not committed:** full render output, to a gitignored `catalog-out/`.

`preferences.json` lives in `catalog-out/` while it is being written and is
copied to `docs/superpowers/specs/2026-08-26-ui-differentiation-preferences.json`
when the run finishes, so the ignored directory stays purely derived output.

---

## 4. Deliverable

`docs/superpowers/specs/2026-08-26-ui-differentiation-catalog-design.md` (this
file) gains a per-set catalog section as the sets are built: contact sheet
inlined, ten options named with axis position and rationale, plus a note on
that set's inherent constraints.

### 4.1 How to choose — pairwise A/B, not a grid

Picking one from a 10-up contact sheet is a bad ask: ten-way choice is hard,
and it yields a single pick with no information about anything else. **Review
is pairwise instead**, producing a ranked preference dataset per set.

This also demotes the minimal → radical axis from an assumption to a
hypothesis. The axis orders the options as authored; the preference data can
contradict it, and that disagreement is itself a finding.

**Tool:** `tools/param-pages/ab_server.mjs` — a local node server (no deps)
serving a single-page comparator. Two in-context renders side by side, keyboard
`←`/`→` to choose, `space` to skip a pair that is too close to call. Each
judgement is appended to `catalog-out/preferences.json` as it happens, so
results are readable live and nothing depends on an export step. Local only,
not portable, deliberately.

**Scheme:** ~16 pairs per set, ~200 judgements total. Pairs are randomised but
weighted toward options whose standing is least certain, so time goes where it
resolves the most. Scores are fitted with **Bradley-Terry**
(`tools/param-pages/rank.mjs`), giving a strength per option plus a confidence
interval.

Properties that matter: **stopping early is safe** — a partial dataset still
ranks, just with wider intervals — and every additional judgement improves the
fit rather than being wasted. A skipped pair is recorded as a skip, not
discarded; a set with many skips is telling you its options are
indistinguishable, which is a result.

**Two phases.** Phase 1 is within-set (knob vs knob). Comparing a knob to a
font is meaningless. Phase 2, once per-set leaders exist, compares **whole-page
composites** of coherent combinations — that is where cross-set consistency is
actually judged, and it is a much shorter run.

**Cross-set constraints exist and are recorded per set.** The font pick changes
what fits in a 32px label cell, so it constrains sets 4 (enum square, `ENUM_W`
is 20) and 5 (label cell). Font is therefore built first despite being the
largest.

Contact sheets are still generated and committed, but as the **record** of what
was in the catalog, not as the review mechanism.

### 4.2 Out of scope for SCH-50

- Shipping any option. No production draw code changes, no default flips.
- Grid geometry (§1.4).
- Real animation implementation — needs renderer frame state (§2.3).
- Momentary / trigger buttons, per the issue.

### 4.3 Sequencing

1. **Font (12)** — long pole, and constrains 4 and 5.
2. **Sets 4, 5** — text-bearing, unblocked once font construction rules exist.
3. **Sets 1, 2, 3, 6, 11** — independent widgets.
4. **Viz 7–10** — share one treatment vocabulary, generated together once it
   is settled.
5. **Animation (13)** — frame strips, last.
6. **A/B run** — server up, ~200 judgements, Bradley-Terry fit, dataset
   committed.

The A/B tooling does not depend on all thirteen sets existing. It reads
whatever `styles/` exports, so judging can start as soon as the first few sets
render rather than waiting for the whole catalog — which also surfaces problems
with the comparator early, while they are cheap to fix.

---

## Appendix: verified constants

Read from source on 2026-08-26. An earlier automated exploration reported
several of these off by one; these are the checked values.

| Constant | Value | File |
|---|---|---|
| `W` | 128 | `render_page_movy.mjs:417` |
| `HEADER_H` | 7 | `:429` |
| `ROW0_Y` | 10 | `:433` |
| `LBL0_Y` | 25 | `:434` |
| `ROW1_Y` | 33 | `:435` |
| `LBL1_Y` | 48 | `:436` |
| `CELL_W` | 32 | `:437` |
| `RULE_Y` | 55 | `:466` |
| `FOOTER_Y` | 56 | `:467` |
| `FOOTER_H` | 8 | `:468` |
| `LBL_H` | 7 | `:486` |
| `KW` | 17 | `:487` |
| `ENUM_W` | 20 | `:503` |
| `BOX_H` | 15 | `:519` |
| `ARC_START_DEG` | 230 | `:744` |
| `ARC_SWEEP_DEG` | 260 | `:745` |
| `VIZ_ROWS` | 13 | `viz_draw.mjs:281` |
| `VIZ_MIN_W` | 26 | `viz_draw.mjs:296` |
