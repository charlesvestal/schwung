# Microcosm Audio FX Plugin — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a granular audio effects processor inspired by the Hologram Microcosm pedal — 11 algorithms (44 variations), grain engine, delay engine, post-processing chain (pitch mod, SVF filter, FDN reverb), hold/freeze, and reverse toggle.

**Architecture:** Single-file C DSP plugin using `audio_fx_api_v2` (instance-based, in-place stereo int16 processing). A circular capture buffer feeds a shared grain engine with per-algorithm scheduling policies. Bank 4 uses a separate multi-tap delay engine. Post-engine: LFO pitch mod → SVF lowpass → 4-line FDN stereo reverb → dry/wet mix.

**Tech Stack:** C99, ARM64 cross-compilation via Docker, Schwung `audio_fx_api_v2` plugin API, 44100 Hz / 128 frames/block / stereo int16.

**Repo location:** `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-dioramatic/`

---

## Overview

The implementation follows 7 phases matching the design document's priority order. Each phase produces a deployable, testable module. No automated tests — testing is manual on Move hardware.

Memory budget at 44100 Hz:
- Capture buffer: 88,200 stereo samples × 4 bytes = ~345 KB
- Hold buffer: 88,200 stereo samples × 4 bytes = ~345 KB  
- Delay buffer: 88,200 stereo samples × 4 bytes = ~345 KB
- 32 grains × ~64 bytes = ~2 KB
- FDN reverb: 4 lines × ~8K samples × 4 bytes = ~128 KB
- Allpass diffusers: ~32 KB
- **Total: ~1.2 MB** — well within ARM64 constraints

---

### Task 1: Scaffold the module repo

**Files:**
- Create: `schwung-dioramatic/src/module.json`
- Create: `schwung-dioramatic/src/help.json`
- Create: `schwung-dioramatic/src/dsp/dioramatic.c`
- Create: `schwung-dioramatic/src/dsp/plugin_api_v1.h` (copy from schwung-cloudseed)
- Create: `schwung-dioramatic/src/dsp/audio_fx_api_v2.h` (copy from schwung host)
- Create: `schwung-dioramatic/scripts/build.sh`
- Create: `schwung-dioramatic/scripts/Dockerfile`
- Create: `schwung-dioramatic/scripts/install.sh`

**Step 1: Create directory structure**

```bash
mkdir -p /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-dioramatic/{src/dsp,scripts,dist,build}
```

**Step 2: Create module.json**

```json
{
  "id": "dioramatic",
  "name": "Dioramatic",
  "abbrev": "DIO",
  "version": "0.1.0",
  "description": "Granular effects processor — 11 algorithms with grain, glitch, and delay engines",
  "author": "charlesvestal",
  "license": "MIT",
  "dsp": "dioramatic.so",
  "api_version": 2,
  "capabilities": {
    "chainable": true,
    "component_type": "audio_fx",
    "ui_hierarchy": {
      "levels": {
        "root": {
          "name": "Dioramatic",
          "params": [
            {"key": "algorithm", "label": "Algorithm", "type": "enum", "options": [
              "Mosaic", "Seq", "Glide", "Haze", "Tunnel", "Strum",
              "Blocks", "Interrupt", "Arp", "Pattern", "Warp"
            ], "default": "Mosaic"},
            {"key": "variation", "label": "Variation", "type": "enum", "options": ["A","B","C","D"], "default": "A"},
            {"key": "activity", "label": "Activity", "type": "float", "min": 0.0, "max": 1.0, "default": 0.5, "step": 0.01, "unit": "%"},
            {"key": "repeats", "label": "Repeats", "type": "float", "min": 0.0, "max": 1.0, "default": 0.5, "step": 0.01, "unit": "%"},
            {"key": "shape", "label": "Shape", "type": "float", "min": 0.0, "max": 1.0, "default": 0.5, "step": 0.01, "unit": "%"},
            {"key": "filter", "label": "Filter", "type": "float", "min": 0.0, "max": 1.0, "default": 1.0, "step": 0.01, "unit": "%"},
            {"key": "mix", "label": "Mix", "type": "float", "min": 0.0, "max": 1.0, "default": 0.5, "step": 0.01, "unit": "%"},
            {"key": "space", "label": "Space", "type": "float", "min": 0.0, "max": 1.0, "default": 0.3, "step": 0.01, "unit": "%"},
            {"key": "time_div", "label": "Time", "type": "enum", "options": ["1/4","1/2","1x","2x","4x","8x"], "default": "1x"},
            {"key": "pitch_mod_depth", "label": "Pitch Depth", "type": "float", "min": 0.0, "max": 1.0, "default": 0.0, "step": 0.01, "unit": "%"},
            {"key": "pitch_mod_rate", "label": "Pitch Rate", "type": "float", "min": 0.0, "max": 1.0, "default": 0.3, "step": 0.01, "unit": "%"},
            {"key": "filter_res", "label": "Resonance", "type": "float", "min": 0.0, "max": 1.0, "default": 0.0, "step": 0.01, "unit": "%"},
            {"key": "reverb_mode", "label": "Reverb Mode", "type": "enum", "options": ["Room","Plate","Hall","Ambient"], "default": "Room"},
            {"key": "reverse", "label": "Reverse", "type": "enum", "options": ["Off","On"], "default": "Off"},
            {"key": "hold", "label": "Hold", "type": "enum", "options": ["Off","On"], "default": "Off"}
          ],
          "knobs": ["activity", "repeats", "shape", "filter", "mix", "time_div", "space", "algorithm"]
        }
      }
    }
  }
}
```

