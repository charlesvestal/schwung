# schwung-breath Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build `schwung-breath`, a breath-gated MIDI FX module. Held pads/keys sound only while the performer blows into Move's mic; breath also drives a continuous CC2 / aftertouch stream for synth timbre.

**Architecture:** New external repo `../schwung-breath`, modeled on `../schwung-keydetect` (also reads mic audio). Loaded into chain MIDI FX slot via the existing `midi_fx_api_v1` plugin contract. Mic audio is read each block via `host_api_v1->mapped_memory + audio_in_offset` inside `tick()`. Note state machine, envelope detector, curves, and CC/AT emission all live in `dsp/breath.c`. JS UI handles calibration flow and live breath meter via Shadow UI param hierarchy.

**Tech Stack:** C99 DSP (cross-compiled ARM64 in Docker), JS UI (`.mjs`), midi_fx_api_v1, host_api_v1 mapped_memory, chain_params JSON, slot autosave.

**Reference design:** `docs/plans/2026-05-17-schwung-breath-design.md` (this directory).

---

## Pre-flight: critical assumption to verify in Task 2

The whole design depends on **MIDI FX plugins being able to read mic audio from `host_api_v1->mapped_memory + audio_in_offset` during `tick()`**. The header guarantees the pointer; chain_host.c needs to be confirmed to pass through a real mapped pointer (not a stub).

If it doesn't work, the host fix is a small chain_host.c change to ensure `subplugin_host_api.mapped_memory` is set from the real SPI mailbox before each chain tick. Treat that as a sub-task of Task 2 if needed.

---

## Task 1: Bootstrap the new repo

**Files:**
- Create: `../schwung-breath/` (new git repo at `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-breath/`)
- Copy structure from: `../schwung-keydetect/`

**Step 1:** Create the repo and copy template scaffolding.

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent
mkdir schwung-breath
cd schwung-breath
git init
# Copy template files (build/scripts/workflow), NOT keydetect-specific source
cp -r ../schwung-keydetect/scripts ./scripts
cp -r ../schwung-keydetect/.github ./.github
cp ../schwung-keydetect/.gitignore ./.gitignore 2>/dev/null || true
mkdir -p src/dsp test
```

**Step 2:** Strip keydetect references — adapt `scripts/build.sh`, `scripts/install.sh`, `scripts/Dockerfile`, and `.github/workflows/release.yml` to read `id=breath` everywhere. Module id is `breath` (no prefix); asset name is `breath-module.tar.gz`.

Look for: `keydetect`, `KeyDetect`, `KD`, `keyfinder` and replace with `breath`, `Breath`, `BR`, (none — no third-party DSP).

**Step 3:** Copy the plugin headers we need directly (the build runs in Docker without access to the host repo):

```bash
cp ../schwung-keydetect/src/dsp/plugin_api_v1.h src/dsp/
cp ../schwung/src/host/midi_fx_api_v1.h src/dsp/
```

**Step 4:** Commit:

```bash
git add . && git commit -m "init: scaffold schwung-breath from keydetect template"
```

**Verify:** `ls src/` shows `dsp/`; `scripts/build.sh` exists and references `breath`.

---

## Task 2: Stub DSP that proves mic audio is readable from MIDI FX

This is the critical-path validation. Build a no-op MIDI FX that, each block, computes the peak amplitude of the incoming mic audio and stashes it for `get_param("debug_peak", …)` to read. Then install and confirm it changes when you blow on the mic.

**Files:**
- Create: `../schwung-breath/src/dsp/breath.c`
- Create: `../schwung-breath/src/module.json`

**Step 1:** Minimal `module.json`:

```json
{
    "id": "breath",
    "name": "Breath",
    "abbrev": "BR",
    "version": "0.1.0",
    "description": "Breath-gated MIDI FX. Blow into Move's mic; held notes sound while you blow.",
    "author": "charlesvestal",
    "license": "MIT",
    "dsp": "breath.so",
    "api_version": 1,
    "capabilities": {
        "chainable": true,
        "component_type": "midi_fx",
        "audio_in": true,
        "ui_hierarchy": {
            "levels": {
                "root": {
                    "label": "Breath",
                    "knobs": [],
                    "params": [
                        {"key": "debug_peak", "label": "Peak"}
                    ]
                }
            }
        }
    }
}
```

**Step 2:** Minimal `dsp/breath.c`:

```c
#include "midi_fx_api_v1.h"
#include "plugin_api_v1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    const host_api_v1_t *host;
    float last_peak;  /* updated each tick */
} breath_inst_t;

