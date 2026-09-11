# Slot Mod Routes — RESUME

**Branch:** `feat/slot-mod-routes` (pushed to `origin`). Parked 2026-09-07.

Three documents, in the order you want them:

| | |
|---|---|
| **this file** | where you are, and the first thing to do |
| `2026-09-07-slot-mod-routes-HANDOFF.md` | the full record: what works, what does not, every trap |
| `2026-09-07-slot-mod-routes.md` | the original 10-task plan, task by task |

---

## Where this is

A slot's two LFOs became **eight mod routes** — each picking LFO / Velocity /
Pressure / CC / Note and aimed at any parameter of any component in the slot.

**It does not work.** On the device the modulation never reaches the
destination; the target parameter does not move. Two blockers, independent:

1. **It does not reach the destination.** Undiagnosed. ← start here
2. **Eight sibling Mod pages is the wrong shape.** Charles wants one Mod page
   listing all eight, divable per row.

All 316 host tests pass and the ARM build is clean. **That is not evidence the
feature works** — see below.

---

## Read this before trusting any test

The on-device harness reported 18/18 on a feature that never reached a plugin.
It read `<key>:effective`, which is **`chain_mod.c`'s own table** — the bus
computing `base + sum(contributions)` and storing it. That read never asks the
plugin anything.

And no ordinary param read *can* show the plugin's value while a key is
modulated: a plain read answers the BASE by design (#276), `:effective` answers
the chain's number. Both are the chain's.

**So `tools/pytest-schwung/tests/device_mod_routes.py` is not to be trusted as
written.** Fix it before using it. A destination-side channel is needed:

- the module's own `synth:state` blob (it serialises live plugin values)
- the chain trace, armed **before** the process starts
  (`touch /data/UserData/schwung/chain_midi_trace_on`, then reboot — arming it
  on a running process produced no log)
- a temporary counter exported as a param
- the sound

---

## Get running (5 minutes)

```sh
cd .claude/worktrees/mod-matrix          # or make a fresh worktree on the branch
git submodule update --init --recursive libs/link   # build.sh hard-fails without it

./scripts/build.sh
./scripts/install.sh local --skip-modules --skip-confirmation
```

Host tests:

```sh
make -C tests/host test
for t in tests/host/*.sh; do bash "$t" >/dev/null || echo "FAIL: $t"; done
```

Device bus (for poking params and injecting MIDI):

```sh
ssh ableton@move.local /data/UserData/schwung/bin/schwung-testd &
ssh -N -L 47777:127.0.0.1:47777 ableton@move.local &
```

Then set up a route and play the kick pad:

```
Shift+Vol+Track 1  →  jog to "Mod 1"  →  Src = Velocity, click Targ to aim it
```

---

## The first hour

**1. Split the problem with one read.** Arm a route, play a note, then read
`synth:<key>:modulated`.

- `0` → the emit is failing. `chain_mod_emit_value` returns -1 when
  `find_param_by_key` cannot resolve the target's metadata. Look there.
- `1` → the bus has the contribution and the write to the plugin is the
  problem. Go to step 2.

**2. Is `mod_tick` running at all?** It is driven from `render_block` and, on
silent frames, from the shim's `mod:tick`. A drum module with nothing playing
is silent, so the idle path matters. A counter incremented in `mod_tick` and
exported as a param answers this in one build.

**3. Is `chain_mod_apply_effective_value` reaching the plugin's `set_param`?**
For an INT/ENUM target it is rate-limited by `MOD_INT_ENUM_MIN_INTERVAL_MS` and
skipped entirely when the value has not moved by `MOD_FLOAT_CHANGE_EPSILON`.

**The host E2E is the lever.** `tests/host/test_mod_route_e2e.c` drives the
ladder and the bus against a fake plugin that *records every `set_param` it
receives*, and it passes. So the host path is correct end to end and the device
contradicts it. Whatever differs between them is the bug — that is the smallest
search space available.

---

## Then

4. **Fix `device_mod_routes.py`** to assert on the destination, not the bus.
5. **The Mod list page** — Charles's design call. Notes on the shape are in the
   handoff under *THE DESIGN CHANGE TO MAKE FIRST*; the `Buses` door in
   `SLOT_GRID_ACTIONS` is the closest existing precedent.
6. **Velocity pinned at 127** on the test-bus path — note number and CC from
   the same messages varied correctly, so it may be Move-side normalisation on
   that synthetic path. A pad played by hand settles it.

---

## Four traps, so you do not pay for them twice

- **`MOD_SRC_LFO` is 0** and that is the entire migration story — an absent
  `src` in an old patch memsets to zero and *is* the LFO it always was. No
  migration pass exists because none is needed.
- **The key stem differs by screen.** Slot routes are `modN:`; Master FX keeps
  `lfoN:`, because the **shim** parses those with a literal `strncmp` that has
  never heard of mod routes. Rename them and every Master FX LFO control writes
  a key nothing reads, silently, with the page still drawing.
- **Injected MIDI must go in on cable 2**, not cable 0. Cable 0 is Move's own
  control surface and never reaches a chain slot. Measured: the identical route
  read `effective=64` on cable 0 and `121` on cable 2.
- **A slew test must prime LOW first.** Note-offs latch nothing by design, so
  velocity survives them — arm a route after a hard note and its first block
  seeds straight onto the target, which is correct and looks identical to a
  slew that does nothing.

---

## Loose ends

- `../schwung-catalog-site/manual.html` §6 is rewritten in the working tree but
  **not committed** — that repo is dirty with an unmerged Boot menu section, so
  it needs hunk-by-hunk staging. It also describes the eight-page shape, so it
  wants revising once the Mod list page lands.
- `docs/CHAIN.md`, `docs/SHADOW_UI.md`, `CLAUDE.md` (two bullets) and
  `help_content.json` are written and describe the **current** eight-page
  shape. Same edit needed when the design changes.
- No PR. Branch only.
- The Move has since moved to another branch — nothing measured on it now
  describes this work.
