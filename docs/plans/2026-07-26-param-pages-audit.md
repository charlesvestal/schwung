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

## 13. Open decisions

1. **`child_prefix` vs `padScoping`** (§5) — the remaining blocker for pad-scoped
   drum modules. Promote the declared contract with a key template, or bless a
   config file?
2. **Layout spec name** — `movy_config.json` as-is, or `param_ui.json` with the
   former as a read alias?
3. **Overflow page ordering** — declaration order (current) or grouped by type?
4. **Graphics detectors** (envelope / LFO / filter) — still deferred. They infer
   grouping from key names, which the June design doc rules out. Re-introduce
   driven by declared hints in the layout spec.
5. **Should `asciiFold` move into `src/shared/param_format.mjs`** so the list
   editor gets it too? Probably yes, as a small separate change.

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
- **Native binding**: a `PARAM_PAGES` view in the shadow UI, the Param View
  setting, jog/level-skip/jump-index wiring, and the staggered one-read-per-tick
  value cursor. Needs hardware to verify — in particular that eight live values
  per page do not cost more than the frame budget allows.
- **Screen reader**: the strings exist (`announce_page.mjs`, tested over all 608
  fleet pages) but nothing calls them yet. Wiring is part of the native binding:
  page change, knob touch, knob turn, and a read-the-page gesture. Until then the
  list remains the default surface under TTS.
