# One list engine — design, 2026-08-23

> *"does it make sense to have global settings be 'knobs' like slot and master
> fx settings? if nothing else, we need to get them movy-list-ified, right?"*
>
> *"but it sounds like we have to forever maintain the list path, should we
> combine these into one engine with two modes?"*
>
> *"make sure we're not ending up in a rabbit hole of multiple parallel paths.
> i dont want to have to update things in multiple places when it's one thing."*
>
> *"our display engine should be shared across, i don't want to update the list
> view for slots and have it not fix something in mfx."*

This started as a Global Settings design and turned into an engine design,
because each answer exposed that the real problem was one level up. The
deliverable is **one list engine**; Global Settings is its first new consumer
and its proof case.

---

## 1. Where things actually stand

**The knob grid is the default now.** `tests/host/test_param_view_default.sh`
pins `paramViewGlobal = 1`. The hierarchy list editor is no longer the primary
surface with a grid bolted on; it is the fallback for screen-reader users and
for people who explicitly chose List. It costs ~506 references across ~34
functions spanning `shadow_ui.js` from 1943 to 13502.

**Screen-reader mode forces the list, globally.** `paramPagesEnabled()`
(`shadow_ui_param_pages.mjs:114`) returns false when TTS is on, *before* it
consults `param_view`. So the list is not a minority surface — for every
screen-reader user it is the *only* surface. Its comment frames the rule as
provisional — *"only navigable by ear once the announce calls are proven on
hardware"* — so it is a placeholder for grid announcements nobody has validated
on device, not a permanent design.

**One engine with two modes is already three-quarters built.**
`drawParamPages` (`shadow_ui_param_pages.mjs:617`) draws `PAGE_KNOBS`,
`PAGE_MENU`, `PAGE_PRESET` and `PAGE_ITEMS`. Three of those are lists, drawn
inside the movy page chrome by the controller itself. The comment above it is a
changelog of that migration happening one kind at a time, each pulled in because
handing off to the *other* engine produced a bug:

- `PAGE_MENU` — refusing it ejected slot settings to "No presets"
- `PAGE_PRESET` — the eject landed in the list editor, whose jog is wired to the
  preset browser, so **jogging past** a preset page auditioned every preset it
  crossed
- `PAGE_ITEMS` — *"a soundfont or NAM-model list is a real list, so it can be
  five rows in the page chrome rather than a separate screen"*

Three bugs at the same seam is not three coincidences.

**And the list itself is already split three ways.** This is the finding that
reframed the document:

| population | count | drawn by |
|---|---|---|
| contract pages in the page chrome | 4 kinds | `page_controller.mjs` |
| ordinary view lists | 32 call sites | `drawMenuList` |
| **hand-rolled row loops** | **6 sites** | copy-pasted `for` + `fill_rect` + `print` |

The six are not scattered. They are concentrated entirely in the slot / Master
FX / knob / LFO family — the exact screens named in the question:

| site | function | draws |
|---|---|---|
| `shadow_ui_settings.mjs:43` | `drawChainSettings` | Save-As "Edit/OK" preview |
| `shadow_ui_settings.mjs:84` | `drawChainSettings` | slot settings items |
| `shadow_ui_master_fx.mjs:458` | `drawMasterNamePreview` | Save-As "Edit/OK" preview |
| `shadow_ui_slots.mjs:250` | `drawSlotSettings` | slot settings items |
| `shadow_ui.js:16360` | `drawKnobEditor` | knob mapping rows |
| `shadow_ui.js:17318` | `drawLfoEdit` | LFO param rows |

`patches`, `store` and `tools` are clean — they use `drawMenuList` exclusively.

Sites 1 and 3 are the same code, character for character apart from variable
names:

```js
const listY = LIST_TOP_Y + 16;
for (let i = 0; i < 2; i++) {
    const y = listY + i * LIST_LINE_HEIGHT;
    const isSelected = i === namePreviewIndex;       // masterNamePreviewIndex
    if (isSelected) fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
    print(LIST_LABEL_X, y, i === 0 ? "Edit" : "OK", isSelected ? 0 : 1);
}
```

**Fix the slot one and Master FX stays broken.** That is not a risk this design
might introduce; it is the current state, and it is the literal pair the
question named.

---

## 2. The governing constraint

**One thing changed in one place.** This overrides every other preference here.

