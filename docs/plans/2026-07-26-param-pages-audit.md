# Param Pages — fleet audit and page model

**Date:** 2026-07-26
**Status:** audit complete; planner, metadata, renderer, navigation and
validator built and tested (branch `param-pages`). Not yet wired into the
shadow UI — that needs hardware.
**Branch:** `param-pages`
**Context:** [2026-06-24 shared parameter UI layer design](2026-06-24-shared-parameter-ui-layer-design.md)

Goal: bring a knob-page parameter UI into Schwung core as a shared library
(`src/shared/param_pages/`), rendered by the native shadow UI and importable by
tool modules. This document audits what the module fleet actually declares,
then proposes the model that has to cover it.

Evidence base: a live 76-module capture (`device-dump.json`, 2026-07-15, from
`DimaDake/schwung-movy`, MIT) carrying each module's `module.json`,
`ui_hierarchy`, `chain_params` and preset state; plus the current native editor
in `src/shadow/shadow_ui.js` and the contract in `docs/MODULES.md`.

---

## 1. Headline finding: knobs-only pages would be a regression

| measure | count |
| --- | --- |
| distinct keys reachable from any level's `knobs[]` | 2271 |
| distinct keys listed in any level's `params[]` | 3137 |
| **`params[]`-only keys (list-visible today, on no knob)** | **879, across 57 of 76 modules** |

Worst affected: forge 106, osirus 106, surge 103, minijv 90, signal 66,
euclidrum 63, dexed 41, eucalypso 32, chordism 30, hush1 22.

`knobs[]` is the module author's chosen *eight*, not their parameter set. A page
model built from `knobs[]` alone — which is what Movy does, and what its own
inventory reports in its "hidden" column — would hide **28% of the fleet's
declared parameters** relative to the list editor we ship today.

**Consequence: overflow pages are mandatory, not a nice-to-have.** Page 1 of a
level is its `knobs[]` (author intent preserved); subsequent pages are that
level's `params[]` entries not already on a knob, eight at a time. This is the
main place the Schwung version must diverge from — and improve on — the Movy
implementation being ported.

---

## 2. What the fleet declares

76 modules, 423 levels.

### Level shapes

| shape | levels |
| --- | --- |
| knobs only | 344 |
| knobs + nav entries | 62 |
| items list (`items_param`, no knobs) | 12 |
| editable params, no knobs | 3 |
| nav only | 2 |

406 levels declare `knobs`, 405 declare `params` — **almost every level has
both**. A walk that renders knobs *or* recurses (rather than both) drops most of
the tree; this is the single biggest bug in Movy's pre-2026-07-25 walk and must
not be reproduced.

### `knobs[]` length

| length | levels |
| --- | --- |
| 0 | 17 |
| 1–3 | 39 |
| 4 | 59 |
| 5–7 | 77 |
| 8 | 220 |
| **9–17** | **11** |

Eleven levels exceed one page: breakbeat/root (17), impressive-chords/root (15),
surge/scene (11), and 9-knob levels in eucalypso ×4, freak, mrdrums ×2, rex.
Continuation pages (`Name - 2`) are required.

### Parameter types

`chain_params` (3610 entries): float 1685, int 1125, enum 774, **filepath 22**,
wav_position 2, string 1, canvas 1.

Inline in `params[]` (388 typed entries): float 212, enum 118, int 56,
filepath 4, **toggle 2**.

Two contract notes: `filepath` is materially used (22 params) and needs a page
kind wired to `filepath_browser.mjs`; `toggle` appears inline but is not in the
documented type list in `docs/MODULES.md` — reconcile.

Inline metadata is widespread and must be honoured, not just `chain_params`:
`default` 330, `min`/`max` 268 each, `step` 247, `unit` 124, `options` 116,
`display_format` 16.

### Metadata gaps are rare

Only **3 modules** declare knob keys with no `chain_params` entry:
impressive-chords (15 — it publishes no `chain_params` at all, relying entirely
on inline hierarchy metadata), sfz (8), clap (2).

So type/range **inference is a 3-module fallback, not a load-bearing layer.**
Keep it dumb and self-correcting (first successful read fixes the guess), and
never let it infer *structure*. This preserves the "render declarations, don't
infer layout" rule from the June design doc.

### Modules with no hierarchy at all

branchage, belt-in, po32-drum, smack-in (4 modules) publish `chain_params` with
no `ui_hierarchy` — they render **nothing** in Movy today. A
`chain_params`-pagination fallback fixes all four and future-proofs new modules.

---

## 3. Modelling "dynamic" — four different things

The word covers four mechanisms with different invalidation rules. Conflating
them is how this gets slow or stale.

### D1. Value-dynamic — every module

Values change under the UI (LFOs, automation, preset loads, DSP-side drift).