**Step 3: Copy API headers**

Copy `plugin_api_v1.h` from schwung-cloudseed and `audio_fx_api_v2.h` from schwung host into `src/dsp/`.

**Step 4: Create build.sh**

```bash
#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Microcosm Module Build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
cd "$REPO_ROOT"
echo "=== Building Microcosm Module ==="
mkdir -p build dist/dioramatic

${CROSS_PREFIX}gcc -Ofast -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    src/dsp/dioramatic.c \
    -o build/dioramatic.so \
    -Isrc/dsp \
    -lm

cat src/module.json > dist/dioramatic/module.json
[ -f src/help.json ] && cat src/help.json > dist/dioramatic/help.json
cat build/dioramatic.so > dist/dioramatic/dioramatic.so
chmod +x dist/dioramatic/dioramatic.so

cd dist && tar -czvf dioramatic-module.tar.gz microcosm/ && cd ..
echo "=== Build Complete: dist/dioramatic/ ==="
```

**Step 5: Create Dockerfile** (identical to cloudseed)

```dockerfile
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu make \
    && rm -rf /var/lib/apt/lists/*
ENV CROSS_PREFIX=aarch64-linux-gnu-
WORKDIR /build
```

**Step 6: Create install.sh** (copy pattern from cloudseed, adjust module id)

**Step 7: Create minimal dioramatic.c skeleton**

Create a minimal plugin that compiles and passes audio through unchanged (identity effect). Must export `move_audio_fx_init_v2`, implement `create_instance`, `destroy_instance`, `process_block`, `set_param`, `get_param`. Instance struct should have all parameter fields from module.json with defaults. `process_block` is a no-op passthrough initially.

**Step 8: Build and verify compilation**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-dioramatic
./scripts/build.sh
```

**Step 9: Init git repo and commit**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-dioramatic
git init && git add -A && git commit -m "feat: scaffold microcosm module repo"
```

---

### Task 2: Capture buffer + grain engine core (Phase 1a)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

This task implements the circular capture buffer and the shared grain playback engine — the foundation everything else builds on.

**Step 1: Add capture buffer to instance struct**

```c
#define SAMPLE_RATE 44100
#define CAPTURE_SECONDS 2
#define CAPTURE_SAMPLES (SAMPLE_RATE * CAPTURE_SECONDS)  /* 88200 */
#define MAX_GRAINS 32

typedef struct {
    float l, r;
} stereo_sample_t;

typedef struct {
    stereo_sample_t buffer[CAPTURE_SAMPLES];
    int write_pos;
} capture_buffer_t;

typedef struct {
    int active;
    int start;           /* position in capture buffer */
    int length;          /* grain duration in samples */
    float position;      /* fractional read position */
    float speed;         /* playback rate */
    int direction;       /* +1 forward, -1 reverse */
    float pan;           /* -1.0 to +1.0 */
    float amplitude;     /* current envelope amplitude */
    float env_phase;     /* 0.0-1.0 envelope progress */
    float env_inc;       /* per-sample envelope increment */
    int env_shape;       /* 0=hann, 1=trapezoid, 2=triangle, 3=rectangle */
} grain_t;
```

