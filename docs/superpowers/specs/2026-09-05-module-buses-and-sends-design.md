# Module buses and global sends — design

**Status:** approved design, not implemented
**Date:** 2026-09-05

A module that can render its voices separately gains per-voice insert chains and
send levels, without the chain host learning anything about that module. Worked
example: mrdrums with distortion on the kick, chorus→phaser on the hats, and a
delay and a reverb on two device-wide sends.

## The unifying principle

**Every chain in Schwung is 8 positions.** Main, each bus, Send A, Send B and
Master FX are all one thing: `MAX_AUDIO_FX` positions of audio FX, edited by one
screen, permuted by one `chain_reorder.c`, persisted in one shape. This design
adds no second cap and no second editor.

## Three concepts

| | Scope | What it is | Count |
|---|---|---|---|
| **Splittable voice** | module | a voice the module can render into its own buffer | the module's own |
| **Bus** | slot | a user-made set of voices + an 8-position insert chain + a level to each send | up to 4 per slot, **plus Main** |
| **Global send** | device | an 8-position chain fed by every bus in every slot, returning pre-Master-FX | 2 |

**Main is bus 0** — implicit, never created or deleted, holding every voice not
assigned elsewhere. Its insert chain *is* the slot's existing main chain, and it
carries send levels like any other bus. A module that cannot split therefore has
exactly the chain it has today, with two send knobs added.

### Why the grouping is user-owned and arbitrary

Closed hat and open hat want one chorus. A bus is therefore a **set** of voices,
many-to-one and editable, not a voice and not an author-declared group. The
grouping lives entirely in the chain: the module renders at its finest
granularity and never honours a routing parameter.

### Why the sends are global

A per-slot send means four reverbs when all four slots want one. One device-wide
reverb is the same feature at a quarter of the cost on core 3. Note that Master
FX is **not** already this: it is an insert on the summed bus, not a send/return.

## Module contract

### `split_voices` — a flat, ordered list

```
get_param("split_voices") -> [{"id":"kick","label":"Kick"},{"id":"chh","label":"Closed Hat"}, ...]
```

Presence of this key is the opt-in; absence means the module is unsplittable and
nothing in the UI offers buses for it.

**The list is flat and ordered, and its index is the buffer index.** This is not
a stylistic choice. The bus→voice map has to be resolved in C on the SPI
callback, and `chain_json.c`'s helpers are flat key scans that cannot walk
`ui_hierarchy`'s `levels` in order — the same constraint that makes
`synth:last_note` report a note rather than a voice index. C handles only
indices; the JS UI resolves ids to labels for display.

The bus config stores **ids**, not indices, so that a module adding a voice in a
later version does not silently re-point every existing bus. Ids are resolved to
indices once, when the synth loads — a bounded string compare over the voice
list, not per frame.

### `move_plugin_render_split` — a dlsym'd symbol, not a struct field

```c
/* Optional. Discovered by dlsym, exactly like fx_on_midi.
 *
 * ACCUMULATES into each buffer; the chain clears the distinct buffers first.
 * voice_out[i] MAY ALIAS: two voices in one bus receive one pointer, and
 * every voice in no bus receives the main-mix pointer. The aliasing is what
 * makes the sparse case free — there is no per-voice buffer to sum.
 *
 * Runs on the SPI callback, like every other module entry point. */
void move_plugin_render_split(void *instance,
                              int16_t *const *voice_out,
                              int n_voices, int frames);
```

**It is a separate exported symbol and not a new field on `plugin_api_v2_t`.**
Appending to that struct is what boot-looped a device via breakbeat's header
drift; a module cannot extend the ABI from its side, and a dlsym'd symbol is
absent-or-present with no offset to get wrong.

Accumulation clamps per sample, as the existing inject mix in `v2_render_block`
already does. A module exporting the symbol still exports `render_block`, used
whenever no bus exists.

## Signal flow

Inside `v2_render_block`, replacing the single synth render:

```
1  clear main_buf and each DISTINCT bus_buf[]
2  build voice_out[]:  voice i -> bus_buf[bus_of[i]], else main_buf
3  render_split(voice_out)          (or render_block -> main_buf if unsplittable)
4  for each bus b:   its 8 FX in series on bus_buf[b]
5  for each bus b (Main included), each send s:
       send_accum[s] += bus_buf[b] * level[b][s]        <- POST-insert
6  main_buf += every bus_buf[b]
7  the main 8 FX on main_buf   (still deferred to chain_process_fx under
                                external_fx_mode)
```

