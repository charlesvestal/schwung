# Snapshot / Recall gesture — design

Date: 2026-09-01
Status: approved, not implemented

## Problem

While tweaking a rig you want to be able to mark where you were, change
everything, and get back. Today the only way back is a User Preset per
component — a save, a name, and one component at a time.

## The gesture

**Shift + Copy (CC 60)** takes a snapshot. **Shift + Delete (CC 119)**
recalls it.

Both are free: every `shadow_shift_held` branch in `src/schwung_shim.c`
(~7400–7620) was checked, and neither CC 60 nor CC 119 appears in any of
them. Claiming them costs Move's own Shift+Copy and Shift+Delete while the
gesture is armed, which is accepted.

Rejected: **triple-tap jog or volume**. Jog click already carries five
meanings, a timing-based tap cannot be printed in a footer, and a mistimed
triple-tap silently performs a single-tap action instead. A two-button combo
is nameable.

Rejected: **Shift+Undo** — Move maps it to Redo.

## Scope

One snapshot covers **everything**: all 4 chain slots (MIDI FX, synth, audio
FX) and all 8 Master FX positions.

## Storage — set-associated, re-seeded on every set load

`set_state/<uuid>/snapshot/`, holding `slot_0..3.json` and
`master_fx_0..7.json` in the same format as set state.

Sets are per-directory (`activeSlotStateDir`, `shadow_ui.js:20427`). A
global snapshot directory would be the one piece of chain state that does not
travel with the set, and would be wrong the moment you changed sets or booted
into a different one. Living in the set dir also means it is deleted with the
set for free.

**The snapshot is re-seeded from the set's own state on every set load**,
overwriting any stored one. That makes it explicable in one sentence:

> The snapshot is how this set was when you loaded it, or the last time you
> pressed Shift+Copy in this session.

It is never older than the current session. A snapshot that persisted across
loads and reboots would be more powerful, but it is invisible state that a
two-button gesture can trigger — a snapshot you do not remember taking
restoring under your hands is a bad surprise.

It still lives on disk rather than in RAM so that a `shadow_ui` restart
mid-session (overtake exit, set change) does not silently lose it. Because it
is overwritten on every set load, persistence never outlives the explanation.

**Set duplication** (`shadow_ui.js:21010`) does *not* copy the snapshot. The
duplicate is loaded fresh and would be re-seeded immediately anyway.

**Seeding also runs once at `shadow_ui` startup**, after boot restore settles,
if the active set has no snapshot dir. Without this, a device that upgrades and
boots straight into its existing set has no snapshot until the next set change,
and the first Shift+Delete would do nothing. Startup seeding is conditional on
absence — an existing snapshot survives a `shadow_ui` restart, which is the
whole reason it is on disk.

## Capture — reuse `buildSlotPatchJson`

`buildSlotPatchJson` (`shadow_ui.js:8175`) already is the capture function:
it enumerates every position, reads `<prefix>:state` with the correct
tri-state branch, and records module id, bypass flag and preset record. It is
what periodic autosave writes and what `SET_CHANGED` reads back
(`shadow_ui.js:21103`). A second capture path in RAM would be a parallel
implementation of code that has already been debugged the hard way.

Take calls it per slot with `forAutosave: false` — 3 state retries rather than
1, because an explicit gesture has no second chance — plus the Master FX
builder, and writes into the snapshot dir via temp-name-then-rename, so a
failed take leaves the previous snapshot whole.

**The set's own `slot_N.json` is not a usable baseline.** Autosave rewrites it
every ~10 s (`AUTOSAVE_INTERVAL = 300`), so by the time you press Shift+Delete
the set file already contains your tweaks. That is why the snapshot needs its
own directory rather than a read of the set file at recall time.

## Recall — state only, never `load_file`

Parse the snapshot files **in JS**. For each position whose recorded module id
still matches what is loaded, write `setSlotParam(slot, "<prefix>:state",
blob)` and the bypass flag.

Deliberately **not** `load_file`: `SET_CHANGED` uses it because it must restore
module *identity*, and that reinstantiates — cutting reverb tails and resetting
arp phase. A recall is an A/B, so it writes state only and instances keep
running.

A position whose module was swapped since the snapshot is **skipped and
counted**, never silently reloaded.

### The stateless-module case is real

