# Master FX as a variable-length chain — design, 2026-08-21

Residual 2.2 Step 4, per `docs/plans/2026-08-20-chain-handoff.md`. Steps 0–3
have landed: the cap is 8, key routing is cap-derived, and the row is
`chain_diagram.mjs` with windowed scroll.

**Decided by Charles:** full variable-length — insert, remove AND move, so
Master FX becomes a list exactly like a slot chain. And **indirect the param
cache first**, so a permutation rotates pointers rather than memmoving ~1 MB on
the SPI callback.

---

## 1. What it is

Master FX today is a fixed array of 8 positions with one gesture: swap a module
in place. "Remove" is picking `None`, which unloads and leaves a **hole**. With
four positions a hole was barely visible; at eight, a run of empty `--` boxes
between two real effects is the common case.

After this, Master FX is a chain: `+` boxes that add where they are drawn,
Shift+Jog to move, and removal that closes the gap — with the same guarantee the
slot chains got:

> Adding, removing or moving something in a chain must not perturb the modules
> already in it.

## 1b. The real requirement: STOP HAVING TWO EDITORS

**Charles:** *"I have had issues where master fx and chain edit end up out of
sync with features. We want this to be as shared as possible."*

This is the governing constraint, not a nice-to-have, and the evidence is
current rather than historical:

- **The knob card, added 2026-08-20, works only in `CHAIN_EDIT`.** Master FX
  knobs still raise the old `Value: 0.62` box. The spec for it said *"Scope is
  CHAIN_EDIT. Master FX keeps today's overlay; residual 2.2 revisits Master
  FX."* That is how the drift happens — one reasonable-sounding scope boundary
  at a time.
- Master FX had **no windowed scroll** until Step 3 of this same residual,
  years after the slot diagram got one.
- Master FX has **no reorder at all**, which is the entire reason Step 4 exists.
- There are **24 `MasterFx`-prefixed functions** in `shadow_ui.js` shadowing
  chain-editor equivalents.

Adding insert/remove/move to Master FX as its own implementation would make it
worse: an eighth parallel copy, and a guarantee of this conversation happening
again.

**So Step 4 converges the two editors first, then adds the feature once.**

### The target abstraction

The two screens differ in exactly two things: **where the components come from**
and **how a param key is addressed**.

| | Slot chain | Master FX |
|---|---|---|
| Param key | `getSlotParam(N, "fx1:cutoff")` | `getSlotParam(0, "master_fx:fx1:cutoff")` |
| Components | `slotChainComponents(N)` | `MASTER_FX_CHAIN_COMPONENTS` |
| Sections | `midiFx`, synth, `fx` | `fx` only |

Everything else — the diagram, the picker, bypass, the hierarchy editor, the
LFO target picker, the knob card, the verbs, the footer grammar — is the same
screen wearing two implementations.

Introduce a **chain target**:

```
{ kind: "slot" | "master",
  slot,                       /* IPC slot index: N, or 0 for master */
  key(componentKey, param),   /* "fx1:cutoff" | "master_fx:fx1:cutoff" */
  components(),               /* bounded by the PUBLISHED COUNT, not the cap */
  hasSynth, hasMidiFx }       /* master has neither */
```

`drawChainEdit` and `drawMasterFx` become one draw parameterised by it;
`VIEWS.MASTER_FX` becomes `CHAIN_EDIT` with the master target. Every feature
added after that lands in both by construction.

### The rule that keeps it converged

Shared code is necessary but not sufficient — a shared function with a
`if (target.kind === "master") return;` in it drifts just as well. So:

> **Any test of chain-editor behaviour runs against BOTH targets.** A feature
> that cannot state what it does on Master FX is not finished.

That is the mechanism. `tests/host/test_chain_gestures.sh` and the knob-card
tests are the first candidates to parameterise.

## 2. Most of this is already built

| Piece | Where | Reusable? |
|---|---|---|
| Shape model (`insertAt`/`removeAt`/`moveBy`, caps, id parsing) | `src/shared/chain_model.mjs` | **Yes**, pure and section-generic |
| Diagram with windowed scroll | `src/shared/chain_diagram.mjs` | **Already adopted** in Step 3 |
| Array permutation | `src/modules/chain/dsp/chain_permute.h` | Yes in principle — **but see §3** |
| Verb emission | `writeChainShape` in `shadow_ui.js` | Pattern reusable, keys differ |
| Picker `Move Left`/`Move Right` | `shadow_ui.js` (~3537) | Pattern reusable |

