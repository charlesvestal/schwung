# shadow_ui.js Decomposition (Cleanup Step 8) Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Break the 15,030-line `src/shadow/shadow_ui.js` into navigable
view/subsystem modules and collapse its four parallel `switch(view)`
dispatchers into one `HANDLERS[view] = {draw, jog, select, back}` registry —
with **zero behavior change** and the on-device shadow UI verified after every
commit.

**Architecture:** Subsystems-first, not views-first. The four switches
(`handleJog` ~10410, `handleSelect` ~10759, `handleBack` ~11676, the draw
dispatch ~13500) can't be cleanly collapsed while individual view handlers
reach into *shared* sub-state (the Help overlay and the settings-value engine,
both shared by MASTER_FX and GLOBAL_SETTINGS). So we first lift the shared
subsystems into their own modules behind the existing `ctx` bridge, then
extract per-view handlers into the draw modules that already exist, and only
then build the registry. Each step is independently shippable and reversible.

**Tech Stack:** QuickJS ES modules (`.mjs`/`.js`), the `ctx` bridge
(`src/shadow/shadow_ui_ctx.mjs`), source-invariant shell pins under `tests/`,
`node --check` for syntax, Docker cross-build + `install.sh local` for deploy,
on-device md5 hash-match + SPI-holder health check for verification.

---

## Background: why views-first fails (scoped 2026-06-13)

Every candidate first-cut was scoped and found tangled:

- **Views** (`MASTER_FX`, `GLOBAL_SETTINGS`) branch into the **Help overlay**
  sub-state (`helpDetailScrollState`, `helpNavStack`, `helpReturnView`) and the
  **settings-value engine** (`getMasterFxSettingValue` @~10012,
  `adjustMasterFxSetting` @~10123, `getMasterFxSettingsItems` @~1052,
  `GLOBAL_SETTINGS_SECTIONS` @~790).
- **Help** is woven as `else if` sub-branches inside two views' switch-cases,
  plus a ~140-line loader (~4444–4581). 57 refs.
- **Settings engine** looks liftable but reads/writes ~6 cached-state globals
  (`autoUpdateCheckEnabled`, `previewEnabled`, `padSelectGlobal`,
  `textPreviewGlobal`, `filebrowserEnabled`, `midiIndicatorEnabled`) that also
  gate live input behavior — so the state must be re-homed before the engine
  moves.
- **Canvas / wav_position** are special param-editor *types* threaded through
  the hierarchy editor and the tick/MIDI fast paths — defer to last.

## The `ctx` bridge (the state-ownership mechanism)

`src/shadow/shadow_ui_ctx.mjs` exports an empty `ctx = {}`. After its own
declarations run, shadow_ui.js populates it (~line 12642+):

```javascript
// Shared LIVE state — modules read AND write the real variable:
Object.defineProperty(_ctx, 'selectedSlot', { get: () => selectedSlot, set: v => { selectedSlot = v; }, enumerable: true });
// Functions / constants:
_ctx.VIEWS = VIEWS;
_ctx.enterChainEdit = enterChainEdit;
```

View modules `import { ctx } from './shadow_ui_ctx.mjs'` and touch
`ctx.selectedSlot` **inside function bodies only** (ctx is populated after
init). Proven handlers: `handleSlotsJog`/`handleSlotsSelect` in
`shadow_ui_slots.mjs`.

**State-ownership rule for this plan:**
- State used by **one** module → make it module-local (a `let` in the `.mjs`).
- State shared across modules or read by core dispatch → keep the `let` in
  shadow_ui.js, expose via `Object.defineProperty(_ctx, …, {get,set})`.
- Functions/navigation a module needs → `_ctx.fn = fn`.
- **`ctx` is a documented fork surface.** Only ADD properties; never rename or
  remove existing ones. New additions are backward-compatible.

## Per-task verification protocol (do EVERY task)