**Rule:** never rebuild the page set. Poll with a staggered cursor — one
`shadow_get_param` per tick, suppressed for ~100 ticks after a knob delta.
Movy's own perf work found bulk refresh blocking **~186 ms per cycle**; the
native list already sidesteps this by only reading visible rows, and a knob page
shows eight values at once, so the cursor is not optional.

### D2. List-dynamic — 12 levels, 12 modules

`items_param` / `select_param` levels whose contents are runtime strings:
dexed/banks, obxd/banks, sf2/soundfont, sfz/jump, nam/models, nam/cabs,
midiverb/unit, surge/category_jump, clap/category_jump, minijv/save_slot,
minijv/load_expansion, minijv/expansions.

**Rule:** the page exists statically; its *contents* are fetched on entry and on
explicit invalidation. Never cached across a module swap or a load.

### D3. Count-dynamic — 22 preset levels

`list_param` + `count_param` + `name_param`, where the count and the names both
change when the underlying bank/soundfont/expansion changes.

**Rule:** the preset page reads count and name live. Never cache a count across
a bank change. See §4.

### D4. Structure-dynamic — the hierarchy itself changes

This is real and already handled natively: `drawHierarchyEditor` polls
`is_loading` and **re-fetches both `ui_hierarchy` and `chain_params` on the
loading→ready transition**, then reloads the level and drops cached knob
contexts. Modules with ROM/expansion loads (Virus, minijv) change their tree
after the module reports ready.

Note `is_loading` appears in **no module's `chain_params`** — it, along with
`load_error`, `preset_name`, `bank_name` and `preset_names`, is an out-of-band
status key. The page model must read these outside the declared param set.

**Rule:** the page set carries a fingerprint —
`hash(moduleId, ui_hierarchy, chain_params, mode)` — and is rebuilt when it
changes. Rebuild preserves the user's position by page *name* where possible,
falling back to page 0.

### D5. Mode-dynamic — minijv only

`modes: ["patch","performance"]` + `mode_param: "mode"`. Natively this is a root
branch: with `modes` present the editor opens on a **mode-select screen**
(`hierEditorLevel = null`), and choosing one writes `mode_param` and enters that
level. Mode change is a page-set scope change → rebuild.

Only one module in the fleet uses it. It still has to work, because it is minijv.

---

## 4. Preset handling

Presets are not parameters and should not be modelled as one.

**Shape.** The preset triple decorates an otherwise ordinary level — obxd/root
has 8 knobs, 13 nav entries *and* `list_param`/`count_param`/`name_param`. So a
level can be both "the Main knob page" and "the preset browser". 22 levels
across 20 modules do this.

**Name resolution**, in precedence order:
1. bulk `preset_names` (JSON array) — one read
2. per-index `preset_name_N`
3. live poll of `name_param` for the current index only

minijv forces option 3: **2427 presets, `names: null`**. Any design that
materialises a name list is wrong.

**Async.** `is_loading` gates a re-fetch; `load_error` must surface (the native
browser word-wraps it into two lines); `bank_name` is the header while browsing,
`preset_name` while editing.

**Proposed model:** a level carrying the preset triple emits a `preset` page
*before* its knob pages, reusing the existing browser renderer verbatim. The
current preset name additionally decorates the header on every page of that
component, because that is what an instrument should show.

---

## 5. Per-instance parameters — an existing contract nobody uses

`docs/MODULES.md:907` documents `child_prefix` / `child_count` / `child_label`
for repeated elements, and `shadow_ui.js` implements it: a selector page, then
keys rewritten as `${child_prefix}${index}_${key}` (`buildHierarchyParamKey`,
`normalizeVisibilityConditionKey`).

**Exactly one level in the 76-module fleet uses it**: minijv's `part_selector`
(`child_prefix: "sram_part_"`, 8 parts). One level out of 423. Note minijv does
*not* use it for its four tones — those are declared as four separate levels
with fully concrete keys, which is why its tone subtrees are near-identical
copies.

Meanwhile six modules — mrdrums, forge, weird-dreams, essaim, krautdrums,
po32-drum — solve exactly this problem through a Movy-side config
(`padScoping`: alias prefix, concrete key template, pad digits, per-suffix
overrides). That accounts for a large share of the "hidden params" in §1
(mrdrums 216, weird-dreams 187, forge 106).

This is a **contract gap, not a rendering gap**: the framework has the feature,
it is effectively undiscovered, so a third party invented a parallel one. Deciding
between promoting `child_prefix` and blessing `padScoping` is a prerequisite for
the port — it determines whether pad-scoped params are addressable from declared
metadata alone, or need a per-module config file.

Recommendation: extend `child_prefix` with the one thing `padScoping` has and it
lacks (a key *template* rather than a fixed `prefix + index + _` shape, plus
per-suffix overrides), then express the six modules' scoping in the standard
contract. One mechanism, declared, no config file needed.

