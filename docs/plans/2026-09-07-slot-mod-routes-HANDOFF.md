# Slot Mod Routes — HANDOFF (parked 2026-09-07)

**Branch:** `worktree-mod-matrix`, 15 commits, worktree at
`.claude/worktrees/mod-matrix`. Not pushed, no PR.

**Plan:** `docs/plans/2026-09-07-slot-mod-routes.md` (all 10 tasks executed).

**State: PARKED, and it is NOT finished.** The DSP is solid and verified on
hardware; the UI is not usable yet. Read "What is actually wrong" before
resuming — the remaining work is a redesign, not a bug hunt.

---

## The idea

A slot's two LFOs become **eight mod routes**. A route is a SOURCE aimed at one
parameter of one component in the slot:

```
mod1:src   = lfo | velocity | pressure | cc | note
mod1:target / mod1:target_param   where it goes
mod1:depth / polarity / slew      how much, which way, how smoothly
```

The chain host already carried a source-agnostic modulation bus
(`chain_mod.c`: 32 targets × 8 sources, non-destructive base/effective
overlay). Only the two LFOs fed it. This work generalises the **producer** and
leaves the bus alone.

---

## THE DESIGN CHANGE TO MAKE FIRST

> "we should have a **mod page** that has a list of all 8 mods and you can dive
> into them. 8 pages is crazy." — Charles, on the device

He is right, and this is the reason the feature is parked rather than shipped.
The current build appends **Mod 1 … Mod 8 as eight sibling knob pages** to
every slot's grid, between *Sends* and *Actions*. That is eight jog steps to
cross a slot's settings, on the screen you pass through to reach anything else.

The shape to build instead: **one "Mod" entry that opens a LIST of the eight
routes**, each row summarising itself (`1  Vel → fx2:cutoff  60%`), and diving
into a row gives that route's knob page. One page in the walk, not eight.

Notes for whoever does it:

- The route PAGE itself is fine and does not need redesigning — it is the
  eight-cell layout below, already rendered and inspected. What changes is how
  you REACH it.
- `planPages` emits a level's grids then its menu, and a `menu` on a level
  costs that level a second page (see `CLAUDE.md`). A list-of-routes is closer
  to the **Buses** door (`SLOT_GRID_ACTIONS`, `action: "buses"`) than to a
  level — that is the existing precedent for "a row that opens a list of
  configurable things", and it is already wired for the slot grid.
- The row summary wants `lfo_target_label.mjs` (already returns
  `{short, header, long}`) plus the source word from `MOD_SOURCE_LABELS`.
- Deleting the eight levels means `slotGridHierarchy` stops calling
  `modLevels(MOD_ROUTE_INDICES)` on root. `modParams` / `modKnobKeys` stay as
  they are — they build the page, whatever opens it.
- `tests/host/test_shadow_slot_grid_contract.sh` pins the page ORDER and count
  (`2 + MOD_ROUTE_COUNT` grids); it will need reworking with the shape.
- Master FX is unaffected: two LFOs, two pages, unchanged, and it must stay
  that way (see the key-stem trap below).

---

## What is actually wrong

**1. The design above.** Eight pages is the blocker.

**2. "this isn't working anyway."** Reported on the device after the Targ fix
was deployed; NOT diagnosed. What is known:

- The Targ door not opening the picker WAS a real bug, fixed in `f59a623a`,
  rebuilt and redeployed. Whether Charles retested after that deploy is
  unknown — the report may predate it.
- **Velocity did not vary on the device.** Injecting notes over the test bus,
  the velocity byte arrived pinned at 127 whatever was sent, while note number
  and CC **from the same messages** varied correctly (114 vs 17, 121 vs 6). So
  notes reach the latch and the latch works; the velocity byte is already 127
  on arrival. Earlier in the same session, on the same build, it read 121 vs 7
  correctly — so something in Move's state changed across a reboot.
  **Unresolved.** The next step is a pad played by hand (the real path; test-bus
  injection is a synthetic substitute) or the MIDI trace armed BEFORE the
  process starts (`touch /data/UserData/schwung/chain_midi_trace_on` then
  reboot — arming it after start produced no log).

Everything else on the device passed, and those results are worth trusting:
pressure 121↔6, CC74 121↔6 with CC75 correctly inert, note-track 114↔17, slew
gliding `10→11,13,15,17,19,21→120` against an instant jump at slew 0, the base
holding at 64 throughout, disable restoring it, eight routes summing to 50
(predicted 50.8).

---

## What is built, and how it was verified

