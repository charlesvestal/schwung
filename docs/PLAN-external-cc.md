# Plan: External MIDI CC Support

## Part 1: Chain Host CC Routing Fix

### Problem

The chain host intercepts CCs 71-78 from ALL sources for knob parameter mapping.
This blocks external controllers from sending standard MIDI CCs (e.g. CC 74 = filter cutoff)
to synths. Values other than 1/127 are silently swallowed.

### Solution: Source-Aware CC Routing

Three routing rules in `chain_host.c` `v2_on_midi` and `plugin_on_midi`:

| CC Range | Source | Behavior |
|----------|--------|----------|
| 71-78 | INTERNAL | Knob mapping (relative encoder, as today) |
| 71-78 | EXTERNAL | Pass through to synth `on_midi` |
| 102-109 | EXTERNAL | Remap to 71-78 knob mapping (virtual knobs) |

### Files to Change

- `src/modules/chain/dsp/chain_host.c`
  - V1 path (`plugin_on_midi`, ~line 3307): Add source check on 71-78 block
  - V2 path (`v2_on_midi`, ~line 5851): Add source check on 71-78 block
  - Both paths: Add second block for 102-109 from EXTERNAL, remap cc to (cc - 31) before applying knob logic
  - Add `#define KNOB_EXT_CC_START 102` / `#define KNOB_EXT_CC_END 109`

### No Other Files Need Changes

- The shim dispatches external MIDI with `MOVE_MIDI_SOURCE_EXTERNAL` already
- Shadow UI knob display reads from knob_mappings state, unaffected by CC number
- Patch files store knob mappings by target/param, not by CC number
- Move hardware still sends 71-78 as INTERNAL, unchanged

---

## Part 2: Per-Module CC Implementation

### Standard CC Map (all synths should implement these where applicable)

| CC | Name | Parameter | Notes |
|----|------|-----------|-------|
| 1 | Mod Wheel | varies | Already done in most synths |
| 5 | Portamento Time | glide/portamento | Mono synths |
| 7 | Volume | master volume/level | All synths |
| 64 | Sustain Pedal | hold notes | All polyphonic synths |
| 71 | Resonance | filter resonance | All synths with filters |
| 72 | Release | amp envelope release | All synths |
| 73 | Attack | amp envelope attack | All synths |
| 74 | Cutoff | filter cutoff | All synths with filters |
| 75 | Decay | amp envelope decay | All synths |
| 76 | Vibrato Rate | LFO rate | Synths with LFO |
| 77 | Vibrato Depth | LFO depth/amount | Synths with LFO |
| 120 | All Sound Off | kill all sound | All synths |
| 123 | All Notes Off | release all notes | All synths |

CCs are absolute (0-127). Map to parameter range: `value / 127.0 * (max - min) + min`.

### Per-Module Implementation

Each module's `on_midi` function gets a CC handling block. Pattern:

```c
if (len >= 3 && (msg[0] & 0xF0) == 0xB0) {
    uint8_t cc = msg[1];
    uint8_t val = msg[2];
    float norm = val / 127.0f;  // 0.0-1.0
    switch (cc) {
        case 7:  inst->volume = norm; break;
        case 74: inst->cutoff = norm; break;
        case 71: inst->resonance = norm; break;
        // ...
        case 120: case 123: all_notes_off(inst); break;
    }
}
```

---

#### Braids (`move-anything-braids/src/dsp/braids_plugin.cpp`)

Currently handles: CC 1 (FM amount)

| CC | Parameter | Range |
|----|-----------|-------|
| 1 | `fm` | 0-1 (done) |
| 7 | `volume` | 0-1 |
| 70 | `timbre` | 0-1 |
| 74 | `cutoff` | 0-1 |
| 71 | `resonance` | 0-1 |
| 18 | `color` | 0-1 |
| 73 | `attack` | 0-1 |
| 75 | `decay` | 0-1 |
| 72 | `release` | 0-1 |
| 15 | `filt_env` | 0-1 |
| 120/123 | panic | all notes off |

---

#### Rings (`move-anything-rings/src/dsp/`)

Currently handles: (needs audit — likely minimal)

| CC | Parameter | Range |
|----|-----------|-------|
| 1 | mod wheel | TBD |
| 7 | volume | 0-1 |
| 70 | `structure` | 0-1 |
| 74 | `brightness` | 0-1 |
| 71 | `damping` | 0-1 |
| 18 | `position` | 0-1 |
| 120/123 | panic | all notes off |

---

#### DX7/Dexed (`move-anything-dx7/src/dsp/dx7_plugin.cpp`)

Currently handles: CC 1 (mod wheel LFO), CC 64 (sustain), CC 123 (panic)

| CC | Parameter | Range |
|----|-----------|-------|
| 7 | `output_level` | 0-100 |
| 76 | `lfo_speed` | 0-99 |
| 77 | `lfo_pmd` | 0-99 (pitch mod depth) |
| 15 | `lfo_amd` | 0-99 (amp mod depth) |
| 16 | `feedback` | 0-7 |
| 120 | all sound off | (add) |

---

#### OB-Xd (`move-anything-obxd/src/dsp/obxd_plugin.cpp`)

Currently handles: CC 1 (mod wheel), CC 64 (sustain)

