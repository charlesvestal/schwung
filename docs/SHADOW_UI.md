# Shadow UI internals

Split out of `CLAUDE.md`, which keeps the shortcuts, the SHM layout and the
slot features, and points here.

Covers `src/shadow/shadow_ui.js` and its `.mjs` siblings: input dispatch order,
the synthesised contracts, the component load gate, User Presets and the
trailing pages, and Master FX persistence.

The tri-state read rule these all depend on — **a param read has THREE answers,
not two** — stays in `CLAUDE.md`, because it is cited from everywhere.

### Whatever is drawn LAST must be fed FIRST

`onMidiMessageInternal` (`src/shadow/shadow_ui.js`) is a run of early-outs ahead
of the per-view switch, and the draw path is a switch with the overlays painted
after it. **The two orders are the reverse of each other**, so an overlay added
to the bottom of the draw path has to be added to the TOP of the input path, and
nothing about either site says so.

The knob grid's early-out is the one that bites, because it is first and it
claims the jog. Text entry sits ~100 lines below it. That was safe only while no
keyboard could be raised over `PARAM_PAGES` — and then User Presets became a
trailing page INSIDE the grid, `enterPresetSaveAs` opened the keyboard without
calling `setView` (its sibling `enterPresetDeleteConfirm` does), so `view` stayed
`PARAM_PAGES` and the grid ate the jog while the keyboard was drawn on top of it.

**The symptom pointed at the wrong subsystem.** Pad typing kept working, so it
read as a keyboard bug: `decodeInput` (`shared/param_pages/page_input.mjs`)
returns `null` for notes 68–99, so pads fall through, but it decodes CC 14 as
navigation and consumes it. A half-working overlay is the signature of a
dispatch-order bug, not a broken handler — check what is *upstream* of the
handler before reading the handler.

Guard the grid block (`&& !isTextEntryActive()`) rather than hoisting the
overlay to the top: the feedback-gate and canvas-steal blocks sit between the
two, and the feedback gate is a safety modal that must keep outranking
everything. Precedence among overlays is deliberate, so moving one is a change
in its own right. `tests/host/test_text_entry_outranks_grid.sh` pins the order,
and pins the `decodeInput` jog-vs-pads asymmetry separately so a future change
that starts claiming pad notes fails loudly instead of silently.

### Global Settings is a SYNTHESISED CONTRACT, not a screen

It runs on the same page engine as a module, a slot's settings and Master FX's
settings — one list, one chrome, one set of widgets. The declaration is
`src/shadow/shadow_ui_global_grid.mjs` (pure, no host globals, tested with no
device by `tests/host/test_global_settings_contract.sh`); the concrete backends
and the cache-var writes that cannot leave `shadow_ui.js` are `globalGridIoFor()`
there. Entry is `enterGlobalSettingsGrid()`, modelled on
`enterMasterFxSettingsGrid`.

**Seven sections are seven PAGES**, jogged through on one axis with the section
picker on click — Display, Audio, Screen Reader, Set Pages, Shortcuts, Services,
Updates. Six are knob pages; Updates is a menu page. **One section, one page** is
load-bearing: a ninth param in any section paginates silently and the bank bar
takes over a split nobody chose. Audio sits at exactly eight. The contract test
pins the per-section counts rather than trusting the shapes.

Three consequences worth knowing:

- **`[Help...]` lives on the Updates page**, one row under `[Module Store]`. It
  used to be a peer of the sections; it cannot be a page of its own (that is an
  eighth page, pinned against twice) and a one-entry menu page is the shape
  Master FX already records as a mistake. See `UPDATES_ACTIONS`.
- **`VIEWS.GLOBAL_SETTINGS` is now only the help viewer's host.** The section
  list, the in-section list, the four `globalSettings*` state vars and the three
  switch arms that drove them are gone. `runGlobalActionFromGrid` /
  `maybeReturnToGlobalGrid` are the third instance of the slot / Master FX modal
  hand-off, and reconcile the same way rather than hooking each exit.