---

## 6. `visible_if` — much smaller than assumed

Fleet usage: **3 params in 1 module** (mrsample: loop_start, loop_end,
loop_xfade_ms), **0 levels**.

The evaluator already exists and is context-parameterized
(`evaluateVisibilityConditionForContext`, `shadow_ui.js:1963`), supporting
`equals`/`not_equals`/`gt`/`lt`/`truthy`/`falsey` with fail-open on unreadable
keys. Wire it into the walk — it is cheap — but it does not justify design
weight, and it is not the differentiator I previously claimed.

---

## 7. Proposed page model

A **PageSet** is built per `(slot, component)` from `ui_hierarchy` +
`chain_params` + runtime state, and is an ordered, flat list of typed pages.

| kind | source | renderer |
| --- | --- | --- |
| `preset` | level preset triple | existing browser |
| `knobs` | level `knobs[]`, 8/page + continuations | **new** grid |
| `overflow` | level `params[]` keys not on a knob, 8/page | **new** grid |
| `menu` | level `params[]` nav entries | existing `drawMenuList` |
| `items` | `items_param`/`select_param` | existing list |
| `modes` | hierarchy `modes` + `mode_param` | existing list |
| `child` | `child_prefix` selector | existing list |
| `special` | one param of type canvas / wav_position / string / filepath | existing editors |

Only two of eight kinds are new code. The rest is dispatch to renderers that
already exist — which is why this is smaller than it looks, and why the
"fallback to the list" idea from earlier discussion is better expressed as
**page-kind dispatch**: new param types get a kind, not an exception.

Walk rules (from the Movy 2026-07-25 traversal design, validated here against
the same fleet): visit both `params` nav edges and `children`, always; no depth
cap; dedupe by exact knob key-list; `children`-reached levels are
prefix-transparent; label precedence `level.name` → nav-entry label →
`level.label` → key; orphan sweep at the end so every declared knob is
reachable. `children` is variously `null`, absent, or the literal string
`"None"` — normalise all three.

### Library rules (non-negotiable, or the tool case breaks)

The same library must serve the native editor (owns input, commits values) and a
tool like Movy (owns input, commits to its own step data for p-locks):

1. **No param I/O inside.** Values in as arguments; caller does reads/writes.
2. **No screen ownership.** Render into a rect; no `clear_screen()`.
3. **No input handling.** Export the physical-knob → param-index map; caller routes.
4. **No module-level state.** Movy has 4 tracks × 5 components live at once.
5. **Injectable draw context** (`{ fillRect }`) so it tests headless in node.
6. **Optional per-cell decoration** — value override + `locked` flag — which is
   all p-locks need from the renderer.

Knob feel comes from the existing `src/shared/knob_engine.mjs` on both paths, or
a p-locked value will move differently from the same param edited live. Movy's
range normalisation idea (`MIN_STEP_RANGE_FRAC`, a consistent ~100-detent sweep
regardless of unit range) is worth folding *into* `knob_engine` so the list view
gets it too.

---

## 8. Acceptance target

**minijv is the fleet in one module.** 55 levels, 418 `chain_params`, 2427
presets, `modes`, `children: "None"`, a nav-only `tone_selector`, four
near-identical tone subtrees (so sibling pages must be prefixed or four pages
read "Filter"), 90 `params[]`-only keys needing overflow pages, three
`items_param` levels, and an expansion load that changes the tree at runtime.

If the planner renders minijv correctly, it renders the fleet.

Secondary targets: **surge** (31 levels, 303 params, 675 presets — volume),
**forge/mrdrums** (per-instance scoping, §5), **impressive-chords** (no
`chain_params` at all), **branchage/belt-in/smack-in/po32-drum** (no hierarchy),
**sf2** (2 knobs, 6 list params, runtime soundfont list — the case where a knob
page is nearly empty and overflow carries it).

---

## 9. Test fixture

Golden tests over a real fleet capture are the de-risking mechanism: assert page
counts, page names and per-page key lists for all 76 modules, headless in node,
in the style of `tests/shadow/test_knob_engine.sh`.

The Movy dump is 2.2 MB and is megadake's capture. **Recommendation: write our
own dumper** (a small tool that walks installed modules and emits the same
shape) so the fixture is ours to regenerate as the fleet moves, and check in a
trimmed version — hierarchy + chain_params + preset counts only, no raw `params`
blobs. Use the Movy dump now as the cross-check that our dumper agrees with a
known-good capture.

---

## 10. What was built (2026-07-26 → )

All of it pure, node-testable, and living in `src/shared/param_pages/`. Nothing
is wired into the shadow UI yet; that step needs hardware.