`denis` and `branchage` implement no `state` (`shadow_ui.js:8146`). They
snapshot as `""` and recall as a no-op. Per the tri-state rule the two kinds
of miss are different facts and must be branched on the **raw** value before
any truthiness test: `""` means the module served us and has no state, `null`
means the read did not complete. Both are excluded from the restore set and
both count toward "skipped"; the distinction is logged.

## Architecture

The shim cannot do ~20 param round-trips on the SPI callback, so it only
detects the combo and raises a flag; `shadow_ui.js` — SCHED_OTHER, and running
continuously even with the display hidden — does the work. The gesture
therefore works whether or not the shadow UI is on screen.

### Shim (`src/schwung_shim.c`)

Alongside the existing Shift+Capture / Shift+Sample branches:

- `d1 == CC_COPY && d2 > 0 && shadow_shift_held` → set
  `SHADOW_UI_FLAG_SNAPSHOT_TAKE (0x100)`, zero the slot so Move never sees it.
- `d1 == CC_DELETE && d2 > 0 && shadow_shift_held` → set
  `SHADOW_UI_FLAG_SNAPSHOT_RECALL (0x200)`, same.

Both gated on `shadow_ui_enabled` and suppressed in overtake mode (the
existing `overtake_active` early-out covers them).

`ui_flags` is `uint8_t` with all 8 bits used; it widens to `uint16_t`. It
takes a reserved byte, `sizeof(shadow_control_t)` is unchanged, and both
mappers use `sizeof`. A `_Static_assert` beside the field pins that it can
hold every defined flag — the `write_idx` lesson: a field too narrow to reach
its own range reads as a working guard while being dead code.

### Cost

~13 occupied positions typical at ~2.8 ms per round trip ≈ 36 ms, one-shot on
the tick that services the flag — about two dropped UI frames on a gesture the
user just pressed. The loop yields per tick above a position budget so a fully
populated 20-position rig does not stall the UI for ~56 ms.

## Feedback

A new `OVERLAY_SNAPSHOT` alongside `OVERLAY_SET_PAGE`, same lifecycle: JS sets
the type and a frame timeout, `drawSnapshotToast` renders into the shadow
display, `shadow_set_display_overlay(1, x, y, w, h)` blits it over Move's
native screen, and it clears through the existing "no overlay active" branch.
The shim's `any_overlay` gate already includes the JS-set `display_overlay`, so
no new shim overlay state is needed. ~1 s.

- Take: `Snapshot saved`
- Recall: `Snapshot restored`
- Recall with misses: `Snapshot restored` / `2 skipped` — the count is the
  number of **occupied** positions in the snapshot that were not restored
  (module swapped since, or no `state` to restore). Empty positions are not
  counted; a position empty in both the snapshot and the live chain is not a
  miss. Reasons go to `debug.log`, not the screen.

`announce()` speaks the same string, so the screen reader and the OLED never
disagree. No count on take: a component that cannot be snapshotted surfaces at
recall, which is where it matters.

The toast box is sized from **measured text width**, not a character count —
the font is proportional and clipping is in pixels.

## Testing

- `tests/host/test_snapshot_gesture.sh` — source-invariant pins that CC 60 and
  CC 119 are claimed only behind `shadow_shift_held`, that both zero the slot,
  and that both sit behind the `overtake_active` early-out. Written against a
  mutated copy first, to prove it can fail.
- `tests/host/test_ui_flags_width.c` — compiled unit asserting `ui_flags` can
  hold every defined `SHADOW_UI_FLAG_*` and that `sizeof(shadow_control_t)` is
  unchanged.
- Node unit on the recall matcher — id-guard and tri-state accounting over a
  table of match / swapped / empty-position / `""` / `null`, asserting the
  count the toast reports. That count is the only thing between a partial
  restore and a silent one.
- On hardware: snapshot, sweep a filter, recall, confirm the reverb tail does
  **not** cut; swap a module and confirm it is reported skipped; load a set and
  confirm Shift+Delete reverts to load state with no prior Shift+Copy.

## Out of scope

- More than one snapshot slot.
- Restoring module *identity* (undoing a module swap). That needs `load_file`
  and its reinstantiation cost; if the id-guard skips turn out to bite, it
  belongs on a separate, explicitly heavier gesture.
- Any on-screen indicator that a snapshot exists. The re-seed rule means one
  always does.
