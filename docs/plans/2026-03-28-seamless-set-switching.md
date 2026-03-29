# Seamless Set Switching via Slot-Level Fade Envelopes

## Problem

When switching patches/sets in shadow mode, audio stops abruptly on unload and starts abruptly on load, causing audible clicks and discontinuities. The current approach hard-mutes output for 8 blocks (~23ms) via `g_mute_countdown` / `MUTE_BLOCKS_AFTER_SWITCH` in chain_host.c, but this still produces pops at the mute boundaries and doesn't cover the case where `dlopen`/file I/O blocks the audio thread.

## Goal

Eliminate audible discontinuities when changing sets by adding smooth fade-out/fade-in envelopes at the slot level. Old patch keeps rendering during fade-out, all blocking I/O happens while silent, new patch fades in cleanly.

## Design

### Fade Envelope State

Add to `shadow_chain_slot_t` in `shadow_chain_mgmt.c`:

```c
typedef struct {
    float gain;            // current gain 0.0-1.0
    float target;          // 0.0 (fading out) or 1.0 (fading in)
    float step;            // per-sample gain change
    int pending_patch;     // patch index to load once fade-out completes (-1 = none)
    uint8_t pending_clear; // flag: tear down DSP once fade-out completes
} slot_fade_t;
```

Constants:
- Fade duration: ~50ms = 2205 samples at 44100Hz
- Step size: 1.0 / 2205 ≈ 0.000453

### Audio Mix Path

In the shim's audio mix loop (where slot outputs are combined into the shared audio buffer), replace the current direct copy with a per-sample gain ramp:

```c
for (int i = 0; i < frames * 2; i++) {
    out[i] = (int16_t)(slot_out[i] * fade->gain);
    fade->gain += (fade->target > fade->gain) ? fade->step : -fade->step;
    if (fade->gain < 0.0f) fade->gain = 0.0f;
    if (fade->gain > 1.0f) fade->gain = 1.0f;
}
```

### Patch Load Sequence (Changed)

Current flow:
1. UI sets `ui_patch_index` + increments `ui_request_id`
2. Shim immediately calls `set_param("load_patch", index)` — blocking
3. DSP does `dlclose` → `dlopen` → file I/O → sets `g_mute_countdown = 8`

New flow:
1. UI sets `ui_patch_index` + increments `ui_request_id` (unchanged)
2. Shim sets `fade.target = 0.0` and `fade.pending_patch = index`
3. Old patch **keeps rendering** during ~50ms fade-out
4. Each render cycle, shim checks: if `fade.gain == 0.0 && fade.pending_patch >= 0`:
   - Call `set_param("load_patch", index)` — all blocking I/O happens here, while slot is silent
   - Set `fade.pending_patch = -1`
   - Set `fade.target = 1.0` — begin fade-in
5. New patch audio fades in over ~50ms

### Slot Clear Sequence (Changed)

Current flow:
1. UI requests `SHADOW_PATCH_INDEX_NONE`
2. Shim immediately clears synth/FX params, sets `slot.active = 0`

New flow:
1. UI requests `SHADOW_PATCH_INDEX_NONE`
2. Shim sets `fade.target = 0.0` and `fade.pending_clear = 1`
3. Old patch keeps rendering during fade-out
4. When `fade.gain == 0.0 && fade.pending_clear`:
   - Do the actual teardown (clear params, set `slot.active = 0`)
   - Set `fade.pending_clear = 0`

### chain_host.c Changes

- Remove `g_mute_countdown` and `MUTE_BLOCKS_AFTER_SWITCH` logic from `plugin_render_block()` (and v2 equivalent)
- The fade is now handled at the shim layer, so the DSP layer just renders normally

## Files Changed

| File | Change |
|------|--------|
| `src/host/shadow_chain_mgmt.c` | Add `slot_fade_t`, modify audio mix loop, defer load/clear until fade-out completes |
| `src/modules/chain/dsp/chain_host.c` | Remove `g_mute_countdown` / `MUTE_BLOCKS_AFTER_SWITCH` muting logic |

## Files Not Changed

- Shadow UI (JS) — request mechanism unchanged
- `shadow_constants.h` — shared memory protocol unchanged
- No new shared memory segments or IPC

## Edge Cases

- **Rapid patch switching**: If a new patch is requested while fading out for a previous request, update `pending_patch` to the newest index. Only one load happens.
- **Slot already silent**: If `gain == 0.0` when a load is requested (e.g. slot was cleared), skip fade-out and load immediately, then fade in.
- **First patch load on empty slot**: No fade-out needed. Load, then fade in from 0.0.

## Estimated Scope

~80 lines of C across two files. No new dependencies, threads, or shared memory.
