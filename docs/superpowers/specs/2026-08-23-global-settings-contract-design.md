# Global Settings as a contract, and the list layout — design, 2026-08-23

> *"does it make sense to have global settings be 'knobs' like slot and master
> fx settings? if nothing else, we need to get them movy-list-ified, right?"*
>
> *"but it sounds like we have to forever maintain the list path, should we
> combine these into one engine with two modes?"*

Two questions, and the second one reframes the first. Global Settings is the
last screen in the shadow UI with its own bespoke navigation. Making it a
synthesised contract is the small half. The large half is that doing so
otherwise means writing its list surface **twice** — once in the contract, once
in the hand-rolled code — which is the duplicate maintenance the second question
is about.

---

## 1. Where things actually stand

**The knob grid is the default now.** `tests/host/test_param_view_default.sh`
pins `paramViewGlobal = 1`. The hierarchy list editor is no longer the primary
surface with a grid bolted on; it is the fallback for screen-reader users and
for people who explicitly chose List. It costs ~506 references across ~34
functions spanning `shadow_ui.js` from 1943 to 13502.

**Screen-reader mode already forces the list, globally.**
`paramPagesEnabled()` (`shadow_ui_param_pages.mjs:114`) returns false when TTS
is on, *before* it consults `param_view`. Every screen behind that gate
inherits the behaviour. Its comment frames this as provisional rather than
principled — *"only navigable by ear once the announce calls are proven on
hardware"* — so it is a placeholder for grid announcements nobody has validated
on device, not a permanent rule.

**One engine with two modes is already three-quarters built.**
`drawParamPages` (`shadow_ui_param_pages.mjs:617`) draws `PAGE_KNOBS`,
`PAGE_MENU`, `PAGE_PRESET` and `PAGE_ITEMS`. Three of those are lists, rendered
inside the movy page chrome by the controller itself. The comment above it is a
changelog of that migration happening one kind at a time, each pulled in because
handing off to the *other* engine produced a bug:

- `PAGE_MENU` — refusing it ejected slot settings to "No presets"
- `PAGE_PRESET` — the eject landed in the list editor, whose jog is wired to the
  preset browser, so **jogging past** a preset page auditioned every preset it
  crossed
- `PAGE_ITEMS` — *"a soundfont or NAM-model list is a real list, so it can be
  five rows in the page chrome rather than a separate screen"*

Three bugs at the same seam is not three coincidences. **The only page kind with
no list rendering is `PAGE_KNOBS`.**

So the ask is not "combine two engines". It is: give `PAGE_KNOBS` a list
layout, at which point `param_view` stops being a fork *between* engines and
becomes a layout choice *inside* one.

---

## 2. Global Settings today

`GLOBAL_SETTINGS_SECTIONS` (`shadow_ui.js:2456`) is 7 sections plus a `[Help...]`
action, item counts 6/8/6/1/1/3/2. Around it:

- four module-level state vars — `globalSettingsSectionIndex`, `…InSection`,
  `…ItemIndex`, `…Editing`
- three switch arms — jog (~14590), select (~15434), back (~15907)
- `drawGlobalSettings` (`shadow_ui_settings.mjs:105`), a two-branch body
- `getMasterFxSettingValue` (13792) and `adjustMasterFxSetting` (13912), both
  **misnamed**: they serve Global Settings, not Master FX

It already calls `drawMenuList`, so it is half movy-list-ified. What it lacks is
the movy chrome, the bank bar, the jog-click section picker, and the enum
picker.

---

## 3. What we are building

### 3.0 The governing constraint

> *"make sure we're not ending up in a rabbit hole of multiple parallel paths.
> i dont want to have to update things in multiple places when it's one thing."*

This overrides every other preference here. Concretely, **one thing changed in
one place** means:

| Concern | The one place |
|---|---|
| which params are on a page | `page_plan.mjs` |
| type, range, divability, opacity | `param_meta.mjs` |
| the displayed value string | `param_format.mjs` (+ `options` / `short_options`) |
| stepping a value | `knob_engine.mjs` |
| announcements | `announce_page.mjs` |
| chrome (header, bank bar, brackets, footer) | `render_page_movy.mjs` |
| five-row list geometry | `MENU_LIST_*` in `page_controller.mjs` |
| what a Global Setting *is* | `shadow_ui_global_grid.mjs` |

Everything above is already single-sourced today except the last row. Adding an
enum option, a param, a section, or a format must touch exactly one of these —
never two.

The **only** thing that legitimately differs between the grid surface and the
list surface is pixel layout: eight cells in two rows of four, versus five rows
of label-and-value. That difference is irreducible. Everything else is shared,
and any proposal that duplicates a row of the table above is wrong regardless of
how reasonable the scope boundary sounds.

### 3.1 There is no second renderer

An earlier draft of this document proposed "a new renderer in
`src/shared/param_pages/`, beside `render_page_movy.mjs`." **That was the
rabbit hole**, and it is struck.