static const host_api_v1_t *g_host = NULL;

static void *create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    breath_inst_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->host = g_host;
    return s;
}

static void destroy_instance(void *inst) { free(inst); }

static int process_midi(void *inst, const uint8_t *in, int in_len,
                        uint8_t out[][3], int out_lens[], int max_out) {
    (void)inst; (void)max_out;
    /* Pass through for stub */
    if (in_len > 3 || in_len <= 0) return 0;
    memcpy(out[0], in, in_len);
    out_lens[0] = in_len;
    return 1;
}

static int tick(void *inst, int frames, int sr,
                uint8_t out[][3], int out_lens[], int max_out) {
    (void)sr; (void)out; (void)out_lens; (void)max_out;
    breath_inst_t *s = (breath_inst_t *)inst;
    if (!s->host || !s->host->mapped_memory) return 0;
    const int16_t *audio_in =
        (const int16_t *)(s->host->mapped_memory + s->host->audio_in_offset);
    float peak = 0.0f;
    for (int i = 0; i < frames * 2; i++) {
        float v = fabsf((float)audio_in[i] / 32768.0f);
        if (v > peak) peak = v;
    }
    s->last_peak = peak;
    return 0;
}

static void set_param(void *inst, const char *k, const char *v) { (void)inst; (void)k; (void)v; }

static int get_param(void *inst, const char *k, char *buf, int blen) {
    breath_inst_t *s = (breath_inst_t *)inst;
    if (strcmp(k, "debug_peak") == 0) {
        return snprintf(buf, blen, "%.4f", s->last_peak);
    }
    if (strcmp(k, "chain_params") == 0) {
        return snprintf(buf, blen,
            "[{\"key\":\"debug_peak\",\"name\":\"Peak\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.001}]");
    }
    return -1;
}

static midi_fx_api_v1_t api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = create_instance,
    .destroy_instance = destroy_instance,
    .process_midi = process_midi,
    .tick = tick,
    .set_param = set_param,
    .get_param = get_param,
};

midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &api;
}
```

**Step 3:** Build via Docker: `./scripts/build.sh`. Expect `dist/breath/breath.so` to be produced and packaged as `dist/breath-module.tar.gz`.

**Step 4:** Install on hardware:

```bash
./scripts/install.sh   # from local tarball
```

**Step 5:** Smoke-test in chain. Open chain UI on the device, add `Breath` as a MIDI FX in any slot. Watch the "Peak" param value:

```bash
ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log"
# Blow on the mic; Peak should rise toward 1.0
```

**If Peak stays at 0.0:** `host_api_v1->mapped_memory` isn't valid in chain MIDI FX context. Fix in `src/modules/chain/dsp/chain_host.c`: when initializing `subplugin_host_api`, ensure `mapped_memory` and `audio_in_offset` are propagated from the outer host. Look for where `subplugin_host_api` is populated.

**Step 6:** Commit:

```bash
git add . && git commit -m "feat: stub DSP that reads mic audio in tick() — validates MIDI FX audio access"
```

---

## Task 3: Host-side test harness for envelope detection

Build a small C runner that feeds a WAV file (simulated breath envelope) through the breath detector and prints `breath` per block. This lets us iterate on envelope math without hardware round-trips.

**Files:**
- Create: `../schwung-breath/test/breath_runner.c`
- Create: `../schwung-breath/test/Makefile`
- Create: `../schwung-breath/test/gen_test_wav.py` (Python helper, host-only)

**Step 1:** Write a Python generator that produces a 44.1 kHz stereo int16 WAV with a synthesized breath envelope: 1 s silence + 1 s ramp-up to ~0.5 + 1 s hold + 1 s ramp-down + 1 s silence. Content is white noise scaled by the envelope (mic-like).

**Step 2:** Write `breath_runner.c` that:
- Reads a WAV via libsndfile (or stdlib if simpler).
- Loops in 128-frame blocks calling a `breath_block(int16_t *audio, int frames, breath_state_t *st)` function (extracted from `breath.c` into `breath_core.c` so it's testable host-side without the MIDI FX glue).
- Prints `block_num, peak, env_smooth, breath_norm, breath_curved` to stdout per block.

**Step 3:** Makefile target: `make test` builds and runs the runner against the generated WAV, comparing first few blocks' breath_norm to expected ranges (e.g., during the silence section, breath_norm should be near 0; during the hold, > 0.4).

**Step 4:** Commit:

```bash
git add test/ && git commit -m "test: host-side breath envelope runner with synthetic WAV"
```

**Verify:** `make test` exits 0, prints envelope trace showing rise → hold → fall.

---

## Task 4: Refactor — split DSP core from MIDI FX glue

To make the core testable, separate envelope/gating math from the plugin API plumbing.

**Files:**
- Create: `../schwung-breath/src/dsp/breath_core.h`
- Create: `../schwung-breath/src/dsp/breath_core.c`
- Modify: `../schwung-breath/src/dsp/breath.c` (delegate to `breath_core`)

**Step 1:** `breath_core.h` declares:

```c
typedef enum { BREATH_CURVE_LINEAR, BREATH_CURVE_LOG, BREATH_CURVE_EXP, BREATH_CURVE_S } breath_curve_t;
typedef enum { BREATH_VOICING_LEGATO, BREATH_VOICING_STACK } breath_voicing_t;

