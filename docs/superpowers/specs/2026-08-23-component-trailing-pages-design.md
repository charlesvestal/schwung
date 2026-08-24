# Trailing pages: User Presets and Module, on every chain component

**Date:** 2026-08-23
**Branch:** `component-actions-page` (worktree, off `main`)
**Status:** design approved, not yet planned

## The problem

User Presets already works for every chain component and needs no per-module
code — a preset is the component's opaque `<prefix>:state` blob, the same one
slot autosave and chain patches use, stored under
`/data/UserData/schwung/presets/<module-id>/`. What it lacks is **reach**: the
only way in is Shift+Click a component → module picker → the indented
`[User Presets]` row (`enterComponentSelect`, `shadow_ui.js:9236`). Nothing on
the component's own pages says the feature exists.

Removing or swapping a module has the same problem in reverse: it is reachable
only from that same picker, where removal is spelled "None" at the top of a
module list.

Both are things you do *to* the loaded component, and neither appears anywhere
in the pages you actually look at while using it.

## What we are building

Two synthetic pages appended after a component's own last page, reached by
jogging to the end:

1. **User Presets** — the existing browse/detail screen, plus a `*` mark and a
   real overwrite Save.
2. **Module** — `Swap Module` and `Remove Module`.

Scope: real loaded chain components (synth, audio FX 1–4, MIDI FX) in the four
chain slots, **grid view only**.

## Scope exclusions, and why

| Excluded | Why |
|---|---|
| Slot Settings, Master FX Settings | Settings screens, not modules. They are synthesised contracts with no module id to key a preset folder on, and nothing to swap or remove. |
| Master FX chain-position modules | `__user_presets__` is injected in `enterComponentSelect` only, never in `enterMasterFxModuleSelect` — Master FX components have **no user presets today**. This inherits that gap rather than widening it. Giving Master FX presets is a separate piece of work. |
| Empty chain positions | No component, so no pages to append to. |
| List view (`param_view = 0`) | `HIERARCHY_EDITOR` is a separate code path with no pages to jog through. Building the same feature there is a second definition of one thing — the exact failure the one-list-engine work was undoing. The grid is the default (`paramViewGlobal = 1`, pinned by `tests/host/test_param_view_default.sh`); list-view users keep today's Shift+Click route, which is unchanged. |
| Tool consumers of the planner | A sequencer embedding the grid for parameter locks has no slot to swap a module in. The append is therefore **opt-in**, never automatic. |

## Where the pages come from

### PAGE_MENU is the right primitive, and it is already built

`page_plan.mjs` defines `PAGE_MENU` as *"a list of entries that are NOT
parameters — an action with a name and a consequence and nothing to show"*, and
its comment names our use case verbatim: the other four page kinds are all
param-driven, so *"none of them can express Save / Save As / Delete / Knob
Mapping."*

The whole path exists and works:

- planned by `page_plan.mjs` from a level's `menu: [{label, action}]`
- drawn by the controller in the page chrome (`page_controller.mjs:2630`),
  with the module name, page name and bank bar intact
- activated in two steps — first click enters, second fires
  (`onClick`, `page_controller.mjs:2273`)
- dispatched host-side via `controllerIo.runAction(action)` or
  `ctx.runSlotAction(slot, action)` (`shadow_ui_param_pages.mjs:853`)

The controller deliberately never performs the action: *"the host owns whatever
Save or Knob Mapping means."*

**Zero of the 95 modules in the fleet capture declare `menu`.** It is a complete
mechanism currently exercised only by synthesised contracts (Slot Settings,
Master FX Settings, Global Settings).

Two properties fall out for free:

- **Both pages are doors.** A menu page must be entered before its entries can
  be fired, and draws inert brackets until then. Jogging past the end of a
  module therefore cannot fire Remove Module.
- **A failed contract read cannot manufacture them.** `planPages` already
  returns `pages: []` with `unresolved: true` when the `ui_hierarchy` read
  fails, on the rule that *"a plan is a statement about what a module declares.
  With a failed read we have no such statement, so we make none."* The append
  must sit on the resolved paths only.

### The append belongs in `planPages`, not in the hierarchy

The obvious approach — inject a synthetic level into the module's hierarchy and
reference it last from root — is what Slot Settings does
(`shadow_ui_slot_grid.mjs:299`), and its comment explains why the level is
separate: *"a level emits its menu straight after its own grids, before any
level it navigates to, so a menu on root would land second."*

Verified against the real planner. A menu on `root`, with one child level:

```
0 knobs "Main"   1 menu "Main Menu"   2 knobs "Filter"
```

The trick works for Slot Settings because it **synthesises its entire
hierarchy** and therefore owns root. We do not own a module's hierarchy, and
three fleet facts make injection fail outright:

- **11 of 95 modules publish no `ui_hierarchy` at all.** They are paginated from
  `chain_params` by a fallback that never touches `levels` — there is no object
  to inject into.
- **minijv has no `root` level.** Its walk root is `patch`.
- **The walk root is mode-dependent.** With `modes` present the level names *are*
  the mode names and `rootKey` is chosen from the active mode
  (`page_plan.mjs:305`), falling back to the first level key. Injection would
  have to reimplement that resolution and stay in step with it.