The five-row list already lives *inside* `page_controller.mjs` and already draws
`PAGE_MENU`, `PAGE_PRESET` and `PAGE_ITEMS` through the movy primitives. Its
geometry is exported for exactly this reason:

> *"Exported because other screens in the page chrome — the module picker, for
> one — must sit in exactly this rect or the two look subtly unlike each other.
> **One definition, not a matching pair of magic numbers.**"*

So a knobs page shown as a list is **the existing menu-page list fed the page's
params instead of its entries**. What is genuinely new is small:

- map a `PAGE_KNOBS` page's params to list rows (label + formatted value) —
  the formatting call is the one `render_page_movy` already makes
- route jog-to-edit through the same `knob_engine` step and the same `io.write`
  the grid turn already uses

`param_view` then selects a *layout* inside one engine, not a path between two.
TTS forces list at the seam `paramPagesEnabled()` already owns. Announcements
come from `announce_page.mjs` — which also retires the hierarchy editor's
bespoke `announceParameter` / `announceMenuItem`.

**The rejected alternative** is a mode flag threaded through
`render_page_movy.mjs`'s 1638 lines. That is the `geom` all-or-nothing trap in
another costume: a partial `{cellW}` makes every cell origin `NaN`, reaches
`line()`'s `for(;;)`, and freezes the `shadow_ui` tick. Layout selection belongs
at the page-draw call site, not woven through the widget code.

### 3.2 `shadow_ui_global_grid.mjs`

The Global Settings contract, modelled on `shadow_ui_slot_grid.mjs` and pure the
same way: it declares data and takes accessors, reads no globals, needs no
framebuffer to test.

Seven sections become seven hierarchy levels, each planning to one page. Six are
`PAGE_KNOBS`; **Updates** is `PAGE_MENU` (two actions, nothing to show).
`[Help...]` stays a navigation entry into the existing help stack rather than
becoming a page. Every section fits one page — no pagination anywhere, which is
what makes sections-as-levels work cleanly instead of needing the bank bar to
paper over an awkward split.

Navigation goes from two levels (section list → item list) to one jog axis with
a section picker on click.

**Why a separate file, given the drift warning.** `shadow_ui_slot_grid.mjs`
holds the slot *and* Master FX contracts in one file on purpose, and says so:

> *"Master FX getting its own file is precisely how the two chain editors
> drifted apart in the first place: one reasonable-sounding scope boundary at a
> time, until the knob card worked on one screen and not the other."*

That warning has to be answered, not stepped around. The test it implies is
**shared substance**, not topical similarity: those two live together because
they *share the LFO pages outright* — `lfoParams` / `lfoLevels` are one
declaration serving both — and differ only in values page, actions and key
prefix. Split them and you get two copies of the LFO pages, which is the drift.

Global Settings shares **no** pages with either: no LFO, no chain prefix, no
preset actions, a wholly different accessor set. There is nothing to duplicate
by separating it, and nothing to unify by merging it — merging would produce one
file holding two unrelated things, which is not the same property.

The check that actually matters is §3.0's table, and it is unaffected either
way. If Global Settings ever *does* grow a page shared with the slot contract,
that page moves into the shared file — the rule is the substance, not the
filename.

### 3.3 Accessor routing

~19 entries mapping keys to their backend: `shadow_get_param` / `shadow_set_param`,
`tts_get_*` / `tts_set_*`, `overlay_knobs_get_mode` / `…_set_mode`,
`display_mirror_get` / `…_set`, feature flags.

The grid needs absolute `writeParam(key, value)`, but `adjustMasterFxSetting` is
**delta-based and side-effectful**. Most branches also call
`saveMasterFxChainConfig()` and write a cache var (`cachedLinkAudioRouting`,
`cachedResampleBridgeMode`, `cachedLatencyCompEnabled`, `cachedUsbcOutPersist`,
…). Those side effects must move into `writeParam`, or toggling Link routing
from the new surface sets the param and never persists it — silently.

### 3.4 Modal hand-off

Two writes raise modals drawn and answered under `case VIEWS.GLOBAL_SETTINGS`:
`resample_bridge` → the Schwung Mix warning, and `link_audio_routing` /
`link_audio_publish` → `warnIfLinkDisabled`.

From a contract surface these need the `runSlotActionFromGrid` /
`runMasterFxActionFromGrid` hand-off — exit, `suppress…Once`, set the view,
reconcile back via a `maybeReturnTo…Grid`. This is the **third** instance of
that pattern. It reconciles rather than hooking each exit, for the reason
`maybeReturnToSlotGrid` records: hooking each exit is how the original bug got
there.

### 3.5 Deletions

The four `globalSettings*` state vars, the three switch arms, and
`drawGlobalSettings`'s body. Roughly 200 lines of bespoke input handling out of
`shadow_ui.js`.

