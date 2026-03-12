# Slot LFO Design

Per-slot parameter LFOs integrated into slot settings, using the existing modulation runtime from the chain host.

## Decision: Slot Setting, Not Module

A parameter LFO modulates params of other modules in the chain. It doesn't process audio or generate sound. Making it a slot-level feature avoids wasting a chain slot and lets it naturally target any module in the chain (synth + FX). The modulation runtime already lives in chain_host — the infrastructure is at the right level.

## Overview

- 2 LFOs per shadow slot
- LFO oscillator runs in C inside chain_host's `render_block` (344 Hz tick rate)
- JS shadow UI handles configuration via sub-menus in slot settings
- Uses existing `chain_mod_emit_value` for non-destructive parameter overlay
- Uses existing `sampler_get_bpm()` fallback chain for tempo
- LFO config saved with patches (backwards compatible)

## Data Model

```c
typedef struct {
    int active;           // Has valid target
    int shape;            // 0=sine, 1=tri, 2=saw, 3=square, 4=s&h
    float rate_hz;        // Free-running rate (0.1-20.0 Hz)
    int rate_div;         // Tempo-synced division index
    int sync;             // 0=free, 1=tempo-sync
    float depth;          // 0.0-1.0
    float phase_offset;   // 0.0-1.0 (displayed as 0-360 degrees)
    char target[16];      // Component: "synth", "fx1", "fx2", "midi_fx1", "midi_fx2"
    char param[32];       // Parameter key
    double phase;         // 0.0-1.0, accumulates each render_block
    float last_sh_value;  // Held value for S&H shape
} lfo_state_t;
```

Two instances per `chain_instance_t`: `lfo_state_t lfos[2]`.

## C-side: LFO Engine

### Phase Accumulation

In `render_block`, after processing audio, tick each active LFO:

- **Free mode**: `phase += rate_hz * frames / sample_rate`
- **Sync mode**: Convert division to Hz via `sampler_get_bpm()`: `rate = bpm / 60.0 * beats_per_division`

### Waveform Computation

Phase offset applied at computation, not accumulation:

```c
double effective_phase = fmod(lfo->phase + lfo->phase_offset, 1.0);
float signal = compute_shape(lfo->shape, effective_phase, &lfo->last_sh_value);
```

All shapes output bipolar (-1 to +1). S&H picks a new random value when phase wraps past 1.0.

### Modulation Emit

```c
chain_mod_emit_value(inst, "lfo1", target, param, signal, depth, 0.0, 1/*bipolar*/, 1/*enabled*/);
```

The existing modulation runtime handles range scaling to target param min/max, clamping, and rate-limited application.

### Beat Alignment

On MIDI Start (0xFA), reset `phase = 0.0` for all synced LFOs. Free-running LFOs ignore transport. This anchors synced LFO cycles to beat 1.

### Tempo Source

Uses existing `sampler_get_bpm()` fallback chain:
1. Active MIDI clock (measured from 0xF8 ticks)
2. Current Set's tempo (Move project metadata)
3. Last measured clock BPM
4. Settings file tempo
5. Default 120 BPM

Chain host already receives `get_bpm` via `process_host_t`.

### Division Table

| Index | Label | Beats per cycle |
|-------|-------|----------------|
| 0 | 1/1 | 4.0 |
| 1 | 1/2 | 2.0 |
| 2 | 1/4 | 1.0 |
| 3 | 1/8 | 0.5 |
| 4 | 1/16 | 0.25 |
| 5 | 1/32 | 0.125 |
| 6 | 1/4T | 1.333 |
| 7 | 1/8T | 0.667 |
| 8 | 1/16T | 0.333 |

### Config via Params

JS pushes config changes via `shadow_set_param`:
- `lfo1:shape`, `lfo1:rate_hz`, `lfo1:rate_div`, `lfo1:sync`
- `lfo1:depth`, `lfo1:phase_offset`
- `lfo1:target`, `lfo1:target_param`

When target changes, clear old source via `chain_mod_clear_source(inst, "lfo1")`.

## JS-side: Shadow UI

### Slot Settings Menu

Two new action items in `CHAIN_SETTINGS_ITEMS`, before Save/SaveAs/Delete:

```javascript
{ key: "lfo1", label: "LFO 1", type: "action" },
{ key: "lfo2", label: "LFO 2", type: "action" },
```

### LFO Sub-menu

Each opens a sub-menu with title showing status (e.g. "LFO 1: Sine > Synth:Cutoff" or "LFO 1: Off"):

| Item | Type | Range | Notes |
|------|------|-------|-------|
| Shape | enum | Sine/Tri/Saw/Square/S&H | Knob adjustable |
| Sync | enum | Free/Sync | Toggle |
| Rate | float or enum | 0.1-20.0 Hz (free) or division list (sync) | Switches based on sync mode |
| Depth | float | 0-100% (maps to 0.0-1.0) | |
| Phase | float | 0-360 degrees (maps to 0.0-1.0) | |
| Target | action | None / loaded components | Opens component picker |
| Param | action | shows selected param name | Opens param picker (after target selected) |

### Target Picker (Two-Step)

1. Select component: lists only loaded components (Synth, FX1, FX2, MIDI FX1, MIDI FX2)
2. Select parameter: lists params from that component's `chain_params` metadata

Sets both `lfo1:target` and `lfo1:target_param` via `setSlotParam`.

## Patch Persistence

LFO config added to patch JSON:

```json
{
  "synth": { ... },
  "fx1": { ... },
  "lfos": {
    "lfo1": {
      "shape": "sine",
      "sync": 1,
      "rate_hz": 2.0,
      "rate_div": 2,
      "depth": 0.5,
      "phase_offset": 0.0,
      "target": "synth",
      "target_param": "cutoff"
    },
    "lfo2": null
  }
}
```

- `null` or absent = inactive
- Backwards compatible: older patches without `"lfos"` get no LFOs
- Once loaded, config lives in set state (slot override as usual)

## Edge Cases

- **Target becomes invalid** (module removed/swapped): `chain_mod_emit_value` returns -1, LFO stays configured but does nothing. User can re-pick or clear in UI.
- **Phase reset**: NOT reset on patch load (avoids clicks). Only reset on MIDI Start for synced LFOs.
- **No MIDI clock in sync mode**: Falls back through `sampler_get_bpm()` chain. LFO keeps running.
- **Knob + LFO on same param**: Both work. Knob turns update base value, LFO modulates around it. This is correct behavior.
- **Display during modulation**: Shadow UI param editor already shows effective values via `get_param` — values will visually move when LFO is active.
- **S&H**: New random value generated each time phase wraps past 1.0.