- **The screen reader forces the LIST layout** (`paramPagesLayout()`), because
  Global Settings enters the page chrome even with TTS on — it has no hierarchy
  editor behind it, and it is the screen you go to to turn TTS off.
  `paramPagesEnabled()` still refuses the chrome for every *component*; that
  seam is unchanged.

Persistence is **three** things and conflating them loses a write silently: a
shared `saveMasterFxChainConfig()` sink (derived from the routing table, never
hand-listed), a key-specific saver welded to the assignment, or backend-owned.
Stored values are **not** indexes — `resample_bridge` stores 0 and **2**.

### A timed-out read empties NOTHING, and latches nothing

`loadChainConfigFromSlot`'s `readPosition` was `moduleId && moduleId !== ""`,
which puts `null` (the read did not complete) in the same branch as `""` (the
position is empty) — the comment there said so, having considered only the
unserved case. Loading a module blocks the SPI callback (the thread that also
serves param requests) and `applyComponentSelectionConfirmed` re-syncs
**immediately after its fire-and-forget module write**, i.e. inside that
window. So the position read `null`, was recorded as EMPTY, and
`chainConfigFresh[slot] = true` declared it authoritative — *"clean by
definition once it returns"* was true of the call, not of the answer.

An empty box in the diagram is a `+`, so the position the user had just filled
opened the **module picker** instead of the editor.

**It takes a SECOND defect to make that permanent**, and this is the part worth
remembering: the module signature is a separate set of reads taken milliseconds
later, and they straddled the end of the load. The config read stale-empty; the
signature read the real module. `applySlotModuleSignature` reloads the config
only when the signature **changes** — so the *correct* read is what did the
damage, by matching, and a correct signature never changes again. Osirus logged
a clean 124 ms load at 13:48:53.700–.824 and the editor still drew the position
empty fifteen seconds later, while slot settings — same key, different path —
said "Synth Virus".

Now: a failed read keeps the position it had, leaves the slot **un-fresh** so
the next frame re-reads, and `getSlotModuleSignature` answers **null** rather
than inventing an empty chain (`applySlotModuleSignature` refuses null). A
failed `*_count` keeps the section length — 0 from a timeout truncates the whole
section, not one position.

Falling out of it for free: the picker writes the chosen module into
`chainConfigs` **before** the DSP write, so "what we already had" during the
load window *is* the module just picked — the box shows it throughout, and
there is no loading state to maintain. `tests/host/test_chain_config_read_failure.sh`
lifts the real functions and drives that sequence, reads failing on frame 1 and
landing on frame 2.

### A component editor WAITS; it does not decide from one read

Opening a component's editor used to be one read of `<prefix>:ui_hierarchy` and
`if (!hierarchy) enterComponentEditFallback(...)` — which is the three-answers
defect ("A param read has THREE answers", `CLAUDE.md`) one layer above the
controller that solves it, and the fallback is
irreversible. For MiniJV and Osirus, the two slowest things in the fleet to
come up, that drew an editor with **nothing in it**: neither ships a
`ui_chain.js`, so the fallback lands on the bare preset browser, and the preset
reads it makes there fail for the same reason the hierarchy read did.

What made the entry the wrong place to give up is that **everything which knows
how to wait is behind it** — the grid's `Loading...` hold, its bounded contract
retry, its ten-second recovery probe, the list editor's `is_loading` re-fetch.

`src/shared/component_load_gate.mjs` answers **ENTER / HOLD / FALLBACK**, and
`openComponentEditor` (`shadow_ui.js`) is the one gate both editors — slot
chain and Master FX — enter through. HOLD raises `VIEWS.COMPONENT_LOADING`
("Loading...", `Back: exit`) and asks again: ~0.5 s apart for ~20 s, then every
ten seconds for as long as the screen is up. **There is no give-up-and-show-the
-fallback ending**, on purpose — a blank editor is the failure being fixed.

