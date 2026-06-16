# Move FX

Move FX gives each **Move track** its own **insert-FX mini-bus** — a small
audio-effects chain that the track's Move-side signal runs through before it
hits the master mix. Four of them (one per Move track / channel) sit alongside
Master FX in a generic **FX-bus picker**. It's the classic *channel insert*
model: an effect placed directly in one track's path, as opposed to a shared
send/return.

---

## What it does (plain terms)

- **Four Move FX buses**, one per Move track. Each is a small FX chain of up to
  **2 insert effects** in series.
- **Per-bus volume.** Each bus has a **Volume** that scales its output into the
  master mix.
- **Routed vs. peeled.** A Move track normally **rides its Schwung synth slot**
  (`Move>SchwFX` On) — its audio shares that slot's chain. Turning `Move>SchwFX`
  **Off** *peels* the track onto its own Move FX bus. The picker lets you open
  any of the 4 Move FX buses directly; opening one whose track is still routed
  prompts **"Route to this chain instead?"** to peel it for you.
- **Shared presets.** You can save/recall a Move FX chain (its 2 effects) as a
  preset. The preset list is **shared across all 4 Move FX buses** — a preset
  saved from Move 1 can be loaded onto Move 3 and vice-versa. (Presets capture
  the effects only, not the per-bus volume.)
- **Per-set persistence.** Move FX chains and per-bus volume save and restore
  with the Move Set.

### Using it

1. Open the **FX-bus picker** and choose **Master FX** or one of **Move 1–4
   FX**. (Master FX behaves exactly as before — it's now just one of the buses
   the picker can open.)
2. If the Move FX you pick belongs to a track still routed to its Schwung slot,
   confirm **Yes** at the *"Route to this chain instead?"* prompt to peel it
   (this sets `Move>SchwFX` Off for that track). Already-peeled buses open
   directly.
3. In the bus editor, load effects into the 2 slots, tweak params, and set the
   **Volume**. Move FX buses have **no LFOs**.
4. **Presets:** in the bus editor's **settings** menu use **[Save Preset] /
   [Save As] / [Delete]**; to **load**, scroll the chain editor all the way
   **left** to the preset column and click to open the **Move FX Presets**
   picker. Because the store is shared, a preset saved on any bus is loadable
   on any other.

Each Move FX bus also taps the global **Send A/B** buses: set its **Send A** /
**Send B** levels in the bus editor's **settings** menu to feed the shared send
returns post-insert.

---

## How it works (technical)

### Topology

```
Move track 0 (peeled: move_to_slot==0) ─► [Move FX 1: fx1..2] ─► ×volume[0] ─┐
Move track 1 (peeled)                  ─► [Move FX 2: fx1..2] ─► ×volume[1] ─┤
Move track 2 (peeled)                  ─► [Move FX 3: fx1..2] ─► ×volume[2] ─┼─► ME bus ─► Master FX ─► out
Move track 3 (peeled)                  ─► [Move FX 4: fx1..2] ─► ×volume[3] ─┘
(routed track: move_to_slot!=0 → rides its Schwung synth slot's chain instead)
```

Buses are **4** (`MOVE_FX_SLOTS`, one per Move track), each with **2** insert FX
slots (`MOVE_FX_BLOCKS`). Each Move FX bus is hosted exactly like Master FX — an
array of `master_fx_slot_t`
(`shadow_move_fx_slots[MOVE_FX_SLOTS][MOVE_FX_BLOCKS]`) — so all the Master-FX
module hosting, presets, and bypass machinery is reused rather than duplicated.
Per-bus strip state is a small `move_fx_strip_t { float volume; }` array
(`shadow_move_fx_strip[MOVE_FX_SLOTS]`).

### Mix path (`src/schwung_shim.c`)

Per audio block, for each Move track whose synth slot is **peeled**
(`move_to_slot == 0`) and that has a live Move track input:

1. The track's Move audio is run through its Move FX bus's insert slots
   (`shadow_move_fx_slots[s][b]`, in series, honoring per-slot bypass), idle-
   gated so an empty/silent strip costs nothing.
2. The processed result is scaled by `shadow_move_fx_strip[s].volume` and summed
   into the **master mailbox** and the **ME bus** (pre–Master FX), so Move FX
   output is colored by Master FX like everything else.