Appending after the walk handles all four shapes — hierarchy, no-hierarchy
fallback, modes, no-root — in one code path.

`page_plan.mjs` is **pure**: no param I/O, no drawing, no globals, no
module-level state. Appending caller-supplied page data keeps it pure. The
planner gains an option carrying the menu pages to append; it appends them after
the walk, through the same name-claiming every other page goes through, and on
resolved paths only. It never learns what "Remove Module" means — that stays
host-side, which is the same boundary that keeps actions out of the editors.

## Page 1: User Presets

The existing `shadow_ui_presets.mjs` screen, reached as a page instead of only
through the picker. Mechanics unchanged: scrolling auditions live (debounced),
Back reverts to the state captured on entry, the detail screen's Load commits,
autosave is suppressed while auditioning.

Three changes:

**Header says "User Presets".** A module may also publish its own factory
preset browser via `list_param`/`count_param`/`name_param`, which plans as a
`PAGE_PRESET` page. Both are called "presets" and both can be on the same
module, so the two must be distinguishable on sight. They are different classes
of thing: a factory browser is the module's own content, a user preset is a
snapshot you made.

**The loaded preset is marked, with `*` when modified.** The state blob carries
no name, so which user preset is current is new bookkeeping.

**Save overwrites; Save As does not.** Today the screen can only save as new —
*"Save never overwrites: a name collision auto-appends a number"* — and the
detail screen offers only `DETAIL_LOAD` and `DETAIL_DELETE`. A `*` you cannot
clear is decoration, so overwrite-in-place Save is added. Save is hidden when no
preset is loaded, matching `SLOT_GRID_ACTIONS`, where `Save As` is `always: true`
and `Delete` is `always: false` filtered on `hasPreset`. Delete targets the
loaded preset.

### Current-preset bookkeeping

`{name, hash}` per slot+prefix. Set on Load and on Save/Save As; cleared on
Delete and on component swap or removal.

The DSP never sees it — it is pure UI state — so it needs no C struct field and
no SHM change. It rides in the slot's existing autosave JSON (`slot_N.json`),
read back in JS on load, the same way bypass round-trips today.

### The `*` is computed on entry, never on the draw path

The mark is a comparison of the live `<prefix>:state` blob against the stored
one. A param read is ~2.8 ms against a 1.68 ms whole-page render — **an IPC read
costs more than redrawing the entire screen** — and `test_chain_edit_read_budget.sh`
exists specifically to stop reads leaking into draw. State blobs are also the
largest values on the wire.

So: read on page entry and after a write we performed, cache the verdict, redraw
from the cache. This mirrors `knobCardOpen`, which reads every value on
touch-down and none on the draw path.

## Page 2: Module

Two rows, both forwarding to flows that already exist and are not modified:

- **Swap Module** → the same picker as Shift+Click (`enterComponentSelect`)
- **Remove Module** → what picking "None" in that picker does today, immediately

Remove needs no confirmation dialog of its own: the page is a door, so reaching
the row already took a deliberate click, and removal is recoverable by picking
the module again.

This is a **second way** to do both, not a replacement. The picker keeps "None"
and keeps its `[User Presets]` row — that is what list-view users rely on.

## The fourth `run*ActionFromGrid`

Firing an action from the grid that opens a modal or another view, and returning
to the grid afterwards, is an established pattern with three instances:
`runSlotActionFromGrid`, `runMasterFxActionFromGrid`, `runGlobalActionFromGrid`.
The third one's comment says so and names the two properties that make them
work — notably that it asks **whether something else is now on screen** rather
than enumerating which actions leave, *"so a test on the key would be right
today and silently wrong for the fourth action."*

Ours is that fourth action. Rather than a fourth copy, extract the shared
reconciler and have all four use it. The reconcile-don't-hook shape is
load-bearing and documented as such: the modal *"has many ways to finish —
confirm, decline, Back, and for Save a decline that returns to the name preview
instead of exiting. Hooking each one means being wrong about exactly one of
them, which is how the original bug got here."*

## Testing

**Static / unit**

- `page_plan.mjs`: trailing pages land last for all four hierarchy shapes —
  ordinary hierarchy with child levels, no-hierarchy `chain_params` fallback,
  `modes` (minijv), and no-`root`. Assert position, not just presence.
- Trailing pages are **absent** when the append is not requested (tool
  consumers) and when `unresolved: true`. The second is the important one: a
  failed read must not produce a Remove Module button.
- Conditional rows: Save and Delete present only with a preset loaded, Save As
  always.
- `slot_N.json` round-trips the current-preset record across a reload.
- Read budget: extend the existing budget test so the Presets page's draw path
  performs no state-blob read.

**On device** (per component type — synth, audio FX, MIDI FX)

- Jog to the end of a module: User Presets then Module, both inert until clicked
  into.
- Save → `*` clears; turn a knob → `*` appears; Save again → clears; Save As →
  new preset becomes current; Load → `*` clears; Delete → removes the loaded one.
- Swap and Remove behave identically to the Shift+Click path.
- Reboot preserves the current-preset mark.
- A module with no `ui_hierarchy` (one of the 11) still gets both pages after its
  paginated params.
- minijv: pages land last in each mode.
- Switch `param_view` to List: the old Shift+Click route still works and nothing
  new appears.
