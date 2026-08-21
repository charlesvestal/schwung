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