**Step 2: Implement capture buffer write**

In `process_block`, before any grain processing, convert input int16 to float and write into the circular capture buffer. Advance write_pos with wrap.

**Step 3: Implement grain envelope lookup tables**

Pre-compute 4 envelope tables (256 entries each) at instance creation:
- Hann: `0.5 * (1 - cos(2π * i/255))`
- Trapezoid: 10% attack, 80% sustain, 10% release
- Triangle: linear up then down
- Rectangle: all 1.0 with 1ms fade (first/last ~4 entries)

**Step 4: Implement grain playback**

For each active grain each sample:
1. Read from capture buffer at `grain->start + (int)grain->position` with linear interpolation
2. Look up envelope amplitude from the selected envelope table using `env_phase * 255`
3. Apply pan (equal-power: `L *= cos(pan_angle), R *= sin(pan_angle)`)
4. Accumulate into wet output buffer
5. Advance `position += speed * direction`, advance `env_phase += env_inc`
6. Deactivate grain when `env_phase >= 1.0`

**Step 5: Wire into process_block**

```
for each frame:
  1. Write input to capture buffer
  2. Accumulate all active grains into wet_l, wet_r
  3. Mix: out = dry * (1-mix) + wet * mix
  4. Convert back to int16, write in-place
```

**Step 6: Build and verify compilation**

```bash
./scripts/build.sh
```

**Step 7: Commit**

```bash
git add -A && git commit -m "feat: capture buffer and grain engine core"
```

---

### Task 3: Mosaic algorithm — all 4 variations (Phase 1b)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

Mosaic is the signature Microcosm sound: overlapping loops at multiple speeds creating shimmering cascading harmonies.

**Step 1: Add tempo/timing infrastructure**

```c
typedef struct {
    float bpm;
    int time_div;        /* 0=1/4, 1=1/2, 2=1x, 3=2x, 4=4x, 5=8x */
    int subdivision_samples;  /* current grain length in samples */
    int trigger_counter;      /* counts samples since last grain trigger */
} tempo_state_t;
```

Time division multipliers: `{0.25, 0.5, 1.0, 2.0, 4.0, 8.0}`. Base interval = `60.0/bpm * SAMPLE_RATE`. Subdivision = base × multiplier. Use `host->get_bpm()` if available, else default 120.

**Step 2: Implement Mosaic grain scheduling**

Mosaic triggers a new grain at intervals based on Activity (more voices = more frequent triggers). The trigger interval = `subdivision_samples / (1 + activity * 3)` (so at max activity, triggers 4x as often).

**Step 3: Implement Mosaic grain configuration per variation**

When a grain triggers, configure it based on the current variation:

- **Var A**: Speed randomly chosen from {1.0, 2.0}. Pan randomized ±0.5.
- **Var B**: Speed from {0.5, 1.0}. Pan randomized.
- **Var C**: Speed always 2.0. Pan randomized.
- **Var D**: Speed from {0.5, 1.0, 2.0, 4.0}. Pan randomized.

Grain start position: `write_pos - random(0, subdivision_samples)` (reads from recent audio).
Grain length: `subdivision_samples`.
Repeats controls feedback/sustain: at high Repeats, grains are louder and overlap more. Map Repeats to grain amplitude (0.3 at Repeats=0, 1.0 at Repeats=1) and to trigger density multiplier.

**Step 4: Implement algorithm dispatch**

Add `algorithm` and `variation` fields to instance. In `process_block`, call `mosaic_schedule_grains(inst)` when algorithm==0. Structure for easy addition of more algorithms later:

```c
switch (inst->algorithm) {
    case 0: mosaic_tick(inst); break;
    // future algorithms here
}
```

**Step 5: Build, deploy, test on hardware**

```bash
./scripts/build.sh && ./scripts/install.sh
```

Test: Load in Signal Chain as audio FX. Play audio through it. Verify Mosaic produces overlapping grains at different speeds. Test all 4 variations via parameter changes. Test Activity and Repeats interaction.

**Step 6: Commit**