typedef struct {
    /* config */
    float breath_min, breath_max;
    breath_curve_t curve;
    float on_threshold, off_threshold;
    int cc_number;
    int send_aftertouch;
    int vel_from_breath;
    int vel_floor;
    breath_voicing_t voicing;
    /* runtime */
    float peak;            /* per-sample rectified peak with release */
    float env_smooth;      /* one-pole smoothed envelope */
    float breath;          /* current 0..1 post-curve */
    int blowing;
    uint64_t below_off_samples;  /* debounce counter */
    uint8_t held_pads[128];      /* simple presence array */
    int held_pads_lifo[32];      /* order of presses for "newest" lookup */
    int held_pads_lifo_count;
    uint8_t sounding[128];
    uint8_t last_cc_value;
    uint8_t last_at_value;
} breath_state_t;

void breath_init(breath_state_t *st);
void breath_set_config(breath_state_t *st /* … */);  /* simple setters */

/* Process one block of stereo audio. Returns nothing; updates st->breath. */
void breath_envelope_block(breath_state_t *st, const int16_t *audio_lr, int frames);

/* Note state machine inputs */
void breath_handle_pad_on(breath_state_t *st, uint8_t pitch, uint8_t velocity);
void breath_handle_pad_off(breath_state_t *st, uint8_t pitch);

/* Block-rate emit step: produces note on/off + cc + at messages.
 * Returns count written into out[]. */
int breath_emit_block(breath_state_t *st, uint8_t out[][3], int out_lens[], int max_out);
```

**Step 2:** Move stub logic from `breath.c` into `breath_core.c`. `breath.c` now wires plugin API to core: `process_midi` calls `breath_handle_pad_on/off`; `tick` calls `breath_envelope_block` then `breath_emit_block`.

**Step 3:** Update `test/Makefile` to compile against `breath_core.c`. Re-run `make test`.

**Step 4:** Commit:

```bash
git commit -am "refactor: split breath_core from MIDI FX glue"
```

**Verify:** `make test` still passes; behavior unchanged from Task 2 (debug_peak still works on device).

---

## Task 5: Implement envelope detection in breath_core

**File:** `../schwung-breath/src/dsp/breath_core.c`

**Step 1:** Constants:

```c
#define BREATH_SR 44100
/* 30ms attack: alpha = 1 - exp(-1/(SR*0.030)) = 1 - exp(-1/1323) ~ 0.000756 per sample;
 * but we run per-block so use per-block coef: 1 - exp(-frames/(SR*0.030)) */