| Concern | The one place |
|---|---|
| which params are on a page | `page_plan.mjs` |
| type, range, divability, opacity | `param_meta.mjs` |
| the displayed value string | `param_format.mjs` (+ `options` / `short_options`) |
| stepping a value | `knob_engine.mjs` |
| announcements | `announce_page.mjs` |
| chrome (header, bank bar, brackets, footer) | `render_page_movy.mjs` |
| five-row list geometry | `src/shared/list_geometry.mjs` (leaf, no imports) |
| **drawing a list row** | **`drawMenuList`** — §3 |
| **the chrome around a list** (header band, footer, list rect) | **`drawMenuList`** — §3, re-skinned to movy |
| what a Global Setting *is* | `shadow_ui_global_grid.mjs` — §5 |

Every row except the last two is already single-sourced. Adding an enum option,
a param, a section, a format, or a row style must touch exactly **one** of
these.

The **only** thing that legitimately differs between the grid surface and the
list surface is pixel layout: eight cells in two rows of four, versus five rows
of label-and-value. That difference is irreducible. Everything else is shared,
and any proposal that duplicates a row of this table is wrong regardless of how
reasonable the scope boundary sounds.

---

## 2a. Correction, found during execution: the geometry was NEVER single-sourced

The table above originally named `page_controller.mjs` as the one home of the
list geometry. **That was wrong, and believing it cost a whole task.**

`src/shared/chain_ui_views.mjs` carried a complete SECOND copy — `TITLE_Y`,
`TITLE_RULE_Y`, `LIST_TOP_Y = 15`, `LIST_LINE_HEIGHT`, `LIST_HIGHLIGHT_HEIGHT`,
`LIST_LABEL_X = 4`, `LIST_VALUE_X`, `FOOTER_*` — and **every** shadow view
module imported `LIST_TOP_Y` from there, then handed it back as
`listArea.topY`. So when §3's re-skin moved `menu_layout.mjs`'s default to 10,
~20 call sites overrode it with the stale 15.

Two consequences, and the second is the lesson:

- On the device the re-skin was **inert**: an 8-row dead band under the header
  band, and a 4-row window where the work claimed 5.
- `tests/host/test_list_behavior.sh` imported `LIST_TOP_Y` from
  `menu_layout.mjs`, so it measured a rect **nothing rendered with**. It passed,
  it reported the intended 4→5 gain, and it was describing a screen that did not
  exist. A green matrix only proves the axis you chose.

There was also a THIRD list-row renderer, `createSelectListRenderer`
(`chain_ui_views.mjs:129`) — dead, zero callers, already marked
`@deprecated` by the 2026-06 sweep.

Fixed by `src/shared/list_geometry.mjs`: a true leaf that imports nothing, from
which `menu_layout.mjs`, `chain_ui_views.mjs`, `render_page.mjs` and
`render_page_movy.mjs` all re-export the names their consumers already use — so
no view module changed. The test now greps `src/` for exactly one
`export const` per geometry name, and asserts by identity that the value
`menu_layout` holds IS the one the screens receive, before believing any window
arithmetic.

**The general rule this earns:** before trusting that a concern is
single-sourced, grep for a second `export const` of its name. An import that
looks like it reaches the shared definition may reach a stale twin, and every
test above it will agree with the twin.

## 3. One list, wearing the movy chrome (first, not later)

> *"BEHAVIOR baseline is the goal, but we want one list that looks good in
> slots and in mfx and as a file picker. it's one list."*
>
> *"pixel baseline is not the goal, unified is. it should look like Movy stuff:
> header, footer, etc."*

**`drawMenuList` is already the one list.** 53 call sites outside
`menu_layout.mjs`, and the file picker is one of them
(`drawFilepathBrowser`, `shadow_ui.js:13437`). It is not short of consumers.

What it lacks is the **chrome**. Its callers pair it with `drawMenuHeader` —
title text at `TITLE_Y = 2`, rule at `TITLE_RULE_Y = 12`, list from
`LIST_TOP_Y = 15` — while the page chrome uses a 7px header band, the bank bar
at row 7, and the list at `MENU_LIST_Y = 10`. Same footer rule at y55; the
divergence is entirely above the list.

So the work is three moves, and **the point of all three is that the look
changes**:

1. **Re-skin `drawMenuList` to the movy chrome** — header band, footer, list
   rect at `MENU_LIST_X/Y/W`. All 53 callers inherit it at once; that is the
   "looks good in slots and in mfx and as a file picker" property, and it is a
   property of there being one widget rather than of any caller.
2. **Bring the six stragglers onto it** (§1's table), so nothing draws its own
   row.
3. **Have `page_controller` draw its rows through it too**, so the page chrome
   and every other screen are not merely similar but identical.

### 3.0 Why `menu-style-v2` is not built on

There is an unmerged branch, `menu-style-v2` (worktree at
`.worktrees/menu-style-v2`), whose 12 commits already re-skin
`drawMenuHeader`, `drawMenuFooter` and `drawMenuList`. It is **superseded, not
merged**, and the reason is a date:

| | |
|---|---|
| `menu-style-v2` — design and all 12 commits | **2026-04-19**, in one day |
| movy grid first landed | 2026-08-16 |
| movy re-cut against its Elektron reference | 2026-08-19 |

**v2 predates the movy design language by four months.** Its geometry was
reverse-engineered from `capture_2.json` mockups before movy existed, so it was
never an attempt to match it — the two are alternative directions, not
variations. Three further reasons it is the wrong base:

- **Its mechanism is the thing being removed.** V2 ships as a feature flag: a
  `shadow_control_t` field, JS bindings, a `features.json` key, a Global
  Settings toggle, and ~15 `v2 ? V2_X : X` ternaries *inside* `drawMenuList`.
  That is a second code path living in the widget this design exists to make
  singular. The end state carries no style choice at all.
- **It is stale on a divergent branch** — 454 files and ~98k insertions from
  main, carrying unrelated work (speaker EQ, Link Audio drain). Rebasing four
  months across a `menu_layout.mjs` that §3 rewrites anyway costs about what
  doing the work costs.
- **The fleet already wears movy.** The knob grid, enum picker, module picker
  and the three list page kinds in `page_controller` are movy today. Choosing
  tamzen would mean re-skinning *those* to match the menus.

The branch stays in place as a record. Nothing here builds on it, and its flag
never ships.

### 3.1 The baseline is BEHAVIOUR, not pixels

An earlier draft asserted "no screen should look or act different afterwards"
and proposed pixel-hash identity as the gate. **The first half was wrong and it
made the gate wrong.** Every screen is *supposed* to look different afterwards —
that is the deliverable. A test demanding pixel identity would fail on success
and pass only if the work were not done.

What must hold across the change is **behaviour**:

- the same items appear, in the same order
- the selected index moves the same way under the same jog input
- the scroll offset reveals the same item at the same boundary
- the same value string renders for the same item
- edit mode is enterable and the jog changes the value

Those are assertable without a framebuffer. Pixel renders stay in the loop as a
**review artifact** — regenerated freely, read case by case by a human — not as
a pass/fail identity check. The repo already distinguishes these: a reviewed
fixture change means reading the render, and
`tests/host/test_chain_editor_snapshot.sh` refreshed its Master FX hashes
exactly once, at the step whose purpose was to move those pixels.

### 3.2 Edit affordance

`drawChainSettings` and `drawLfoEdit` draw `< value >` chevrons; `drawSlotSettings`
draws `[value]`; `drawGlobalSettings` brackets the *label*; `drawKnobEditor` has
no edit state. Four spellings of one idea. `drawMenuList` already implements
`editMode` → `[value]`, so that is the survivor and the other three converge on
it. A caller-side special case here would be the same mistake at smaller scale.

Converging first matters because building the new surface on top of the drift it
is meant to end is how there came to be six. Add the guard in §7.3 in the same
pass so the count can only go down.

---

## 4. The list layout for `PAGE_KNOBS` — no second renderer

An earlier draft proposed "a new renderer in `src/shared/param_pages/`, beside
`render_page_movy.mjs`." **That was the rabbit hole**, and it is struck.

The five-row list already lives *inside* `page_controller.mjs` and already draws
three page kinds through the movy primitives. Its geometry is exported for
exactly this reason:

> *"Exported because other screens in the page chrome — the module picker, for
> one — must sit in exactly this rect or the two look subtly unlike each other.
> **One definition, not a matching pair of magic numbers.**"*

So a knobs page shown as a list is **the existing menu-page list fed the page's
params instead of its entries**. What is genuinely new is small:

- map a `PAGE_KNOBS` page's params to list rows (label + formatted value) — the
  formatting call is the one `render_page_movy` already makes
- route jog-to-edit through the same `knob_engine` step and the same `io.write`
  the grid turn already uses

`param_view` then selects a *layout* inside one engine, not a path between two.
TTS forces list at the seam `paramPagesEnabled()` already owns. Announcements
come from `announce_page.mjs`, which also retires the hierarchy editor's bespoke
`announceParameter` / `announceMenuItem`.

**The rejected alternative** is a mode flag threaded through
`render_page_movy.mjs`'s 1638 lines. That is the `geom` all-or-nothing trap in
another costume: a partial `{cellW}` makes every cell origin `NaN`, reaches
`line()`'s `for(;;)`, and freezes the `shadow_ui` tick. Layout selection belongs
at the page-draw call site, never woven through widget code.

### 4.1 The blast radius, stated plainly

`paramPagesEnabled()` is a **global** seam. The moment TTS routes to the
controller's list layout instead of the hierarchy editor, it does so for slots,
Master FX and all ~95 module contracts at once — there is no per-contract
opt-in, and adding one would itself be a parallel path.

So the staging is: §3 and §5 land without touching that seam (Global Settings
gains a contract; TTS users keep the hierarchy editor for components exactly as
today). Flipping TTS onto the list layout is §6, and it is a deliberate
single act with the whole fleet behind it — not a side effect of this work.

The transitional state has TTS served by the hierarchy editor for components and
by the controller for Global Settings. That is **two paths, which is what
exists today** (hierarchy editor + hand-rolled Global Settings); the count does
not grow, and one of the two is now the shared one.

---

## 5. Global Settings as the first consumer

### 5.1 Today

`GLOBAL_SETTINGS_SECTIONS` (`shadow_ui.js:2456`) is 7 sections plus a
`[Help...]` action, item counts 6/8/6/1/1/3/2. Around it: four module-level
state vars (`globalSettingsSectionIndex`, `…InSection`, `…ItemIndex`,
`…Editing`), three switch arms (jog ~14590, select ~15434, back ~15907),
`drawGlobalSettings` (`shadow_ui_settings.mjs:105`), and two **misnamed**
helpers — `getMasterFxSettingValue` (13792) and `adjustMasterFxSetting` (13912)
serve Global Settings, not Master FX.

### 5.2 The contract

`shadow_ui_global_grid.mjs`, modelled on `shadow_ui_slot_grid.mjs` and pure the
same way: it declares data and takes accessors, reads no globals, needs no
framebuffer to test.

Seven sections become seven levels, each planning to one page. Six are
`PAGE_KNOBS`; **Updates** is `PAGE_MENU` (two actions, nothing to show).
`[Help...]` stays a navigation entry into the existing help stack. Every section
fits one page — no pagination anywhere, which is what makes sections-as-levels
work without the bank bar papering over an awkward split. Navigation goes from
two levels to one jog axis with a section picker on click.

**Why a separate file, given the drift warning.** `shadow_ui_slot_grid.mjs`
holds the slot *and* Master FX contracts together on purpose:

> *"Master FX getting its own file is precisely how the two chain editors
> drifted apart in the first place: one reasonable-sounding scope boundary at a
> time, until the knob card worked on one screen and not the other."*

The test that warning implies is **shared substance**, not shared topic: those
two live together because they share the LFO pages outright — `lfoParams` /
`lfoLevels` is one declaration serving both — and splitting them would produce
two copies of it. Global Settings shares no pages with either: no LFO, no chain
prefix, no preset actions, a wholly different accessor set. There is nothing to
duplicate by separating it.

This is a question about where a *declaration* lives. The *engine* question —
which is the important one — is answered by §2, §3 and §4, and is unaffected
either way. If Global Settings ever grows a page shared with the slot contract,
that page moves into the shared file: the rule is the substance, not the
filename.

### 5.3 Accessor routing

~19 entries mapping keys to their backend: `shadow_get_param` /
`shadow_set_param`, `tts_get_*` / `tts_set_*`, `overlay_knobs_*`,
`display_mirror_*`, feature flags.

The contract needs an absolute `writeParam(key, value)`, but
`adjustMasterFxSetting` is **delta-based and side-effectful**: most branches also
call `saveMasterFxChainConfig()` and write a cache var (`cachedLinkAudioRouting`,
`cachedResampleBridgeMode`, `cachedLatencyCompEnabled`, `cachedUsbcOutPersist`).
Those side effects move into `writeParam`, or toggling Link routing sets the
param and never persists it — silently.

### 5.3a Persistence is THREE things, not one

Found while converting `adjustMasterFxSetting`. The 25 Global Settings params do
not share one save path:

| kind | count | how it persists |
|---|---|---|
| shared sink | 6 | `saveMasterFxChainConfig()` — the `PERSISTING_KEYS` set |
| own saver | 7 | a dedicated call welded to the assignment (`saveParamViewConfig`, `savePadTypingConfig`, `saveFilebrowserConfig`, `saveAutoUpdateConfig`, …) |
| backend-owned / session | 12 | the backend persists it (`tts_set_*`), or it is deliberately not persisted |

The distinction is load-bearing: a write in group 1 that skips its
`saveMasterFxChainConfig()` sets the param and loses it on reboot, **silently**.
`PERSISTING_KEYS` is therefore **derived from the routing table**, not
hand-listed — a hand-list rots the first time a key changes category, and the
mutation that widens it to every key is the one that proves the test is not
vacuous.

### 5.5a Stored values are not indexes

`resample_bridge` stores `[0, 2]`. Treating an enum's index as its value writes
mode `1`, which does not exist — and does so silently. Any enum whose stored
values are not `0..n-1` needs its real values declared (`GLOBAL_ENUM_VALUES`)
and round-tripped in the test.

### 5.5b An unknown wire state needs its own option

`usbc_out_persist` annotates a bool with the source last seen on the wire. But
the source can be **unobserved** (`-1`) before anything has crossed it, and the
old code simply omitted the annotation in that case — a formatter can drop a
parenthetical; a declaration cannot.

Mapping unknown onto `On (Mic)` was rejected: this row exists *because* Move's
own Settings screen goes stale, so it "is the only honest read of what is
actually routed". A confident "Mic" that has never been observed is worse than
the stale screen it was added to correct. The declaration gets a fourth option —
`["Off", "On", "On (Mic)", "On (Main Out)"]` — where plain `On` means
*persistence is on, source not yet seen*. All three On indexes store `1`.

**The general rule:** a contract must be able to express every state its
formatter could, including "not known". A state the old code showed by omission
becomes an explicit option, not a default that guesses.

### 5.4 Modal hand-off

Two writes raise modals drawn and answered under `case VIEWS.GLOBAL_SETTINGS`:
`resample_bridge` → the Schwung Mix warning, and `link_audio_routing` /
`link_audio_publish` → `warnIfLinkDisabled`. These need the
`runSlotActionFromGrid` / `runMasterFxActionFromGrid` hand-off — exit,
`suppress…Once`, set the view, reconcile back via `maybeReturnTo…Grid`. Third
instance of that pattern. It reconciles rather than hooking each exit, for the
reason `maybeReturnToSlotGrid` records: hooking each exit is how the original
bug got there.

### 5.5 `usbc_out_persist` needs no exception

An earlier draft called this "the one display conflict" and sanctioned a
divergence. **Wrong** — the mechanism already ships.

The value renders as `"On (Main Out)"`, a bool annotated with the source last
seen on the wire, because Move's own Settings screen goes stale after Schwung
restores it. A three-character enum square cannot show that. But `short_options`
is exactly that mechanism: `render_page_movy.mjs:1206` consults it **for the
square only**, while every surface with room — the held-knob header, and now the
list — uses the long `options`. `SLOT_GRID_PARAMS` already relies on it
throughout (`THR`/`Thru`, `AUT`/`Auto`).

So the annotation is the long option and `"ON"` is the short one. One
declaration, two renderings, no per-surface case.

**Generalised:** any value too long for a cell is a `short_options` entry, never
a second code path. A case `short_options` cannot express is a signal to extend
the shared formatter — not to branch on which surface is drawing.

### 5.6 Deletions

The four `globalSettings*` state vars, the three switch arms, and
`drawGlobalSettings`'s body — roughly 200 lines of bespoke input handling out of
`shadow_ui.js`.

---

## 6. Retiring the hierarchy editor

Stated goal, explicit follow-up, **not** in this scope.

`param_meta.mjs` already classifies the whole fleet, and the opaque tail is
small:

| | float | int | enum | opaque |
|---|---|---|---|---|
| `chain_params` | 1685 | 1125 | 774 | filepath 22, wav_position 2 |
| inline params | 212 | 56 | 118 | filepath 4, toggle 2 |

~28 opaque params against ~3970 ordinary ones. `KIND_OPAQUE`, `OPAQUE_TYPES`,
`divable` and `divable_mark` all live in the shared library already.

**The one deep coupling is `openParamEditorFromGrid` (`shadow_ui.js:2317`).**
When the grid dives an opaque param it does not open an editor; it *exits into
the other engine* — `exitParamPages()` → `suppressParamPagesOnce` →
`enterHierarchyEditor` → find the level listing the param → drive that editor's
cursor onto the row → open it. The comments there already record two bugs from
that maneuver: Master FX's `fx2:sample_path` prefix stripping, and granny's
`position` living in a level the page was not on.

Full replacement means the filepath browser, text entry and the LFO two-step
picker become things the **controller** opens directly — the same move
`PAGE_MENU` / `PAGE_PRESET` / `PAGE_ITEMS` already made. Not new architecture;
the fourth and fifth instances of a pattern with a track record.

This is also where §4.1's TTS flip belongs, since it is the act that makes the
hierarchy editor unreachable. Scoped after the list layout exists, with real
code in hand.

---

## 7. Testing

Each of these fails **silently**, which is why they are pinned rather than left
to review.

1. **Persistence side effects.** A `writeParam` that skips its
   `saveMasterFxChainConfig()` / cache-var write sets the param and loses it on
   reboot. Assert per-key that a write reaches the persistence call.
2. **Surface agreement, no exceptions.** Grid and list must display the same
   value for the same contract — **every key, no allow-list**. An exceptions
   list is how §2's table grows a second column, so the test is written with
   nowhere to put one. `usbc_out_persist` passes via `short_options` (§5.5), not
   via exemption.
3. **No new hand-rolled rows.** `src/` currently holds **seven**
   `fill_rect(…, LIST_HIGHLIGHT_HEIGHT, …)` sites: the six in §1 plus
   `menu_layout.mjs:109`, which *is* `drawMenuList` and is the sanctioned one.
   After §3 the count outside `menu_layout.mjs` is **zero**; the test pins zero
   and fails on any reintroduction. Scanned across `src/`, not from a
   hand-maintained list, so a new file is covered on arrival. Kin to
   `test_master_fx_slots_js.sh`, which fails on `MASTER_FX_SLOTS` drift rather
   than trusting two copies to be checked by hand.

   **Its limit, stated:** this pins one *idiom*, not the property. A future
   hand-rolled list that reaches for its own constants instead of
   `LIST_HIGHLIGHT_HEIGHT` passes it while being exactly the thing it forbids —
   a green matrix only proves the axis you chose. The test is a tripwire on the
   known copy-paste path, and review still owes the general case.
4. **No second definition.** The list layout must not introduce its own copy of
   anything in §2's table — no second list geometry, no second formatter, no
   second metadata resolver. Derived from the exports so it widens on its own.

Contract purity is testable with no UI, no device and no framebuffer — hand
`shadow_ui_global_grid.mjs` its accessors, as `shadow_ui_slot_grid.mjs`
demonstrates. Host tests only; on-hardware behaviour verified manually per
`CLAUDE.md`, and §3 in particular needs each of its five screens compared before
and after.

---

## 8. Order of work

1. **§3** — converge the six onto `drawMenuList`, growing it a chevron
   affordance. Pure refactor; any visible change is a regression. Land test 7.3
   in the same pass.
2. **§4** — `PAGE_KNOBS` as a list layout inside `page_controller.mjs`. Land
   tests 7.2 and 7.4.
3. **§5** — Global Settings contract, accessor routing, modal hand-off,
   deletions. Land test 7.1.
4. **§6** — follow-up: opaque params into the controller, then flip TTS, then
   delete the hierarchy editor.

---

## 9. Out of scope

- Retiring `enterHierarchyEditor` and its ~34 functions (§6)
- Moving the filepath browser / text entry / LFO picker into the controller
- Flipping `paramPagesEnabled()` for TTS (§4.1) — deliberately deferred to §6
- Grid announcements for screen-reader users
- Any change to `param_view`'s default
