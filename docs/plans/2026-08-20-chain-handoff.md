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

---

#### AMENDED 2026-08-20 after reconnaissance — three things above are wrong

**"It still reloads on renumber" is not true, because there is no renumber.**
Master FX has **no insert, no remove and no move**, at any level: the whole
gesture set on `VIEWS.MASTER_FX` is jog, click (edit or pick), Shift+click
(swap), Mute+click (bypass), plus preset actions. "Remove" is expressed as
picking `None`, which unloads in place and leaves a hole rather than closing the
gap. So job 2 is **inventing a feature** — a gesture, a `master_fx:move` /
`master_fx:remove` verb pair, and the permutation — not porting
`chain_reorder.c`. Scope accordingly.

**The screen does not fit, and this is probably the largest piece of work.**
`src/shadow/shadow_ui_master_fx.mjs:117` is `TOTAL_W = 5 * BOX_W + 4 * GAP` =
118px of a 128px display: five 22px boxes that exactly fill the screen. Nine
boxes is 214px, drawn off-screen with no clipping and no error, taking the
bypass `B` and the LFO `~` markers with them. Master FX has none of the
windowed-scroll machinery `chain_diagram.mjs` got. **This is a design decision
and it blocks the visual half of the raise.**

**The cap raise costs +512 KB of static BSS.** Each `master_fx_slot_t` embeds a
64 KB `chain_params_cache` (`shadow_chain_mgmt.h:47`) and there is a *second*
parallel 64 KB file-static cache per slot (`shadow_chain_mgmt.c:74`). That is
128 KB per slot, so 4 → 8 adds half a megabyte to the shim image on a Move,
with no warning. Decide deliberately: accept it, shrink the cache, or indirect
it behind a pointer. **Indirecting changes how the permutation must be written**
(it becomes a genuine owned buffer), so decide before writing it.

**Two sites the list above omits, both the "never referenced the constant"
shape:**
- `shadow_chain_mgmt.c:2163` — `lfo->target[2] >= '1' && lfo->target[2] <= '4'`.
  A **character** range, so grepping `<= 4` misses it. An LFO aimed at `fx5`
  silently stops modulating. Also caps the design at 9 slots without saying so.
- `shadow_ui.js:16524`/`:16539` — the `master_fx_N.json` copy/seed loops are
  bounded by `SHADOW_UI_SLOTS`, conflating the four instrument slots with the
  four Master FX slots. After the raise, duplicating a set copies
  `master_fx_0..3` and never touches `4..7`.

**The two ladders are not equivalent.** `shadow_direct_set_param` (~1729) drops
an unmatched key. The shadow_param handler (~2613) ends
`else { mfx_slot = 0; param_key = fx_key; }`, so `fx5:foo` is **routed to slot
0 with a garbage key** — silent corruption of a different running module, not a
silent no-op. Highest-severity site in the area.

**`master_fx_slot_t` does NOT have the owned-buffer problem** that segfaulted
the per-slot permutation: every member is a value or an inline array. But
`chain_permute.h` still cannot be reused off the shelf —
`MFX_RUNTIME_CHAIN_PARAMS_MAX` (65536) exceeds `CHAIN_PERM_MAX_ELEM` (2048), so
`chain_perm_arrays_ok` refuses it. And three per-position arrays are
**file-static, outside the struct**, so `test_chain_permute.sh`'s
collector-versus-struct trick is structurally blind to them. Vacating a position
must go through `shadow_master_fx_slot_unload`, not `memset` — the latter leaks
a `dlopen` handle and an FX instance per operation, which is quieter than a
crash and therefore worse.

**There is no behavioural test coverage on Master FX at all.** All nine
Master-FX-related tests are source-text greps. Every one of them would pass with
the cap at 8 and every literal 4 left in place — including the misroute above,
while it corrupted FX 1.

**Step 0 decisions — SETTLED 2026-08-20 by Charles.**

- **Layout: windowed scroll, like the slots.** And it should not be a new
  implementation. `scrollWindow(total, selected, capacity)` in
  `chain_model.mjs:203` is fully generic — it takes counts, not chains — and
  `drawChainDiagram` takes a components array. So Master FX builds
  `fx1..fx8 + settings` as components and adopts `chain_diagram.mjs` wholesale,
  inheriting the scroll, the box geometry, the bypass `B` and the LFO markers,
  and dropping `shadow_ui_master_fx.mjs`'s own `TOTAL_W` row. The two screens
  also stop looking like different products, which is the direction the movy
  chrome work already took everything else.
- **Memory: accept the +512 KB. It is not a consideration.** Measured on the
  device: 1849 MB total, 1232 MB available. Half a megabyte is 0.04% of
  headroom. It is BSS, so runtime RAM rather than image size on a root FS that
  *is* full — the two are easy to conflate here and only the second one is ever
  tight. Do not spend effort shrinking or indirecting it for size.
  **It comes back at Step 4 for a different reason entirely:** with the
  cache embedded, permuting a position means moving 128 KB structs, and the
  permutation runs on the SPI callback. Indirecting it there is a performance
  fix that happens to also fix the size — and it is what would make
  `chain_permute.h` usable (`CHAIN_PERM_MAX_ELEM` is 2048). Decide it as part of
  Step 4, not before.

**Revised order** (the handoff's "best done together" is wrong — bundling makes
a regression unbisectable):
- **Step 1**: harden the C string routing **at the current cap of 4**, with a
  cap-derived test. Zero behaviour change.
- **Step 2**: route the ~21 JS literals through one constant, **still at 4**.
  Zero behaviour change.
- **Step 3**: flip 4 → 8. With 1 and 2 done this is the one-liner the handoff
  imagined.
- **Step 4**: only then the new gestures, verbs and permutation.

Steps 1 and 2 need no decisions and are in progress. Step 3 is blocked on
Step 0.

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
