# Chain work — handoff, 2026-08-20

Branch `feat/variable-length-chain`, ~107 commits off `main`, all deployed and
exercised on hardware. Everything below is what is LEFT.

---

## 1. Where things stand

The chain became a variable-length list (8 MIDI FX + 8 audio FX, synth anchored
in the middle), and then shape edits became a **permutation** rather than a
reload. Read `docs/superpowers/specs/2026-08-20-variable-length-chain-design.md`
for the original design; the permutation supersedes its persistence section.

**Verified on device by Charles:** chains longer than two; 8-FX teardown with no
click; mid-chain removal; reorder and remove preserving running audio; MIDI FX
chaining serially (arp 1 feeds arp 2); the MIDI `+` inserting at the front; the
insert crash fixed; footer BACK pinned right; Save / Save As / Delete from the
knob grid; diagram labels; the synth band.

**The contract that now governs this area** — do not regress it:

> Adding, removing or moving something in a chain must not perturb the modules
> already in it. *"we can't change phase of a running module by just adding a new
> one, that doesnt make sense."*

`chain_reorder.c` + `chain_permute.h` permute the per-position arrays; the
instance simply *is* at its new index. `writeChainOrder` and its three carries
(opaque state, modulation base, LFO target) were **deleted** — they existed only
to compensate for a teardown that no longer happens. The picker *swap* clearing
logic stays, because a swap really does destroy and create a module.

Traps that were nearly missed and will be again — all documented in `CLAUDE.md`:
23 per-position fields; **three** string tables key positions by name
(`mod_target_state_t.target`, `lfo_state_t.target`, **`knob_mapping_t.target`**);
and owned pointer buffers must ROTATE, never be zeroed (that one shipped and
segfaulted on device).

---

## 2. Residuals, in the order I would do them

### 2.1 Chain editor knob overlay — show more than the value

**Charles's words:** *"fix the overlay from the chain editor, so those knobs have
a better view: either show the actual control (or row) or the whole UI panel, or
something, so it's not just the value - we have so much more we can show."*

Turning a knob in the chain editor currently gives back a bare value. The screen
is 128x64 and the renderers to do better already exist — `render_page_movy.mjs`
draws labelled cells, dials, bars and enum squares, and `viz_draw.mjs` draws the
graphic groups. The chain editor uses none of it for knob feedback.

Options worth sketching before building:
- the single **row** for the parameter being turned (label + value + control),
- the **control itself**, large, for the duration of the turn,
- the module's whole **panel** (its knob page) while any knob is held.

Where to look: `buildKnobContextForKnob` / `cachedKnobContexts` in
`shadow_ui.js` already resolve which parameter each of the 8 knobs drives and
its metadata; `drawChainEdit` owns the screen. The knob-grid page renderer is
the obvious thing to borrow rather than reimplement.

**Watch the frame budget.** An IPC read is ~2.8 ms — more than a whole page
render (1.68 ms) — so build the overlay from the knob context that is already
cached, not from fresh reads per frame. See `docs/tracing.md` and the
`param_tally_on` diagnostic.

### 2.2 Master FX: 8 slots, and the permutation

Master FX still has its own 4-slot chain in the shim and **does not** go through
`chain_reorder.c` — it still reloads on renumber, which is the bug class fixed
everywhere else.

Two jobs, best done together since both touch the same arrays:
1. `MASTER_FX_SLOTS` 4 → 8 (`src/host/shadow_chain_mgmt.h:21`).
2. Give it the permutation, so its reorder/remove stop tearing down running FX.

**Expect the cap raise to break siblings.** It did seven times on the chain side
(see `CLAUDE.md` and the memory note). Known Master FX sites that enumerate by
hand: the `fx1:`/`fx2:`/`fx3:`/`fx4:` ladders in `shadow_chain_mgmt.c` (~1729 in
`shadow_direct_set_param`, ~2613 in the shadow_param handler), `MASTER_FX_OPTIONS`
in `shadow_ui.js`, and `makeMfxLfoCtx`'s `getTargetComponents`, which walks
`0..4` by hand. **Grep for the literal bounds (`<= 4`, `"fx4"`), not for the
constant** — the sites that break are the ones that never referenced it.
Also check `master_fx_N.json` persistence and whether anything assumes four
files.

### 2.3 Four hardware checks not re-confirmed after the permutation

These passed BEFORE the permutation deleted the carry machinery. The mechanism
underneath them changed completely, so the earlier passes do not carry over:

- a modulated param keeping its base value across a move
- deleting a modulated FX clearing the LFO target
- a picker **swap** not letting the new module inherit modulation
- a knob mapped to FX 4+ persisting across a patch save/reload

### 2.4 Release checklist

`CLAUDE.md` is current. Still outstanding: `../schwung-catalog-site/manual.html`
(user-visible: 8 FX and 8 MIDI FX, Shift+jog reorder, picker Move Left/Right,
the new footer grammar, MIDI FX chaining), `src/shared/help_content.json`, and a
version bump in `src/host/version.txt` + `module-catalog.json`.

### 2.5 The PR split — a decision, not a task

107 commits spanning three features: param-pages/footer, the variable-length
chain, and the permutation redesign. The natural seam is `427ae5cd` (where the
DSP work starts). Splitting costs a re-test at the seam; not splitting costs
whoever bisects this later. Charles has not decided.

### 2.6 Parked, needs its own piece of work

**Module loading calls `dlopen()`, `fopen()` and `malloc()` on the SCHED_FIFO 90
SPI callback.** Confirmed while proving the permutation is race-free:
`shadow_inprocess_handle_param_request` runs inside `shim_pre_transfer`. That is
exactly what `docs/REALTIME_SAFETY.md` forbids. Pre-existing and NOT made worse
(a permutation is strictly cheaper than the reload it replaced), but it is real,
and it is the likely cause of any load-time audio hiccup.

---

## 3. Working notes for whoever picks this up

**Environment**
- `rg` is NOT installed. **32 failures in `tests/host/*.sh` are the baseline on
  every branch** — confirm the count is unchanged rather than chasing them. Note
  some print `rg is required to run this test` and others `rg: command not
  found`, so filter on both if you classify them.
- `grep` is a shell function that silently swallows output. Use `command grep`;
  an empty result is not proof of absence.
- Test `.sh` files use single-quoted `node -e '...'` blocks — **no apostrophes or
  single quotes anywhere inside, including comments**. Use backticks.
- `.serena/project.yml` is dirty at session start. Leave it; never commit it.
- Deploy is `./scripts/build.sh && ./scripts/install.sh local --skip-modules
  --skip-confirmation`. Verify with `md5sum` against the local build — timestamps
  lie, because incremental builds preserve mtimes.

**Testing, and this is the load-bearing part**

Three bugs reached hardware today with a green suite and killed mutation
matrices. Each was correct along the axis being tested:

- reorder preserved ORDER (tested) and reset the module's parameters (not tested)
- `fx_count` was honest (tested) and `fx3_module` was unserved (not tested)
- the permutation moved the right elements (tested, 28/28 mutations killed) and
  NULLed an owned pointer (not tested — the fixture used values, not owned
  buffers, so no mutation could express the failure)

So: after proving behaviour, ask separately what INVARIANTS must survive, and
assert those in a form that does not care about ordering. Check the fixture
represents the same KIND of data as production. **Re-run the whole matrix after
a fix, not just the new cases** — one mutation went from killed to survived
because a new guard masked an older check.

An unserved param key reads back as `""`, not an error. That has now caused two
separate silent bugs; suspect it whenever something "does nothing".