1. **Pin test** (source-invariant shell test under `tests/shadow/`): write it,
   watch it fail for the right reason, make it pass.
2. **Syntax:** `node --check` on every `.mjs`/`.js` touched. Must print nothing.
3. **Suite:** run `for t in tests/{host,shadow,store,build}/*.sh; do bash "$t";
   done` and confirm the pass count is `prior + (new tests)`; the 21 stale
   failures must not grow. Baseline at plan start: **80 pass / 21 stale-fail.**
4. **Build:** `./scripts/build.sh` (the `cp -u` steps at build.sh:547/659/692
   carry the JS into `build/`). Confirm `md5 -q src/<f>` == `md5 -q build/<f>`.
5. **Deploy:** `./scripts/install.sh local --skip-modules --skip-confirmation`
   (reboots the Move).
6. **Verify on device:** `ssh ableton@move.local "md5sum
   /data/UserData/schwung/<path>"` matches local; `ssh root@move.local` confirm
   `MoveOriginal` holds `/dev/ablspi0.0` (grep `ablspi` in `/proc/<pid>/maps`)
   and shadow SHM segments exist (`ls /dev/shm | grep schwung`).
7. **Human smoke test:** hand the user the exact on-screen flow to exercise
   (each task lists it). Keep moving; don't block unless a later task depends on
   the result.