#define BREATH_ATTACK_MS 30.0f
#define BREATH_RELEASE_MS 80.0f
```

**Step 2:** In `breath_envelope_block`:
- Per sample: `s = fabsf((L + R) * 0.5f / 32768.0f)`; `peak = fmaxf(peak * release_coef, s)`.
- After the loop: `env_smooth += (peak - env_smooth) * attack_coef_per_block`.
- Normalize: `n = clamp((env_smooth - breath_min) / (breath_max - breath_min), 0, 1)`.
- Apply curve (linear default — Task 6 adds the rest).
- Store in `st->breath`.

Compute `release_coef` once at init: `release_coef = expf(-1.0f / (BREATH_SR * BREATH_RELEASE_MS * 0.001f))`. Same for attack (per-block: `1.0f - expf(-128.0f / (BREATH_SR * BREATH_ATTACK_MS * 0.001f))`).

**Step 3:** Run `make test`. Confirm:
- Silence section: `breath` < 0.05.
- Ramp-up section: `breath` rises monotonically.
- Hold at 0.5 input: `breath` settles near 0.5 (post-normalize, since min ≈ 0 max ≈ 1 by default).
- Ramp-down: monotonic fall.

**Step 4:** Commit: `git commit -am "feat: envelope detection — rectified peak + one-pole smoother"`.

---

## Task 6: Implement curve types

**File:** `../schwung-breath/src/dsp/breath_core.c`

```c
static float apply_curve(float n, breath_curve_t c) {
    if (n <= 0.0f) return 0.0f;
    if (n >= 1.0f) return 1.0f;
    switch (c) {
        case BREATH_CURVE_LOG: return logf(1.0f + 9.0f * n) / logf(10.0f);
        case BREATH_CURVE_EXP: return n * n;
        case BREATH_CURVE_S:   return n * n * (3.0f - 2.0f * n);
        case BREATH_CURVE_LINEAR:
        default: return n;
    }
}
```

Use in `breath_envelope_block`. Extend `breath_runner.c` to take a `--curve` flag; re-run with each curve and visually check monotonicity + endpoints (0→0, 1→1).

**Commit:** `git commit -am "feat: breath curves (linear/log/exp/S)"`.

---

## Task 7: Threshold + hysteresis + blowing state

**File:** `../schwung-breath/src/dsp/breath_core.c`

**Step 1:** Inside `breath_envelope_block`, after computing `st->breath`:

```c
if (!st->blowing && st->breath > st->on_threshold) {
    st->blowing = 1;
    st->below_off_samples = 0;
    /* note emission handled in emit_block */
} else if (st->blowing) {
    if (st->breath < st->off_threshold) {
        st->below_off_samples += (uint64_t)frames;
        /* 30 ms debounce */
        if (st->below_off_samples >= (BREATH_SR * 30) / 1000) {
            st->blowing = 0;
        }
    } else {
        st->below_off_samples = 0;
    }
}
```

Track a `blowing_changed_this_block` boolean so `emit_block` can act on the transition.

**Step 2:** Extend `breath_runner.c` to print `blowing` per block. Verify with the test WAV: blowing flips on during ramp-up, stays on through hold, flips off during ramp-down (with the 30 ms debounce).

**Commit:** `git commit -am "feat: hysteretic breath on/off with debounce"`.

---

## Task 8: Note gating — Legato voicing

**File:** `../schwung-breath/src/dsp/breath_core.c`

**Step 1:** Implement `breath_handle_pad_on(st, pitch, vel)`:
- If pitch already in `held_pads`, just bump in LIFO; else add.
- If `st->blowing`:
  - If `sounding[]` empty → emit `note_on(pitch, vel_from_breath ? vel_from_breath_now() : 100)`. (Defer emission to `emit_block`; just queue.)
  - Else (something is sounding in Legato) → queue `note_off(prev_sounding)` + `note_on(pitch, vel_from_breath_now())`.
- If not blowing: just arm — nothing to emit now.

Emission queue: a small ring buffer of pending messages inside `breath_state_t` that `emit_block` drains.

**Step 2:** Implement `breath_handle_pad_off(st, pitch)`:
- Remove from `held_pads` and LIFO.
- If pitch was sounding: queue `note_off(pitch)`. If any pad still held → queue `note_on(newest_held_pad, vel_from_breath_now())`.

**Step 3:** In `breath_envelope_block`, when `blowing_changed_this_block`:
- On 0→1 transition: queue `note_on(newest_held_pad, vel_from_breath_now())` if any pad held.
- On 1→0 transition: queue `note_off` for everything in `sounding[]`.

**Step 4:** `breath_emit_block(st, out, out_lens, max_out)`:
- Drain queued messages into `out[]`; update `sounding[]` to reflect actual emissions.
- Cap at `max_out` (16 per the MIDI FX API contract); shouldn't exceed in practice.
- Append CC + AT in Task 10.

**Step 5:** Extend the test runner to inject synthetic pad note-on / note-off events at chosen block numbers. Tests for Legato:
- Hold pad A from block 0; blow ramps up at block 200. Expect `note_on(A)` exactly when `blowing` flips on.
- While blowing, press pad B at block 400. Expect `note_off(A) + note_on(B)`.
- Release pad B at block 500 while still blowing and pad A held. Expect `note_off(B) + note_on(A)`.
- Release pad A at block 600. Expect `note_off(A)` (no more pads held).
- Blow ramps down at block 800. Expect no extra events (nothing sounding).

**Commit:** `git commit -am "feat: Legato voicing — note gating state machine"`.

---

## Task 9: Note gating — Stack voicing

**File:** `../schwung-breath/src/dsp/breath_core.c`

**Step 1:** Implement Stack variant — branch on `st->voicing`:
- pad note-on while blowing → queue `note_on(pad)` directly; don't kill anything.
- pad note-off while blowing → queue `note_off(pad)` only if it's in `sounding[]`.
- Breath on transition → queue `note_on` for every held pad.
- Breath off transition → queue `note_off` for every sounding pad.

**Step 2:** Test:
- Hold A + B + C; blow on. Expect 3 `note_on`s within one block.
- Release B while blowing. Expect `note_off(B)`. A and C still sounding.
- Blow off. Expect `note_off(A) + note_off(C)`.

**Commit:** `git commit -am "feat: Stack voicing"`.

---

## Task 10: CC2 / aftertouch stream

**File:** `../schwung-breath/src/dsp/breath_core.c`

**Step 1:** At end of `breath_emit_block`:

```c
uint8_t v = (uint8_t)(st->breath * 127.0f + 0.5f);
if (v != st->last_cc_value) {
    out[count][0] = 0xB0 | (channel & 0x0F);
    out[count][1] = (uint8_t)st->cc_number;
    out[count][2] = v;
    out_lens[count++] = 3;
    st->last_cc_value = v;
}
if (st->send_aftertouch && v != st->last_at_value) {
    out[count][0] = 0xD0 | (channel & 0x0F);
    out[count][1] = v;
    out_lens[count++] = 2;
    st->last_at_value = v;
}
return count;
```

Note: MIDI FX API doesn't pass a channel — we use channel 0 by default and let the chain slot rewrite the channel downstream (same as built-in chord/arp FX do).

**Step 2:** Test: breath ramp up should emit CC2 with monotonically rising value. With `send_aftertouch=true`, AT messages should appear too. Both should drop with breath fade after note_off.

**Commit:** `git commit -am "feat: CC2 + channel aftertouch stream"`.

---

## Task 11: Parameter plumbing

**File:** `../schwung-breath/src/dsp/breath.c` (the MIDI FX glue)

**Step 1:** Implement `set_param` for each param key in the design (voicing, breath_min, breath_max, curve, on_threshold, off_threshold, cc_number, send_aftertouch, vel_from_breath, vel_floor). Convert string to typed value, write into `breath_state_t`.

For `voicing`: accept `"Legato"` / `"Stack"`. For `curve`: accept `"Linear"` / `"Log"` / `"Exp"` / `"S"`.

**Step 2:** Implement `get_param` to return current values as strings. Add `breath` (read-only, current envelope value 0..1) for the live meter.

**Step 3:** Implement `chain_params` get_param return — the JSON array of param metadata:

```c
return snprintf(buf, blen,
    "["
    "{\"key\":\"voicing\",\"name\":\"Voicing\",\"type\":\"enum\",\"options\":[\"Legato\",\"Stack\"]},"
    "{\"key\":\"breath_min\",\"name\":\"Min\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.001},"
    "{\"key\":\"breath_max\",\"name\":\"Max\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.001},"
    "{\"key\":\"curve\",\"name\":\"Curve\",\"type\":\"enum\",\"options\":[\"Linear\",\"Log\",\"Exp\",\"S\"]},"
    "{\"key\":\"on_threshold\",\"name\":\"On Thresh\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"off_threshold\",\"name\":\"Off Thresh\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"cc_number\",\"name\":\"CC #\",\"type\":\"int\",\"min\":0,\"max\":119},"
    "{\"key\":\"send_aftertouch\",\"name\":\"Send AT\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
    "{\"key\":\"vel_from_breath\",\"name\":\"Vel from Breath\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
    "{\"key\":\"vel_floor\",\"name\":\"Vel Floor\",\"type\":\"int\",\"min\":1,\"max\":127}"
    "]");