| module | what it does |
| --- | --- |
| `page_plan.mjs` | `ui_hierarchy` + `chain_params` → an ordered list of typed pages |
| `param_meta.mjs` | key → declared metadata, with the list editor's precedence, plus classification |
| `render_page.mjs` | draws a page through an injected draw context |
| `page_nav.mjs` | stepping, level-skip, jump index, rebuild reanchor |
| `validate_contract.mjs` | what a module declares vs what can be rendered |
| `announce_page.mjs` | screen-reader strings for a grid |
| `page_controller.mjs` | interaction model: state, knob feel, staggered reads, rebuild |
| `page_input.mjs` | Move MIDI → intents |

Development tooling in `tools/param-pages/`:

- **`harness.mjs`** — a headless 128x64 framebuffer implementing the device's
  `fillRect` / `print` / `textWidth`, rendering text through the *actual device
  font atlas* (`font5x7.json`, extracted from `build/host/font.png` by
  `gen_font_table.py`, mirroring `js_display_load_font`'s auto-trim). Previews
  are pixel-identical to the OLED, which is what made the layout decisions below
  possible with no Move on the desk. Also tracks two things the device cannot
  report: characters missing from the font, and pixels drawn off-screen.
- **`preview.mjs`** — render any fleet module's pages as half-block art or PNG.
- **`validate.mjs`** — the contract report.
- **`cases.mjs`** — the pinned snapshot cases.

Tests (all in the CI-gated `tests/host/`): planner golden tests over 76 modules,
metadata precedence, 1144 render sweeps plus 11 half-block snapshots, navigation,
and the validator including its false-positive traps.

---

## 11. Decisions taken while building

Recorded because each one was a real fork, and several were settled by looking
at a render rather than by reasoning.

**No font ships with the library.** The device's `print()` uses a 5x7 atlas and
fits five characters in a 32 px cell — the same size Movy's bundled 8pt font
renders at. Carrying a second font buys nothing and costs a maintenance burden.

**Cell layout: dial by default, bar as the alternative.** A third layout (small
dial *and* value) was built, looked at, and deleted — it is the bar layout with a
worse widget in the same space.

The default was initially bar, on the grounds that it shows every value at once.
Charles pushed back (2026-07-26): once a held knob puts its full name and value
in the header strip, the dial layout has informational parity, and dials are
quicker to parse — you are usually reading relative position, and a pointer angle
beats a fill length for that. Eight dials are also eight distinct shapes rather
than eight similar rectangles. That is right, and the default flipped.

What the dial genuinely loses is *simultaneous* numeric readout: on a levels or
mixer page, eight glances become eight touches. `revealValues` closes most of
that — while a modifier is held, every label swaps for its value — and
`LAYOUT_BAR` remains for anyone who wants it permanently.

**The held knob puts its FULL name and value in a strip over the header.** A
30 px cell cannot render "Resonance"; truncating it in place yields "Reson",
which is no better than the abbreviation already there. The screen width can,
and the module name is the one thing you do not need while turning something.
This is the real answer to five-character labels — the gesture, not a cleverer
abbreviation.

**Label shortening keeps the distinguishing word.** "Filter Env" → "FlEnv", never
"Fltr". Short single words truncate ("Cutoff" → "Cutof"), long ones devowel
("Resonance" → "Rsnnc"), three-word names initialise ("Low Freq Osc" → "LFO").

**Enums show boxed option text, not a bar.** A bar cannot say "up_down".

**Page 1 of a level is exactly `knobs[0..7]`, always.** The shim already maps the
physical knobs to that array (`buildKnobContext`), so a rebalanced first page
would make one knob do different things in the list and on the grid. Overflow
keys are spread evenly across the *following* pages only.

**Orphan continuation pages are accepted, and navigation compensates.** A level
with nine keys yields 8 + 1 given the invariant above; 52 of 572 grid pages hold
one or two controls. Eliminating them means breaking the invariant, so instead
`stepLevel` skips them (minijv: 76 stops → 53) and the jump index ignores them
(57 entries, 25 groups).

**Root's children carry no prefix.** "Filter", not "Root/Filter" — prefixes start
one level down, where they stop minijv showing four pages called "Filter".

**Page names are allocated, not formatted.** A `claim()` appends " - N" for the
smallest free N, which numbers continuations *and* disambiguates real collisions
with one mechanism: freak and granny each declare a child level called "main"
alongside root's own "Main" page, with different knobs, so both must appear.

**Presets get their own page, first in the level's set** (decided with Charles,
2026-07-26). A level is routinely both the Main knob page and the preset browser;
a knob is the wrong control for minijv's 2427 or surge's 675 presets.

**Rendering degrades to fit its rect.** Asked for less height, the cell drops the
value line, then the label, rather than overflowing — this is what lets a tool
draw the grid beneath its own header.

---

## 11b. UX pass (2026-07-26)