Send levels are **post-insert**: the kick's distortion is in what reaches the
reverb. Pre/post is not a per-send option; wanting a different send level for a
voice is what making a second bus is for.

`send_accum[]` is **shim-owned and shared by all four slots** — that is what
"global" means here. Steps 1–6 are internal to the slot and its stereo output is
unchanged in shape, so `external_fx_mode` and `chain_process_fx` (the
Move→Schwung path where the shim runs the main 8 separately) need no change.

### The shim half

```
per frame:  four slots render, accumulating into send_accum[0..1]
            Send A: 8 FX on send_accum[0]
            Send B: 8 FX on send_accum[1]
            shadow mix += both returns
            Master FX on the mix
            master volume -> DAC
```

Sends live in `shadow_chain_mgmt.c` beside `master_fx`, with cap-derived key
routing mirroring `master_fx_key.h`, and the shim stays the authority for
persistence (`send_fx_N.json` beside `master_fx_N.json`) — the same rule that
`master_fx:modules` already follows, for the same reason: an overtake tool or a
Remote UI client can write straight to the shim, and an in-file mirror that has
not seen those writes will overwrite them with `{}`.

## Two consequences that are part of the design

### Stems 6 and 7

Today the four slot stems sum to the master exactly. A shared send return belongs
to no slot and would silently break that invariant. The two send returns are
therefore emitted as **two further stems**, restoring the exact sum. (Everything
stays pre-Master-FX, as stems already are.)

### On-demand allocation, off the RT thread

A bus chain costs ~9.1 MB (8 positions x ~1.07 MB of `chain_param_info_t`, plus
8 x 64 KB of cached `ui_hierarchy`). Eager allocation would be 4 buses x 4 slots
~= 146 MB and a ~36 MB `calloc` **inside the SPI callback**, since
`create_instance` runs there.

Bus chains are therefore allocated **when the bus is created**, on a SCHED_OTHER
worker, and published to the audio path by pointer. Slot memory is unchanged
until a bus exists. Device budget for reference: 1849 MB total, ~1020 MB
available.

## User interface

The chain row is horizontal; buses hang below the synth box. **Down** on the
synth box opens the bus list; **down** on a bus row opens that bus's chain in the
ordinary chain editor.

```
[MIDI FX]-[ mrdrums ]-[FX1]-[FX2]           down on the synth
              |
              v
   Buses           A   B                    down on a bus row
  >Kick    dist    20  00                        |
   Hats    cho>pha 00  15                        v
   Main    --      05  30                   [cho]-[pha]-[ ] ...  (8 positions)
```

- Bus list rows: name, insert summary, and the two send levels.
- Actions on the list: create, rename, delete, assign voices (multi-select over
  `split_voices`), set send levels.
- Send levels are also reachable as a knob-grid page, so they can be ridden.
- Send A and Send B are siblings of the Master FX screen — the same screen, three
  instances.
- A slot whose synth publishes no `split_voices` shows no bus affordance at all.

## Persistence

- Per slot, in `slot_N.json`: `buses: [{name, voices:[id], fx:[...], sends:[a,b]}]`.
  Main's send levels are stored alongside as bus 0.
- Device-wide: `send_fx_N.json`, shim-authoritative, beside `master_fx_N.json`.
- A bus whose voice ids no longer exist in a swapped-in module keeps its chain
  and reports the orphaned ids rather than silently re-pointing — the
  snapshot/recall rule, where a partial restore that reports nothing is
  indistinguishable from a working one.

## Testing

- `tests/host/`: bus routing math with aliased pointers; accumulate-and-clamp;
  send accumulation across four slots; send key routing derived from the cap (as
  `test_master_fx_slot_routing` is), so it widens with the cap.
- Stems: master must equal the sum of stems 1–4 plus 6 and 7, bit-exactly.
- Screens rendered through the PNG harness and looked at, not reasoned about.
- A module that exports no `split_voices` must produce a byte-identical render to
  today's path.

## Risks

- **CPU is the ceiling, not memory.** A slot can now hold 40 FX positions
  (main 8 + 4 buses x 8) against a ~2370 us frame budget in which a Tape-Echo-class
  effect measures 0.37 ms. Nothing here throttles; the cost must be *visible* on
  the existing CPU page (`/system/cpu`), because exceeding it is a device-wide
  dropout rather than a quiet degradation. Accepted deliberately: the answer is
  lighter plugins and shared sends, not a lower cap.
- Only mrdrums implements `render_split` at first. Every other module falls back
  to `render_block` into Main and offers no buses — graceful, and the fallback is
  the tested default rather than an error path.