```bash
git add -A && git commit -m "feat: Mosaic algorithm with 4 variations"
```

---

### Task 4: Shape envelope + SVF filter + pitch modulation (Phase 2)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Wire Shape knob to grain envelope selection**

Map Shape 0.0–1.0 to 4 zones:
- 0.00–0.25 → Hann (env_shape=0)
- 0.25–0.50 → Trapezoid (env_shape=1)
- 0.50–0.75 → Triangle (env_shape=2)
- 0.75–1.00 → Rectangle (env_shape=3)

All newly triggered grains use the currently selected envelope shape.

**Step 2: Implement 2-pole SVF lowpass filter**

State-variable filter after grain engine output, before mix:

```c
typedef struct {
    float ic1eq, ic2eq;  /* state variables */
} svf_state_t;

/* Per-sample SVF tick */
static inline void svf_process(svf_state_t *s, float cutoff_hz, float Q, float in, float *out) {
    float g = tanf(M_PI * cutoff_hz / SAMPLE_RATE);
    float k = 1.0f / Q;
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;
    float v3 = in - s->ic2eq;
    float v1 = a1 * s->ic1eq + a2 * v3;
    float v2 = s->ic2eq + a2 * s->ic1eq + a3 * v3;
    s->ic1eq = 2.0f * v1 - s->ic1eq;
    s->ic2eq = 2.0f * v2 - s->ic2eq;
    *out = v2;  /* lowpass output */
}
```

Cutoff mapping: `filter` 0→1 maps to 80 Hz → 18000 Hz (exponential: `80 * pow(225, filter)`).
Resonance: `filter_res` 0→1 maps to Q 0.5 → 20 (cap below self-oscillation).

Apply to wet signal only (L and R independently, separate state).

**Step 3: Implement LFO pitch modulation**

Sine LFO modulates grain playback speed:

```c
typedef struct {
    float phase;  /* 0.0-1.0 */
} lfo_t;
```

- Rate: `pitch_mod_rate` 0→1 maps to 0.1–10 Hz
- Depth: `pitch_mod_depth` 0→1 maps to 0–100 cents (speed multiplier: `pow(2, depth * lfo_value / 1200)`)
- Apply to each grain's effective speed during playback

**Step 4: Wire into process_block signal flow**

```
capture → grain engine → pitch mod (per grain) → accumulate wet → SVF filter → [reverb placeholder] → mix with dry → output
```

**Step 5: Build, deploy, test**

Test Shape knob changes envelope character. Test Filter sweeps from dark to bright. Test pitch mod adds chorus-like wobble at low depth, dramatic warble at high.

**Step 6: Commit**

```bash
git add -A && git commit -m "feat: shape envelopes, SVF filter, pitch modulation"
```

---

### Task 5: FDN stereo reverb (Phase 3)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Implement 4-line FDN reverb**

```c
#define FDN_LINES 4
#define FDN_MAX_DELAY 8192

typedef struct {
    float delay_lines[FDN_LINES][FDN_MAX_DELAY];
    int delay_lengths[FDN_LINES];
    int write_pos[FDN_LINES];
    float feedback[FDN_LINES];
    float lp_state[FDN_LINES];  /* per-line lowpass for damping */
    /* Input allpass diffusers */
    float ap_buf[4][2048];
    int ap_pos[4];
    int ap_len[4];
    float ap_coeff;
} fdn_reverb_t;
```

Use Hadamard matrix mixing between delay lines. Each line has:
- Different prime-length delay
- One-pole lowpass for high-frequency damping
- Feedback coefficient controlling decay time

**Step 2: Define 4 reverb mode presets**

| Mode | Delay lengths (samples) | Feedback | Damping | Pre-delay |
|------|------------------------|----------|---------|-----------|
| Room (A) | {1087, 1283, 1447, 1663} | 0.75 | 0.7 | 0 |
| Plate (B) | {2017, 2389, 2777, 3191} | 0.85 | 0.5 | 441 |
| Hall (C) | {3547, 4177, 4831, 5557} | 0.92 | 0.4 | 882 |
| Ambient (D) | {5501, 6469, 7481, 8179} | 0.97 | 0.3 | 1323 |

All delay lengths are prime to avoid metallic artifacts.