Done by rendering the fleet's *worst* pages rather than its nicest ones. Each
of these was a real defect visible on screen, not a hypothetical.

**Eight cells reading "Enabl".** euclidrum declares all eight lane switches as
`name: "Enabled"`; only the key says which lane. Shortening each label in
isolation cannot see that — 47 of 572 pages had two or more cells reading the
same. Labels are now resolved per *page*: on collision a discriminator is
derived from what differs in the keys, giving "Enab1".."Enab8". Where the keys
do not separate them either (aphex's "EG1 Trig" and "EG1+2 Trig", one key a
prefix of the other) the whole group is numbered by knob — mixing derived and
numbered discriminators is how two cells both end up ending in "2". Collisions
fleet-wide: **47 pages → 0**.

**Sparse pages looked broken.** A three-control page left five cells blank,
which reads as a failed render rather than "this section has three things".
Unused knob positions now carry a quiet centred tick.

**Modulated params were invisible** — a regression against the list, which
appends `~`. A cell cannot spare a character, so it takes a tick in the
top-right corner. The predicate is injected, since modulation is host state.

**76 pages, navigable only by jogging.** Jog-click with no knob held was doing
nothing, so it opens a **section picker**: minijv folds to 16 named sections
with page counts. Jog scrolls, click jumps, Back closes one layer, and reaching
for a knob dismisses it — an unambiguous "I want the grid" rather than a modal
to back out of.

**Nothing told you any of that.** The grid uses all 64 px so it has no room for
a footer, and none of the gestures are guessable. A first-run panel lists them,
cleared by any input and never shown again that session. The lines come from the
caller, not the library — gestures belong to whoever owns the input mapping.

**The header showed a model number where a synth shows a patch name.** It now
shows the loaded preset name, folded into the staggered read rotation as one
extra stop rather than an extra poll.

Two bugs surfaced while testing the above: only *grid* page names went through
the uniqueness allocator, so minijv had two sections both called "Presets" —
and `reanchor()` matches by name after a rebuild, so it could have landed on the
wrong one. Every page kind is now allocated, and the picker numbers a repeated
section name since it strips the " - N" suffix.

---

## 11c. Elektron UX review (2026-07-27)

Movy took Elektron's *layout* — eight knobs, two rows of four, pages you step
through — and stopped there. The interaction patterns behind that layout are
where most of the usability actually lives, so this is a pass over them:
what applies to Move's control surface, what we took, and what we deliberately
did not.

### Taken

| Elektron | Here |
| --- | --- |
| `[FUNC]` + encoder = fine adjust | **Shift is precision mode**: floats go ~10x finer *and* every label becomes its value. On Elektron those are two things; on a 128 px screen they want to be one, because chasing a number and being able to read it are the same moment. Deliberately inert for ints and enums — an int already moves in whole units, and faking a finer step would make it feel broken rather than precise. |
| A page button returns to the sub-page you last used | **Section memory.** Returning to a section lands where you left it. Worth most where getting back costs most: minijv's tone subtrees are 15 pages each. Section jumps only — a plain jog still walks the set in order. |
| Large value readout while turning | The **held-knob strip** over the header, carrying the full name and value. |
| Encoder acceleration | Already ours, via `knob_engine.mjs` — and the grid reuses it precisely so a value moves identically in the list and on the grid. |
| Parameter locks | The `decorations` contract: per-cell value override plus a `locked` flag, with the sequencer supplying both. |
| Page-position indicator | The segmented rule, one segment per page. |

Plus one Elektron does *not* have and the fleet asks for: **reset to the
declared default** — 744 params across 39 modules declare one and there was no
way back to it short of reloading a preset.

The gesture for it took two attempts, and the first was dangerous. Shift +
jog-click on a held knob was wrong because **Shift is precision mode**: while
fine-adjusting you are already holding Shift with a knob under your finger, and
the jog is live for section stepping — so a destructive action sat one stray
press away from the most delicate operation in the UI. It is now **Mute +
touch**, which is the modifier Schwung already uses for destructive and state
actions in this very view (Mute+JogClick bypasses a module, Mute+Track mutes a
slot), is forwarded to the shadow UI, and is not a key you are holding while
tuning. Double-tapping a knob was considered and rejected for the same family of
reason: lifting and re-placing a finger mid-adjustment is normal, and a
double-tap would read it as a reset.

### Adapted, not copied

**Elektron's own displays mostly show name + value text, not dials** — Digitakt
and Digitone have no dial graphics at all. Their parameters are precise and
numeric, so the number *is* the readout. We default to dials anyway, because
that is Movy's aesthetic and because Move's params skew continuous; `LAYOUT_BAR`
is the Elektron-shaped alternative and is one setting away. Worth knowing the
divergence is deliberate rather than an oversight.