---

## 4. `usbc_out_persist` needs no exception

An earlier draft called this "the one display conflict" and sanctioned a
divergence between the two surfaces. **That was wrong** — the mechanism already
exists.

`usbc_out_persist` renders as `"On (Main Out)"`: a bool annotated with the
source last seen on the wire, because Move's own Settings screen goes stale
after Schwung restores the value, so this row is the only honest read of what is
actually routed. A three-character enum square cannot show that.

But `short_options` is exactly that mechanism, and it already ships:
`render_page_movy.mjs:1206` consults it **for the square only**, while every
surface with room — the held-knob header, and now the list — uses the long
`options`. `SLOT_GRID_PARAMS` already relies on it throughout (`THR`/`Thru`,
`AUT`/`Auto`, `ALL`/`All`).

So the annotation is the long option and `"ON"` is the short one. One
declaration, two renderings of it, no per-surface special case, and nothing new
to build.

**Generalised:** any future value too long for a cell is a `short_options`
entry, never a second code path. If a case ever arises that `short_options`
genuinely cannot express, that is a signal to extend the shared formatter — not
to branch on which surface is drawing.

---

## 5. Retiring the hierarchy editor

Stated goal, explicit follow-up, **not** in this scope.

`param_meta.mjs` already classifies the whole fleet, and the opaque tail is
small:

| | float | int | enum | opaque |
|---|---|---|---|---|
| `chain_params` | 1685 | 1125 | 774 | filepath 22, wav_position 2 |
| inline params | 212 | 56 | 118 | filepath 4, toggle 2 |

~28 opaque params against ~3970 ordinary ones. `KIND_OPAQUE`, `OPAQUE_TYPES`,
`divable` and `divable_mark` all live in the shared library already — not in the
hierarchy editor.

**The one deep coupling is `openParamEditorFromGrid` (`shadow_ui.js:2317`).**
When the grid dives an opaque param it does not open an editor; it *exits into
the other engine* — `exitParamPages()` → `suppressParamPagesOnce` →
`enterHierarchyEditor` → find the level that lists the param → drive that
editor's cursor onto the row → open it. The comments there already record two
bugs caused by that maneuver: Master FX's `fx2:sample_path` prefix stripping,
and granny's `position` living in a level the page was not on.

Full replacement therefore means the filepath browser, text entry and the LFO
two-step target picker become things the **controller** opens directly — the
same move `PAGE_MENU` / `PAGE_PRESET` / `PAGE_ITEMS` already made, for the same
reason. Not new architecture; the fourth and fifth instances of a pattern with a
track record.

That work is deliberately scoped **after** the list layout exists, with real code
in hand, rather than guessed at now.

---

## 6. Why Global Settings is the pilot

It is the simplest contract in the system: no presets, no child levels, no
filepath params, no canvas, no LFO targets, no modulation, every section fits
one page. It exercises **none** of §5's hard parts.

That is exactly what makes it the right surface to prove the list layout and
the two-mode `param_view` switch on — low risk, and no throwaway code, because
the contract it produces is the one it keeps.

Rejected: *unify first, Global Settings after* — puts the pilot risk on the
busiest screens in the UI and makes Global Settings wait behind a large job.
Rejected: *Global Settings first, unify later* — knowingly writes ~200 lines
destined for deletion, which is the duplicate maintenance this design exists to
stop.

---

## 7. Testing

All three fail **silently**, which is why they are pinned rather than left to
review:

1. **Persistence side effects.** A `writeParam` that skips its
   `saveMasterFxChainConfig()` / cache-var write sets the param and loses it on
   reboot. Assert per-key that a write reaches the persistence call.
2. **Surface agreement, with no exceptions.** The grid and list layouts must
   display the same value for the same contract — **every key, no allow-list**.
   An exceptions list is how §3.0's table grows a second column, so the test is
   written to have nowhere to put one. `usbc_out_persist` passes via
   `short_options` (§4), not via exemption.
3. **No second definition.** A guard test in the spirit of
   `test_master_fx_slots_js.sh` (which fails on `MASTER_FX_SLOTS` drift between
   C and JS): the list layout must not introduce its own copy of anything in
   §3.0's table — no second list geometry, no second formatter, no second
   metadata resolver. Derived from the exports, not from a hand-maintained
   list, so it widens on its own.

Contract purity is testable with no UI, no device and no framebuffer — hand
`shadow_ui_global_grid.mjs` its accessors, as `shadow_ui_slot_grid.mjs` already
demonstrates. Host tests only; on-hardware behaviour verified manually per
`CLAUDE.md`.

---

## 8. Out of scope

- Retiring `enterHierarchyEditor` and its ~34 functions (§5)
- Moving the filepath browser / text entry / LFO picker into the controller
- Grid announcements for screen-reader users — the TTS-forces-list gate stays
  exactly as it is
- Any change to `param_view`'s default