**Step 3: Implement allpass input diffusers**

4 series allpass filters at the input to smear transients:
- Lengths: {142, 107, 379, 277} (primes)
- Coefficient: 0.6

**Step 4: Wire reverb into signal chain**

Reverb sits after SVF filter, before dry/wet mix. Space knob (0–1) controls reverb send level. Reverb processes wet signal only.

```
wet_signal → SVF filter → reverb_send = wet * space → fdn_process → wet += reverb_out → mix with dry
```

**Step 5: Build, deploy, test**

Test all 4 reverb modes. Verify Room is tight, Ambient is massive. Test Space knob from dry to drenched.

**Step 6: Commit**

```bash
git add -A && git commit -m "feat: 4-mode FDN stereo reverb"
```

---

### Task 6: Seq algorithm (Phase 4a)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Implement Seq grain scheduling**

Seq slices captured audio and re-triggers in shuffled tempo-synced patterns. Uses a step sequencer approach:

- Maintain a sequence of 8 "slice positions" pointing to recent capture buffer locations
- Each step triggers at subdivision intervals
- Shuffle pattern randomizes playback order

**Step 2: Implement 4 Seq variations**

- **A**: Shuffled slices with per-grain filter variation. Activity controls filter depth.
- **B**: Alternating normal/half-speed. Activity blends between speeds. At max Activity, add a sustained pad grain (long, quiet, overlapping).
- **C**: Overlapping layers with filter sweeps. Activity adds more layers.
- **D**: At high Activity, add bit-crush effect (reduce sample resolution by quantizing float values to fewer steps).

**Step 3: Add bit-crush utility**

```c
static inline float bitcrush(float sample, float bits) {
    float levels = powf(2.0f, bits);
    return roundf(sample * levels) / levels;
}
```

**Step 4: Build, deploy, test. Commit.**

---

### Task 7: Glide algorithm (Phase 4b)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Implement Glide grain scheduling**

Short overlapping loops with pitch that shifts over time. Each grain's speed changes during playback (portamento effect).

- Grain speed starts at 1.0 and glides toward a target speed over the grain's lifetime
- Activity controls the rate of pitch shifting (how fast the glide occurs)
- Repeats controls tail length/decay

**Step 2: Implement 4 Glide variations**

- **A**: Target speed > 1.0 (ascending). Target = `1.0 + activity * 1.0`
- **B**: Target speed < 1.0 (descending). Target = `1.0 - activity * 0.5`
- **C**: Alternating up/down targets per grain
- **D**: Random target speed per grain (range 0.5–2.0)

**Step 3: Modify grain playback to support speed glide**

Add `speed_target` and `speed_glide_rate` to grain struct. Per-sample: `speed += (speed_target - speed) * glide_rate`.

**Step 4: Build, deploy, test. Commit.**

---

### Task 8: Haze algorithm (Phase 4c)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Implement Haze grain scheduling**

Clusters of micro-grains creating an immediate wash. Very short grains (10–50ms), triggered at high density, with randomized start positions within the last ~500ms of captured audio.

- Activity controls density (grains per second: 4 at 0, 60+ at max) and spread (how far back in capture buffer to reach)
- Repeats controls sustain/decay of the wash (grain amplitude envelope)

**Step 2: Implement 4 Haze variations**

- **A**: Short diffused, grains at speed 1.0, lengths 10–30ms
- **B**: Many simultaneous grains, heavily randomized positions and lengths
- **C**: Mix of speed 1.0 and 2.0 grains (octave shimmer)
- **D**: Mix of speed 1.0 and 0.5 grains (darker texture)

**Step 3: Build, deploy, test. Commit.**

---

### Task 9: Tunnel algorithm (Phase 4d)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Implement Tunnel grain scheduling**

Cyclical micro-loops generating drones. One or two long grains that loop continuously from a small region of the capture buffer. Creates a sustained drone from the captured audio.

- Activity controls depth of sample/filter manipulation within the loop
- Repeats controls decay time (how long the drone sustains after input stops — controlled via amplitude fade)

**Step 2: Implement 4 Tunnel variations**

- **A**: Drone with filter modulation (LFO sweeps the filter cutoff within the drone)
- **B**: Drone with pitch-shifted overtone grains added (speeds 2.0, 3.0)
- **C**: Drone with delay feedback (feed drone output back into a short delay line)
- **D**: Drone with bit-reduction (progressively crush the looping grain)

