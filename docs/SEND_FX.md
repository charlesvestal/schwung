# Send FX

Send FX adds two **post-fader send buses** (Send A and Send B) to Schwung,
each with its own effects chain, alongside a generic **FX-bus picker** that
also hosts the existing Master FX. It's the classic mixer *aux/return* model:
every track can feed a portion of its signal to a shared effect (a reverb, a
delay, …) instead of putting a separate copy of that effect on each track.

---

## What it does (plain terms)

- **Two send buses, A and B.** Each is a small FX chain of up to **4 effects**.
- **Per-track send amounts.** Every chain slot (track) has a **Send A** and
  **Send B** level (0–100%) in its settings — how much of that track is sent
  to each bus. The sends are **post-fader**: they follow the track's own
  volume, so turning a track down also pulls it out of the sends (like a
  console's post-fader aux).
- **Per-bus return level.** Each bus has a **return level** that sets how much
  of the (effected) bus comes back into the main mix.
- **Send A → Send B.** Send A can additionally feed **Send B** (the "**→ Send
  B**" amount in Send A's settings), so you can chain effects in parallel —
  e.g. a delay (A) whose repeats are then washed through a reverb (B). It's
  post-fader on A's return, and feedback-safe (B can't bleed back into A).
- **Presets, shared between A and B.** You can save/recall a whole send chain
  as a preset; the preset list is shared, so a preset saved from A can be
  loaded onto B and vice-versa.
- **Per-set persistence.** Send chains, return levels, the A→B amount, and the
  per-track send levels all save and restore with the Move Set.

### Using it

1. Open the **FX-bus picker** and choose **Send A**, **Send B**, or **Master
   FX**. (Master FX behaves exactly as before — it's now just one of the
   buses the picker can open.)
2. In the bus editor, load effects into the slots, tweak params, set the
   **return level**, and **save/load presets**. Sends have no LFOs (those
   menu items are hidden for sends; Master keeps them).
3. On a **track** (chain slot), open its settings and raise **Send A** /
   **Send B** to feed that track into a bus.
4. For parallel chaining, raise **→ Send B** in **Send A**'s settings.

---

## How it works (technical)

### Topology

```
track 0 ──(slot:send_a)──┐                       ┌── return_level[A] ──┐
track 1 ──(slot:send_a)──┼─► send_accum[A] ─► [A FX1..4] ─► send_buf ──┤
track N ──(slot:send_a)──┘                  │                          ├─► ME bus ─► Master FX ─► out
                                            └─(a:to_b · return_level[A])┐         │
track k ──(slot:send_b)──┐                                             ▼         │
              ...        ┼─► send_accum[B] ─► [B FX1..4] ─► return_level[B] ──────┘
                         ┘
```

Buses are **2** (`SEND_BUS_COUNT`), each with **4** FX slots (`SEND_FX_SLOTS`).
Each send bus is hosted exactly like Master FX — an array of
`master_fx_slot_t` (`shadow_send_fx_slots[SEND_BUS_COUNT][SEND_FX_SLOTS]`) —
so all the Master-FX module hosting, presets, and bypass machinery is reused
rather than duplicated.

### Mix path (`src/schwung_shim.c`)

Per audio block, after the per-slot chains run:

1. Each chain slot's **post-fader** output is multiplied by its `slot:send_a` /
   `slot:send_b` level and summed into the bus accumulator `send_accum[bus]`.
2. For each active bus, the accumulator is clamped to `int16`, run through the
   bus's FX slots (`process_block`), then scaled by `shadow_send_return_level[bus]`
   and summed into the **ME bus** (pre–Master FX), so the returns are colored
   by Master FX like everything else.
3. **A→B:** if `shadow_send_a_to_b_level > 0`, Send A's *return-scaled* output
   (`send_buf · return_level[A] · a_to_b`) is added into `send_accum[B]` **before**
   B is processed. Because buses run in order (A=0 then B=1) and A is already
   finished, B can never feed back into A — feedback-safe by construction. A
   still returns to the master independently (parallel, not a re-route).

### Parameters

| Key | Where | Meaning |
| --- | --- | --- |
| `slot:send_a`, `slot:send_b` | per chain slot | post-fader send level 0–1 to bus A/B |
| `send_fx:<a\|b>:fx<1-4>:<param>` | host | param on a send-bus FX slot (`module`, `bypassed`, plugin params, `state`, `chain_params`, `ui_hierarchy`) |
| `send_fx:<a\|b>:return_level` | host | per-bus return level 0–1 (default 1.0) |
| `send_fx:a:to_b` | host | Send A → Send B amount 0–1 (default 0) |
| `save_send_preset` / `update_send_preset` / `delete_send_preset` | DSP | shared send-preset writes |
| `send_preset_count` / `send_preset_name_<i>` / `send_preset_json_<i>` | DSP | shared send-preset reads |

`send_fx:` SET/GET is handled in **both** host param paths
(`shadow_direct_set_param` and `shadow_inprocess_handle_param_request` in
`src/host/shadow_chain_mgmt.c`).

### UI — one editor, many buses

The former Master-FX editor is **genericized** into a single
**bus-descriptor-driven** editor (`src/shadow/shadow_ui_master_fx.mjs`). A
descriptor (`FX_BUS.master` / `sendA` / `sendB`) carries the bus's `id`,
`paramPrefix` (`master_fx:` vs `send_fx:a:` …), `slotCount`, `hasLfo`,
component list, and preset config. The **FX-bus picker**
(`VIEWS.FX_BUS_PICKER` / `enterFxBusPicker` in `src/shadow/shadow_ui.js`)
selects the active bus before entry; only one editor is open at a time, so the
editor's transient state is repopulated per bus on entry.

- Sends set `hasLfo:false`, so the LFO settings items (`mfx_lfo1`/`mfx_lfo2`)
  are filtered out of their settings menu; Master (`hasLfo:true`) keeps them.
- The **home-screen knob-context cache** is keyed on `activeFxBus.id` so knob
  param mappings don't leak across buses once more than one bus exists.

### Persistence

Two layers:

- **Per-set** (mirrors Master-FX per-set files): each send slot's chain is
  written to `send_fx_<bus>_<slot>.json` in the set's state dir, and bus-level
  values (`return_level` for A/B, plus `send_a_to_b`) to `send_fx_meta.json`.
  `send_a_to_b` is always written (default 0) so it can't leak across sets.
  Per-track `slot:send_a/b` save with the chain config; `shadow_state.c` also
  mirrors `send_return_level` + `send_a_to_b` for boot restore.
- **Presets** are a separate **shared** store (`presets_send/`, one list for
  both buses), wrapping up to 4 FX slots under a `send_fx` root.

### Notable upstream-worthy fixes carried here

- **`json_get_section_bounds` (`chain_host.c`)** now anchors on the key's
  colon and only treats the value as a section when it *is* an object.
  Previously a null-valued (empty) FX slot would grab a *later* slot's object,
  corrupting saved presets with gaps (filled/empty/filled). This also fixes a
  latent **Master**-preset bug, so it's worth carrying regardless of sends.
- **Send `ui_hierarchy` falls back to `module.json`**, so modules without an
  explicit hierarchy can still open their params on a send bus.

### Files touched

| File | Role |
| --- | --- |
| `src/host/shadow_chain_types.h` | per-slot `send_a`/`send_b` fields |
| `src/host/shadow_chain_mgmt.{c,h}` | `shadow_send_fx_slots[]`, send load/unload, `send_fx:` SET/GET, return levels, A→B global |
| `src/host/shadow_state.c` | persist `slot_send_a/b`, `send_return_level`, `send_a_to_b` |
| `src/schwung_shim.c` | post-fader send accumulation, return mix, A→B tap |
| `src/shadow/shadow_ui_master_fx.mjs` | generic bus-descriptor FX editor |
| `src/shadow/shadow_ui_slots.mjs` | `slot:send_a/b` settings, `getSendFxDisplayName` |
| `src/modules/chain/dsp/chain_host.c` | shared send-preset store, `json_get_section_bounds` fix |
| `src/shadow/shadow_ui.js` | `FX_BUS` descriptors, FX-bus picker, send settings, A→B item, per-set persistence, bus-keyed knob cache |
