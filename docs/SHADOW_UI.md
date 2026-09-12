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

**Six sections are six PAGES**, jogged through on one axis with the section
picker on click — Display, Audio, Screen Reader, Set Pages, Shortcuts, System.
All six are knob pages; Connect and Help are write-only trigger rows on System.
**One section, one page** is load-bearing: a section that SPLIT would put a jog
step in the middle of a scrolling list, and sections-as-levels would be gone
without a symptom.

**But there is no limit on how LONG a section may be, and believing otherwise
cost a real change.** Eight is the number of physical KNOBS — a grid page has
eight cells and nowhere to put a ninth. This screen is pinned to the list
(`layout: LAYOUT_LIST`, see `paramPagesLayout`), which draws five rows of a page
and scrolls the rest; `knobRows()` reads a page's keys with no cap, so a page of
any length lists correctly. The planner was chunking these levels at eight
anyway — the grid's rule leaking into a screen the grid never draws — so a ninth
param silently became a second page named `<Section> - 2` holding one row. On
the strength of that, Audition was moved out of Audio into Display to make room
for Save Stems. It is back in Audio, and `enterGlobalSettingsGrid` passes
**`paginate: false`** beside its layout pin. Audio currently holds eight.

**`paginate` is a property of the CONTRACT and must not be inferred from the
layout.** `paramPagesLayout()` returns `LAYOUT_LIST` whenever the screen reader
is on or Param View says List, so deriving it there would rearrange all 95
modules' pages behind a preference — and a module's pages are authored
groupings, unlike a settings section. It rides in the chrome next to `layout`
(`paramPagesPaginate`), reaches the controller through `load()`, and is carried
on the controller state so the two REPLAN paths (mode change, `visible_if`
change) plan it the same way the first plan did. Default `true`: every other
caller keeps the grid's chunking.

The contract test plans with `paginate: false`, the way the screen does, and
pins the per-section counts (7/8/6/1/4/3) plus the exact page list.

Three consequences worth knowing:

- **Help and Connect are write-only PARAMS on the System page, not a menu.**
  Help has been in three places: an action on `root` (which plans to no page,
  so it had no surface anywhere), then a row on a menu page — and a `menu` on a
  level costs that section a SECOND page. The planner emits a level's menu
  after its grids ("Menu LAST", `page_plan.mjs`) and **nothing anywhere merges
  menu entries into a knobs page**, so one section meant two jog steps and a
  second page name to invent, for three rows that fit one screen.

  `access: "write"` collapses them: a two-option enum becomes a momentary
  (`isTrigger`, not turnable, not divable), `page_controller.onClick` routes a
  list row for it straight to `fireTrigger`, and the write is what the io turns
  back into an action. Two details that are not obvious and are both pinned —
  **both options are the same WORD** ("Open"), because a list row draws its
  value unconditionally and a door has no state, and the second is what
  `fireTrigger` speaks (it was "..." for one round, which sounds like nothing);
  and the keys are **served a read** despite having none, because an unserved
  key makes `announceTouch` say "not read yet" every time the cursor lands on
  it. See `SYSTEM_PARAMS`.
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

### A Track tap dismisses, and "Keep Schwung" is the ONLY thing that changes

Global Settings -> Display -> **Keep Schwung** (`stay_in_shadow` in
`features.json`, **default ON**). Tapping a Track button while the shadow UI is
up switches to that slot's editor; off, it hands the screen back to Move.

**The default is a REVERSAL of what Schwung shipped with**, and deliberate: a
track button selects a track everywhere else on this hardware, and the dismiss
was never a decision — it was the only way out before Shift+Track existed. Every
exit survives the flip (tap Note/Session, Shift+Track, Back), so what changes is
a reflex and not the reachability of Move. An existing install keeps whatever it
has: `install.sh` preserves the key and falls back to the default only when it
is absent, so a device that already wrote `false` stays off until the user says
otherwise.

**A default-ON flag is parsed by testing for `"false"`.** The first cut tested
for `"true"` while defaulting off; flipping the default without flipping the
test would leave a flag the file can only ever turn ON, so switching it off in
the UI would persist a value the next boot ignores. `set_pages_enabled` and
`ext_midi_remap_enabled` are the two neighbours that already get this right, and
`tests/host/test_stay_in_shadow.sh` pins all five sites that decide the default
(shim boot value, the parse, the JS binding fallback, the declared contract,
install.sh) because a default that is on in some paths and off in others reads
as flakiness rather than as a wrong constant.

**It is a BOOL because of the WIDGET.** It shipped for one round as an enum of
`["Exit", "Stay"]` — two options, identical meta, identical click behaviour
(`flipsOnClick` focuses it in a list and flips it on a grid) — and still drew
differently from every switch beside it: `detectSwitch` picks the switch pill
via `isBooleanMeta`, whose option test is `BOOL_OPTION`
(`off|on|no|yes|0|1|false|true|disabled|enabled`), so "Exit"/"Stay" fell through
to the ENUM SQUARE, the widget that means "there is a list behind this", and
peeked its options on the knob. Reported from the device as *"why is the setting
a menu, unlike display mirroring?"*. The rule is right and stays — a switch
draws its state as a POSITION and cannot show the word "Stay", which is what
`docs/PARAM_PAGES.md` records as "suppressed on the WIDGET, never on the option
count" over 134 fleet cells. A genuine two-way choice (Saw/Square) is an enum
square; a boolean is spelled as a boolean. **A two-option enum and a bool are
NOT interchangeable, and nothing about the click path tells you so** — the whole
difference is which widget draws it.

Three things about the behaviour are easy to get wrong, and each fails silently:

- **It is enforced in the SHIM, not in JS.** The dismiss it suppresses is a
  shim-side state change (`shadow_display_mode = 0` in the cable-0 Track CC
  block of `schwung_shim.c`); JS never sees the tap. What the setting
  substitutes is the `SHADOW_UI_FLAG_JUMP_TO_SLOT` hand-off Shift+Vol+Track
  already raises, so the JS side needed no new code path at all.
- **The jump fires on the PRESS, outside the long-press block.** That block is
  gated on `LONG_PRESS_ACTIVE()`, i.e. on the shadow-UI trigger mode — put the
  jump inside it and the setting works on `Both`/`Hold` and does nothing on
  `Shift+Vol`, which reads as a broken toggle rather than a gated one. The
  tap-dismiss in that block is guarded by `!STAY_IN_SHADOW()`; without the
  guard the jump lands and the release undoes it a frame later.
- **Shift+Track still dismisses, on either setting.** It is the way out of the
  screen once a plain tap no longer is, and a setting that closes the only
  remaining exit is not a setting. Mute+Track (slot mute), Shift+Mute+Track
  (solo) and a volume touch during the press are excluded for the same reason
  the dismiss excludes them.

### A Track LONG-PRESS is a toggle, and it is its own inverse

From Move, holding a Track for 500 ms opens that slot's editor. From the shadow
UI, holding it **dismisses and leaves you on that Move track** — so the same
gesture takes you back and forth and there is no second key to learn for the
return trip. Requested from the device: *"long pressing a track should dismiss
shadow ui and take you to that move track. This way you can long press between
both."*

**The dismiss direction is not a new mechanism**, and that is the whole reason
it is one branch. The fire block already injected a synthetic Track TAP on every
long-press — a release to close the real 500 ms hold, then a crisp press/release
pair Move reads as a tap, with the user's eventual real release swallowed — to
make Move's selected track follow the slot (the field report behind it: *"it
does change to slot 2 on schwung, but still shows me the [old] pads"*). That
injection stays OUTSIDE the branch and runs both ways, which is exactly what
makes the dismiss land on the track the editor was about instead of wherever
Move happened to be.

Two things to keep straight:

- **Only the OPEN direction sets `SHADOW_UI_FLAG_JUMP_TO_SLOT`.** Raising it on
  the way out is a flag set while leaving. It is not needed for the round trip
  either: long-pressing that track again re-opens through the open arm, which
  sets it.
- **Overtake needs no guard here and must not grow one.** The overtake early-out
  above the Track CC block `continue`s before any long-press timer starts, so
  the fire block cannot run at all. A second check there would be dead code that
  reads as load-bearing.

With Keep Schwung on, the PRESS also switches to that slot before the hold
completes — so a long-press shows you the slot for half a second and then hands
the screen back. That is confirmation of the target, not a race: both writes
agree about which track you are going to.

`tests/host/test_track_longpress_toggle.sh` pins the branch, the shared
injection and the surviving escape hatches.

The byte rides in `shadow_control_t.stay_in_shadow`, appended at the end of the
struct and read live so the toggle takes effect on the next tap. Appending is
free only because `CONTROL_BUFFER_SIZE` is 256 for a struct that uses ~86 —
before that the `==` assert on an exactly-sized segment made growth read as
forbidden. The schwung-manager reads the same byte by RAW OFFSET, so
`tests/host/test_stay_in_shadow.sh` compiles an `offsetof` probe and compares it
to the Go constant rather than trusting two hand-kept numbers; the manager also
maps `min(declared, on-disk)` bytes and bounds-checks every accessor, because a
segment an older shim left behind is SHORTER and reading past its end is SIGBUS,
not a zero. `install.sh` REWRITES `features.json` from a fixed key list, so the key
is carried over there too — one that is not listed resets on every deploy.

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