**Step 3: Build, deploy, test. Commit.**

---

### Task 10: Strum algorithm (Phase 4e)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Implement onset detection**

Simple onset detector: track RMS energy of input. When RMS crosses a threshold (rising edge), record the capture buffer position as a "note onset."

```c
#define MAX_ONSETS 8

typedef struct {
    int positions[MAX_ONSETS];
    int count;
    int head;
    float prev_rms;
} onset_detector_t;
```

**Step 2: Implement Strum grain scheduling**

Rhythmic chains of recent note onsets. When a new onset is detected, trigger a rapid cascade of grains reading from recent onset positions.

- Activity controls density of the rhythmic pattern (cascade speed)
- Repeats controls repetitions of the strum cascade

**Step 3: Implement 4 Strum variations**

- **A**: Single most recent onset, repeated continuously
- **B**: Many copies of most recent onset overlapping (phasing)
- **C**: Cascading chain cycling through multiple onsets
- **D**: Like C but with added double-speed (octave up) grains

**Step 4: Build, deploy, test. Commit.**

---

### Task 11: Blocks algorithm (Phase 5a)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Implement Blocks glitch scheduling**

Incoming audio triggers predictable glitches or random bursts. Uses a probability system: at each subdivision boundary, roll a dice to decide whether to trigger a glitch grain.

- Activity controls stutter rate and density (probability of glitch per step)
- Repeats controls additional event probability and sustain

**Step 2: Implement 4 Blocks variations**

- **A**: Regular, predictable stutters (probability tied directly to Activity, no randomness in timing)
- **B**: Random bursts at varying intervals (timing is randomized)
- **C**: Pitch-shifted glitch bursts (grain speeds randomly from {0.5, 1.0, 1.5, 2.0})
- **D**: Combined stutters with filter sweep (modulate SVF cutoff during glitch grains)

**Step 3: Build, deploy, test. Commit.**

---

### Task 12: Interrupt algorithm (Phase 5b)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Implement Interrupt scheduling**

Unique algorithm: dry signal passes through normally, but at random intervals, glitch grains "interrupt" the dry signal. During interruption, the wet signal temporarily replaces the dry.

Need an `interrupting` flag on the instance. When active, mix goes to 100% wet for the duration of the interrupt grain(s).

- Activity controls depth of manipulation during interrupts
- Repeats controls frequency of interruption events

**Step 2: Implement 4 Interrupt variations**

- **A**: Rearranged versions of playing interrupt the signal
- **B**: Pitch-shifted interruptions
- **C**: Filter sweep + delay during interruptions
- **D**: Bit-crushed and heavily manipulated interruptions

**Step 3: Build, deploy, test. Commit.**

---

### Task 13: Arp algorithm (Phase 5c)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Define musical interval table**

```c
/* Semitone ratios for arp patterns */
static const float arp_intervals[] = {
    1.0f,       /* unison */
    1.05946f,   /* minor 2nd */
    1.12246f,   /* major 2nd */
    1.18921f,   /* minor 3rd */
    1.25992f,   /* major 3rd */
    1.33484f,   /* perfect 4th */
    1.41421f,   /* tritone */
    1.49831f,   /* perfect 5th */
    1.58740f,   /* minor 6th */
    1.68179f,   /* major 6th */
    1.78180f,   /* minor 7th */
    1.88775f,   /* major 7th */
    2.0f,       /* octave */
};
```

**Step 2: Implement Arp grain scheduling**

Grains are pitch-shifted to musical intervals and triggered in rapid sequence. Each arp step triggers a grain at the next interval in the pattern.

- Activity controls arp speed (step duration) and number of notes in the arp
- Repeats controls sequence length / how many times the arp repeats

**Step 3: Implement 4 Arp variations**

- **A**: Ascending: unison → 3rd → 5th → octave → repeat
- **B**: Descending: octave → 5th → 3rd → unison → repeat
- **C**: Up-down: unison → 3rd → 5th → octave → 5th → 3rd → repeat
- **D**: Random: random interval selection each step

**Step 4: Build, deploy, test. Commit.**

---