**The empty answer needs a second question.** A module that declares no
hierarchy and a position whose module has not finished arriving BOTH answer
`""`. `<prefix>_module` separates them: the chain host publishes the name only
after `create_instance` returns (`chain_host.c:504`). Named + no hierarchy
falls back **immediately**, so the well-behaved fleet never sees the hold, and
entering still costs the one read it always did (`module` and `is_loading` are
read lazily, on the ambiguous branch only).

The wait is view-agnostic — it sits in front of the destination choice, so it
works with Param View on Knobs or List and with the screen reader on — and it
is drawn and serviced on **both** draw paths, main and co-run. The probe runs
*before* the dispatch, so a probe that lands opens the editor on that frame.
`tests/host/test_component_load_hold_wiring.sh` pins all of that from source;
`test_component_load_gate.sh` unit-tests the decision, including that a named
module with no hierarchy is **not** held.

Not a regression: the old gate is byte-identical at `v0.11.6`. What changed is
how long these two modules take to answer.

### User Presets

Per-component preset snapshots for any chain module (synth, audio FX, or MIDI FX). Reached from the component's own knob-grid **"My Presets"** page — its `Preset` row / `Load…` action. That is the only door: the module-swap list used to carry an indented `[User Presets]` row as a second one, and it was removed once My Presets became a page on the component itself. A swap list is for swapping, and the presets now sit one jog from the controls they belong to, beside the Save / Save As / Delete that were already there. A preset captures that component's opaque `<prefix>:state` blob (`synth` / `fx1`..`fx4` / `midi_fx1`) — the same string slot autosave and chain patches use — saved to `/data/UserData/schwung/presets/<module-id>/<name>.json`. Keyed by **module id**, so a preset saved on a module in one slot is offered wherever that module is loaded (cross-slot reuse).

**The browser is exactly ONE thing: choose a preset.** Picking a row LOADS it
immediately and commits — there is no per-preset Load/Delete detail screen.
Save, Save As and Delete are not offered here at all; they live on the
component's own "My Presets" grid page (see below). This was three separate
hardware reports, one cause: the verbs had moved to the grid page but the
browser still offered its own copies — *"loading a preset shouldnt show
load/delete, it should just load it (delete is on the main menu)"*, *"after
deleting i get to a menu of [save current] not the preset (none) page"*,
*"i also see [save current] if i load without saving"*. Scrolling the list
**auditions live** (debounced) **when Global Settings → Audition is on**;
Back reverts to the slot's original state. That gate (`browser_preview`,
shared with the file browser's WAV preview) **defaults to OFF**: auditioning
applies state to the live slot, and this list stopped being hard to reach the
moment it became reachable from a page at the end of every component. Off
disables the audition, not the list — a pick still loads, and with it off the
browser pays no `:state` read on entry. Autosave is suppressed while
auditioning (`isPresetPreviewActive()`) so an uncommitted preview is never
persisted into `slot_N.json`. Impl: `src/shadow/shadow_ui_presets.mjs` (view
module). Developer state-contract notes in `docs/MODULES.md`.

A committed Load, or a completed Delete (still reached exclusively from the
grid's My Presets page, via `enterPresetDeleteConfirm` — the SAME
confirm-delete screen as before, just with no detail screen left in front of
it), both exit through `VIEWS.CHAIN_EDIT`. `maybeReturnToComponentGrid` (see
below) is what routes the arrival back onto the My Presets page
specifically, by NAME, and falls through to the plain chain editor when the
position no longer holds a module to show one for (Remove Module).

### Every component's knob grid ends with two pages it never declared

Load a synth, audio FX or MIDI FX in one of the 4 slots and its knob-grid jog
sequence ends with two pages neither the module nor its author put there:
**My Presets** (row 1 a readout — `Preset` / `(none)` or `Name` / `* Name` —
then `Load…`, `Save` and `Delete` only with a preset loaded, `Save As`
always) and **Module** (`Swap Module`, `Remove Module`). Both are doors: a
`PAGE_MENU` must be entered before an entry fires, so jogging past the end
cannot fire Remove Module by accident.