### A module that takes the PADS must be released by an invariant, not by an exit list

A component's `ui_chain.js` can take the pads with `host_pad_block(1)` — 9W9
does, so it can run Shift+Pad lane select and forward the notes to Move itself
— and it lowers them from its **own `tick()`**, which the draw switch calls
from exactly one place: `case VIEWS.COMPONENT_EDIT`. Anything that stops that
tick strands the flag, and the shim enforces `pad_block` *inside* the
`shadow_display_mode` branch, so what the user gets is **pads dead in the
Schwung UI and fine the moment they are back on a Move track** — with knobs,
jog and Back all still working, which is why it reads as anything but an input
filter.

Reported from the field 2026-09-08 as *"it happens from time to time, I don't
know how to reproduce it"*. The device's log carried **exactly one `pad_block
ON` and no `OFF`, ever**: the byte had been stuck for thirteen hours across two
shim inits, because `/dev/shm` outlives `restart-move.sh` and nothing cleared
it at boot. That is also why it looked unrelated to anything done recently —
the visit that raised it was half a day earlier.

**The exits cannot be enumerated, and two attempts proved it on hardware.**
Guarding `unloadModuleUi()` fixed the long-press dismiss and left a jump to
Global Settings dead, because the module stays LOADED across a view change and
no unload happens. Guarding `setView()` too would still have missed co-run,
which stops the tick with **no view change at all** — and a Track tap alone
already means dismiss *or* switch-slot depending on Keep Schwung. So the JS
states the invariant once, every frame, in `reconcilePadBlock()` beside
`reconcileCcClaim()`: the pads are blocked only while somebody is actually
running who can unblock them. There are exactly two such owners — a component
UI whose `tick()` really is being called, and the on-screen keyboard, which
uses pads as keys and can be open over other views.

Three properties make that restate possible rather than merely tidy:

- **`js_host_pad_block` is IDEMPOTENT, and compares against the SHM** rather
  than a remembered value. An unchanged restate costs a byte compare and logs
  nothing; the old unconditional `shadow_ui_log_line` would have flooded
  `debug.log` at 60 Hz and made a per-frame reconcile unshippable.
- **Comparing against the SHM is also what lets the caller restate instead of
  memoise.** The shim drops this flag unilaterally — on the display-mode edge
  and at init — without telling JS, so a mirror latches. Same rule as
  `pad_observe`; 9W9 memoises in `padBlocked` and is exposed to exactly that.
- **The shim keeps the two drops JS cannot make**: the `display_mode` 1→0 edge
  (beside `pad_observe`, covering a `shadow_ui` that exited or crashed) and the
  init clear (covering a stale segment across a Move restart, without which an
  already-stuck device stays stuck through every restart the user tries).

Nothing is owed on either drop, unlike the claim latches in the same block:
what was withheld is a pad **note**, so the worst a mid-hold drop hands Move is
an unmatched note-off. Copy/Delete is the case that needs a latch, and keeps
one. `tests/host/test_pad_block_lifecycle.sh`.

**And the KEYBOARD does not lower it either — the reconcile does.**
`closeTextEntry` used to write `host_pad_block(0)` on the way out, which was
right for exactly as long as the second owner above could only be open over
views that are **not** `COMPONENT_EDIT`. A module-owned param grid with a
`Save As` row ends that: a keyboard opened and closed over a component that
owns the pads wrote 0 straight over its claim, leaving pads going to Move in
the middle of a mode the module still believes it owns — and healing only if
that module happens to re-state the flag every tick rather than on entering
the mode (9W9 memoises in `padBlocked`, so it does not). The close hands the
decision back instead: the reconcile skips only while a keyboard is up, so the
frame after it closes is already the invariant being restated. It is the same
answer as the exits: **one site states the rule, nothing enumerates the ways
out of it.** The cost is one frame of pads withheld after a keyboard closes
over a view that did not want them.

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
always) and **Module** (`Module Help` when there is any, then `Swap Module`,
`Remove Module`). Both are doors: a `PAGE_MENU` must be entered before an entry
fires, so jogging past the end cannot fire Remove Module by accident.

**`Module Help` is conditional, and Back off it returns to the MODULE.** The
row appears only when `getModuleHelpChildren(id)` finds a `help.json` with a
non-empty `children` array under one of the five module bases — a row that
opens an empty viewer teaches that the feature is broken, and most of the fleet
ships no help at all. (Cached per id, misses included: `componentTrailingMenus`
runs on every PLAN, and an uncached miss is five failed opens each time. A
module's `help.json` cannot change without an install, and an install restarts
`shadow_ui`, so there is no invalidation to get wrong.)

It seeds `helpNavStack` with **exactly one frame** — that module's own topics,
titled with its display name — rather than opening the Help tree with the module
selected. That is the whole point: Back off the first frame empties the stack and
lands you back in the module, instead of climbing up into `Modules > … > Help`,
a place the user was never in.

The help viewer has no view of its own (`VIEWS.GLOBAL_SETTINGS` and
`VIEWS.MASTER_FX` draw it), so this hand-off is the ONE component action that
does not converge on `VIEWS.CHAIN_EDIT`, and `maybeReturnToComponentGrid` cannot
see it. It carries its own return pair (`componentHelpReturnSlot` /
`componentHelpReturnKey`) and its own reconciler, `maybeReturnToComponentHelp`,
wired at the same poll site one line above `maybeReturnToGlobalGrid` and gated on
the same view — the first to fire moves `view` off `GLOBAL_SETTINGS`, so the
second cannot double-fire behind it. It lands on the **`Module`** page with its
menu open, the row the user clicked. Opening help from anywhere else
(`handleMasterFxSettingsAction("help")`, the one choke point both `[Help…]`
entries route through) **drops any pending component return**: the flag survives
a navigation away from `GLOBAL_SETTINGS`, and only a later arrival could spend
it — on somebody else's session.

`runComponentActionFromGrid` returns straight out of the `module_help` case for
the same reason: leaving `componentModalFromGrid` raised for a `CHAIN_EDIT`
arrival this flow never makes would fire it on an unrelated one later.

**The help footer names where Back ACTUALLY goes** (`helpBackTarget`). Both help
draws used to compute it as *the frame below the top one*, which is right for a
nested list and wrong for the other two screens — reported from hardware as
*"the footer shows back braids but that's not always true when you're up a
menu"*:

| screen | Back lands on | footer |
|---|---|---|
| detail | the frame it was opened FROM | that frame's title |
| list, depth > 1 | the parent frame | the parent's title |
| either, when that frame is frame 0 of a Module Help session | the module's topic list | `List` |
| list, depth 1 | out of the viewer | the module (Module Help) or `Settings` (`[Help…]`) |

It decides WHICH FRAME first (`inDetail ? depth - 1 : depth - 2`, below 0 meaning
"leaves") and labels it second — deciding the frame first is what stopped the two
draws giving different answers for the same destination.

**The module name is reserved for the Back that really leaves.** Frame 0 of a
Module Help session is *titled* with the module — it is that module's topic list
— so the name meant two different destinations on adjacent screens: at the top
`Back: Braids` leaves for the Braids knob grid, one level in `Back: Braids`
returns to the Braids topic list, and the header already said `Braids` on the
first of those. Reported from hardware: *"the top level is the module name, so
it's confusing it stays the same."* One level in now reads `Back: List`. Only for
a component session — a `[Help…]` session's frame 0 is titled `Help`, which
collides with nothing and is a truer label than `List`.

The "leaves" case asks `componentHelpReturnSlot >= 0` — the same pending return
`maybeReturnToComponentHelp` reconciles on — rather than inspecting the frame
title, so the footer and the actual destination cannot disagree. A footer naming
a screen you do not arrive on is worse than no footer: it is the one thing on the
display claiming to know the way out.

The help **detail** also draws the shared scrollbar now instead of the arrows it
had kept — see `docs/PARAM_PAGES.md` on `drawScrollbar`, which is exported for
exactly this.

**The header reads `Help: <module>` on that first frame.** It was the bare
module name — the same word the knob grid it was opened from already wears, so
the screen said what it was *about*, not what it *was*. Only that frame:
a nested frame is headed with its own topic (`CONTROLS`), and a `[Help…]`
session's first frame is literally titled `Help`.

The 18-character cap in front of it is gone with it. `drawHeader` fits the left
side to the bar in **pixels** (`fitText`/`FONT4_MEASURE`, measuring the right
side first and giving the left the remainder — 124px with no right side), so a
char cap is a second, blinder truncation in front of a good one: measured across
the 133 modules installed on the device, 12 have a `Help: <name>` longer than 18
characters and **every one of them fits** (widest, `V8 tuneSample Slicer`, is
118px). The other screens that still cap by characters are out of scope and the
test is scoped to the help draws accordingly.

**...and it gained a fifth line, by using the list's row pitch.**
`scrollable_text.mjs` had its own `LINE_HEIGHT = 10` against the shared list's 9.
Both draw the same 5x7 font into the same `LIST_TOP_Y..FOOTER_RULE_Y` rect, so
the help *list* fitted five rows there and the help *text* fitted four — a line
of help thrown away per screen, to a constant with no reason behind it. The pitch
is now `LIST_LINE_HEIGHT`, and the two `createScrollableText` sites ASK
`visibleLinesFor(LIST_TOP_Y, FOOTER_RULE_Y)` (`drawMenuList`'s own formula)
instead of passing a hard-coded 4 — which is the half that would have silently
lost the line again the next time the rect moved.

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
channel value separates them. Parsed by the shim at init
(`shadow_resample.c`), so the filter is in force before the
first SPI frame. An out-of-range stored value fails **open** (All) rather than
muting every FX with no visible cause.

### The LFO target picker groups by LEVEL, and the grouping must be LOSSLESS

An LFO's target was chosen from ONE flat list — every modulatable key the
component declares, in `chain_params` order. Against the 95-module fleet
capture that is **418 rows for minijv**, 303 for surge, 250 for forge, 213 for
mrdrums: one unbroken jog-scroll, while the module author's own section names
sat unused in the same `ui_hierarchy` the knob grid pages from. **84 of the 95
publish `levels`.**

`src/shared/lfo_target_groups.mjs` groups them; the picker gained a step
(`VIEWS.LFO_TARGET_GROUP`) between component and param. Two rules make that
safe rather than merely tidier:

- **Named the same as the grid's pages.** Both come out of
  `param_pages/level_walk.mjs`, which exists for this reason — it was extracted
  from `page_plan.mjs` when the second consumer arrived. **No screen shows a
  page title next to the picker's row for the same level**, so a second copy of
  the naming rule would drift and nothing would ever report it: the user would
  find `Oper1/Env` in one place and `Env` in the other with no way to know they
  were the same thing.
- **Lossless.** The union of the groups is exactly the flat list — same keys,
  same labels, no duplicates — with an orphan sweep into a trailing **"Other"**
  (mrdrums: 193 of its 213). Grouping must never cost a target, because the
  routing it would have made is one the DSP would have honoured.
  `tests/host/test_lfo_target_groups.sh` asserts this over every module.

**A child level lists TEMPLATES, not keys** — and matching them raw is how the
module that needed grouping most got none of it. mrdrums declares 16 pads on
`root` and again on `pad_settings` (`child_key_template: "p{index}_{key}"`), so
those levels list `vol` / `pan` / `start` while `chain_params` publishes
`p01_vol` … `p16_mode`. Both levels collected nothing, were dropped as empty,
and **all 200+ concrete keys fell to the orphan sweep** — reported from the
device as "mrdrums has everything under Other". `page_plan.mjs` has always
resolved these through `child_key.mjs`. The grouper expands to **one group per
instance** ("Pad 3"), and levels sharing a `child_index_param` **merge into one
set of instances**, for the reason `childPickerNeeded` gives: two levels naming
one index are two views of one focus, and keying by level splits a single pad
across two lists.

**The focused-instance alias must keep a named home.** A child module publishes
both `p03_start` and `pad_start`, and the alias is the one an LFO should target
— the concrete key is what left `<alias>:modulated` answering 0. It appears in
no level, so it is **inferred** from the level's declared keys with a floor of
two matches (one coincidence is not a family). **Order is load-bearing**: infer
before the concrete expansion has claimed anything and "largest family wins"
picks `p01_` over `pad_` — measured, that put pad 1's twelve keys on the root
page, left "Pad 1" holding four, and made the union 216 of 212.

**The group step is SKIPPED, not emptied**, when there is nothing to group: no
usable hierarchy (11 of 95 publish no `levels`), a list of 8 or fewer, or a walk
that yields one group (11 single-level modules). An extra menu level over six
rows costs a click and saves no scrolling. `lfoTargetGroups` is cleared on both
entry points, because **Back branches on it** — leaving a previous component's
groups behind sends Back to a section screen this component never showed. The
sequence that produces it needs two components in a row, so a test starting
from a fresh state cannot see it.

**No mode filter, deliberately.** `planPages` drops levels owned by an inactive
mode; this walks every mode root. A routing at a mode-inactive param is still
valid, and minijv has no `root` at all, so gating would reach half its tree and
the orphan sweep would dump the rest into "Other" — a worse answer than naming
the level it came from. The picker and the grid therefore disagree about
minijv's level list, and the walker's "call the root Main" rule is dropped when
there are two roots (otherwise the second is claimed as "Main - 2" when the
module already calls it "Performance").

**The cursor lands on the routing the LFO already has.** All three indices used
to reset to 0, so re-aiming an LFO pointed at param #143 cost the same 143 jog
steps as the first time, with the answer sitting in `target` / `target_param`
the whole while. Nothing new is persisted — the stored routing IS the memory,
so it cannot go stale or need a heal. Seeding is scoped to the component the
routing names: an index carried across components points at whatever happens to
sit at that ordinal in a module that knows nothing about it.

`ui_hierarchy` is read on entry and `null` is **not** "declares no levels" —
that is the granny bug. It is retried once and then falls back to the flat
list; nothing caches, so the cost is one wrong-shaped menu rather than a
latched plan.

### Module Lists

Named collections of module ids at `/data/UserData/schwung/module_lists.json`,
with `Favorites` seeded at index 0. Filed from the knob grid's **Module** page
(`Add to List`, whose value column counts the lists holding this module), and
consumed by the swap picker's filter row.

Every rule lives in `src/shared/module_lists.mjs`, which imports nothing and
takes its `{ readFile, writeFile }` injected — so `tests/host` runs it under
node. `shadow_ui.js` draws it and wires the gestures; it holds no rules,
because a rule reachable only through a 22k-line UI file is a rule with no
test.

- **A corrupt file is reported, not replaced.** `loadLists` answers
  `{ state, corrupt }`; a missing file is *not* corrupt (that is the first
  run), an unparseable one is, and a corrupt session persists nothing. A file
  this version cannot read may be one a later version can, and overwriting it
  destroys the only copy — the same distinction the param channel draws
  between `""` and `null`.
- **`filterIds` answers `null` for a list that does not exist**, never the
  identity. Showing every module under a filter name that means nothing reads
  as the filter being broken rather than as the list being gone. So does
  `toggleMembership` for a list it cannot find: `false` there would read as
  "removed", and a caller announced a removal that never happened.
- **Lists are global; the picker hides the ones that do not apply.** A synth
  picker offers only lists with an installed sound generator in them
  (`listsWithAnyOf`), so it can never land on an FX-only list and draw an empty
  screen.
- **The picker filter is re-resolved on every entry, before it is applied.** It
  persists across pickers within a session — that is the workflow win — but a
  filter whose list was deleted, or which has no member of this type, falls
  back to All and announces it.
- **The cursor scans; it does not count.** `chainMoveEntries` is spliced in
  *under the loaded module*, so with a filter that hides that module the moves
  end up mid-list and the obvious "one past the filter row plus the moves"
  arithmetic opens the picker on `Move Right`. `pickerFirstSelectableIndex`
  walks instead.
- **The synthetic rows are never filtered.** `None`, `Move Left` / `Move
  Right` and `[Get more...]` survive every filter; a filtered picker with no
  way to clear the position and no way to the store is a dead end. They are not
  all shaped alike either — `None` carries the EMPTY id while the others are
  `__`-prefixed, so `pickerRealIds` is written once rather than twice.
- **Favorites cannot be renamed or deleted**, and its Rename/Delete rows are
  **absent** rather than present-and-refusing — a row that answers a click by
  doing nothing teaches that the screen is broken. Clear works.
- The lists views are OURS, so `Back` is their only exit and the return to the
  Module page is written at that one site. There is no reconciler:
  `maybeReturnToComponentHelp` exists because help is hosted by
  `GLOBAL_SETTINGS` and has three ways out.

**`drawFooter` DROPS a pair that does not fit — silently, and along with every
pair after it.** The membership screen asked for `JOG SEL, CLK TOGGLE, BACK
MODULE` and rendered `JOG SEL … BACK MODULE`: the screen's primary action was
the one word missing from its own footer, and nothing reported it. Found by
rendering it and looking (`tools/param-pages/preview_module_lists.mjs`), not by
reading it. JOG is the pair worth losing when a footer is tight — every list on
this device jogs. Two related rules fell out of the same render: a footer must
name the verb of the row **under the cursor** (the picker's filter row cycles
and loads nothing, so `drawChainPicker` reads `clickVerb` off the row), and a
one-row screen must not name a JOG with nowhere to go.

### The metronome is missing by CONSTRUCTION, and detection is one exact string

Under Move→Schwung, `rebuild_from_la` zeroes the mailbox and rebuilds it from
Link Audio slots 0–3 — the four per-track channels — so it can insert per-slot
FX. **Move mixes its metronome at MASTER, not into a track**, so it is absent
from the reconstruction by construction. This is not a bug with a fix; nothing
recovers it but generating our own click.

**`Main − Σ(tracks)` is not the metronome.** Main is deliberately unsubscribed
by the sidecar for measured reasons ("Main's ring overran", `link_subscriber.cpp`),
and `Song.abl` carries `returnTracks` and a `masterTrack` — so the residual is
metronome *plus* returns *plus* master-chain colouring. Two people have now
reached for that subtraction; it does not work.

**Detection is Move's own announcement, and the match is EXACT.** `MoveOriginal`
holds `"Metronome\nOn"` and `"Metronome\nOff"` at 0x169474 and 0x1909d8, in the
middle of its notification strings (`"Clip\ncreated"`, `"Notes\ndeleted"`), and
pushes them out as `com.ableton.move.ScreenReader.text` — which `shadow_dbus.c`
already receives through its catch-all `type='signal'` match.

This is **not** the removed mute auto-correct. That rule matched any text
*ending in* `" muted"` / `" soloed"`, so Move's own "Lay Down Kit muted" and
Schwung's TTS looping back through the same handler both fired it, and it
**persisted** the result — silencing slots across projects. Here the comparison
is whole-string equality after normalising case and whitespace, Schwung never
utters either string, and a `NONE` result changes nothing, so no unrelated
announcement can clear the flag.

**It is never persisted, and that is what makes boot correct.** `metronome`
appears in neither `/data/UserData/settings/Settings.json` nor `Song.abl`, so
**Move does not persist it either** — the metronome is off at every boot, and
initialising `shadow_metronome_on = 0` is the truth rather than a guess. Writing
it to disk is the one change that could make it wrong.

Two rejected sources, so they are not re-litigated: the **step-6 icon LED** also
lights while Shift is held and in other step contexts, so lit ≠ metronome on;
and **`mIsMetronomeOn` is an assert-expression string** (it sits beside
`iPos != mSteps.end()`), not a reflection table, in a stripped binary — there is
no deterministic memory anchor.

**Where the click is mixed is the whole design.** It goes into `mailbox_audio`
*after* `native_capture_total_mix_snapshot_from_buffer(unity_view)` and *before*
the `rebuild_from_la && mv < 0.9999f` scaling: out of the Quantized Sampler,
Skipback and the resample bridge, so a resample stays clean — and still tracking
the volume knob and getting speaker EQ. `tests/host/test_metronome_mix_point.sh`
pins that order, because moving the call up one block keeps the click working and
silently starts recording it.

The render is gated on `rebuild_from_la` in **every** mode, `On` included.
Outside Move→Schwung, Move's own metronome is audible, so the gate is what stops
it doubling — placed once, where it cannot be forgotten, rather than expressed as
a fourth mode.

**The click was early by TWO stacked errors, and only one of them was a latency.**
Reported from the device as "sounds early, moreso at lower tempos, it is tempo
scaled" — which is the sentence that rules out a latency, because a Link Audio
transit is the same milliseconds at any tempo. Measured against a sequenced
hihat in one `mailbox_out.pcm` capture, at two tempos so the terms separate:

```
 20 BPM   click early by 144.3 ms  (sd 0.7, n=7)