| Piece | Where | Verified by |
|---|---|---|
| Source types, 7-bit→bipolar map, slew, MIDI latch | `src/host/mod_src.h` | `test_mod_src.c` — 16 mutants, 15 caught |
| `modN:` / legacy `lfoN:` key parser | `src/host/mod_route_key.h` | `test_mod_route_key.c` — behaviour, cap as a parameter |
| `lfo_process_midi` takes a count | `src/host/lfo_common.h` | `test_lfo_process_midi_count.c` |
| The eight routes, the param ladders | `chain_mod_routes.c` | `test_mod_route_keys.sh` + the E2E |
| `mod_tick` dispatch | `chain_host.c` | `test_mod_tick_dispatch.sh` |
| Persistence (`mod_routes`, legacy `lfos`) | `chain_patch.c` | `test_chain_patch_roundtrip.c`, `test_mod_config_roundtrip.sh` |
| Ladder + bus, driven in order | — | **`test_mod_route_e2e.c`** — 11 mutants, 10 caught |
| The route pages | `shadow_ui_slot_grid.mjs` | contract test + 7 snapshot renders, each looked at |
| Targ opens the picker | `shadow_ui.js` | `test_grid_target_dive.sh` |
| On real hardware | — | `tools/pytest-schwung/tests/device_mod_routes.py`, 18/18 |

`make -C tests/host test` + all `tests/host/*.sh`: **0 failures.** ARM
cross-compile clean. The four pure units also RUN on the Move's own ARM64 with
bit-identical float results.

---

## Traps found the hard way — do not re-learn these

**`MOD_SRC_LFO` is 0, and that is the whole migration story.** Every route in
every patch on disk predates the `src` field, so it parses as absent, memsets to
zero, and IS the LFO it always was. No migration pass, no version stamp. Do not
"tidy" that constant.

**The key STEM differs by screen.** A slot route is `modN:`; a Master FX route
keeps `lfoN:`, because its params are parsed in the SHIM
(`shadow_chain_mgmt.c`) by a literal `strncmp` on `"lfo1:"`/`"lfo2:"` that has
never heard of mod routes. Renaming the master keys leaves every Master FX LFO
control writing a key nothing reads — silently, page still drawing. Caught only
by the snapshot baseline.

**A REAL BUG was found in shipped code**, not introduced here:
`chain_mod_clear_target_entry(restore_base=1)` assigned `effective = base` and
then called `chain_mod_apply_effective_value`, which RECOMPUTES effective from
`base + sum(active contributions)` — still active, because the `memset` comes
after. So the "restore" wrote the MODULATED value and destroyed the evidence.
The single-source path was safe by luck; the clear-all path was not, and it is
reachable from any sub-plugin via `host->mod_clear_source`. Fixed in `2cac6e23`.

**Grep-based pins cannot see inside a regex literal.** The Targ bug
(`/^slot:lfo([12]):target$/` left behind after the rename) survived 316 passing
tests: the drift pin greps for literals, the contract test builds pages without
clicking one, the snapshot renders pages without clicking one, and the device
harness drives params bypassing the UI. `test_grid_target_dive.sh` now
EVALUATES the matcher instead of reading for it.

**Injected MIDI must go in on CABLE 2.** Cable 0 is Move's own control surface,
so an injected cable-0 note never reaches a chain slot. Measured: the identical
route reads `effective=64` (unmoved) on cable 0 and `121` on cable 2. The first
run looked like a dead latch and was a wrong cable.

**A slew test must prime LOW first.** Note-offs latch nothing by design, so
velocity survives them — arm a route after a hard note and its first block
SEEDS straight onto the target. Correct behaviour, indistinguishable from a
slew that does nothing.

**Three of my own assertions passed for the wrong reason** and were only caught
by mutation: `-DLFO_COUNT=8` was vacuous (the header redefines it); the CC-clamp
test passed with the clamp DELETED because `cc[]` is the last struct member and
the OOB read found a zero; and at depth 1.0 the correct bipolar half-range
scaling and a missing halving both clamp to the same answer. **Mutate before
believing a green test on this feature.**

**The page budget is tight and bites twice.** Ungated slew made nine visible
cells; demoting `phase_offset` to a declared non-knob gave the planner a
`"Mod 1 - 2"` overflow holding one cell. A control that should not be on the
grid must not be DECLARED for the grid.

---

## Device state

The Move is running **this branch build, not a release**, with 9w9 in slot 0 and
Mod 1 armed (Velocity → BD Tune). `freeverb` in fx1 was already there.

**Nothing of Charles's was written**: every `slot_state/*/slot_0.json` and the
global `slot_0.json` are still dated Sep 6 — the loaded synth and the route are
live in RAM only and clear on reboot.

To put it back: reinstall a release, or reboot and reinstall from `main`.

---

## Loose ends

- **`../schwung-catalog-site/manual.html` §6 is rewritten but NOT committed.**
  That repo is dirty with an unmerged Boot menu section, so it must be staged
  hunk by hunk, never as a file. If the design changes to a Mod list page, that
  section needs revising before it is committed at all.
- No PR. The branch is local.
- `docs/CHAIN.md`, `docs/SHADOW_UI.md`, `CLAUDE.md` (two bullets) and
  `help_content.json` are all written and describe the CURRENT eight-page shape.
  They will need the same edit as the UI when the list page lands.

---

## If picking this up

1. Read this file, then `docs/plans/2026-09-07-slot-mod-routes.md` for the
   task-by-task detail.
2. Do the **Mod list page** first — it is the reason this is parked.
3. Then settle the velocity question on hardware, with a pad and a hand.
4. `tools/pytest-schwung/tests/device_mod_routes.py` re-runs the whole device
   check; it needs `schwung-testd` started and tunnelled (see its docstring).
