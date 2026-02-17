# Feature Spec: Input Type Modes for Line In Plugin

**Date:** 2026-02-14
**Status:** Draft

## Summary

Add an **Input Type** parameter to the Line In module supporting three source classes on the same physical input:

- **Line** (default, current behavior)
- **Hi-Z (Guitar)**
- **Phono**

Each mode applies distinct gain staging, conditioning, and noise handling before routing to the existing signal chain path.

## Goals

- Make the post-conditioning signal land at a predictable internal reference level (~-18 dBFS RMS) across input types.
- Preserve expected tonal response per input type (especially guitar high-frequency taming and phono RIAA).
- Sensible defaults per mode; optional manual override for gate and advanced controls.
- No user clipping surprises.

## Non-goals

- Amp/cabinet simulation (this is input conditioning, not an amp sim).
- Automatic input type detection.
- Full phono cartridge loading configuration (future work).

## Platform Constraints

| Constraint | Reality |
|---|---|
| Display | 128x64 monochrome, 1-bit |
| Controls | 8 physical knobs + jog/menu navigation |
| UI system | Shadow UI via `ui_hierarchy` levels, not desktop GUI |
| Audio | 44100 Hz, 128 frames/block, stereo interleaved int16 |
| CPU | ARM (Ableton Move); must be efficient |
| Plugin API | v2: string-based set_param/get_param, render_block |

**No dropdowns, no collapsible sections, no multi-meter views.** All interaction through knob turns and menu navigation within `ui_hierarchy` levels.

## UI Hierarchy Design

The module uses a **fully static** multi-level hierarchy. All levels are always present in `module.json` — the Shadow UI renders them as-is. Parameters that don't apply to the current input type simply have no effect when adjusted (e.g., turning "Cable Comp" in Line mode does nothing). Mode-specific advanced settings are split into three separate sub-levels so the user navigates to the relevant one.

### Root Level

Primary controls. Knobs 1-4 mapped to the most-used parameters.

```
┌──────────────────────────┐
│ Line In                  │
│                          │
│   Input Type    [Line  ] │
│   Input Trim    [  0 dB] │
│   Output Trim   [  0 dB] │
│   Gate          [  Auto] │
│   Gate Amount   [  50 %] │
│ > Gate Settings          │
│ > Line Settings          │
│ > Guitar Settings        │
│ > Phono Settings         │
│                          │
│ K1:Type K2:In K3:Out K4:Gate │
└──────────────────────────┘
```

**Knobs (root):** `input_type`, `input_trim`, `output_trim`, `gate_amount`

### Gate Settings Level

Manual gate controls (only meaningful when Gate = Manual, but always navigable).

**Knobs:** `gate_threshold`, `gate_attack`, `gate_release`, `gate_range`

### Line Settings Level

Line-mode advanced controls: `hpf_freq`, `safety_limiter`

### Guitar Settings Level

Guitar-mode advanced controls: `cable_comp`, `soft_clip`

### Phono Settings Level

Phono-mode advanced controls: `riaa_eq`, `subsonic_freq`, `hum_notch`, `hum_freq`

## module.json: ui_hierarchy

The hierarchy is fully static. All levels and parameters are defined once and never change at runtime. Parameters that don't apply to the current input type simply have no effect.

See the `module.json` implementation below for the complete schema.

## Signal Chain (Per-Sample in render_block)

```
Audio Input (host mailbox, int16 stereo)
  │
  ├─ 1. Input Trim (gain in dB, smoothed)
  │
  ├─ 2. Mode Conditioning Block
  │     ├─ Line:   HPF (optional), Safety Limiter (optional)
  │     ├─ Guitar: HPF + Cable Comp (high-shelf EQ) + Soft Clip (optional)
  │     └─ Phono:  RIAA de-emphasis + Subsonic HPF + Hum Notch
  │
  ├─ 3. Noise Gate / Expander (post-conditioning)
  │
  ├─ 4. Output Trim (gain in dB, smoothed)
  │
  └─ Output (int16 stereo → chain or audio out)
```

All processing in float32 internally; convert int16→float on entry, float→int16 on exit with clipping.

## Mode Definitions

### 1. Line Mode

**Use case:** Line-level source, minimal coloration.

| Parameter | Default | Notes |
|---|---|---|
| Input Trim | 0 dB | |
| HPF | Off | Optional rumble/DC removal |
| Safety Limiter | Off | Fast transparent limiter for accidental overs |
| Gate | Auto | Light profile |