### Task 14: Multi-tap delay engine + Pattern algorithm (Phase 6a)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Implement delay engine (separate from grain engine)**

```c
#define DELAY_BUFFER_SAMPLES 88200  /* 2 seconds at 44100 */
#define MAX_DELAY_TAPS 8

typedef struct {
    int delay_samples;    /* tap delay time */
    float feedback;       /* per-tap feedback */
    float level;          /* per-tap output level */
    float lp_state;       /* per-tap lowpass filter state */
    float lp_coeff;       /* lowpass coefficient */
    float speed;          /* pitch shift (1.0 = normal) */
} delay_tap_t;

typedef struct {
    stereo_sample_t buffer[DELAY_BUFFER_SAMPLES];
    int write_pos;
    delay_tap_t taps[MAX_DELAY_TAPS];
    int active_taps;
} delay_engine_t;
```

**Step 2: Implement delay processing**

Per sample:
1. Write input + feedback sum to delay buffer
2. For each active tap, read from `write_pos - tap->delay_samples` with interpolation
3. Apply per-tap lowpass filter
4. Accumulate tap outputs as wet signal

**Step 3: Implement Pattern algorithm**

Activity controls number of active taps (1–8). Repeats controls feedback.

**4 rhythmic patterns** (tap spacings as fractions of subdivision):

- **A**: Linear: {1/8, 2/8, 3/8, 4/8, 5/8, 6/8, 7/8, 1} — evenly spaced
- **B**: Dotted: {1/6, 1/3, 1/2, 2/3, 5/6, 1, 7/6, 4/3} — dotted/syncopated
- **C**: Triplet: {1/3, 2/3, 1, 4/3, 5/3, 2, 7/3, 8/3}
- **D**: Irregular: {1/8, 3/8, 1/2, 5/8, 1, 9/8, 3/2, 2} — complex pattern

**Step 4: Route algorithms 9-10 through delay engine instead of grain engine**

```c
if (inst->algorithm >= 9) {
    delay_engine_process(inst, ...);
} else {
    grain_engine_process(inst, ...);
}
```

**Step 5: Build, deploy, test. Commit.**

---

### Task 15: Warp algorithm (Phase 6b)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Implement per-tap pitch shift and filter for Warp**

Warp uses the delay engine but adds pitch shift and progressive filtering to each tap.

**Step 2: Implement 4 Warp variations**

- **A**: Ascending pitch per tap (each subsequent tap is shifted up by `activity * 2` semitones)
- **B**: Progressive LP filtering (each tap has lower cutoff: `18000 * pow(0.5, tap_index * activity)`)
- **C**: Combined pitch + filter
- **D**: Reverse delay reading (read backward from tap position) + pitch warp

Activity controls depth of warp (pitch shift amount, filter sweep depth).
Repeats controls feedback/tail length.

**Step 3: Build, deploy, test. Commit.**

---

### Task 16: Hold/Freeze sampler (Phase 7a)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Add hold buffer to instance**

```c
typedef struct {
    stereo_sample_t buffer[CAPTURE_SAMPLES];
    int length;          /* captured length */
    float read_pos;      /* current playback position */
    int active;          /* currently holding */
    float fade;          /* crossfade state 0-1 */
    int fade_samples;    /* ~50ms = 2205 samples */
} hold_state_t;
```

**Step 2: Implement hold engage**

When hold is set to "On":
1. Copy current wet output into hold buffer (last `subdivision_samples` worth)
2. Set active=1, begin crossfade from live processing to looped hold buffer
3. Loop the hold buffer indefinitely

**Step 3: Implement hold release**

When hold is set to "Off":
1. Crossfade from hold buffer back to live processing over ~50ms
2. Set active=0

**Step 4: Wire hold into process_block**

When holding, replace wet signal with hold buffer output (after crossfade). Live input still feeds capture buffer so the player can continue playing on top.

**Step 5: Build, deploy, test. Commit.**

---

### Task 17: Reverse toggle (Phase 7b)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Implement reverse for grain engine**

When `reverse` is "On", all newly triggered grains get `direction = -1`. Existing grains keep their current direction (no mid-grain flip).

**Step 2: Implement reverse for delay engine**

When reverse is on, delay taps read backward from their tap position. This means reading `write_pos - delay_samples + offset` instead of `write_pos - delay_samples - offset`, progressing forward through the buffer from the tap point.