**Dedicated page buttons** (`[TRIG] [SRC] [FLTR] [AMP] [LFO] [FX]`) are the
single biggest thing we cannot copy: they are fixed, labelled, and direct, and
Move has no spare buttons to give. The **section picker** is the substitute —
one click, a named list, jog and confirm. It is worse than a dedicated button
and much better than jogging 76 pages.

There is also a structural difference worth naming: **an Elektron machine's
pages are fixed, so muscle memory transfers**. Ours are generated from whatever
each module declares, so they differ per module. That is exactly why section
names, the picker and section memory matter more here than they do on an
Elektron — the user cannot rely on position, so the UI has to carry the names.

### Rejected

- **Copy/paste of pages and params** (`[FUNC]`+`[REC]`/`[STOP]`) — kit and
  sequencer territory; Move's Copy/Delete are claimed elsewhere, and a param UI
  copying a page into another module's page is not well defined.
- **Randomise** (`[FUNC]`+`[YES]`) — plausible, but destructive with no undo in
  this view, and no gesture left that is not a worse fit for something else.
- **Trig conditions, retrig, microtiming, per-track scale** — sequencer, not a
  parameter UI. These belong to Movy.
- **Sound browser with tags and categories** — the preset page plus the existing
  browser already cover this, and duplicating it would be the "grid reimplements
  a screen the list already has" mistake.

### What the shadow UI can actually receive

Settled by reading `schwung_shim.c`, not by guessing. In non-overtake shadow
mode the shim forwards a deliberately short list:

| | Forwarded to `shadow_ui.js` |
| --- | --- |
| CC | 3 (jog click), 14 (jog turn), 51 (Back), 40–43 (track), 71–78 (knobs), 88 (Mute) |
| Notes | 0–7 (knob touch), 40–43 (track), 68–99 (pads, only while `pad_block`) |

Everything else — including **CC 49 (Shift)** and the **step buttons (notes
16–31)** — never arrives.

**This cost a real bug.** The view module tracked Shift from CC 49, which meant
every shift gesture (section step, reveal, fine adjust, reset to default) would
have been silently dead on hardware: no error, no log, four features quietly
doing nothing. Shift is published in shared memory instead and read with
`shadow_get_shift_held()`, which is why the rest of `shadow_ui.js` polls it.
Note the asymmetry: an overtake **tool** sharing this library *does* receive
CC 49, because the overtake path forwards everything — so `page_input.mjs`
still decodes it and only the host reads it out of band.

### Ruled out: step buttons and pads

**Step buttons as direct page access**, with the step LEDs showing position, is
the closest Move could get to Elektron's dedicated page buttons — and it is not
available. Two independent reasons:

1. The shim does not forward notes 16–31 to the shadow UI, so it would need a C
   change; and
2. more decisively, **the steps and pads belong to the rest of the Move UI**.
   The shadow UI is not an overtake module: Move's own surface is live
   underneath it, and taking sixteen buttons away from it to page a parameter
   editor is not a trade worth making.

So the **section picker is the permanent answer**, not a stand-in. That is also
why section names, section memory and the picker's grouping carry the weight
they do here: with no dedicated page buttons and no fixed page layout, naming
and memory are all the user has.

---

## 11d. Pulled in from Movy v0.25.0 (2026-08-01)

megadake shipped v0.25.0 with a **params-list-exposure** branch that found the
same gap this work found independently — a level's `params[]` entries never
reaching the UI. Their count: 721 params across 45 modules. Ours: 879 across 57.
Both landed on the same shape, `keys(level) = knobs ++ extraParams`. Encouraging
for the shared-library case: two people reading the same contract reached the
same design.

Their version carried refinements ours did not. Taken, each measured against our
own fixture first:

| Taken | Effect here |
| --- | --- |
| Exclude **selector params** (`list/count/name/items/select/mode`) from grids | 2 modules. They drive their own page kind, and browsing 2427 presets by encoder is what the preset page exists to avoid. |
| Exclude **`ui_`-prefixed keys** | 3 keys. Module UI state (`ui_current_pad`, `ui_preset_path`), not musical parameters. |
| **Global dedupe of overflow** | 4 keys across 2 modules that occupied cells on two pages. |
| **Page rule grouped by section**, full width, 1 px separators | Every module now gets a real per-page ruler; before, everything past 24 pages fell back to a proportional marker. |
| **Async metadata re-resolution** | osirus publishes `rom_index` as `["(loading)"]` and it is in our fixture that way — baked at load, it would read "(loading)" all session. |

One refinement of our own on top: all three exclusions apply **only to keys
pulled in from `params[]`**. A key the author put on `knobs[]` is intent and is
honoured whatever it is called. The coverage test names its exclusions and
asserts the set stays under 25 fleet-wide, so the filter cannot quietly widen
and reintroduce the 28% regression by the side door.

Two bugs of ours surfaced while doing it:

- **The fingerprint hashed `chain_params` *length*.** A module republishing real
  enum options in place — no new params, no new levels — read as unchanged, so
  the placeholder would never clear. It hashes content now.
- **The page rule's rounding** left the last page several pixels wider than the
  rest. Leftover pixels now spread across segments so no two differ by more than
  one, and coalescing runs of equal-height flush segments keeps a 76-page module
  inside the draw-call budget (125 → 82).

Their P3 — read-back scaling with page count — we already avoid by construction:
the cursor cycles only the *current page's* keys, never the whole param set.

---

## 12. Contract quirks found

Each of these is a live issue in the fleet, and most affect the existing list
editor too. `node tools/param-pages/validate.mjs --level warn` reproduces them.

1. **Text the display cannot draw.** 15 strings across 5 modules use characters
   absent from the 5x7 atlas, so they render as *nothing* today: forge's
   "Copy A→B" and "Swap A↔B", aphex's "MW→MG", signal's "Save → A", sfz's "¢"
   unit, and euclidrum's "—" enum option, which is invisible entirely. The
   library now folds these to ASCII (`asciiFold`). **The list editor should
   adopt the same fold** — this is not a grid-specific bug.
2. **`toggle` is undocumented.** Used inline by real modules, absent from the
   type list in `docs/MODULES.md`. Treated as a two-option enum. Either document
   it or stop shipping it.
3. **`ui_type` is a second spelling of `type`.** `wav_position` appears as
   `ui_type` 19 times and as `type` twice. Both must be honoured; the contract
   should say which is canonical.
4. **`unreachable-params`, 9 modules.** Declared in `chain_params`, listed in no
   level, so no UI can reach them. mrdrums accounts for 209 of them — the
   per-pad concrete keys behind the alias split in §5.
5. **`empty-range`, 2 modules** (osirus `bank_index`, sfz `knob_preset`): `max <=
   min`, so a knob cannot move them. Distinct from the *legitimate* runtime-sized
   case (hush1/sf2 declare `preset` as 0..-1 because `count_param` sizes it) —
   the contract should have one blessed way to say "sized at runtime".
6. **`no-hierarchy`, 4 modules** (branchage, belt-in, po32-drum, smack-in):
   `chain_params` only. They render nothing in Movy; the planner paginates them.
7. **Out-of-band status keys.** `is_loading`, `load_error`, `preset_name`,
   `bank_name`, `preset_names` appear in *no* module's `chain_params` but are
   read by the UI. They are part of the contract in practice and undocumented in
   principle.
8. **sfz and clap expose generic knob slots** (`knob_0..7`, `param_6..7`) with no
   declared metadata, labelled at runtime. The only place the library guesses.

---

## 13. Decisions taken in Charles's absence, and what is still open

**Settled 2026-07-26 while Charles was away, rather than left blocking:**

1. **`vbar` deleted.** Two call sites read `meta.render === "vbar"` and zero
   fleet modules declare `render` at all. It came from Movy's config vocabulary,
   where the field is hand-authored. When a layout spec lands it deserves a
   considered widget vocabulary, not one orphan case that survived a port.
2. **`child_prefix` extended, not `padScoping` blessed.** `child_key_template`
   with `{index}`/`{key}`, `child_index_base`, `child_index_digits` and
   `child_key_overrides` — additive, with the legacy form pinned byte-identical
   because minijv ships it. Demonstrated rather than argued: expressing mrdrums'
   pad scoping in the extended contract takes it from 209 unreachable params to
   6, and those six are genuine module-level globals. Documented in
   `docs/MODULES.md`. Six modules can now drop their config file.

**Still open, and genuinely Charles's:**

3. **Layout spec name and shape.** Earlier notes treated this as constrained by
   adoption — Movy's `loader.ts` comment names Forge as a module that ships its
   own `movy_config.json`. Checked against the 76-module device dump
   (2026-08-16): **no module ships one. Zero.** Forge's config is *bundled
   inside Movy* (`src/modules/forge.json`), as are 13 others. The mechanism has
   a real load path and no users — the same shape as `child_prefix` (§5).

   So there is no installed base and nothing to migrate. The file should be
   designed rather than inherited. Still worth agreeing with megadake so both
   renderers read the same thing, but the conversation is "neither of us has
   adopters yet, let's agree the shape", not "please rename your format".