The genuinely new work is C-side: the permutation itself, the count, and the
cache indirection.

## 3. The structural blocker — decide this first

**There is no include path from `src/host/` to `src/modules/chain/dsp/.`** This
is stated deliberately in `src/host/shadow_fx_key.h`, which declines to bound
itself by the chain caps for exactly this reason, and adds:

> *"A copy of a cap is precisely the bug class that made fx3..fx8 silent in the
> first place."*

So `shadow_chain_mgmt.c` cannot `#include "chain_permute.h"` today, and
**duplicating it is explicitly the wrong answer.** Three options:

1. **Promote the pure headers to a shared C location** both units include —
   `chain_permute.h` and its one dependency `chain_key_index.h`. They are
   already header-only, dependency-free, and designed to be compiled natively
   by `tests/host`; nothing about them is chain-specific. **Recommended.**
2. **Add an include path** from the shim build to `src/modules/chain/dsp/`.
   Cheapest edit, but it makes the shim reach into a dlopen'd module's private
   directory for headers, which is the boundary the codebase currently keeps.
3. **Write a second permutation for Master FX.** Rejected: two implementations
   of "move these parallel arrays and miss nothing" is how a module ends up
   driving another module's parameters.

Note `chain_permute.h` also **refuses elements over `CHAIN_PERM_MAX_ELEM`
(2048)**, which the 64 KB param cache exceeds — §4 removes that obstacle as a
side effect.

## 4. Indirect the param cache

Today each `master_fx_slot_t` embeds `char chain_params_cache[65536]`, and
`shadow_chain_mgmt.c` keeps a *second* 64 KB file-static cache per slot. Moving
a position therefore means moving 128 KB per slot — and the permutation runs on
the SPI callback, where the budget is ~900 µs after the transfer.

Indirect both behind pointers, allocated once per position at init and **never
null** — the `PERM_OWNED` shape from `chain_permute.h`. Then a permutation
rotates pointers.

**This is the exact shape that segfaulted the slot chain**, and the failure is
recorded in `CLAUDE.md`: vacating an owned-buffer position must **rotate** the
buffer displaced off the end of the shift and clear its *contents*, never null
the pointer. `v2_load_midi_fx_slot` parsed a param table through a NULLed
pointer and took the SPI callback down. The mitigation is that this is now a
known, named failure with a test pattern — not that it is unlikely.

## 5. Master FX must publish a count

The slot chain's UI reads `fx_count` back from the DSP. Master FX is a fixed
array and publishes nothing — `makeMfxLfoCtx`'s comment says so deliberately.
Once positions can be removed, the UI has no way to know where the chain ends.

Add `master_fx:fx_count` (get), and have the JS bound its component list by it
rather than by `MASTER_FX_SLOTS`. **Bound loops by the published count, never by
the cap** — that rule is already in `CLAUDE.md` and was learned the expensive
way.

## 6. What the permutation must move

Established by reading the struct and the file:

- `shadow_master_fx_slots[]` — every member is a **value or inline array**
  today. After §4, two members become owned pointers.
- **Three file-static arrays outside the struct** (`shadow_chain_mgmt.c:74-76`):
  `mfx_runtime_chain_params_cache`, `..._cached`, `..._last_fetch_ms`. These
  must permute in lockstep. Miss one and FX 3 serves FX 5's param metadata.
- **One string table keyed by position name**: `shadow_master_fx_lfos[].target`
  (`lfo_state_t.target`, `char[16]`), compared by `strcmp` at four sites. A
  move must re-aim it; a remove must clear the entry that matched.
  Master FX has **no** modulation-target table and **no** knob-mapping table,
  so this is one string to re-aim against the slot chain's three. The one
  genuinely easier thing here.
- Vacating a position must go through `shadow_master_fx_slot_unload`, **never
  `memset`** — the struct holds a `dlopen` handle and an FX instance, so
  zeroing it leaks both. Quieter than the slot chain's crash, and therefore
  worse.

## 7. The existing test technique does not transfer

`tests/host/test_chain_permute.sh` works by deriving the array list from the
**struct definition** and failing when a member is not in the collector. Three
of Master FX's per-position arrays are **file-static and outside any struct**,
so that derivation is structurally blind to them.