Move FX strips are kept **independent of the synth slot's mute/solo** — a peeled
Move track is its own voice.

### Parameters

| Key | Where | Meaning |
| --- | --- | --- |
| `move_fx:<1-4>:fx<1-2>:<param>` | host | param on a Move-bus FX slot (`module`, `bypassed`, plugin params, `state`, `chain_params`, `ui_hierarchy`) |
| `move_fx:<1-4>:volume` | host | per-bus output volume 0–4 (default 1.0) |
| `slot:move_to_slot` | per chain slot | 1 = Move track rides its Schwung synth slot; 0 = peeled to its Move FX bus |
| `save_move_preset` / `update_move_preset` / `delete_move_preset` | DSP | shared Move-preset writes |
| `move_preset_count` / `move_preset_name_<i>` / `move_preset_json_<i>` | DSP | shared Move-preset reads |

`move_fx:` SET/GET is handled in the host param paths
(`shadow_direct_set_param` and `shadow_inprocess_handle_param_request` in
`src/host/shadow_chain_mgmt.c`); the shared Move preset SET/GET keys live in the
chain DSP (`src/modules/chain/dsp/chain_host.c`), mirroring the Master preset
store.

### UI — one editor, many buses

The former Master-FX editor is **genericized** into a single
**bus-descriptor-driven** editor (`src/shadow/shadow_ui_master_fx.mjs`). A
descriptor (`FX_BUS.master` / `moveFx1`…`moveFx4`) carries the bus's `id`,
`paramPrefix` (`master_fx:` vs `move_fx:1:` …), `slotCount`, `hasLfo`, component
list, settings items, and preset config. The Move buses are built
**programmatically** (`for mvSlot in 1..MOVE_FX_SLOTS_JS`), so raising
`MOVE_FX_BLOCKS_JS` needs no further UI edits — the generic editor adapts.

The **FX-bus picker** (`VIEWS.FX_BUS_PICKER` / `enterFxBusPicker` in
`src/shadow/shadow_ui.js`) shows **Master FX + all 4 Move FX rows** and selects
the active bus before entry; only one editor is open at a time. Opening a Move
row whose track is routed raises an in-picker **route-to-chain confirm** overlay
(`pendingMoveFxRouteConfirm` / `moveFxRouted`, refreshed in the render path) —
**Yes** writes `slot:move_to_slot = 0` (a single, co-run-safe write) and opens
the bus; **No** dismisses.

- Move buses set `hasLfo:false`, so the LFO settings items are filtered out of
  their settings menu; Master (`hasLfo:true`) keeps them.
- The shared **Move FX preset** actions ([Save Preset]/[Save As]/[Delete]) are
  appended to the Move bus settings; loading is via the chain editor's `-1`
  preset column (the same generic flow Master uses). Buses with a preset store
  (`presetCountKey` set) reach that column.

### Persistence

- **Per-set** (mirrors Master-FX per-set files): each Move slot's chain is
  written to `move_fx_<slot>_<block>.json` in the set's state dir, and per-bus
  volume to `move_fx_meta.json` (`strips: [{volume}, …]`). On set change/init
  all 4 strips are re-applied, defaulting to unity volume when a set has no
  meta, so the global `shadow_move_fx_strip[]` can't inherit the previous set's
  levels. The per-track `slot:move_to_slot` routing saves with the chain config
  (`shadow_state.c` writes `slot_move_to_slot`).
- **Shared presets** live in `/data/UserData/schwung/presets_move/` — one flat
  list, written/read by the DSP, wrapping each preset's 2 FX slots under a
  `move_fx` root (`{ name, version, move_fx: { fx1, fx2 } }`).

### Shared FX-bus picker — same foundation as the Send FX PR

The **FX-bus picker / generic multi-bus editor** here is the *same* foundation
introduced in the Send FX PR (`#115`). It is bundled in **both** PRs so that
either can be merged independently — neither depends on the other. The picker /
generic-editor hunks (and the `json_get_section_bounds` fix) are **identical**
between the two PRs, so if both land they merge cleanly with no conflict; the
picker simply gains the Send buses or the Move buses depending on which PR is
applied (or both).