| CC | Parameter | Range |
|----|-----------|-------|
| 7 | `volume` | 0-1 |
| 74 | `cutoff` | 0-1 |
| 71 | `resonance` | 0-1 |
| 73 | `attack` | 0-1 |
| 75 | `decay` | 0-1 |
| 72 | `release` | 0-1 |
| 76 | `lfo_rate` | 0-1 |
| 77 | `lfo_amt1` | 0-1 |
| 5 | `portamento` | 0-1 |
| 15 | `filter_env` | 0-1 |
| 16 | `pw` | 0-1 |
| 17 | `noise` | 0-1 |
| 18 | `osc2_detune` | 0-1 |
| 120/123 | panic | all notes off |

---

#### Hera/Juno-60 (`move-anything-hera/src/dsp/hera_plugin.cpp`)

Currently handles: CC 1, 64 (accepted but no-op), CC 120/123 (panic)

| CC | Parameter | Range |
|----|-----------|-------|
| 7 | `vca_depth` | 0-1 |
| 74 | `vcf_cutoff` | 0-1 |
| 71 | `vcf_resonance` | 0-1 |
| 73 | `attack` | 0-1 |
| 75 | `decay` | 0-1 |
| 72 | `release` | 0-1 |
| 76 | `lfo_rate` | 0-1 |
| 15 | `vcf_env` | 0-1 |
| 16 | `pwm_depth` | 0-1 |
| 17 | `noise_level` | 0-1 |
| 18 | `sub_level` | 0-1 |
| 64 | sustain | (implement properly) |

---

#### Moog/Raffo (`move-anything-moog/src/dsp/moog_plugin.cpp`)

Currently handles: CC 1 (pitch mod), CC 64 (no-op), CC 123 (panic)

| CC | Parameter | Range |
|----|-----------|-------|
| 7 | `volume` | 0-1 |
| 74 | `cutoff` | 0-1 |
| 71 | `resonance` | 0-1 |
| 73 | `attack` | 0-1 |
| 75 | `decay` | 0-1 |
| 72 | `release` | 0-1 |
| 76 | `lfo_rate` | 0-1 |
| 5 | `glide` | 0-1 |
| 15 | `contour` | 0-1 (filter env amount) |
| 16 | `lfo_filter` | 0-1 |
| 64 | sustain | (implement properly) |
| 120 | all sound off | (add) |

---

#### SH-101/Hush1 (`move-anything-hush1/src/dsp/sh101_plugin.c`)

Currently handles: CC 1 (mod), CC 123 (panic)

| CC | Parameter | Range |
|----|-----------|-------|
| 7 | `volume` | 0-1 |
| 74 | `cutoff` | 0-1 |
| 71 | `resonance` | 0-1.2 |
| 73 | `attack` | 0-1 |
| 75 | `decay` | 0-1 |
| 72 | `release` | 0-1 |
| 76 | `lfo_rate` | 0-1 |
| 5 | `glide` | 0-1 |
| 15 | `env_amt` | 0-1 (filter env amount) |
| 16 | `lfo_filter` | 0-1 |
| 17 | `pulse_width` | 0.05-0.95 |
| 64 | sustain | hold notes |
| 120 | all sound off | (add) |

---

#### Chiptune (`move-anything-chiptune/src/dsp/chiptune_plugin.cpp`)

Currently handles: CC 1 (vibrato depth), CC 120/123 (panic)

| CC | Parameter | Range |
|----|-----------|-------|
| 7 | `volume` | 0-15 (int) |
| 73 | `env_attack` | 0-15 (int) |
| 75 | `env_decay` | 0-15 (int) |
| 72 | `env_release` | 0-15 (int) |
| 76 | `vibrato_rate` | 0-10 (int) |
| 16 | `duty` | 0-3 (int) |
| 18 | `detune` | 0-50 (int) |
| 64 | sustain | hold notes |

Note: Integer parameter ranges need scaled mapping, e.g. `val * 15 / 127` for 0-15 range.

---

#### Bristol Mini (`move-anything-bristol/src/dsp/`)

(Needs parameter audit — likely similar to Moog with cutoff, resonance, ADSR, glide)

---

### Modules That Don't Need CC (streaming/passthrough)

- **SF2**: Already forwards all CCs to FluidSynth
- **Surge**: Already forwards all CCs to Surge engine
- **CLAP**: Already forwards all CCs to hosted plugin
- **JV-880**: Already queues all CCs to emulated hardware
- **Virus**: Already tracks all CCs in shared memory
- **RadioGarden, WebStream, AirPlay, Line-In**: No synth engine, no CCs needed

---

## Implementation Order

### Phase 1: Chain host routing fix
1. Add source check to 71-78 knob interception (INTERNAL only)
2. Add 102-109 external virtual knob range
3. Test: external CC 74 reaches synth, external CC 102 controls mapped param

### Phase 2: High-impact synths (most users)
4. OB-Xd — full CC set (popular, many continuous params)
5. Hera — full CC set + fix sustain (popular, iconic filter)
6. Braids — full CC set (popular, many tonal params)

### Phase 3: Remaining synths
7. Moog — full CC set + fix sustain
8. SH-101 — full CC set + add sustain
9. DX7 — add LFO/feedback CCs
10. Chiptune — add volume/envelope/duty CCs
11. Rings — full CC set
12. Bristol — full CC set (needs param audit first)

### Phase 4: Documentation
13. Update MANUAL.md with supported CC table per module
14. Update docs/MODULES.md with CC implementation guide for external module devs