120 BPM   click early by  40.4 ms  (sd 1.0, n=40)

125.00k + L = 144.3        k = 0.997 pulses
 20.83k + L =  40.4        L = 19.6 ms
```

1. **The downbeat is at pulse 1, not 0.** `shadow_transport_pulses` is zeroed on
   MIDI Start and then *incremented* by the first clock — and per the MIDI spec
   that first clock **is** the downbeat. So beats land at `24N+1`, and firing at
   `24N` is one pulse early forever. Tempo-scaled: 20.8 ms at 120 BPM, 125 ms at
   20. Corrected in `METRONOME_BEAT_PULSE_OFFSET` rather than in the counter,
   because the counter is a truthful count of clocks received — it is the
   *interpretation* of where beats sit that was wrong. Rebasing the counter so
   pulse 0 is the downbeat would also remove the 0 → 1 transition the metronome
   needs to detect its very first downbeat, so the first click of every take
   would go missing: a fix that breaks the thing it repairs.

   **`recall_quantize` had the identical off-by-one** and is fixed here too —
   same counter, same wrong grid, but ~20 ms early on a snapshot recall is
   inaudible where the same error on a metronome is not, which is how it
   survived. The constant now lives once, in `src/host/transport_grid.h`, and
   reverting it fails both test suites.
2. **One Link Audio transit**, the 19.6 ms constant — the click is generated from
   Move's clock (frame-aligned to *now*) while Move's audio in the same block is
   a transit older. The trigger is scheduled `METRONOME_LA_COMP_FRAMES` ahead,
   derived from `LATENCY_COMP_TARGET_SAMPLES` so a retune of the LA target —
   which has already moved once, 800 → 1400 — carries the metronome with it.

After: **0.0 ms mean, sd 1.0, range ±1.5 ms over 40 beats**, and ±1.5 ms is the
analysis hop, so the residual is under the measurement floor.

Two lessons worth keeping. **"Tempo-scaled" names the mechanism**: an error
proportional to beat length is a phase error in a pulse count, never a latency,
and the word alone eliminated the hypothesis I had already traced through the
code. And **one tempo cannot separate two terms** — 144 ms at 20 BPM fits a
pulse error, a latency, or any mix; it took a second tempo to solve for both.

**Known gap:** the one-bar **count-in** click plays even with the metronome off
(`isUsingCountIn` in Settings.json), is equally silent under `rebuild_from_la`,
and has no announcement to key off. Record + transport is not a sufficient
signal. Not covered.

### The speaker EQ is jack-following, and Auto is only the DEFAULT now

Same construction problem as the metronome, one layer later: under
`rebuild_from_la` the DAC mailbox is rebuilt from the four per-track Link Audio
channels, so Move's own **MoveSpeakerEnhancer** — which sits on its master bus —
is not in the path, and `speaker_eq_process` in `schwung_shim.c` runs an
emulation of it in its place.

It must never colour headphones, and the jack reading it depends on is the part
that goes wrong: XMOS can broadcast a transient or uncorrected CC 115 val=0
("speaker") while headphones are actually plugged. So the auto logic is biased
hard toward OFF — a speaker reading is trusted only after `SPK_EQ_STABLE_SEC`
of continuity, and the failure it prefers is *less bass on the speaker*, never
*hollow headphones*.

**That bias is a good default and a bad only-option.** A device whose XMOS
insists on "speaker" with a jack in has no way to silence the EQ; one that never
settles on speaker never gets it at all. **Global Settings → Audio → Spkr EQ**
is the escape: `speaker_eq_mode` in `shadow_control_t` (0 Auto, 1 Off, 2 On),
persisted by `js_shadow_speaker_eq_set` to `features.json` as `speaker_eq`,
restored by `loadSpeakerEq()` at startup — the recall_quantize / metronome_mode
/ save_stems shape exactly, because `load_feature_config()` runs once at init
and this has to change without a reboot.

**It is ALSO seeded from the file at init, which the other three are not**, and
that is not belt-and-braces: `0` in SHM is a legitimate mode (Auto), so an
unseeded register cannot be told from a real choice, and the gap before the
shadow UI pushes the value down is exactly the boot window an `Off` user is
choosing this setting to cover. `speaker_eq_setting` is parsed in
`load_feature_config()` and written to the register beside
`shadow_ui_trigger_setting`, which has the same shape for the same reason.

**The key is `speaker_eq`, NOT the legacy `speaker_eq_mode`.** That one belonged
to the toggle removed in `f418af41`, and a stale copy of it is still on the disk
of every device that ever had it — honouring it would silently revive a choice
made against different jack-detect behaviour, which for a device left on `on` is
the hollow-headphone bug coming back on an upgrade.

**On does NOT escape `rebuild_from_la`,** and that is the same rule the
metronome's three modes obey: outside Move→Schwung Move's own enhancer is in the
path, so "force on" would mean running it twice. The gate stays where it is,
once, rather than becoming a fourth mode nobody remembers to condition on.

### The preroll trim was a no-op for five months, and looked like bad timing

The quantized sampler records **through** its preroll — starting on the count-in
rather than on the tick that ends it is what makes the take sample-accurate to
the downbeat (`b76cdfed`, 2026-04-01, which measured the offset down from
15–45 ms to <1.5 ms). The count-in bars are then cut off the front of the WAV on
stop, by `sampler_wav_trim_front`.

**That cut never happened.** The take and its five stems were opened `"wb"`, and
`fread` on a write-only stream returns 0 — so the copy loop broke on its first
pass, before anything moved. The `ftruncate` after it ran anyway, cutting the
preroll's worth of bytes off the **END** of a file whose front was untouched. So
what landed on the card was the **count-in**, at exactly the right duration, with
the take's tail missing. Every visible signal agreed it had worked: the length
was right, the file was there, and `"trimmed N preroll frames"` was logged —
because the caller printed it on a return value of 1, which the function returned
whether or not it had copied a byte.

Two things follow, and they are the general ones:

- **A short read must not become a truncation.** `sampler_wav_trim_front_impl`
  returns 0 and leaves the file **whole** if the copy did not complete. A take
  that kept its preroll can be trimmed by hand; one truncated around unmoved
  audio has lost the end of the performance for good. Same rule as the
  three-answer param read: a failed read may not produce a result.
- **A length assertion would have passed the entire time.** The failing
  implementation gets the frame count exactly right and the audio entirely
  wrong. `tests/host/test_sampler_wav_trim.c` asserts the file **begins** on the
  first post-preroll frame and **ends** on the last recorded one, against a file
  whose every frame is stamped with its own index; and it runs the trim against
  a write-only stream to pin the recovery. The body lives in
  `src/host/sampler_wav_trim.h` so it can be run on the dev machine at all —
  five months on hardware never caught it, and one host test does.
  `tests/host/test_sampler_wav_open_mode.sh` pins the two call sites, because
  the arithmetic was never what was wrong.

### Save Stems: a stem is a SLOT, and that is forced by the FX chain

**Global Settings → Audio → Save** (`save_stems` in `features.json`, mirrored
into `shadow_control_t.save_stems`): **Master** (default) / **Stems** / **Both**.

**One setting, three recorders.** The Quantized Sampler (Shift+Sample),
Skipback (Shift+Capture) and Song Mode's Record button all record through the
same `shadow_sampler.c`, so a per-surface switch would be three places to keep
in step. Song Mode needed no new recording code at all — it already called
`host_sampler_start(path)`; it gained only a label, because pressing Record and
getting seven files when you expected one is a surprise you can only discover
after the take.

**A stem is a SLOT, and the four slot stems ARE the four tracks.** Under
Move→Schwung the shim builds each slot as `move_track[s] + synth[s]` and *then*
runs the slot FX chain on the sum (`schwung_shim.c`, the `rebuild_from_la`
branch). Move's track and Schwung's synth are inseparable after that point, and
tapping them before it would hand back stems without their FX. Under
Move→Schwung the four stems therefore sum to the master **exactly**: the
reconstruction is composited from those four Link Audio channels and nothing
else — the same fact that makes Move's metronome missing there. **Until a
global send carries signal**: a send return belongs to no slot, which is why it
gets a stem of its own (below). With the sends silent the four-way statement
holds exactly as written.

**The fifth stem is MOVE, and it exists for the case the other four cannot
cover.** With Link Audio routing off there is no four-way split to be had: Move
hands us one mixed mailbox, and the slots carry only Schwung's own synths.
Without a Move stem the feature would record four synth-only files and drop the
rest of the music on the floor — silently, since the files would exist. It is
tapped from `native_bridge_move_component` un-scaled by the same smoothed `mv`
`unity_view` uses, so it sits at unity with the slot stems. Under
`rebuild_from_la` it is left INVALID **on purpose**: Move's tracks are already
inside the four slot stems, and a Move file repeating them would double every
instrument in a stem sum.

**The last two stems are the GLOBAL SEND RETURNS (`_SendA`, `_SendB`), and they
exist for the same reason the Move stem does.** A shared return belongs to no
slot: a slot feeds a send post-fader and the wet signal comes back on a
device-wide bus, so without these two the reverb tail would be in the master
file and in **none** of the stems — the exact-sum property above would break the
moment anybody used a send, silently, with every file present and plausible.
They are tapped inside the send loop in `schwung_shim.c`, post-send-chain and
scaled by the return level, which is to say **the identical block
`bus_mix_send()` adds to the master bus**, so slots 1–4 + SendA + SendB still
sum to the master exactly. That is also why the tap recomputes `bus_mix_send`'s
integer scaling by hand instead of handing `shadow_stem_store` a float gain: the
float path rounds where `bus_mix_send` truncates, and the two disagree by one
LSB on roughly half of all samples. It is emphatically **not** read from
`native_bridge_me_component`, which is snapshotted before `fx_target` exists — a
send return read from there would be silence in every file. An unloaded send
writes silence and its file is deleted at finalize like any other.

`SAMPLER_STEM_COUNT` is 7 and the send stems must stay **contiguous and last**:
the tap indexes the table as `SAMPLER_STEM_SEND_A + sb`, and three
`_Static_assert`s in `schwung_shim.c` fail the build on a send bus added without
a stem, on a non-contiguous pair, and on the Move stem moving off the end of the
four slots.

**Stems are pre-Master-FX and pre-master-volume.** MFX processes the mixed bus;
there is no per-stem version of it to capture. With a Master FX chain loaded the
stems will not add up to the master file, and that is arithmetic, not a bug.

**A silent stem's file is DELETED at finalize, never opened lazily.** Every stem
file is opened up front. Opening on first audio is the obvious alternative and
is wrong: the file would then start at the first sound rather than at t=0, and
the stems would no longer line up with each other or with the master. So an
unloaded slot still leaves no file, and alignment is not traded for it.

**The capture gate opens on the RT ARM, not in the worker.** `sampler_state` is
`RECORDING` the moment `sampler_request_start*` returns, so the master ring
starts filling immediately, while `sampler_worker_prepare` runs up to ~200 ms
later. Opening the stem gate there put a few hundred milliseconds into the
master that were missing from the stems — and because the stems are trimmed by
the *master's* preroll count, they would have stayed offset by it for the whole
take. Nothing in the arm needs the worker: the rings are allocated once in
`sampler_init` and their positions are reset on the RT half, so they are ready
before the file that will drain them exists. The mode is latched there too, so
flipping the setting mid-take cannot produce half a take of each shape. The
worker may only ever *close* the gate (no stem file opened).

**Stems are captured BEFORE the master, and the order is load-bearing.** Both
apply the start-of-recording fade-in ramp and the master half is the one that
*consumes* the counter (`sampler_capture_stems` only snapshots it). Reversing
the two lines in `schwung_shim.c` ramps the stems by a block already spent — a
wrong fade on the first 3 ms of every take, audible as a click and attributable
to nothing.

**A divergence is reported, not repaired.** Eight streams (the master and seven
stems) share one ring size and
one drain pass, but each capture drops its block independently when its own ring
is full, so sustained write backpressure could drop a block from one and not
another — and a stem one block short is offset for the rest of the file.
Finalize logs the mismatch rather than padding or truncating, because either
repair guesses *where* the gap was.

**Skipback stems are capped at `SKIPBACK_STEM_MAX_SECONDS` (60 s)** while the
master runs to 5 minutes. Seven rolling buffers at that maximum is ~370 MB,
which is not a budget this device has to spend on a feature that is off by
default; 60 s × 7 is ~71 MB, and that is the **cap** rather than the usual cost
— the default Skipback length is 30 s (~35 MB), and the rings are allocated only
while Save Stems is actually asking for stems. The original anchor here was
"60 s × 5 is 53 MB, the same as one master buffer at its maximum"; the two send
stems broke that arithmetic, and the cap was left at 60 s deliberately rather
than shortened to restore it — shrinking it would silently truncate the stems of
anyone already running a 60 s Skipback, which is a worse failure than the 18 MB. When the master is longer the stems are a **suffix**
of it — both end at the save, so they line up with its tail. The buffers are
allocated and freed by the worker as the setting changes, via
`SHIM_EVT_SKIPBACK_RESIZE`; `skipback_resize`'s "length unchanged" early return
had to move *inside* the `skipback_saving` gate for that, or flipping the
setting without touching Skipback Len allocated nothing.

**Move Input as the sampler source records the master whatever Save says.**
There is no per-slot structure in the line input to split; a stems take there
would be four silent files and a copy of the input.

**Nothing was displaced to make room for it.** The first pass moved Audition out
of Audio believing the eight-cell grid page was a hard cap on a section; it is
not — see the pagination note above. `save_stems` is simply the ninth row in
Audio.

Tests: `tests/host/test_save_stems_contract.sh` (the cross-file rules — the stem
count against the name list and the slot count, the register appended last, the
`WANTS_*` predicates run against all three modes, the capture order, the RT-arm
gate, the skipback memory bound, and the setting's declaration and
persistence), `test_sampler_stem_path.c` (filename derivation),
`test_global_settings_contract.sh` (the section counts, and that Audio stays one
page while holding more than a grid page could).

### The slot BUS screens, and the arrow they were briefly on

A `Buses` action row on the slot's SETTINGS opens the bus list; a bus's own menu
opens its 8-position insert chain. `src/shared/bus_model.mjs` holds every rule
(pure, run by `tests/host/test_bus_model.sh`);
`src/shadow/shadow_ui_buses.mjs` draws them; `shadow_ui.js` owns the state and
the entry. Four screens: the list (buses, then Sends, then New Bus), one bus's
menu (Voices / Inserts / Send A / Send B / Rename / Delete), the voice
multi-select, and the insert chain with its own module picker.

**The door is a ROW, and it was briefly the DOWN arrow.** Down on the chain
editor's synth box opened the list, which took a shim change:
`shadow_control_t.nav_down_claim`, a `host_nav_down_claim()` JS binding, a
frame-sampled read gating a latched both-edge swallow, and a display-mode edge
clear to unstick the latch. All of it is gone, and the reason is worse than
"nobody would find the gesture": **up and down are Move's octave shift and only
DOWN was ever claimed**, so on a splittable synth you could shift up an octave
and not come back. The pair was broken, not borrowed — and it broke at the chain
editor's default resting cursor position. Removing the claim byte also restored
`sizeof(shadow_control_t)` and `stay_in_shadow`'s offset, which schwung-manager
reads raw (`shmconfig.go`, offset 85).

**Three surfaces carry the row, because a slot's settings take three forms.**
`CHAIN_SETTINGS_ITEMS` (the chain editor's Settings position, as a list),
`SLOT_SETTINGS` (the slot list's own screen) and `SLOT_GRID_ACTIONS` (the KNOB
GRID, which is what `enterChainSettings` opens by default — a row only on the two
lists would be unreachable for most users). It is an action row opening a
per-slot sub-editor, which is exactly what `Knobs`, `LFO 1` and `LFO 2` already
are on the same list, so it is that list's existing shape rather than a fourth
kind of thing. The two lists overlap heavily by long-standing accident (Volume
through MPE Mode are on both); that duplication is pre-existing and was MATCHED
rather than left as an asymmetry.

**Back from the bus list returns to whichever list opened it**, and it does so
through a THUNK resolved once at entry (`busListReturn`), not a view id. Two
reasons. Both destinations are re-entered rather than merely set —
`enterChainSettings` is the one place that decides grid-vs-list, and setting
`VIEWS.CHAIN_SETTINGS` from a grid session would drop you on a screen you never
opened. And the thunk ANNOUNCES itself, so the announcement cannot disagree with
the destination: this branch already shipped one Back that said "Chain Editor"
and went elsewhere, from `hierEditorIsMasterFx` — a boolean that could not name
a third chain.

**A slot whose synth publishes no `split_voices` shows NOTHING** — no row on any
of the three surfaces, and no screen. One predicate answers for all three
(`chainSynthSplits`, cached on the module id, false for a read that did not
complete — a stalled channel costs one draw without the row, never a row onto an
empty screen). It is checkable rather than assertable: `chain/len2/synth-splits`
in `tests/fixtures/chain-editor-baseline.txt` is byte-identical to
`chain/len2/sel-synth` and declared so in that test's `SAME_ON_PURPOSE`, so a
footer hint or a reclaimed arrow coming back breaks an EQUALITY rather than
sitting unnoticed in a hash nobody rederives. The `settings/slot/*` and
`settings/slotlist/*` cases render both lists with the row absent, present with
no buses, and present with a count.

The row's value is that count: a number when the slot has buses, nothing when it
has none, and `-` for a read that did not complete — three answers, because "no
buses" and "the channel did not answer" are different sentences (`busCount` in
`bus_model.mjs` returns -1 for the second, never 0).

**`null` from `synth:split_voices` is not "cannot split".** `chain_host.c`
clamps a plugin's -1 to `""` in its `split_voices` branch precisely so the two
cannot collide, which makes a `null` here a real channel failure: the list opens
in a waiting state and the tick retries. `busConfig` and `busVoices` are only
ever assigned from a RESOLVED parse, so a failed read empties nothing and
latches nothing.

**Orphans are shown, in three places.** A bus stores voice IDS and retains the
ones that no longer resolve (`orphans` in `buses:config`, which is the one GET
the UI makes — the per-bus and slot-wide `orphans` keys were a second spelling
nothing read, and are gone). The list marks such a bus `Kick !`, the bus menu carries the
count in its header, and the voice screen lists every unresolved id as its own
row marked `!` — the only way to clear one. Every write to `bus<N>:voices` is a
whole-list replace, so `toggledVoiceIds` CARRIES the orphans: a list rebuilt
from the resolvable voices alone would erase exactly what the count reports.

**A voice renders into one buffer**, so adding it to a bus removes it from
whichever other bus holds it — two writes, and both through
`shadowSetParamBlocking`, because under co-run a fire-and-forget pair shares one
SHM slot and the second clobbers the first.

**A bus insert is a THIRD CHAIN TARGET, and until it was one its parameters
could not be reached at all.** Click on a populated `BUS_CHAIN` position went
to the module picker unconditionally, there was no Shift+Click, and
`buildKnobContextForKnob` had no `BUS_CHAIN` case — so a CloudSeed loaded on a
bus kept its defaults for good and the eight encoders answered `null`, which is
not "no mapping" but no feedback either. The DSP surface was already there
(`chain_bus.c` serves `bus<N>:fx<K>:<param>`, `:chain_params`, `:ui_hierarchy`
and `:state`); only the UI was missing. `busChainTarget(busIndex)` is that
chain, so the knob context, the merged parameter metadata and the entry gate
land on it by construction rather than one scope boundary at a time — the same
argument the two chain editors' convergence note makes at length. Click now
EDITS a loaded insert and ADDS on a `+` or a hole; Shift+Click swaps, which is
where the slot chain has always kept it. The component key ("bus1:fx2") IS the
DSP prefix, exactly as Master FX's is, which is what lets the existing grid
address it with no mapping of its own. Three things needed saying separately
though: the entry gate reads through the bus target (`slotChainTarget` answers
`null` for a bus key, so the hierarchy read would never happen and the gate
could only hold); `chain_params` comes from the bus target for the same reason,
and an empty list is exactly what makes the grid invent a `float 0..1` knob for
every parameter; and Back needed `hierEditorReturnView`, because
`hierEditorIsMasterFx` is a BOOLEAN and a third chain cannot be named by it.
Both destinations are wired, not just the grid — `paramPagesEnabled()` is false
whenever the screen reader is on, so a grid-only bus insert would be uneditable
for exactly the users who cannot see the diagram behind it. Trailing pages (My
Presets / Module) are excluded, inside `componentParamPagesIo`, the way Master
FX is: every action on them is slot-chain shaped.
`tests/host/test_bus_insert_editable.sh` pins each leg, because not one of them
is a pixel.

**The send mixer is ONE PAGE PER SEND PER KIND.** The `Send Mixer` row on the
bus list — named that, not `Sends`, because under a menu called *Buses* the
shorter word reads as *this slot's* sends and means *a mixer for the buses'*
sends, an ambiguity that got worse the moment the slot acquired sends of its own
— opens a synthesised contract (`busSendGridHierarchy` in `bus_model.mjs`)
whose pages carry every level on an encoder; the per-bus rows on that bus's own
menu stay, because a level you have to click into, jog and click out of is not a
level you can RIDE. **Send A** and **Send B** are the buses, and they are the
whole mixer.

**IT CARRIED TWO MORE PAGES, `Voices A` / `Voices B`, AND THEY WERE THE WRONG
TIER.** A voice's send level belongs to the MODULE — dr32 already published
per-pad `send1`/`send2` knobs on its own `pads` level, beside pan and cutoff —
so the same number had two homes that did not agree, which is what this screen
was actually showing. The faders, the `voice<V>_send<M>` grid key, the
`buses:voice<V>:send<M>` route and the `voice_sends` array in the slot document
are all gone; the audio path is untouched. See `docs/CHAIN.md`, "The module owns
a voice's send level". **A level with no keys is omitted, not emitted empty**,
and with the voice pages gone a slot with no buses declares no mixer at all —
which is why `busListRows` offers the door only when there is a bus to ride.

**`paginate` is a whole-CONTRACT switch, not a per-level one**, which is why the
hierarchy declares none: a flag written on a level would be read by nobody.
`enterBusSendsGrid` pins the mixer to one page (`paginate: false`) —
`SLOT_BUSES` = 8 cells against eight knobs, so a split can never happen and the
flag says the grouping is AUTHORED. It was conditional while the voice faders
lived here (32 declared cells on an eight-knob page leave twenty-four
undrawable, worse than the split the pin prevents); it is unconditional again.

The ROOT level carries no knobs, deliberately — the planner names a walk root's
grid page "Main" whatever the level declares, and "Main / Send B" is not a
mixer — so the pages are the levels below it. The io maps a flat grid key onto
the real spelling in one place (`busSendGridRealKey`): `bus<N>_send<M>` →
`bus<N>:send<M>`, and nothing else. A `voice<V>_send<M>` form resolved to
`buses:voice<V>:send<M>` while the host owned those levels; it answers `null`
now, because `chain_bus.c` no longer serves that route and a key the host
refuses in silence is a fader that moves and is never heard. A HOLE does not
renumber: the second present bus is bus 3 and its key
says 3. An unresolved `buses:config` yields a `null` contract rather than an
empty one, because an empty one is a claim — "this slot has no buses" — drawn as
a mixer with no faders on it.

**There is no MAIN row, and the slot's own two send levels are not offered.**
They were, and they did nothing: `inst->main_send_level` is written, serialized,
read back and patch-applied, and **no audio path reads it** —
`chain_drain_sends` in `chain_host.c` says so in as many words. It is inert for
a real reason: at drain time Main's post-insert signal does not exist, because
under the same-frame-FX mode the device always runs `render_block` returns the
raw synth and the slot's own FX chain runs later into a different buffer, so
draining Main there would send a PRE-FX signal while every bus sends a
POST-insert one — two meanings behind one control. Doing it properly needs a
second drain point after `chain_process_fx` (and under `rebuild_from_la` that
point moves again). Until then the row is `Sends`, a door into the mixer and
nothing else, and it is offered when the slot has at least one
bus. (It briefly opened for a splittable module with none, because per-voice
sends need no bus; those faders belong to the module now, so a mixer with
nothing on it would again be a row that answers a click by doing nothing.) The C
fields stay, with the missing drain named beside them, so wiring it later is a
mix-path change and not a re-plumb. `busRowsNow` also drops the row when the
knob grid is not the user's Param View — every screen-reader session — since
the door then opens onto nothing and the same two levels are already rows on
each bus's own menu.

**Buses PERSIST, and for a while the file format had a reader and no writer.**
`bus_parse_section` (`chain_patch.c`) has always read `buses` / `main_sends` out
of a saved slot; nothing emitted them. That is worse than "buses do not
persist": `patch_info_t` is zeroed before the parse, so a document without the
key arrives at `chain_bus_apply_patch` as four absent buses and it **resets all
four**. Build a two-bus kit, load any preset or change sets, and the buses,
voice assignments, inserts and send levels were destroyed mid-session, in
silence. The producer is `busPatchFields` (`bus_model.mjs`), called from
`buildSlotPatchJson`, and it emits per-FX `state` as well — which is what makes
`chain_bus.c`'s staged `fx_state_request` / `fx_state_pending` arm reachable at
all, so a bus reverb comes back with its parameters and without being
reinstantiated. Three rules ride on it: a hole is `{"present":0}` and never a
compaction; key ORDER is load-bearing, because `bus_field` takes the first hit
inside the object's span and an insert's opaque state is inside that span
(`name` before `fx`, `module`/`bypassed` before `state`, and `main_sends`
declared ahead of every component in the document); and a
`buses:config` read
that did not COMPLETE bails the whole save — for an explicit save too, because
the document it would otherwise write is not missing a field, it is a document
that deletes the user's buses on the next load.
`tests/host/test_chain_patch_roundtrip.sh` runs the real producer under node and
feeds what it writes to the real C parser, with the key deleted as the negative
control; a hand-written fixture on both sides is exactly how a format with no
writer passed its tests. Its expectation is built from the **fixture config**,
not from the producer's output — built from the output, the diff would assert
only that the parser echoes whatever the producer said, and a producer that
silently dropped an entry would drop it from both sides and pass.

**Per-voice sends do NOT persist in this document.** They did, as a flat
id-keyed `voice_sends` array, for as long as the host owned them. They are the
module's own parameters now and ride inside the synth's opaque `state` blob,
which this same document already carries — so a slot reload restores them by the
same route every other one of the module's parameters takes. A second copy here
would race the state load on the way back in, and the loser would be a level the
user cannot find. An old document's `voice_sends` key reaches nothing:
`chain_patch.c` has no field left to parse it into.

**The list value column is ~11 characters and carries three facts.** Insert
summary plus both send levels: past two inserts the summary becomes a COUNT
(`3 FX`), because a third abbreviation pushed both levels off the row — seen in
the render, not reasoned about. `valueX` is 52 rather than the default 92 for
the same reason; the eight-character label floor still protects the bus name.

### The FX buses: Master FX is now one of three, and the sends are GLOBAL

**Design credit: PR #121 by legsmechanical.** The send topology below — two
post-fader send buses hosted as `master_fx_slot_t`, a `send_accum[]` in the
shim, return levels, the feedback-safe A→B ordering, shared presets, and one
generic FX-bus picker over all three buses — is that PR's design,
device-verified there and documented in its own `docs/SEND_FX.md`. It is
unmergeable (merge-base 2026-03-04; `main` is 1696 commits ahead and the branch
carries 864 of its own), so this is a re-implementation of its design on current
`main`. The one part not re-implemented is the shared preset store — a send
declares `hasPresets: false` here. `send_fx_key.h` and `docs/CHAIN.md` carry the
same credit.

**Sends are GLOBAL, and that is a cost decision.** Per-slot sends mean four
reverbs when four slots want one reverb. There are two device-wide send buses
instead, each hosted **exactly like Master FX** — the same `master_fx_slot_t`
array, the same "always process, then restore the dry on a bypassed position"
discipline — so there is one piece of chain machinery in the shim, not three.

**THREE TAPS FEED THEM, and only one of them needs anything of the module.** A
per-bus send and a per-voice send both require `split_voices`; the **slot send**
does not, so on an ordinary synth it is what feeds these buses at all. It is
drained by `chain_drain_main_send` from the shim's MIX pass — not the render
pass, where the slot's post-FX audio does not yet exist — and edited in Slot
Settings, never on the bus Send Mixer, which a slot with no buses never sees.
See `docs/CHAIN.md`, "The slot send".

**Post-insert and post-fader.** A slot's per-bus buffers already carry their own
insert chains when `chain_drain_sends` reads them, the slot send is taken after
the slot's own 8 FX, and the slot's effective volume is passed in to both, so
pulling a track down pulls it out of the sends the way a console does. Mute and solo live inside `shadow_effective_volume`, so a muted
slot feeds the sends nothing, and a bypassed synth clears `bus_rendered_mask` so
its voices cannot reach a send through the 1-in-172 probe frame. The level is
quantised to 0..127 and applied **per block**, so it does not follow the main
mix's per-sample fade ramp: a slot fade is a step of at most one block here.

**A→B is applied between A's chain and B's, which makes feedback impossible by
CONSTRUCTION.** There is no point at which B's output can reach A, so there is
no loop to detect and no loop detection to go wrong. It works because A is
processed on an earlier iteration of the same loop — `sb == 1` reads a
`send_out[0]` that is already final for the frame — and it is scaled by A's
*return* level as well as the feed level, because a send from A is post-A's-fader
like every other send here. A `_Static_assert` fails the build if `SEND_BUSES`
ever leaves 2: the A→B block names send 1 by index, and a third bus turns "the
second of two" into "one arbitrary bus", which needs a routing matrix rather
than a special case.

**The returns sum into `fx_target`** — the bus Master FX is about to process —
which is the whole reason the send block sits immediately *above* the MFX loop
rather than below it, and why the send stems are pre-Master-FX like every other
stem.

**One picker, three buses.** Shift+Vol+Menu (and hold-Menu, and Shift+Menu) now
opens `VIEWS.FX_BUS_PICKER` — Master FX, Send A, Send B — rather than the master
bus directly. The shim flag is unchanged: it says "the user asked for the FX
screen", and *which* FX screen that is is a UI decision. Back from a bus returns
to the picker, and Back from the picker is what leaves shadow mode; that is a
change of destination for the Master FX screen, so its footer's `BACK` pair is
**derived** from the shared chrome's pairs rather than written out again, or the
two editors drift on the one word that differs.

**The editor is PARAMETERISED BY PREFIX, not triplicated.** Every key the
Master FX editor writes is `master_fx:` + the component key; a send's is
`send1:` / `send2:`. `FX_BUSES` is the whole table, and two things a send does
NOT have are declared there rather than inferred at a draw site: `hasLfos`
(false — the shim serves no send LFOs, so asking is an IPC round trip that can
only answer `""`, and a false here keeps four reads per frame off the diagram)
and `hasPresets` (false — a send has no preset store yet, so a future one is one
word). `busLevelKeys` names the return level and, for A, the A→B feed, so the
settings menu and the info band read one list instead of two copies of a
conditional.

**Everything the editor holds about "the chain" is keyed by position, not by
bus** — `masterFxConfig`, the selection, the chain-length override — so
switching bus drops all of it. `fxBusSwap` is the one place `currentFxBusIndex`
changes, for that reason.

**The picker's value column asks the SHIM, never the mirror**, one positional
GET per bus, once on entry — three IPC round trips at ~2.8 ms each is already
more than a whole page render, so they must never reach a draw path. The mirror
only reflects a bus that has been *entered* this session, so a never-opened send
would read `Empty` from it with a chain loaded from a previous session. A `null`
prints `--`, not `Empty`: "Empty" is what sends someone looking for the reverb
they just loaded.

**Persistence: the shim says what is loaded, and the levels are filed under the
BUS.** `send<N>:modules` is one positional GET returning the whole chain, never
compacted, for the same reason `master_fx:modules` is (see "The SHIM says what
is loaded" above). Positions restore from `send_fx_<bus>_<pos>.json` through the
**same function** `master_fx_N.json` goes through — two copies of that JSON
scraping is how the sends would end up restoring `state` and not `params` with
nothing to compare against. The two return levels and the A→B amount get their
own `send_levels.json`, because they belong to the **bus** and filing them under
position 0 would lose them the moment that position is emptied — an empty send
with its return up is an ordinary state. They are restored **last**, because
`shadow_send_bus_active()` answers true on a level alone, and raising a return
before its chain exists would put one dry frame through the master bus.

### Loading an FX module happened on the SPI CALLBACK, and the editor could not see it

`fx_slot_load_impl` (`src/host/shadow_chain_mgmt.c`) does a `dlopen`, a
`create_instance` and a `module.json` read. Both `module` param writes —
`master_fx:fxN:module` and `send<N>:fx<M>:module` — reached it from
`shadow_inprocess_handle_param_request`, which the shim calls from
`shim_pre_transfer`. **That is the SPI callback**: SCHED_FIFO 70, core 3,
~2370 µs of slack for the whole device.

The comment above the function said so and called it "pre-existing in kind,
mirrors Master FX". It is not pre-existing in kind: **bus FX were deliberately
moved to a worker for exactly this reason** (`chain_bus.c`,
`chain_bus_worker_fn` / `bus_load_fx`), and the sends were left on the callback
only because Master FX was. The inconsistency was the defect.

**The user-visible failure is not "the device stutters".** The JS param channel
deadline is 100 ms (`SHADOW_PARAM_DEFAULT_TIMEOUT_MS`, `shadow_ui.c`), and a
7.7 MB CLAP bundle takes far longer than that to open. So the write **timed
out**, and so did every read behind it while the callback was still inside the
`dlopen` — which is the whole editor's supply of facts. Reported from hardware
as: the screen sat on "Loading", nothing else would load afterwards, and
`send_fx_0_0.json` still held the previous module. That last one is the
autosave working correctly — `saveSendFxChainConfig` refuses to write on a read
that did not complete, which is the three-answer rule doing its job.

Now:

- **RT records the intent and returns.** `shadow_fx_load_request` is a bounded
  `snprintf` and three stores. The `module` SET answers **error 0 meaning
  ACCEPTED, not loaded**; error 7 is kept for a request that can never be
  served (bad index, no owned buffer).
- **The worker does the expensive half** — `dlopen`, `create_instance`, the
  `module.json` parse, and the `destroy_instance` / `dlclose` of whatever it
  replaced. It is the **shim's existing worker** (`shim_worker.c`), which is
  created from shim init and demotes itself to SCHED_OTHER on cores 0-2 as its
  first action. A `pthread_create` from a module entry point would inherit
  SCHED_FIFO 70 — above Move's own `Link Main` at 35 — and starve the audio
  publisher, i.e. cause the dropouts going off-thread is meant to avoid.
- **RT installs.** The worker builds into a staging `master_fx_slot_t`; the SPI
  thread moves the pointers across and hands the outgoing module to a retire
  ring. **That is why no reader had to change**: `shadow_master_fx_slots` and
  `shadow_send_fx_slots` are read unguarded from the render loop, the MIDI
  forwarder, the param handler and both snapshot serializers, and the RT thread
  stays their only WRITER. Gating each of those instead was the alternative,
  and it is dozens of sites that do not know a gate exists.
- **The gate is a SEQUENCE NUMBER, not a flag** (`src/host/fx_load_gate.h`),
  for the reason `chain_bus.c`'s is: a boolean makes "close" a store by one
  thread and "open" a store by another, so a worker preempted between its check
  and its store re-opens a gate the RT thread closed — putting `dlclose` in a
  race with the render path. Here the RT thread owns **both** stores (`req_seq`
  closes, `done_seq` opens at install) and the worker publishes only the
  stage's own state word, which the install refuses whenever the seq it carries
  is no longer the one being asked for.
- **CLOSE BEFORE YOU PUBLISH.** The request has a payload the gate does not, so
  `req_seq` is bumped *first*, the path is written, and `req_pub` catches up
  last. Reading the path before that is a torn `dlopen` argument.
  `tests/host/test_fx_load_off_callback.sh` fails on the reverse order, on a
  module SET that calls a synchronous loader, and on file I/O or allocation in
  either RT function.

**The editor can now SEE all three states**, which is the half that makes the
move worth anything: `<prefix>:is_loading` answers "1" while a request is in
flight and `<prefix>:load_error` answers "1" when the last settled one failed.
The component entry gate already held on an exact `is_loading == "1"`; what it
lacked was an ending, because **a failed load leaves the position genuinely
empty, which is byte-identical to one still arriving** — and the hold never
gives up on purpose. `ENTRY_FAILED` (`component_load_gate.mjs`) is that ending.

**A loading position names the module it is BECOMING**, not the one leaving and
not "". Both alternatives were wrong in opposite directions: the outgoing name
makes the entry gate open the editor of the module being replaced, and an empty
one makes the autosave read the position as vacated and write `{}` over the
state file. `shadow_fx_load_pending_name` is the single namer, so `:module`,
`:name` and both `modules` snapshots cannot disagree mid-load.

**A restore must WAIT; an interactive pick must not.** Every restore loop
writes `module` and then `state`/`params`, and those used to land on a plugin
the synchronous load had already put in place. `waitForFxPositionSettled`
(`shadow_ui.js`) polls `is_loading` for up to 10 s before the writes that
follow a module write in the three restore paths (set change, patch-library
preset, send set load). The interactive picker has no state to follow it — it
is the path that had to stop blocking, and it does not wait. Boot restore still
uses the **synchronous** loaders (`fx_boot_restore_one`), which is correct:
that runs at shim init, not on the callback.

**A shape verb refuses while a load is in flight.** `fx:insert` / `fx:remove` /
`fx:move` permute the position array a staged realisation is stamped against,
so running one mid-load would install the incoming module wherever the shift
left that index. Error 15, not a queue — the editor already reports a refused
verb, and a queued one would reorder a chain the user has since changed.

Not in scope and unchanged: the Airwindows module (`clap`) picking the first
plugin it finds when its config names none ("No plugin in config, loading first
available" → `ADT`). That string is in `charlesvestal/schwung-airwindows`, not
here; the host reports the *module* the user chose and never presents a
sub-plugin as a choice.

### Snapshot / recall: what it restores, and what it deliberately does not

Shift+Copy snapshots all 4 slots plus all 8 Master FX positions; Shift+Delete
puts it back. One snapshot, overwritten each time.

**A recall writes STATE, never SHAPE.** For each position it writes
`<prefix>:state` and `<prefix>:bypassed`, and nothing else. It does not call
`load_file`, which is what the set-change path uses — `load_file` restores
module *identity*, and doing so reinstantiates: reverb tails cut, arp phase
resets, delay buffers empty. That is precisely the opposite of what an A/B
gesture is for. A position whose module was swapped since the snapshot is
skipped rather than reloaded.

**The skip count is the feature, not decoration.** A partial restore that
reports nothing is indistinguishable from a working one until you notice by
ear, and one bad experience of that makes the whole gesture untrustworthy.
`planRestore` (`src/shared/snapshot.mjs`) counts three distinct misses and the
toast shows the total:

| reason | meaning |
|---|---|
| `swapped` | a different module sits there now |
| `nostate` | right module, but it implements no `state` key — `denis` and `branchage` are the known cases (see `getSlotStateWithRetry`) |
| `empty` | the position held a module in the snapshot and holds nothing now |

A position empty in *both* is not counted. Counting it would make every
partly-filled rig report skips forever and the number would stop meaning
anything.

**Storage is set-associated and re-seeded on every set load.** It lives in
`set_state/<uuid>/snapshot/` as twelve files in the same format as set state.
A global snapshot directory would be the one piece of chain state that does not
travel with the set, and would be wrong the moment you changed sets or booted
into a different one; here it is deleted with the set for free. The forced
re-seed on `SET_CHANGED` is what makes the snapshot mean one sentence — *how
this set was when you loaded it, or the last time you pressed Shift+Copy in
this session* — so nothing invisible survives a set change. A second,
**conditional** seed runs at `shadow_ui` startup when the set has no snapshot
dir: without it a device that upgrades and boots straight into its existing set
would have no snapshot until the next set change, and the first Shift+Delete
would do nothing at all. It stays on disk rather than in RAM so a `shadow_ui`
restart mid-session (overtake exit, set change) does not lose a snapshot the
user took; being overwritten on load is what stops that persistence outliving
the explanation. Set duplication does **not** copy it — the duplicate loads
fresh and would be re-seeded immediately anyway.

**There is no second serializer.** A take is `autosaveAllSlots()` +
`saveMasterFxChainConfig()` followed by a file copy. Those writers already
carry every guard the format has accumulated — the bail-if-empty that protects
a good file from a timed-out read, the skip-if-unchanged that keeps eMMC quiet,
the shim-reports-empty cross-check. A parallel capture path would have to
re-derive all of it and then stay in step with it forever. The copy writes `{}`
for a file that reads back empty rather than skipping it: leaving the previous
snapshot's file in place would splice two snapshots together, and each file
parses fine on its own so nothing downstream could tell.

**`ui_flags` is full, and widening it is not free.** All eight bits are taken,
and `ui_patch_index` (uint16) sits immediately after it at offset 8 with no
padding — so a uint16 `ui_flags` moves every field behind it and changes
`sizeof(shadow_control_t)`, which `shadow_constants.h` asserts as `==`, not
`<=`. The shim and shadow_ui are separate binaries mapping one segment; a
layout change reaching one before the other is silent corruption, not a build
error. Flags 0x0100+ therefore live in `ui_flags_ext`, which was `reserved16`.
`js_shadow_get_ui_flags` presents both as one flat word and
`js_shadow_clear_ui_flags` splits a mask back apart, so JS never has to know
which byte a flag sits in — and `SHADOW_UI_FLAG_EXT_SHIFT` is asserted equal to
`8 * sizeof(ui_flags)` rather than written as `8`, because at any smaller shift
the ext space aliases onto low flags in that flat word.

**The shim only raises the flag.** A take is ~20 param round trips at ~2.8 ms;
the gesture is detected inside the SPI callback, which does none of that.
`shadow_ui` (SCHED_OTHER, running even with the display hidden) does the work,
which is also why the gesture works whether or not the shadow UI is on screen —
and why the shim branch logs nothing: `shadow_log` calls `unified_log`.

Tests: `tests/host/test_snapshot_plan.sh` (the planner and its counts),
`test_snapshot_gesture.sh` (the shim branch), `test_snapshot_wiring.sh` (the JS
wiring and toast geometry), `test_ui_flags_layout.c` (the SHM layout).