**Named "My Presets", not "User Presets"** — the header's right side is a
MEASURED share against a `HEADER_MIN_LEFT` floor (70px), and "USER PRESETS"
(56px) is past it and truncates to "USER PRESE". "My Presets" (46px) fits.
"Presets" alone would be worse: 27 modules in the fleet already plan a page
called that (obxd, sfz, hush1, minijv, sf2, hera, tablor, noisemaker, …), so
`claimName` would dedupe this one to "Presets - 2". Reported from hardware —
rendered PNGs, not text art, are what actually showed the truncation.

**The `*` follows a knob write within one settle, not just a page
re-entry.** Turning a knob on any OTHER page of the same component changes
the live `<prefix>:state` blob the mark compares against, and nothing used to
notice until the page was re-entered — *"changed a knob and * didnt appear
until i exited and re entered the module"*. Fixed without adding a
draw-path read: `componentParamPagesIo`'s `setParam` marks the write pending
(`markComponentParamWrite`); `tickUserPresetStale`, driven from the main
tick (never a draw function) alongside `tickParamPages`, waits out
`CONTRACT_SETTLE_MS` and then asks ONCE — via `paramPagesRefreshTrailing()`,
the same call Save/Load/Delete already use — and only when the grid is still
open on the exact `(slot, component)` that wrote. One read per settle, never
per detent, none once the user has moved on.

**The header shows the loaded USER preset, with the same mark, on every
page of the component** — `S1 > tst` clean, `S1 > * tst` dirty — falling
back to the module's own patch name and then its abbreviation exactly as
before when no user preset is loaded. Asked for on hardware and shipped:
*"should we change the preset in the header from the system preset to the
user preset? (Init -> tst) and then show the * there too?"*. Reads a CACHE
(`userPresetLiveBlobCache`, keyed per slot+prefix), never the DSP —
`userPresetHeaderMark` in `shadow_ui.js`, wired through `ctx` to
`headerTitle()` in `shadow_ui_param_pages.mjs` — so this costs nothing beyond
the read the My Presets page already pays for, and it answers `null`
harmlessly for a synthesised contract (Slot/Master FX/Global Settings) or a
Master FX component, none of which populate a record for their key.

**They are appended by the PLANNER, after the whole walk — not injected into
a level's hierarchy — because injection cannot work for this fleet.** A
level's own `menu:` field (the same PAGE_MENU kind) lands right after that
level's OWN grids, not last: Slot Settings dodges that by giving its menu a
level of its own, which only works because it synthesises its whole
hierarchy end to end. We do not own a module's. And three fleet shapes rule
out injection outright: 11 of the 95 modules in
`tests/fixtures/module-contracts.json` publish no `levels` object at all
(chain_params pagination fallback), minijv has `levels` but no `root`, and
with `modes` present the walk root is whichever mode is active. There is no
level guaranteed to exist that "append to the end" could target.
`planPages({ trailingMenus })` in `src/shared/param_pages/page_plan.mjs`
appends after the walk instead — see `buildTrailingPages`/`appendTrailing`
there and `io.trailingMenus()`/`refreshTrailing()` in
`src/shared/param_pages/page_controller.mjs`.

**A failed contract read cannot manufacture them.** `planPages` returns no
pages at all when `unresolved`, so the append only ever lands on a resolved
plan — the same rule as "a plan is a statement about what a module declares",
under "A param read has THREE answers" in `CLAUDE.md`.

**Scope is exactly the 4 chain slots' real components.** Master FX chain
components are excluded — user presets have only ever been offered for the 4
chain slots' components, never for a Master FX position, so this inherits that
gap rather than widening it. Slot Settings and Master FX
Settings are excluded because they are settings, not modules: no module id to
key a preset folder on, nothing to swap. The exclusion lives in ONE helper,
`componentParamPagesIo` in `src/shadow/shadow_ui.js`, called from every
component `enterParamPages` site, so a new call site cannot silently opt
Master FX in. Grid view only — the list view (`param_view = 0`) is a separate
code path with no pages to jog through and keeps its existing Shift+Click
route.