8. **Commit** one PR-shaped commit. Co-author trailer:
   `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

**Behavior-invariance is the contract.** These are relocations; if a pin test
or the user finds *any* visible/audible difference, it's a bug — revert and
redo, don't "fix forward."

> **Line numbers in this plan are 2026-06-13 snapshots and WILL drift as tasks
> land. Re-grep before each task** (e.g. `grep -n "function handleSelect"`).

---

## Phase A — Promote Help to its own view/module

Removes the Help `else if` branches from MASTER_FX and GLOBAL_SETTINGS handlers
and the `helpReturnView` hack; makes later view extraction tractable.

### Task A1: Add a HELP view + Help module skeleton (no behavior change yet)

**Files:**
- Create: `src/shadow/shadow_ui_help.mjs`
- Modify: `src/shadow/shadow_ui.js` (VIEWS enum ~270–300; ctx wiring ~12642+)
- Test: `tests/shadow/test_help_module_extracted.sh`

**Step 1 — pin test (RED).** Assert `src/shadow/shadow_ui_help.mjs` exists and
exports `enterHelp`, `drawHelp`, `handleHelpJog`, `handleHelpSelect`,
`handleHelpBack`; assert `VIEWS.HELP` exists in shadow_ui.js.

**Step 2** — run it, watch it fail (`No such file`).

**Step 3 — GREEN (skeleton).** Create `shadow_ui_help.mjs` importing `ctx`.
Move the help **state** (`helpContent`, `helpNavStack`, `helpDetailScrollState`)
into the module as module-local `let`s. Move the loader body (~4444–4581) into
`enterHelp(returnView)` in the module; it sets a module-local `helpReturnView`.
Move the help jog/select/back branch bodies (from the MASTER_FX/GLOBAL_SETTINGS
cases) into `handleHelpJog/Select/Back`. Move help drawing into `drawHelp`. Add
`HELP: "help"` to the VIEWS enum. Export all five fns. In shadow_ui.js, expose
via ctx anything the module still needs (`setView`, `announce`,
`handleScrollableTextJog`, scrollable-text helpers, the manual/per-module help
loaders if they stay core).

**Step 4** — `node --check` both files; run the pin test (PASS).

**Step 5 — wire dispatch.** In each of the four switches add a
`case VIEWS.HELP:` that delegates to the matching `handleHelp*`/`drawHelp`. In
MASTER_FX and GLOBAL_SETTINGS handlers, **replace** the inline help branches
with: on "[Help…]" select → `enterHelp(VIEWS.MASTER_FX|GLOBAL_SETTINGS);
setView(VIEWS.HELP)`. `handleHelpBack` pops `helpNavStack`; when empty →
`setView(helpReturnView)`.

**Step 6** — full suite (expect +1), build, deploy, device hash-match + health.

**Step 7 — human smoke test:** Shift+Vol+Menu → Master FX → **[Help…]**:
navigate into a section, scroll a detail page, Back out repeatedly → must land
back on Master FX. Repeat via Shift+Vol+Step2 → Global Settings → Help.

**Step 8 — commit:** `refactor(shadow-ui): promote Help to its own view/module
(step 8 phase A)`.

### Task A2: Delete the now-dead help sub-state from the two view handlers
Remove any `helpReturnView`/help-flag remnants left in shadow_ui.js that the
module now owns. Pin: `rg "helpReturnView" src/shadow/shadow_ui.js` returns
nothing (it lives in the module). Same verification protocol. Commit.

---

## Phase B — Extract the settings-value engine

### Task B1: Re-home the 6 cached-state globals onto ctx (no move yet)

**Files:** Modify `src/shadow/shadow_ui.js` only.
**Test:** `tests/shadow/test_settings_state_on_ctx.sh`.

The engine reads/writes `autoUpdateCheckEnabled`, `previewEnabled`,
`padSelectGlobal`, `textPreviewGlobal`, `filebrowserEnabled`,
`midiIndicatorEnabled`. **Step 1 (RED):** pin that each is exposed via
`Object.defineProperty(_ctx, '<name>', …)`. **Step 3 (GREEN):** add the
defineProperty get/set wiring (the `let`s stay in shadow_ui.js; every existing
in-file reference keeps working unchanged — additive only). Verify, commit:
`refactor(shadow-ui): expose cached settings state via ctx (step 8 phase B1)`.

### Task B2: Move the engine into `shadow_ui_settings_engine.mjs`

**Files:** Create `src/shadow/shadow_ui_settings_engine.mjs`; modify
shadow_ui.js + `shadow_ui_settings.mjs` + `shadow_ui_master_fx.mjs`.
**Test:** `tests/shadow/test_settings_engine_extracted.sh`.

Move `getMasterFxSettingValue`, `adjustMasterFxSetting`,
`getMasterFxSettingsItems`, `GLOBAL_SETTINGS_SECTIONS`, and the
resample/EQ/label tables into the new module; they access the cached state via
`ctx.<name>` (from B1) and call host bindings (globals — fine). Update
shadow_ui.js, settings.mjs, master_fx.mjs to import them directly instead of
through ctx. **Behavior-invariant.** Pin: the three functions/`GLOBAL_SETTINGS_
SECTIONS` no longer defined in shadow_ui.js; defined in the new module.

**Human smoke test:** Global Settings → change a value in several sections
(speaker EQ, screen-reader speed, a boolean toggle) → values persist & display
correctly. Master FX settings list still adjusts. Commit:
`refactor(shadow-ui): extract settings-value engine (step 8 phase B2)`.

---

## Phase C — Extract per-view input handlers into existing draw modules

Now that Help and the settings engine are modular, the view handlers are
liftable, mirroring the proven `handleSlotsJog`/`handleSlotsSelect` pattern.
**One view = one commit**, each: move the inline switch-case body into
`handle<View>Jog/Select/Back` in that view's existing `.mjs`, expose any
remaining shared state/nav via ctx, leave the switch case as a one-line
delegation. Verification protocol every time, with the matching on-screen smoke
test.

- **Task C1: GLOBAL_SETTINGS** → `shadow_ui_settings.mjs` (smallest; ~32-line
  jog, ~60-line select). Smoke: navigate sections, enter a section, edit a
  value, Back out.
- **Task C2: CHAIN_SETTINGS** → `shadow_ui_settings.mjs`. Smoke: slot →
  chain settings list, edit, save/overwrite/delete confirm flows.
- **Task C3: MASTER_FX** → `shadow_ui_master_fx.mjs` (largest; ~177-line
  select). Smoke: component nav, module select, preset save/overwrite/delete,
  per-setting edit.
- **Task C4: TOOLS subsystem** (TOOLS + TOOL_FILE_BROWSER/SET_PICKER/
  ENGINE_SELECT/CONFIRM/RESULT/STEM_REVIEW) → `shadow_ui_tools.mjs`. Larger;
  may split into 2 commits (menu+launch, then the processing sub-views). Smoke:
  open a tool, pick a file/set, run, review result.

After C, every `case VIEWS.X` in all four switches is a one-line delegation to a
`handle*`/`draw*` (core leaf views like FILEPATH_BROWSER/KNOB_EDITOR may already
be, or stay inline if truly core — that's fine).

---

## Phase D — Collapse the four switches into a HANDLERS registry

### Task D1: Introduce the registry with switch-fallback (incremental)

**Files:** Modify shadow_ui.js. **Test:** `tests/shadow/test_handlers_registry.sh`.

Build `const HANDLERS = { [VIEWS.SLOTS]: { draw: drawSlots, jog: handleSlotsJog,
select: handleSlotsSelect, back: handleSlotsBack }, … }`. Rewrite the four
dispatchers to: `const h = HANDLERS[view]; if (h?.jog) { h.jog(delta); } else {
/* existing switch as fallback */ }`. Migrate a FEW already-delegating views
into HANDLERS first, leaving the rest in the fallback switch. Pin: `HANDLERS`
exists and the dispatchers consult it before the switch. Smoke: exercise both a
migrated and a non-migrated view. Commit.

### Tasks D2…Dn: Migrate views into HANDLERS, drain the switches
A few views per commit; each removes their `case` from all four switches and
adds a `HANDLERS[view]` entry. When the last view is migrated, delete the
now-empty fallback switches and the pre-switch guards become the dispatcher
preamble (e.g. `hideOverlay()` in handleJog, `needsRedraw = true` postamble).
Keep the `tick`/`onMidiMessage*`/overtake/co-run fast paths **byte-identical** —
they are NOT part of the registry. Each task: verify + smoke + commit.

---

## Phase E — (Deferred) canvas + wav_position param-editor seam

Only after A–D. These are param-editor *types*, not views: design a small hook
interface (`registerParamEditor(type, {buildMeta, openPreview, tick, draw,
dispatchMidi})`) so the hierarchy editor calls editors generically, then move
canvas (~9314–9651 + 1755/1830) and wav_position (1417–1860 + 7651–7860) into
`shadow_ui_canvas.mjs` / `shadow_ui_wav.mjs`. Keep `resolveCanvasScriptPath` and
the `globalThis.canvas_overlay(s)` hook save/restore byte-identical (module
authors depend on it). This is its own mini-design; re-plan when reached.

---

## Non-goals / guardrails
- **No behavior change anywhere.** Pure relocation + dispatch restructuring.
- Don't touch `globalThis.init/tick/onMidiMessageInternal/onMidiMessageExternal`
  semantics, the overtake lifecycle, or the co-run fast paths.
- Don't rename/remove existing `ctx` properties (fork surface) — add only.
- Don't merge confirm/settings *interaction models* that are deliberately
  different (analytics default-Yes, tool-confirm jog-to-proceed).
- If any phase reveals deeper coupling than scoped here, STOP and re-plan that
  phase rather than forcing it — that's how this plan came to exist.

## Rollback
Each task is one commit on `main` (local, unpushed). Revert the single commit;
redeploy via `install.sh local`. No cross-task state.

## Definition of done
shadow_ui.js materially smaller (target <11k lines), four switches replaced by
one `HANDLERS` registry, Help + settings-engine + per-view handlers in modules,
suite green (stale set unchanged), shadow UI behavior identical on hardware.
