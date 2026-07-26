# Param Pages — fleet audit and page model

**Date:** 2026-07-26
**Status:** audit complete, design proposed, nothing built
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

**Zero modules in the 76-module fleet use it.**

Meanwhile six modules — mrdrums, forge, weird-dreams, essaim, krautdrums,
po32-drum — solve exactly this problem through a Movy-side config
(`padScoping`: alias prefix, concrete key template, pad digits, per-suffix
overrides). That accounts for a large share of the "hidden params" in §1
(mrdrums 216, weird-dreams 187, forge 106).

This is a **contract gap, not a rendering gap**: the framework has the feature,
nobody knows about it, so a third party invented a parallel one. Deciding
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

## 10. Open decisions

1. **`child_prefix` vs `padScoping`** (§5) — blocks pad-scoped drum modules.
   Promote the declared contract, or bless the config file?
2. **Layout spec name and ownership** — `movy_config.json` as-is, or
   `param_ui.json` with the former as a read alias? Forge already ships one.
3. **Preset placement** — dedicated `preset` page before the level's knob pages
   (proposed), or a preset knob on the page as Movy does?
4. **Overflow page ordering** — declaration order, or grouped by type?
5. **Graphics detectors** (envelope / LFO / filter) — deferred. They infer
   grouping from key names, which the June design doc rules out. Re-introduce
   later driven by declared hints in the layout spec rather than guessing.

---

## 11. Sizing

| work | estimate |
| --- | --- |
| page planner + walk + overflow + fingerprinting | ~600 lines, new |
| widget renderer (arc, grid, labels) + font | ~900 lines, ported |
| page-kind dispatch into existing renderers | ~300 lines, new |
| native binding (shadow UI view + setting + a11y) | ~400 lines, new |
| dumper + golden fixtures + tests | ~400 lines, new |

Roughly 1,600 new / 900 ported. The ported share is smaller than the raw Movy
line count because §1 means the planner is a rewrite, not a port — Movy's walk
is the starting point for the *tree traversal* only.