**Step 3: Build, deploy, test. Commit.**

---

### Task 18: Algorithm crossfade + tempo sync polish (Phase 7c)

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`

**Step 1: Implement algorithm/variation crossfade**

When algorithm or variation changes:
1. Store current wet output level
2. Fade out over 50ms (2205 samples) while deactivating old grains
3. Switch algorithm/variation
4. Fade in over 50ms

Add `crossfade_counter` and `crossfade_from_algo` to instance.

**Step 2: Query host BPM each block**

```c
if (g_host && g_host->get_bpm) {
    float bpm = g_host->get_bpm();
    if (bpm > 0) inst->tempo.bpm = bpm;
}
```

Recalculate `subdivision_samples` when BPM changes.

**Step 3: Add MIDI clock handler via on_midi**

Listen for MIDI clock (0xF8) to derive BPM from clock tick intervals. Also handle Start (0xFA), Stop (0xFC).

**Step 4: Build, deploy, full test of all features. Commit.**

---

### Task 19: State save/restore + help.json + final polish

**Files:**
- Modify: `schwung-dioramatic/src/dsp/dioramatic.c`
- Modify: `schwung-dioramatic/src/help.json`

**Step 1: Implement get_param("state")**

Return all parameters as JSON for patch persistence:

```c
if (strcmp(key, "state") == 0) {
    return snprintf(buf, buf_len,
        "{\"algorithm\":%d,\"variation\":%d,\"activity\":%.4f,\"repeats\":%.4f,"
        "\"shape\":%.4f,\"filter\":%.4f,\"mix\":%.4f,\"space\":%.4f,"
        "\"time_div\":%d,\"pitch_mod_depth\":%.4f,\"pitch_mod_rate\":%.4f,"
        "\"filter_res\":%.4f,\"reverb_mode\":%d,\"reverse\":%d,\"hold\":%d}",
        inst->algorithm, inst->variation, inst->activity, inst->repeats,
        inst->shape, inst->filter, inst->mix, inst->space,
        inst->time_div, inst->pitch_mod_depth, inst->pitch_mod_rate,
        inst->filter_res, inst->reverb_mode, inst->reverse, inst->hold);
}
```

**Step 2: Implement set_param("state")**

Parse JSON and restore all fields. Call `apply_parameters()` afterward.

**Step 3: Write help.json**

Document all parameters with user-friendly descriptions for the Shadow UI help system.

**Step 4: Implement ui_hierarchy and chain_params get_param responses**

Return the hierarchy JSON matching module.json for the Shadow UI.

**Step 5: Full end-to-end test**

- All 11 algorithms, all 4 variations each
- Activity/Repeats interaction for each
- Shape envelope changes
- Filter + resonance sweeps
- All 4 reverb modes
- Hold engage/release
- Reverse toggle
- Algorithm switching with crossfade
- State save/restore (load patch, change params, reload)
- Tempo sync from host BPM

**Step 6: Final commit**

```bash
git add -A && git commit -m "feat: complete Microcosm audio FX plugin — 11 algorithms, 44 variations"
```

---

## Summary

| Task | Description | Phase |
|------|-------------|-------|
| 1 | Scaffold repo, build system, passthrough plugin | Setup |
| 2 | Capture buffer + grain engine core | 1a |
| 3 | Mosaic algorithm (4 variations) | 1b |
| 4 | Shape + SVF filter + pitch mod | 2 |
| 5 | FDN stereo reverb (4 modes) | 3 |
| 6 | Seq algorithm | 4a |
| 7 | Glide algorithm | 4b |
| 8 | Haze algorithm | 4c |
| 9 | Tunnel algorithm | 4d |
| 10 | Strum algorithm + onset detection | 4e |
| 11 | Blocks algorithm | 5a |
| 12 | Interrupt algorithm | 5b |
| 13 | Arp algorithm | 5c |
| 14 | Delay engine + Pattern algorithm | 6a |
| 15 | Warp algorithm | 6b |
| 16 | Hold/Freeze sampler | 7a |
| 17 | Reverse toggle | 7b |
| 18 | Crossfade + tempo sync polish | 7c |
| 19 | State save/restore + help + final polish | 7d |
