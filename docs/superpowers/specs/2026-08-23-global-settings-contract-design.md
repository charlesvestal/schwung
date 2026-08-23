# Global Settings as a contract, and the list renderer — design, 2026-08-23

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
renderer, at which point `param_view` stops being a fork *between* engines and
becomes a renderer choice *inside* one.

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

### 3.1 The `PAGE_KNOBS` list renderer

A new renderer in `src/shared/param_pages/`, beside `render_page_movy.mjs`.
Same page object, same `io` accessors, list output. `drawParamPages` selects
between renderers on `param_view`; TTS forces list at the seam
`paramPagesEnabled()` already owns.

Announcements route through the library's existing `announce_page.mjs` rather
than the hierarchy editor's bespoke `announceParameter` / `announceMenuItem`
calls — one place, not two.

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

## 4. The one display conflict

`usbc_out_persist` renders as `"On (Main Out)"` — a bool annotated with the
source last seen on the wire, because Move's own Settings screen goes stale
after Schwung restores the value and this row is the only honest read of what is
actually routed.

A grid switch widget cannot show that annotation. The list can. This is the one
place the two renderers legitimately want to disagree about the same contract,
so it is called out here rather than discovered later: either a
`display_format`, or accept that the annotation appears in list mode only.

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

That work is deliberately scoped **after** the renderer exists, with real code
in hand, rather than guessed at now.

---

## 6. Why Global Settings is the pilot

It is the simplest contract in the system: no presets, no child levels, no
filepath params, no canvas, no LFO targets, no modulation, every section fits
one page. It exercises **none** of §5's hard parts.

That is exactly what makes it the right surface to prove the list renderer and
the two-mode `param_view` switch on — low risk, and no throwaway code, because
the contract it produces is the one it keeps.

Rejected: *unify first, Global Settings after* — puts the pilot risk on the
busiest screens in the UI and makes Global Settings wait behind a large job.
Rejected: *Global Settings first, unify later* — knowingly writes ~200 lines
destined for deletion, which is the duplicate maintenance this design exists to
stop.

---

## 7. Testing

Both of these fail **silently**, which is why they are pinned rather than left
to review:

1. **Persistence side effects.** A `writeParam` that skips its
   `saveMasterFxChainConfig()` / cache-var write sets the param and loses it on
   reboot. Assert per-key that a write reaches the persistence call.
2. **Renderer agreement.** The list and grid renderers must display the same
   value for the same contract, with `usbc_out_persist` (§4) as the declared
   exception.

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