**The `*` leads the name**, e.g. `* Fat Brass` not `Fat Brass *`, because the
list renderer truncates the TAIL: rendered on obxd, `"Fat Brass *"` drew as
`"Fat Br…"` and the one character carrying the information was the first
thing lost. See `presetRowValue` in `src/shared/param_pages/current_preset.mjs`.
It costs no draw-path read — it compares the live `<prefix>:state` blob
against a stored hash at PLAN time and on explicit refresh, never per frame
(`trailingMenus()` has exactly 4 call sites, none inside `render()`).

**Save overwrites; Save As does not.** `overwriteUserPreset` refuses when the
`:state` read returns `null` — a FAILED read, not empty state — because
writing it would replace a good preset with nothing. **Remove Module IS the
picker's `None`**: it goes through `applyChainComponentPick`, the same
function the picker uses, because removal is not one write — it closes the
gap and renumbers everything downstream via a `remove` verb that permutes the
DSP arrays rather than reloading modules (see "Chain shape edits are a
PERMUTATION" in `docs/CHAIN.md`).

### The SHIM says what is loaded, and it says it ONCE

`masterFxConfig` is an in-file mirror that only learns about a position when
something in `shadow_ui.js` puts it there. Anything that loads a master module
by writing `master_fx:fxN:module` **straight to the shim** was therefore
invisible to `saveMasterFxChainConfig()` — an overtake tool (movy is the visible
case), or any Remote UI client, since schwung-manager's
`handleSetMasterFxParam` forwards whatever key it is handed with no allowlist.
The saver took its empty branch, wrote `{}` over a position the shim genuinely
had loaded, and **the whole master chain was gone on the next boot** (two
Discord reports; PR #221). It drifted the other way too: a position cleared
through the shim was written back from the stale mirror.

So the saver asks the shim. **`master_fx:modules`** (GET only) answers with the
whole chain in one string — `[{"id":…,"path":…},…]`, one entry per position,
built by `src/host/master_fx_snapshot.h`. Three properties the reader depends
on and cannot check for itself, all pinned by
`tests/host/test_master_fx_snapshot.c`:

- **positional, never compacted.** An unloaded position is an empty entry. Omit
  it and position 3's module lands on position 1 the moment anything ahead of it
  is empty — a wrong module at boot, not a missing one.
- **it refuses rather than truncates.** A short array parses perfectly and reads
  as "those positions are empty", which is the erase this exists to prevent.
- **quotes and backslashes are escaped.** Not for the odd filename's sake: the
  reader treats unparseable exactly as it treats a failed read, so one strange
  path would silently stop the whole chain persisting.

**One read, not sixteen.** The first cut asked `fxN:name` per position plus
`fxN:module` per loaded one — 8+ IPC round trips at ~2.8ms landing on the
autosave frame, which is the frame the one-slot-per-tick split exists to keep
clean. An IPC read costs more than redrawing the entire screen, so the count is
the thing to fix, not the schedule.

**It also makes the id and the path ONE fact.** Read separately they are two
round trips that fail independently, and a state file pairing this position's id
with the previous module's path restores the wrong module in silence — the boot
loader parses `module_path` and never looks at `module_id`. The path is taken
from the same answer as the id and only when it describes the module being
written; otherwise the startup scan answers. And an id with **no** path is not
written at all: it restores nothing, so putting it over a good file is the same
erase as `{}`.

Three things guard the read itself, all in `saveMasterFxChainConfig`:

- **a null snapshot adopts NOTHING.** Failed is not empty — see "A param read
  has THREE answers". The per-position reads stay as the fallback, because a
  shim older than the JS answers this key with an error and the web updater
  mirrors the shim separately: without the fallback, version skew silently
  restores the data-loss bug.
- **a write still in flight is not read back over.** `shadow_set_param` is
  fire-and-forget under overtake, so a save reached from a tool through `ctx`
  can read the position before its own write lands and adopt the module the user
  just replaced. `masterFxModuleWriteAt` + `CONTRACT_SETTLE_MS`, the same bargain
  as `userPresetWriteAt`.
- **adopting invalidates the display-name caches**, like every other site that
  assigns `.module`. They are keyed by POSITION, not by module, so a position
  that adopts a different module goes on labelling and announcing as the last
  one.

`tests/host/test_master_fx_save_reads_shim.sh` drives the real function through
a fixed dependency list rather than grepping for it — the version that shipped
with #221 was five `rg -q` source pins in `tests/shadow`, a directory **CI does
not run**, so deleting the fix would not have failed anything anywhere.

### A STEP button is not a note, and audio FX were told it was

Audio FX are fed from **three** places, and the only guard any of them had was
`d1 >= 10` — which exists solely to drop the capacitive knob-touch notes 0–9,
and was never a claim about what counts as musical input:

```
src/schwung_shim.c   MIDI_IN cable 0 (Move's own surface)   notes, d1 >= 10
src/host/shadow_midi.c   shadow_chain_dispatch_midi_to_slots    ALL voice msgs, no guard
src/host/shadow_midi.c   shadow_dispatch_direct_external_midi   cable-2 THRU, d1 >= 10
```

So on Move's own surface the **step buttons (16–31) and track buttons (40–43)
reached every loaded audio FX as played notes.** Found with an FX whose note
handler fires a one-shot action (capicola's forced re-slice): in Master FX it
fired on essentially any button press. The ducker had the identical exposure
and merely read as "sensitive". Both `shadow_master_fx_forward_midi` and the
slot `FX_BROADCAST` were affected — the asymmetry is **not** master-vs-slot, it
is broadcast-vs-dispatch: `chain_midi.c:720` handles `FX_BROADCAST` by
forwarding to every audio FX and returning *before* any channel logic, so only
the non-broadcast dispatch was ever channel-matched.

Two guards fix it, in `src/host/fx_midi_filter.h`, and **the split is the
point**:

- `move_surface_note_is_pad(d1)` — cable-0 sites ONLY, where a note number is a
  physical control identity. Replaces `d1 >= 10` at both shim broadcasts.
- `fx_midi_channel_accepts(ch, status)` — applied **inside**
  `shadow_master_fx_forward_midi`, not at its callers, so all three feeds are
  gated by construction and a fourth cannot be added ungated.

Never apply the note-range guard to the external sites: there a note number is
a **pitch**, and clamping to 68–99 silences five octaves of a keyboard.
`tests/host/test_fx_midi_filter_call_sites.sh` asserts that as an *absence* —
a test that only checked "the guard exists" would pass with it wrongly applied.

**Master FX → Settings → MIDI Ch** (`master_fx_midi_channel` in
`shadow_config.json`; param `master_fx:midi_channel`, −1 = All) selects the
listen channel. It lives on `MASTER_FX_SETTINGS_ITEMS_BASE` and in
`MASTER_GRID_PARAMS`, **not** in Global Settings — the first cut put it under
Global → Audio beside the other `master_fx:*` shim settings, which is where the
*plumbing* lives but not where a Master FX setting is looked for, and it was
reported missing from the device. Note the two representations: the wire
(`master_fx:midi_channel`, the config key, and the shim's variable) carries the
REAL channel (−1 = All, 0–15), while an enum cell is addressed by OPTION INDEX
(0–16). They are off by one and disagree about All, so the conversion is pinned
to `createMasterGridIo`'s `getParam`/`setParam` in `shadow_ui_slot_grid.mjs`
(`mfxMidiChannelToIndex` / `…FromIndex`) rather than repeated per call site.

**Default All**, deliberately: Master FX heard everything
before this existed, so any other default silently kills every sidechain in
the field — and a user whose ducker stopped after an update cannot connect
that to a setting they never saw. Note that the channel setting **cannot**
substitute for the pad guard: pads and steps share one cable-0 surface, so no
channel value separates them. Persisted like `usbc_out_persist` and parsed by
the shim at init (`shadow_resample.c`), so the filter is in force before the
first SPI frame. An out-of-range stored value fails **open** (All) rather than
muting every FX with no visible cause.
