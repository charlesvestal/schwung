# Slot Mod Routes — HANDOFF (parked 2026-09-07)

**Branch:** `worktree-mod-matrix`, 17 commits, worktree at
`.claude/worktrees/mod-matrix`. Not pushed, no PR.

**Plan:** `docs/plans/2026-09-07-slot-mod-routes.md` (all 10 tasks executed).

## STATE: PARKED, AND IT DOES NOT WORK

**On the device, the modulation never reaches the destination.** Charles,
playing it: *"it doesn't work, it never reached the destination."* The target
parameter does not move. This is the headline and it is not a UI nit.

An earlier draft of this file said "the DSP is solid and verified on hardware".
**That was wrong, and how it was wrong is the most useful thing here** — see
*The verification was hollow* below. In short: the on-device checks read
`<key>:effective`, which is the modulation bus's OWN bookkeeping. It is
computed by `chain_mod.c` and returned from its table. **Reading it never asks
the plugin anything.** So an 18/18 pass proved the chain calculated a number,
not that any module received one — which is precisely the failure being
reported.

There are therefore TWO blockers, and they are independent:

1. **It does not reach the destination.** Undiagnosed. Start here.
2. **Eight sibling Mod pages is the wrong shape** — Charles wants one Mod page
   listing all eight, divable. A redesign, not a bug.

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

### 1. The modulation does not reach the destination

Reported from the device after the Targ fix was built and deployed. The route
can be configured, the chain reports it active, and the target parameter does
not move.

**Not diagnosed.** What is known, and what is only assumed:

KNOWN:

- The **host** E2E test (`tests/host/test_mod_route_e2e.c`) drives the ladder
  and the bus together against a FAKE PLUGIN that records every `set_param` it
  receives, and asserts the value arrives. That passes. So on the host, the
  path from a configured route to a plugin write is correct, including the
  range scaling and the summing.
- The **device** checks never verified that half at all (below).
- Earlier in the session, injected MIDI moved `:effective` (121 vs 7); later in
  the same session on a rebuilt install it did not. That inconsistency is
  itself unexplained and may be the same fault.

NOT KNOWN — the questions to answer first:

- Does `mod_tick` run at all on the device for this slot? It is driven from
  `render_block` and, on silent frames, from the shim's `mod:tick`. If neither
  fires, nothing is emitted and every param read still looks sane.
- Does `chain_mod_emit_value` find the target's metadata? `find_param_by_key`
  failing makes the emit return -1 and no contribution appears — but
  `:modulated` would then read 0, so check that FIRST, it is one read.
- Does `chain_mod_apply_effective_value` actually reach the plugin's
  `set_param`? For an INT/ENUM target it is additionally rate-limited by
  `MOD_INT_ENUM_MIN_INTERVAL_MS` and skipped when the value has not changed by
  `MOD_FLOAT_CHANGE_EPSILON`.

**How to answer them, since no param read can:** arm the chain MIDI trace
BEFORE the process starts (`touch
/data/UserData/schwung/chain_midi_trace_on` then reboot — arming it on a
running process produced no log), or add a temporary counter to `mod_tick` and
`chain_mod_apply_effective_value` and read it back as a param. The module's own
`synth:state` blob is the one existing channel that reflects PLUGIN-side
values rather than the chain's table.

### 2. The page shape

See "THE DESIGN CHANGE TO MAKE FIRST" above. Independent of (1) — worth doing
whichever is tackled first, but (1) is what makes the feature real.

### 3. Velocity, on the test-bus path

Injecting notes over the test bus, the velocity byte arrived pinned at 127
whatever was sent, while note number and CC **from the same messages** varied
correctly. May be a Move-side normalisation on that synthetic path, may be part
of (1). Not chased. A pad played by hand is the real path and settles it.

---

## The verification was hollow, and this is the lesson

The device harness read `<key>:effective` and watched it change. That number
comes from `chain_mod.c`'s own `mod_target_state_t` table — the bus computing
`base + sum(contributions)` and storing it. **The read is served from that
table and never touches the plugin.**

So the device suite could report 18/18 while no module ever received a value.
It measured the bus doing arithmetic.

Worse, and worth understanding before designing a replacement: **no ordinary
param read can show what the plugin holds while a key is modulated.** That is
by design (#276) — a plain read of a modulated key answers the BASE so that
mod-unaware UIs do not show a jittering knob, and `:effective` answers the
chain's computed value. Both are the chain's numbers. The plugin is not asked
by either.

Any future device verification of this feature must therefore confirm the
DESTINATION by a channel that is not the param bus:

- the module's own `synth:state` blob (it serialises live plugin values),
- the chain MIDI/param trace (armed before process start),
- a temporary counter exported as a param,
- or the audible result.

This is the same class as the CC-clamp assertion that passed with the clamp
deleted, and the bipolar-halving assertion that passed with the halving
removed: **a probe that measures the wrong thing reports green.** Three times
in one feature.

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
| On real hardware | — | `device_mod_routes.py` — 18/18, **but see above: it reads the bus's own table, not the plugin. Do not trust this row.** |

`make -C tests/host test` + all `tests/host/*.sh`: **0 failures.** ARM
cross-compile clean. The four pure units also RUN on the Move's own ARM64 with
bit-identical float results.

**Read that table as "the pieces are individually sound", not as "the feature
works".** The host E2E is the strongest evidence, because it asserts against a
plugin that records what it was told — and the device contradicts it, which is
exactly where the bug must be.

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

**The Move has since been moved to a different branch — it is NOT running this
build.** Anything measured on it after that point says nothing about this work,
and any probe run against it must reinstall from here first:

```sh
./scripts/build.sh
./scripts/install.sh local --skip-modules --skip-confirmation
```

While this branch WAS installed, 9w9 was loaded into slot 0 with Mod 1 armed
(Velocity → BD Tune); `freeverb` in fx1 was already Charles's. **Nothing of his
was written** — every `slot_state/*/slot_0.json` and the global `slot_0.json`
stayed dated Sep 6, so the loaded synth and the route were RAM-only and cleared
on reboot.

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

1. Read this file first, especially *The verification was hollow*. Then
   `docs/plans/2026-09-07-slot-mod-routes.md` for the task-by-task detail.

2. **Find out why it does not reach the destination.** Reinstall this branch,
   then answer the three questions in "What is actually wrong" §1 in order —
   `:modulated` first, because it is one read and it splits the problem in
   half. Do NOT use `:effective` as evidence of anything reaching a plugin.

3. **Fix `device_mod_routes.py` before trusting it again.** As written it
   asserts against the bus's own table and will happily report 18/18 on a
   feature that does nothing. It needs a destination-side channel — the
   `synth:state` blob is the cheapest one that already exists.

4. Then the **Mod list page** — Charles's design call, and the reason this is
   parked rather than merged.

5. Only then the velocity-pinned-at-127 question, with a pad and a hand.

A note on sequencing, learned here: every step of this feature was green before
any of it was played. The host tests are good and worth keeping; they were also
never going to find this. Get one end-to-end audible result early next time,
and treat the wall of green as necessary rather than sufficient.