Replacement: derive from the *file* — every `[MASTER_FX_SLOTS]` array
declaration in `shadow_chain_mgmt.c` and every `[MASTER_FX_SLOTS]` member in
the header must appear in the collector. Count what was examined and fail if
the derivation finds fewer than the known number, so it cannot pass vacuously
(the pattern `test_chain_reorder_routing.sh` already uses).

Also required, and separate from any ordering assertion:

- **Invariants**: no NULL owned pointer after any operation; the multiset of
  buffer pointers is identical before and after; no `dlopen` handle lost.
- **Behavioural**: Master FX has *zero* behavioural coverage today — all nine
  existing tests are source greps. `test_master_fx_slot_routing.c` is the first
  and the model to follow.

## 8. UI

- Components come from a model bounded by the published count, not the cap.
- Two `+` boxes are wrong here — Master FX has one section. **One `+`**,
  appended, matching the audio-FX end of a slot chain.
- Shift+Jog to move, mirroring the slot editor, including the footer changing
  to `JOG MOVE` under Shift so the gesture is discoverable.
- Picker gains `Move Left` / `Move Right` and `None` becomes a real remove.
- `selectedMasterFxComponent` is an index into a list that now changes length:
  re-anchor by **id** across an edit, never by index. `indexOfId` in
  `chain_model.mjs` exists for exactly this and its comment explains why.

## 8b. Order of work

Convergence first, with **no behaviour change**, then the feature once. This is
the same shape as Steps 1 and 2 — harden at the current cap, then flip — which
worked twice and made each raise a one-line diff.

- **4a-1. Pixel baseline for both screens, before anything moves.** DONE
  (`41af213b`) — 71 cases, `tests/host/test_chain_editor_snapshot.sh`.
- **4a-2. The target abstraction, with the baseline UNCHANGED.** Route both
  screens through shared code. Both must still render byte-identically:
  **any pixel that moves in 4a-2 is a bug.** Deletes most of the 24 `MasterFx`
  functions.
- **4a-3. Unify the chrome, deliberately.** 4a-1 found the two screens do not
  merely differ in plumbing: Master FX draws **no footer at all** and never
  reads `isShiftHeld`, wears the older header instead of the movy band, sits
  6px lower and is centred rather than offset (it has no slot-indicator
  column). Converging those is the *point*, but it is a visible change, so it
  is its own step with its own baseline regeneration — a reviewed refresh, per
  this repo's fixture convention.

  Splitting 4a-2 from 4a-3 is what keeps "no behaviour change" a checkable
  claim. Bundled, every pixel diff would be ambiguous between an intended
  unification and a refactor mistake.
- **4b. The knob card lands on Master FX** — free once 4a is done, and it
  repays tonight's drift.
- **4c. Indirect the param cache** (§4). Still no user-visible change.
- **4d. Promote the permutation header** (§3), add `master_fx:fx_count` (§5),
  write the C permutation (§6).
- **4e. Verbs and gestures, added ONCE in shared code** — insert, remove, move
  — so both targets get them in the same commit.
- **4f. Tests, parameterised over both targets** (§7, §1b).

4a is the load-bearing step. If it is skipped, everything after it is an eighth
parallel implementation.

## 9. Risks

1. **The owned-buffer permutation is the known-dangerous part** (§4). It
   segfaulted once, on the SPI callback, in code that had 28/28 mutations
   killed — because the fixture used values where production used owned
   pointers. The fixture must represent the same *kind* of data.
2. **The permutation runs on the SPI callback.** That is pre-existing (module
   loading already `dlopen`s there — residual 2.6) and a permutation is
   strictly cheaper than the reload it replaces, but §4 exists so it stays
   cheap.
3. **`getTargetComponents` costs 8 IPC reads on a cache miss** and a
   permutation invalidates that cache on every gesture, turning a rare miss
   into one per move.
4. **Set persistence**: `master_fx_N.json` has no count and no version field.
   A chain that had a hole and now closes it changes which file holds which
   module. Migration needs stating.
5. **4a is a refactor of a 17,900-line file** that is the shadow UI, cannot be
   run off-device, and has no behavioural test harness beyond the lifted-block
   trick in `test_chain_edit_read_budget.sh`. It must be done in small,
   individually-verifiable moves with the screen rendered headlessly and
   compared before/after — the same way `render_page_movy`'s geometry change
   was proven inert against a per-page snapshot. **A snapshot of both screens,
   captured before 4a starts, is the only thing that can prove "no behaviour
   change" here**, and it must be captured first for the same reason the movy
   baseline had to be.