```

**Step 4:** Define the full `ui_hierarchy` in `module.json` per the design (root + calibration + output sub-levels).

**Step 5:** Build, install, smoke test on device — confirm params show up in Shadow UI and reading/writing them works.

**Commit:** `git commit -am "feat: full param surface + ui_hierarchy"`.

---

## Task 12: Calibration command

**Files:**
- Modify: `../schwung-breath/src/dsp/breath_core.c`, `breath.c`

**Step 1:** Add a calibration state machine to `breath_state_t`:

```c
typedef enum {
    CAL_IDLE,
    CAL_NOISE,    /* 3s noise floor capture */
    CAL_PEAK,     /* 3s peak capture */
    CAL_DONE
} cal_phase_t;

cal_phase_t cal_phase;
uint64_t cal_samples_remaining;
float cal_sum;       /* for RMS during noise phase */
float cal_peak_max;  /* for peak phase */
uint64_t cal_sample_count;
```

**Step 2:** `set_param(inst, "run_calibration", "noise")` → enter `CAL_NOISE` state, 3s remaining, reset accumulators. Likewise `"peak"` → `CAL_PEAK`. `"cancel"` → back to `CAL_IDLE`.

**Step 3:** In `breath_envelope_block`, when not idle:
- During `CAL_NOISE`: accumulate `sum(s²)`, `count`. When samples exhausted: `noise_rms = sqrt(sum/count)`; `breath_min = noise_rms * 1.5`; advance to `CAL_DONE`.
- During `CAL_PEAK`: track running max of `peak` (already computed). When done: `breath_max = peak_max * 0.95`; `CAL_DONE`.

**Step 4:** `get_param("cal_phase", …)` returns `"idle"|"noise"|"peak"|"done"`. `get_param("cal_progress", …)` returns 0..1 float for progress bar.

**Step 5:** Update test runner: simulate calibration sequence on the test WAV, verify `breath_min` and `breath_max` end up reasonable values.

**Commit:** `git commit -am "feat: two-point calibration state machine"`.

---

## Task 13: JS UI — root page and chain UI shim

**Files:**
- Create: `../schwung-breath/src/ui.js`
- Create: `../schwung-breath/src/ui_chain.js` (optional; deferred per design, do minimal stub)

**Step 1:** Standalone `ui.js` (when module is loaded directly, not in chain) renders the param page using `shared/menu_layout.mjs` patterns from other modules. The root has 4 live knobs (Voicing, Curve, On Thresh, Off Thresh) + a sub-menu list for Calibration and Output.

Look at `../schwung-keydetect/src/ui.js` for the pattern (similar audio-input module).

**Step 2:** When in chain context, the Shadow UI renders directly from `ui_hierarchy` in module.json. The JS file matters less. Provide a minimal stub for direct-load.

**Step 3:** Test on hardware: load standalone → param page renders. Then add to chain → Shadow UI renders the hierarchy.

**Commit:** `git commit -am "feat: ui.js param page"`.

---

## Task 14: JS UI — calibration screen

**File:** `../schwung-breath/src/ui.js`

The Shadow UI / chain context doesn't easily render multi-screen calibration flows from the param hierarchy alone. The cleanest approach: the "Run Calibration" entry in the Calibration sub-level is a *button* that, when activated, calls `host_module_set_param("run_calibration", "noise")` and the module's `get_param("cal_phase", …)` is shown live in the same screen with a countdown.

**Step 1:** In `module.json` ui_hierarchy, add a synthetic param `run_calibration` (write-only trigger) and `cal_progress` (read-only). Shadow UI's existing knob/value rendering shows them as fields.

A simpler approach for v1: implement calibration UI **only in standalone mode** (`ui.js`), since standalone gives full JS screen control. The chain Shadow UI just shows current Min / Max with a note to "Open the Breath module standalone to calibrate" — acceptable for a first cut.

**Step 2:** In `ui.js` standalone, add a "Calibrate" menu entry that walks the user through:
- Screen 1: "Stay quiet. 3…2…1…" then captures while showing a live bar of incoming `breath`. Triggered by `set_param("run_calibration", "noise")`. Polls `get_param("cal_phase")` until "done".
- Screen 2: same for peak.
- Screen 3: shows resulting min/max; jog click confirms (no extra action — already stored).

**Commit:** `git commit -am "feat: standalone calibration flow"`.

---

## Task 15: JS UI — live breath meter

**File:** `../schwung-breath/src/ui.js`

**Step 1:** On the root param page, render a 100-pixel horizontal bar at the bottom showing `get_param("breath")` (post-curve, 0..1). Refresh at the standard UI tick rate (~30 Hz). Mark `on_threshold` and `off_threshold` as vertical ticks on the bar.

**Step 2:** Verify on hardware: bar moves when you blow on the mic; ticks visible at correct positions.

**Commit:** `git commit -am "feat: live breath meter on root page"`.

---

## Task 16: Hardware tuning pass

No code changes upfront — this is empirical.

**Steps:**
1. Load Breath into a chain MIDI FX slot with DX7 as the sound generator.
2. Load a sustained DX7 patch (CC2 → output level) and hold a chord on pads.
3. Calibrate.
4. Blow and observe.

**Things to tune (commit each change as a separate small commit):**
- `BREATH_ATTACK_MS` / `BREATH_RELEASE_MS` — adjust if attack feels late or release flutters
- `on_threshold` / `off_threshold` defaults — adjust if false-triggers from room noise or if it takes too much breath to trigger
- 30 ms off-debounce — adjust if natural breath dips kill notes
- Default `curve` — Linear may feel too sensitive at low end; Log or S may feel better

Document the final tuning values in a "Defaults rationale" comment block in `breath_core.c`.

**Commit each tweak:** e.g. `tune: lower on_threshold to 0.05 after testing with DX7 brass`.

---

## Task 17: Update reference design with lessons learned

**File:** `../schwung/docs/plans/2026-05-17-schwung-breath-design.md`

After hardware testing, note any deviations from the design (e.g., chosen defaults, surprises in mic response, anything we punted on). Keep it factual; the design doc stays the canonical reference for v2 work.

**Commit:** in schwung repo: `docs: update breath design with implementation notes`.

---

## Task 18: Tag v0.1.0 release

**Steps:**
1. Bump version in `src/module.json` to `0.1.0`.
2. Update `release.json` on `main` to:
   ```json
   {
     "version": "0.1.0",
     "download_url": "https://github.com/charlesvestal/schwung-breath/releases/download/v0.1.0/breath-module.tar.gz"
   }
   ```
3. Commit: `release: v0.1.0`.
4. `git tag v0.1.0 && git push --tags`.
5. Wait for GitHub Actions to build and attach the tarball.
6. `gh release edit v0.1.0 -R charlesvestal/schwung-breath --notes "$(cat <<'EOF'
   Initial release. Breath-gated MIDI FX with Legato/Stack voicings, two-point calibration, and CC2/AT timbre stream.
   EOF
   )"`

---

## Task 19: Add catalog entry to schwung

**File:** `../schwung/module-catalog.json`

**Step 1:** Add entry under `modules`:

```json
{
  "id": "breath",
  "name": "Breath",
  "description": "Breath-gated MIDI FX. Blow into Move's mic; held notes sound while you blow.",
  "author": "charlesvestal",
  "component_type": "midi_fx",
  "github_repo": "charlesvestal/schwung-breath",
  "default_branch": "main",
  "asset_name": "breath-module.tar.gz",
  "min_host_version": "0.9.15"
}
```

(If we ended up changing `chain_host.c` to support audio access in MIDI FX, bump `min_host_version` to the next schwung release that ships that change, and increment schwung's `latest_version` accordingly.)

**Step 2:** Commit in schwung: `catalog: add Breath MIDI FX module`.

**Step 3:** Verify in Module Store on device — Breath shows up; install works; loads into chain.

---

## Done

The module is in the catalog, installable by anyone, and lives in its own repo on its own release cadence. v2 work (modulator mode, scale-lock, second envelope source) is tracked in the design doc as out-of-scope.