4. **Overflow page ordering** — declaration order (current) or grouped by type?
5. **Graphics detectors** (envelope / filter / LFO / EQ) — position changed
   2026-08-16 after Movy v0.27.0 shipped ~1,350 lines of them.

   Earlier reasoning ("they infer from key names, which the June doc rules out")
   was too rigid: it defends the principle at the cost of shipping nothing,
   since **no module declares anything today**, so detectors would be doing 100%
   of the work on day one. The model instead is Charles's: a module declares its
   visualisations; detectors are the fallback when it does not. Precedence, with
   the module always winning over the host:

   > module `chain_params` `viz` → module layout file → host override → detector → plain knob

   The residual risk is honest and not fully removed by that: a new module can
   still trip a detector wrongly on a user's device, and the override only helps
   after someone notices. Two things reduce it —

   - **Corroborate with declared metadata, not just vocabulary.** Movy's best
     idea in v0.27.0 is not a graphic, it is `isGainRange`: an EQ band must have
     a bipolar, roughly symmetric range before a name like "gain" is believed.
     That single test rejects the crossovers, Q values and random bounds their
     word lists let through. Ported detectors should demand the same
     corroboration; their envelope and filter detectors are looser.
   - **Order by risk, not by value.** Envelope first (ADSR naming is near
     universal and a wrong shape is obvious on screen), then filter, then LFO,
     EQ last — its false positives are the hardest to spot and the ones Movy has
     already had to patch repeatedly.

   Success measure: if detectors are still doing ~100% of the work in a year,
   the declaration path failed and this became a maintenance treadmill. The
   validator reporting what it *inferred* is what makes that visible.
6. **Should `asciiFold` move into `src/shared/param_format.mjs`** so the list
   editor gets it too? Almost certainly yes, as a small separate change — five
   modules render text as nothing in the list today.

---

## 14. Rollout

Decided 2026-07-26: **ship as a preview, default off, and turn it on for
everyone in a later release.** The preview exists to de-risk that flip, not to
decide whether to make it.

- **Setting:** Global Settings → Display → *Param View*: `List` (default) /
  `Knobs`. Sits next to `overlay_knobs`, which already establishes the shape.
- **Scope while previewing:** the grid replaces the list only for grid-able
  levels. Preset browsers, canvas, `wav_position`, text entry and item lists
  render exactly as they do today — that is the page-kind model, not a fallback.
- **Comparison gesture:** with the setting on, Shift + jog-click toggles
  List ↔ Knobs for the session, so people can A/B the same parameters in two
  seconds. With the setting off, nothing about today's behaviour changes.
- **Ships unconditionally, not behind the flag:** the ASCII fold (five modules
  render text as nothing in the *list* today — that is a bug fix, and it must not
  be gated on an experimental toggle) and the contract validator (a dev tool with
  no user surface).

### What the flip depends on

Because the end state is "on for everyone", two items stop being polish:

1. **Screen reader.** `announce_page.mjs` exists and is tested, but nothing calls
   it. When Knobs is the default, TTS users get it by default too, so the flip
   needs both the announce calls wired *and* an automatic override back to the
   list while the screen reader is on.
2. **Hardware perf.** Eight live values per page is eight IPC round trips. The
   staggered one-read-per-tick cursor has to be shown to keep up on a device
   before the default moves.

### What the preview is listening for

Whether people prefer it to the list at all; dial versus bar; whether 76-page
modules navigate acceptably with level-skip and the jump index; whether the
value cursor lags; and whether the 52 single-control continuation pages irritate
in practice or only on paper.

Note the list never fully retires — it remains the renderer for every non-grid
page kind. "Default" means the grid becomes the default *for grid-able levels*.

---

## 15. Next steps

- **Run the dumper** on a Move (`tools/param-pages/dump_contracts_device.js`,
  written but UNVERIFIED — read its header first; it is destructive to the probe
  slot) and install the result with `tools/param-pages/regenerate.mjs`, so the
  fixture stops being a trimmed copy of megadake's capture.
- **Native binding — done, unverified on hardware.**
  `src/shadow/shadow_ui_param_pages.mjs` plus 89 lines in `shadow_ui.js`
  (a VIEWS constant, the setting, one draw case, one tick call, one MIDI
  early-out). The view module is executed in tests by repointing its deployed
  import paths at the repo and injecting a fake `ctx`; `shadow_ui.js` itself is
  parse-checked and its wiring pinned. tests/shadow had 19 stale failures before
  the change and the same 19 after.

  **What still needs a Move**, in order of risk:
  1. That eight live values per page keep up. The staggered cursor is tested for
     *shape* — exactly one read per frame — never for timing against real IPC.
  2. That the whole path actually lights up: enter a slot with Param View =
     Knobs and see a grid.
  3. Draw cost per frame in situ. Measured statically at 52 (bar) / 290 (dial)
     calls per page, budgeted in tests, never timed on the device.
  4. The screen-reader calls, which are written and tested as strings but have
     never been spoken.
- **Screen reader**: the strings exist (`announce_page.mjs`, tested over all 608
  fleet pages) but nothing calls them yet. Wiring is part of the native binding:
  page change, knob touch, knob turn, and a read-the-page gesture. Until then the
  list remains the default surface under TTS.