**Auto Gate defaults (Line):**
- Threshold: -50 dBFS (line sources are typically clean)
- Attack: 3 ms
- Hold: 50 ms
- Release: 200 ms
- Range: 12 dB (reduce, don't mute — preserves tails)

### 2. Hi-Z (Guitar) Mode

**Use case:** Electric guitar into line input. Can't change hardware impedance, but can provide appropriate gain staging and tame harshness from impedance mismatch.

| Parameter | Default | Notes |
|---|---|---|
| Input Trim | +18 dB | Guitar signal is much quieter than line level |
| Cable Comp | Med | High-shelf attenuation above ~4 kHz |
| HPF | 80 Hz | Remove rumble below playing range |
| Soft Clip | Off | Gentle tanh saturation on peaks |
| Gate | Auto | Medium profile |

**Cable Compensation** (high-shelf filter, Q ≈ 0.7):

| Setting | Corner | Attenuation |
|---|---|---|
| Off | — | flat |
| Low | 5 kHz | -2 dB |
| Med | 4 kHz | -4 dB |
| High | 3 kHz | -6 dB |

**Auto Gate defaults (Guitar):**
- Threshold: -40 dBFS
- Attack: 2 ms
- Hold: 100 ms
- Release: 350 ms
- Range: 24 dB
- Hysteresis: 4 dB (open threshold 4 dB above close threshold)

### 3. Phono Mode

**Use case:** Turntable output (assumes phono-level source needing RIAA de-emphasis and substantial gain).

| Parameter | Default | Notes |
|---|---|---|
| Input Trim | +34 dB | Phono signal is very quiet |
| RIAA EQ | On | Standard playback de-emphasis curve |
| Subsonic Filter | 20 Hz | HPF to remove warp rumble |
| Hum Notch | On | Notch at 50 or 60 Hz + first harmonic |
| Hum Freq | 60 Hz | User selectable (50 Hz for EU) |
| Gate | Auto | Light/gentle profile |

**RIAA De-emphasis** (standard IIR approximation):
- Three time constants: 3180 µs (50.05 Hz), 318 µs (500.5 Hz), 75 µs (2122 Hz)
- Implement as cascaded biquad sections (2 sections sufficient for < 0.5 dB error)

**Hum Notch:**
- Primary: 50 or 60 Hz, Q ≈ 10 (narrow)
- Harmonic: 100 or 120 Hz, Q ≈ 10
- Both as second-order IIR notch filters

**Auto Gate defaults (Phono):**
- Threshold: -55 dBFS (vinyl has constant surface noise; gate must be gentle)
- Attack: 8 ms
- Hold: 200 ms
- Release: 600 ms
- Range: 8 dB (expander-style, never fully mutes — avoids chopping between tracks)

## Noise Gate Implementation

### Gate Modes

| Mode | Behavior |
|---|---|
| Off | No gating |
| Auto | Mode-appropriate defaults; `gate_amount` (0-100%) scales threshold margin and range |
| Manual | All 5 gate parameters directly controllable via Gate Settings level |

### Auto Mode Logic

1. Use mode-specific default time constants (attack, hold, release, range).
2. `gate_amount` controls aggressiveness:
   - **Amount 0%**: gate_range = 0 dB (effectively off)
   - **Amount 50%** (default): mode's default range and threshold
   - **Amount 100%**: range at mode's max, threshold raised by 6 dB

### Gate DSP

Per-channel with linked stereo detection (max of L/R envelope for open/close decision, same gain applied to both channels to preserve stereo image).

```
envelope = max(abs(L), abs(R))  // peak detection
smoothed_env = attack/release one-pole follower

if smoothed_env > open_threshold:
    state = OPEN (ramp to 0 dB over attack time)
elif smoothed_env < close_threshold AND hold_timer expired:
    state = CLOSING (ramp to -range dB over release time)
```

Hysteresis: `close_threshold = open_threshold - hysteresis_dB`. Default hysteresis 3 dB (Guitar: 4 dB).

## Instance State

```c
typedef struct {
    /* Parameters */
    int   input_type;       /* 0=Line, 1=Guitar, 2=Phono */
    float input_trim_db;
    float output_trim_db;
    int   gate_mode;        /* 0=Off, 1=Auto, 2=Manual */
    float gate_amount;      /* 0-100, for Auto mode */
    float gate_threshold;   /* dBFS, for Manual mode */
    float gate_attack_ms;
    float gate_hold_ms;
    float gate_release_ms;
    float gate_range_db;

    /* Advanced (Line) */
    int   hpf_freq_idx;     /* 0=Off, 1=20, 2=40, 3=60, 4=80, 5=120 */
    int   safety_limiter;

    /* Advanced (Guitar) */
    int   cable_comp;       /* 0=Off, 1=Low, 2=Med, 3=High */
    int   soft_clip;

    /* Advanced (Phono) */
    int   riaa_eq;
    int   subsonic_freq_idx; /* 0=10, 1=15, 2=20, 3=30, 4=40 Hz */
    int   hum_notch;
    int   hum_freq;          /* 0=50Hz, 1=60Hz */

    /* DSP state */
    float input_gain_smooth;  /* smoothed linear gain */
    float output_gain_smooth;

    /* Biquad filter states (stereo: [0]=L, [1]=R) */
    biquad_t hpf[2];
    biquad_t cable_shelf[2];
    biquad_t riaa_stage1[2];
    biquad_t riaa_stage2[2];
    biquad_t subsonic[2];
    biquad_t hum_notch1[2];   /* fundamental */
    biquad_t hum_notch2[2];   /* first harmonic */

    /* Gate state */
    float gate_envelope;
    float gate_gain;          /* current gate attenuation (linear) */
    int   gate_hold_counter;  /* samples remaining in hold phase */
    int   gate_state;         /* OPEN, HOLD, CLOSING, CLOSED */

} linein_instance_t;
```

## Mode Switching

When `input_type` changes:

1. Recalculate all filter coefficients for the new mode.
2. Reset filter state variables (`z1`, `z2`) to zero to avoid transients.
3. Cross-fade output over 64 samples (~1.5 ms) to avoid clicks.

**Important:** Changing Input Type does NOT reset Output Trim, Gate Mode, or any other user-set parameters. The hierarchy is static — no UI changes on mode switch. Parameters for non-active modes are simply ignored by render_block.

## Gain Smoothing

All gain changes (input trim, output trim, gate gain) use one-pole smoothing to prevent zipper noise:

```c
#define GAIN_SMOOTH_COEFF 0.001f  /* ~3 ms at 44100 Hz */
gain_smooth += GAIN_SMOOTH_COEFF * (gain_target - gain_smooth);
```

## Reusable DSP From Existing Codebase

| Block | Source | Notes |
|---|---|---|
| Biquad HPF/LPF | `move-anything-tapescam` `GS_Biquad*` | Copy struct + SetHighpass/SetLowpass |
| High-shelf EQ | `move-anything-tapescam` `Tone_ComputeShelf` | For cable compensation |
| Soft clip (tanh) | `move-anything-tapescam` | `tanhf(v * softness)` |
| Envelope follower | `move-anything-ducker` | Attack/release envelope phases |
| Param helper | `src/host/param_helper.h` | Reduce set_param/get_param boilerplate |

New code needed: RIAA biquad coefficients, hum notch coefficient calculation, auto-gate noise floor estimation.

## Preset Patches

Add chain patches in `src/patches/`:

| File | Description |
|---|---|
| `linein.json` | Line In — Clean (Line mode, gate off) |
| `linein_guitar.json` | Line In — Guitar (Guitar mode, auto gate, cable comp Med) |
| `linein_guitar_gated.json` | Line In — Guitar Gated (Guitar mode, stronger gate for single coils) |
| `linein_phono.json` | Line In — Phono RIAA (Phono mode, RIAA on, hum notch on) |
| `linein_freeverb.json` | Already exists — update to use new defaults |

## Error Feedback

Since there's no room for toast messages or tooltips on the 128x64 display, error states are communicated through parameter values and naming:

- If input clips: the `input_trim` display briefly flashes or the label could show "CLIP" (implementation detail — depends on Shadow UI capabilities).
- If Phono mode with RIAA Off: no special warning (the user explicitly turned it off).
- Safety limiter active: no visual indicator beyond the parameter being On.

**Practical approach:** Rely on the post-conditioning signal landing at a reasonable level with the defaults. The metering available in the Shadow UI (if any) handles the rest.

## Test Plan

1. **Leveling:** Typical source per mode lands near -18 dBFS RMS post-conditioning with default trim.
2. **Clipping:** Verify int16 output never overflows (clamp in float→int16 conversion).
3. **Gate behavior:**
   - Guitar: no chatter on palm mutes, clean note decay.
   - Line: no pumping on sustained signals.
   - Phono: no track-tail chopping; gentle expansion only.
4. **RIAA correctness:** Sweep test tone through phono mode, compare frequency response to reference curve (< 1 dB error 20 Hz – 20 kHz).
5. **Hum notch:** Verify > 20 dB attenuation at notch frequency, < 1 dB effect at ±1 octave.
6. **Mode switch:** No pop or click when changing Input Type.
7. **CPU:** Measure render_block time; must stay well under 2.9 ms budget (128 samples / 44100 Hz).

## Acceptance Criteria

- [ ] `input_type` enum switches between Line/Guitar/Phono with mode-appropriate defaults.
- [ ] Post-conditioning signal near -18 dBFS RMS with typical sources and default trims.
- [ ] Auto gate behaves per mode profile; Manual gate fully overrides all parameters.
- [ ] Mode switch is click-free (crossfade).
- [ ] All filters (HPF, shelf, RIAA, notch) use stable biquad IIR (no blowup at any setting).
- [ ] Gate uses linked stereo detection (no stereo image shift).
- [ ] Output clamp prevents int16 overflow.
- [ ] Static `ui_hierarchy` with separate Line/Guitar/Phono settings levels.
- [ ] Chain patches provided for all three modes.

## Implementation Notes

- **File changes:** Only `linein.c` and `module.json` in `src/modules/sound_generators/linein/`. No host changes needed.
- **Binary size:** Biquad code is small. Expect < 2 KB additional code.
- **State save/restore:** All parameters round-trip through `set_param`/`get_param` for chain patch persistence. All params are always saved regardless of active mode. On restore, `input_type` should be set first so subsequent params take effect correctly, but order doesn't matter for correctness since filters are recalculated on every relevant set_param call.
- **Static ui_hierarchy:** Defined entirely in `module.json`. The `get_param("ui_hierarchy")` response in C code returns the same static JSON always. No runtime construction needed.
