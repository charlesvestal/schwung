# Gooderer Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build `schwung-gooderer/` — a new sibling Schwung audio FX repo implementing the validated Gooderer design (Soundgoodizer-flavored 3-band multiband + master comp + saturation, with an OTT-style upward branch grafted on via an `ottness` knob).

**Architecture:** Standalone repo cloned from the `schwung-cloudseed/` template. Plugin API v2 (multi-instance, chain-compatible). Single-TU DSP in `src/dsp/gooderer.c`. Float32 internal pipeline; int16 stereo at the boundary. Host-side WAV runner used as the primary test harness (TDD); device validation only at the end. Released via the existing GitHub Actions `tag → tarball` workflow and added to `schwung/module-catalog.json`.

**Tech Stack:** C (C99), GCC ARM cross-compiler via Docker, plugin API v2 from the host repo, Linkwitz-Riley 4th-order TPT/SVF crossovers, peak/RMS detectors, soft-clip tanh saturation.

**Reference docs:**
- Design: `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/docs/plans/2026-04-28-gooderer-design.md`
- Template: `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-cloudseed/`
- Plugin API v2: `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/src/host/plugin_api_v1.h` (lines 187-208) and `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/src/host/audio_fx_api_v2.h`
- Schwung CLAUDE.md sections: "Module Store", "Plugin API v2", "Chain Parameters"

**Source-of-truth mode tables:** see Design doc, sections "Mode tables".

**Memory note honored:** *"Mirror upstream main loop verbatim before refactoring; build host-side WAV runner first."* — Phase 2 below builds the runner *before* any DSP work, and Phase 3 unit-tests each DSP block via that runner.

---

## Repository layout (target)

```
schwung-gooderer/
├── .github/workflows/release.yml
├── .gitignore
├── CLAUDE.md
├── LICENSE
├── README.md
├── release.json
├── scripts/
│   ├── Dockerfile
│   ├── build.sh
│   └── install.sh
├── src/
│   ├── module.json
│   └── dsp/
│       ├── audio_fx_api_v2.h           (copied from host)
│       ├── plugin_api_v1.h             (copied from host, has v2 plugin types)
│       ├── gooderer.c                  (main DSP + V2 API entry point)
│       ├── lr4.c   lr4.h               (Linkwitz-Riley 4th-order crossover)
│       ├── detector.c   detector.h     (peak + RMS detectors)
│       ├── comp.c   comp.h             (compressor curves + smoothing)
│       ├── sat.c    sat.h              (soft-clip saturation)
│       └── modes.c  modes.h            (per-mode preset tables)
└── test/
    ├── runner.c                        (host-side WAV runner)
    ├── wav.c   wav.h                   (minimal RIFF WAV r/w)
    ├── stub_host.c                     (host_api_v1_t stub for tests)
    ├── Makefile                        (native dev-machine build)
    └── fixtures/
        ├── pink_-20dbfs.wav            (generated, 5 sec)
        ├── sine_sweep_-12dbfs.wav      (generated, 10 sec)
        └── static_levels_-40_to_0.wav  (generated, 41 sec)
```

---

## Phase 1 — Repo scaffolding

### Task 1.1: Create the repo by copying the cloudseed template

**Files:**
- Create: `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-gooderer/` (new directory)

**Step 1: Copy the cloudseed repo as the template**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent
cp -R schwung-cloudseed schwung-gooderer
cd schwung-gooderer
rm -rf .git build dist .DS_Store
git init
git checkout -b main
```

**Step 2: Wipe template-specific content that we'll rewrite**

```bash
# Remove cloudseed-specific DSP and tests
rm -f src/dsp/cloudseed.c
rm -f README.md CLAUDE.md release.json
# Keep src/dsp/plugin_api_v1.h and src/dsp/audio_fx_api_v1.h as starting headers — but we'll replace them with the host's current copies in task 1.3
rm -f src/dsp/audio_fx_api_v1.h
```

**Step 3: Verify the skeleton**

```bash
find . -type f | grep -v '.git/' | sort
```

Expected output: `LICENSE`, `scripts/build.sh`, `scripts/install.sh`, `scripts/Dockerfile`, `src/dsp/plugin_api_v1.h`, `src/help.json`, `src/module.json`, `.github/workflows/release.yml`, `.gitignore`.

**Step 4: First commit**

```bash
git add -A
git commit -m "scaffold: copy cloudseed template as starting point"
```

---

### Task 1.2: Fix up paths and IDs throughout

**Files:**
- Modify: `scripts/install.sh`
- Modify: `scripts/build.sh`
- Modify: `.github/workflows/release.yml`

**Step 1: Update install.sh — change `cloudseed` → `gooderer` and use the schwung path**

Cloudseed's install.sh writes to `/data/UserData/move-anything/modules/audio_fx/cloudseed`. The host has been renamed to `schwung`. Use the new path.

```bash
# scripts/install.sh, full replacement:
cat > scripts/install.sh <<'SH'
#!/bin/bash
# Install Gooderer module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/gooderer" ]; then
    echo "Error: dist/gooderer not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing Gooderer Module ==="

ssh ableton@move.local "mkdir -p /data/UserData/schwung/modules/audio_fx/gooderer"
scp -r dist/gooderer/* ableton@move.local:/data/UserData/schwung/modules/audio_fx/gooderer/
ssh ableton@move.local "chmod -R a+rw /data/UserData/schwung/modules/audio_fx/gooderer"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/schwung/modules/audio_fx/gooderer/"
SH
chmod +x scripts/install.sh
```

**Step 2: Update build.sh — change `cloudseed` → `gooderer`**

```bash
# Edit scripts/build.sh: replace every occurrence of cloudseed with gooderer.
sed -i.bak 's/cloudseed/gooderer/g; s/CloudSeed/Gooderer/g' scripts/build.sh
rm scripts/build.sh.bak
```

Verify with `grep -i cloudseed scripts/build.sh` — expected: no matches.

**Step 3: Update release.yml — change tarball name**

```bash
sed -i.bak 's/cloudseed-module/gooderer-module/g' .github/workflows/release.yml
rm .github/workflows/release.yml.bak
```

**Step 4: Sanity check — verify nothing references cloudseed**

```bash
grep -ri "cloudseed" . --exclude-dir=.git
```

Expected: no output (or just `LICENSE` if that file mentions it; check and clear if so).

**Step 5: Commit**

```bash
git add -A
git commit -m "fix: rename cloudseed → gooderer, update install path to /data/UserData/schwung/"
```

---

### Task 1.3: Sync plugin headers from the host repo

Cloudseed's headers are stale (still call the directory `move-anything`); the host's current headers are authoritative. Copy them into the new repo so the build is self-contained.

**Files:**
- Replace: `src/dsp/plugin_api_v1.h`
- Create: `src/dsp/audio_fx_api_v2.h`

**Step 1: Copy current host headers**

```bash
cp /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/src/host/plugin_api_v1.h src/dsp/plugin_api_v1.h
cp /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/src/host/audio_fx_api_v2.h src/dsp/audio_fx_api_v2.h
```

**Step 2: Verify the v2 plugin API and v2 audio FX API are present**

```bash
grep -n "AUDIO_FX_API_VERSION_2\|MOVE_PLUGIN_API_VERSION_2" src/dsp/*.h
```

Expected: matches in both files.

**Step 3: Commit**

```bash
git add src/dsp/
git commit -m "sync: copy current plugin_api and audio_fx_api headers from host"
```

---

### Task 1.4: Write README, CLAUDE.md, LICENSE, .gitignore, release.json

**Files:**
- Create: `README.md`, `CLAUDE.md`, `release.json`
- Modify: `.gitignore`
- Keep: `LICENSE` (MIT, from template — review for attribution)

**Step 1: Create README.md**

```markdown
# Gooderer

Soundgoodizer-flavored 3-band multiband processor for Schwung, with an
OTT-style upward branch on a separate macro.

Three controls:

- `amount`  — LMH mix (dry vs band-processed into master)
- `ottness` — upward-comp blend (0 = downward-only, Soundgoodizer-faithful)
- `mode`    — A / B / C / D (selects crossovers, thresholds, ratios, timings, saturation drives)

See `docs/plans/2026-04-28-gooderer-design.md` in the schwung repo for
full design rationale and the source-of-truth mode tables.

## Build

    ./scripts/build.sh

## Install on Move

    ./scripts/install.sh

## License

MIT.
```

**Step 2: Create CLAUDE.md (terse, just hooks for future work)**

```markdown
# CLAUDE.md — Gooderer

Schwung audio FX module. See:

- Design: `<schwung repo>/docs/plans/2026-04-28-gooderer-design.md`
- Plan:   `<schwung repo>/docs/plans/2026-04-28-gooderer-implementation.md`

## Test loop (host-side, no device needed)

    cd test && make && ./runner

## Build + deploy to Move

    ./scripts/build.sh
    ./scripts/install.sh

## Mode tables

The values in `src/dsp/modes.c` are derived from Goodizer.amxd (Effree
Meyer's Max for Live recreation of IL Maximus's A/B/C/D presets) and
cross-checked against the Image-Line manual + Slooply's per-mode
breakdown. Don't change them without re-validating against the design
doc's "Sources" section.
```

**Step 3: Create release.json (placeholder for v0.1.0, will be auto-updated by CI)**

```json
{
  "version": "0.0.0",
  "download_url": "https://github.com/charlesvestal/schwung-gooderer/releases/download/v0.0.0/gooderer-module.tar.gz"
}
```

**Step 4: Update .gitignore**

```
build/
dist/
.DS_Store
test/runner
test/*.o
test/fixtures/*.wav
```

**Step 5: Commit**

```bash
git add README.md CLAUDE.md release.json .gitignore
git commit -m "docs: README, CLAUDE.md, release.json, .gitignore"
```

---

## Phase 2 — Host-side WAV test runner (BEFORE any DSP)

This is the foundation. Every subsequent DSP task adds a test that runs in this harness. The runner must work end-to-end (read WAV → call a stub DSP → write WAV) before any DSP code is written.

### Task 2.1: Minimal RIFF WAV reader/writer

**Files:**
- Create: `test/wav.h`, `test/wav.c`

**Step 1: Write `test/wav.h`**

```c
#ifndef GOODERER_WAV_H
#define GOODERER_WAV_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int sample_rate;          /* 44100 expected */
    int channels;             /* 2 expected */
    int frames;               /* total frames (samples per channel) */
    int16_t *samples;         /* interleaved L,R,L,R,..., length = frames*channels */
} wav_t;

/* Returns 0 on success, non-zero on error. Caller frees w->samples on success. */
int wav_read(const char *path, wav_t *w);

/* Returns 0 on success. Writes 16-bit PCM stereo. */
int wav_write(const char *path, const wav_t *w);

void wav_free(wav_t *w);

#endif
```

**Step 2: Write `test/wav.c`**

Standard PCM-16 RIFF reader/writer; tolerate the standard `RIFF/WAVE/fmt /data` chunk layout and skip any unrecognised chunks. Around 100 lines. Don't support compressed formats — fail with an error message if `wFormatTag != 1` or `bitsPerSample != 16`.

(Reference: `move-anything/src/host/wav_writer.c` for the writer pattern; reader is a small parser.)

**Step 3: Smoke-test by reading and re-writing a fixture**

```c
/* test/test_wav_smoke.c (temporary file, deleted in step 5) */
#include "wav.h"
#include <stdio.h>
int main(void) {
    wav_t w;
    if (wav_read("fixtures/pink_-20dbfs.wav", &w)) { fprintf(stderr, "read failed\n"); return 1; }
    printf("read: %d frames, %d Hz, %d ch\n", w.frames, w.sample_rate, w.channels);
    if (wav_write("/tmp/roundtrip.wav", &w)) { fprintf(stderr, "write failed\n"); return 1; }
    wav_free(&w);
    return 0;
}
```

**Step 4: Build and run** (use a fixture you create with `sox` or any tool; if you don't have one yet, generate inline with a 1-second sine in C and write it directly with `wav_write`).

```bash
cd test
gcc -O2 -Wall test_wav_smoke.c wav.c -o test_wav_smoke
./test_wav_smoke
```

Expected: prints frame count and sample rate matching the input fixture.

**Step 5: Delete the smoke test, commit**

```bash
rm test_wav_smoke.c test_wav_smoke
git add test/wav.h test/wav.c
git commit -m "test: minimal RIFF WAV reader/writer"
```

---

### Task 2.2: Stub host_api and DSP no-op

**Files:**
- Create: `test/stub_host.c`
- Create: `src/dsp/gooderer.c` (no-op skeleton)

**Step 1: Stub host_api**

```c
/* test/stub_host.c */
#include "../src/dsp/plugin_api_v1.h"
#include <stdio.h>

static void stub_log(const char *msg) { fprintf(stderr, "[gooderer] %s\n", msg); }

const host_api_v1_t* stub_host(void) {
    static host_api_v1_t h = {0};
    h.log = stub_log;
    return &h;
}
```

(Inspect `plugin_api_v1.h` for the actual `host_api_v1_t` field set; fill the rest with NULL — the DSP must not call any other host APIs in v1 of this module.)

**Step 2: No-op DSP skeleton**

```c
/* src/dsp/gooderer.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_fx_api_v2.h"
#include "plugin_api_v1.h"

static const host_api_v1_t *g_host = NULL;

typedef struct {
    char module_dir[256];
    /* state will grow in later tasks */
} gooderer_t;

static void* v2_create_instance(const char *module_dir, const char *config_json) {
    (void)config_json;
    gooderer_t *inst = calloc(1, sizeof(*inst));
    if (!inst) return NULL;
    if (module_dir) strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    return inst;
}

static void v2_destroy_instance(void *p) { free(p); }

static void v2_process_block(void *p, int16_t *audio, int frames) {
    /* no-op — passthrough */
    (void)p; (void)audio; (void)frames;
}

static void v2_set_param(void *p, const char *k, const char *v) { (void)p; (void)k; (void)v; }
static int  v2_get_param(void *p, const char *k, char *buf, int n) { (void)p; (void)k; (void)buf; (void)n; return -1; }

static audio_fx_api_v2_t g_api;
audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    memset(&g_api, 0, sizeof(g_api));
    g_api.api_version     = AUDIO_FX_API_VERSION_2;
    g_api.create_instance = v2_create_instance;
    g_api.destroy_instance= v2_destroy_instance;
    g_api.process_block   = v2_process_block;
    g_api.set_param       = v2_set_param;
    g_api.get_param       = v2_get_param;
    return &g_api;
}
```

**Step 3: Commit**

```bash
git add test/stub_host.c src/dsp/gooderer.c
git commit -m "feat: no-op DSP skeleton + stub host_api for tests"
```

---

### Task 2.3: WAV runner driver and Makefile

**Files:**
- Create: `test/runner.c`
- Create: `test/Makefile`

**Step 1: Write `test/runner.c`**

The runner takes args: `<input.wav> <output.wav> <amount> <ottness> <mode>`. Loads input WAV, calls v2 API: create_instance → set_param × 3 → process_block in 128-frame chunks → destroy_instance, writes output WAV. Asserts sample rate is 44100 and channel count is 2.

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wav.h"
#include "../src/dsp/audio_fx_api_v2.h"
#include "../src/dsp/plugin_api_v1.h"

extern const host_api_v1_t* stub_host(void);
extern audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host);

int main(int argc, char **argv) {
    if (argc != 6) { fprintf(stderr, "usage: %s in.wav out.wav amount ottness mode\n", argv[0]); return 1; }
    const char *in_path = argv[1], *out_path = argv[2];
    const char *amount = argv[3], *ottness = argv[4], *mode = argv[5];

    wav_t w;
    if (wav_read(in_path, &w)) { fprintf(stderr, "read %s failed\n", in_path); return 2; }
    if (w.sample_rate != 44100 || w.channels != 2) {
        fprintf(stderr, "expected 44100 Hz stereo, got %d Hz %dch\n", w.sample_rate, w.channels);
        wav_free(&w);
        return 3;
    }

    audio_fx_api_v2_t *api = move_audio_fx_init_v2(stub_host());
    void *inst = api->create_instance(".", NULL);
    if (!inst) { fprintf(stderr, "create_instance failed\n"); wav_free(&w); return 4; }

    api->set_param(inst, "amount",  amount);
    api->set_param(inst, "ottness", ottness);
    api->set_param(inst, "mode",    mode);

    /* Process in 128-frame blocks (Move's native block size) */
    const int BLOCK = 128;
    for (int i = 0; i < w.frames; i += BLOCK) {
        int n = (w.frames - i < BLOCK) ? (w.frames - i) : BLOCK;
        api->process_block(inst, &w.samples[i * w.channels], n);
    }

    api->destroy_instance(inst);
    if (wav_write(out_path, &w)) { fprintf(stderr, "write %s failed\n", out_path); wav_free(&w); return 5; }
    wav_free(&w);
    fprintf(stderr, "ok: %d frames\n", w.frames);
    return 0;
}
```

**Step 2: Write `test/Makefile`**

```make
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c99 -fno-strict-aliasing -Isrc/dsp
LDFLAGS ?= -lm

DSP_SRC := ../src/dsp/gooderer.c
TEST_SRC := wav.c stub_host.c runner.c
SRCS := $(TEST_SRC) $(DSP_SRC)

runner: $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@

clean:
	rm -f runner *.o

.PHONY: clean
```

**Step 3: Build and run a passthrough test**

You'll need a 44100/stereo fixture. Generate one inline if needed — but easiest:

```bash
# Generate a simple 1-second 1 kHz sine fixture using sox (install if needed)
mkdir -p test/fixtures
sox -n -r 44100 -b 16 -c 2 test/fixtures/sine_1khz_-12dbfs.wav synth 1.0 sine 1000 vol -12dB
```

If sox isn't available, write a tiny C generator instead — but the plan assumes sox below. (Alternative: `ffmpeg`, or a 20-line C generator using `wav_write`.)

```bash
cd test && make && ./runner fixtures/sine_1khz_-12dbfs.wav /tmp/passthrough.wav 0.5 0.5 A
```

Expected stderr: `ok: 44100 frames`. The DSP is a no-op, so `/tmp/passthrough.wav` should be byte-identical to the input.

**Step 4: Verify byte-identity**

```bash
cmp test/fixtures/sine_1khz_-12dbfs.wav /tmp/passthrough.wav
```

Expected: no output (byte-identical).

**Step 5: Commit**

```bash
git add test/runner.c test/Makefile test/fixtures/
git commit -m "test: WAV runner driver with passthrough verification"
```

---

### Task 2.4: Generate validation fixtures

**Files:**
- Create: `test/fixtures/pink_-20dbfs.wav`, `sine_sweep_-12dbfs.wav`, `static_levels.wav`, `silence_1s.wav`
- Create: `test/scripts/generate_fixtures.sh`

**Step 1: Write the fixture generation script**

```bash
# test/scripts/generate_fixtures.sh
#!/bin/bash
set -e
cd "$(dirname "$0")/../fixtures"

# Pink noise at -20 dBFS, 5 sec
sox -n -r 44100 -b 16 -c 2 pink_-20dbfs.wav synth 5 pinknoise vol -20dB

# Sine sweep 20 Hz -> 20 kHz at -12 dBFS, 10 sec
sox -n -r 44100 -b 16 -c 2 sine_sweep_-12dbfs.wav synth 10 sine 20:20000 vol -12dB

# 1 sec of digital silence
sox -n -r 44100 -b 16 -c 2 silence_1s.wav synth 1 sine 1 vol 0
sox silence_1s.wav silence_1s.wav vol 0

# Static-level steps: -40, -39, ..., 0 dBFS, 1 sec each (41 sec total).
# We'll generate one tone at each level and concat.
rm -f static_levels.wav
TMPS=()
for lvl in $(seq -40 0); do
    f="/tmp/level_${lvl}.wav"
    sox -n -r 44100 -b 16 -c 2 "$f" synth 1 sine 1000 vol "${lvl}dB"
    TMPS+=("$f")
done
sox "${TMPS[@]}" static_levels.wav
rm "${TMPS[@]}"

echo "fixtures generated."
```

```bash
chmod +x test/scripts/generate_fixtures.sh
./test/scripts/generate_fixtures.sh
```

**Step 2: Verify all fixtures exist with correct properties**

```bash
for f in test/fixtures/*.wav; do
    soxi "$f"
done | grep -E "(File Size|Sample Rate|Channels)"
```

Expected: every file is `Sample Rate: 44100`, `Channels: 2`.

**Step 3: Commit (without the wavs — they're in .gitignore)**

```bash
git add test/scripts/generate_fixtures.sh
git commit -m "test: fixture-generation script (pink, sweep, static levels, silence)"
```

---

## Phase 3 — DSP units (TDD: each unit gets a focused test before integration)

### Task 3.1: LR4 crossover (Linkwitz-Riley 4th-order)

**Files:**
- Create: `src/dsp/lr4.h`, `src/dsp/lr4.c`
- Create: `test/test_lr4.c`
- Modify: `test/Makefile` (add `test_lr4` target)

**Step 1: Write the failing test**

A 3-band LR4 split (low/mid/high) summed back to a single signal must be **flat** (≤ -90 dB error vs original) at all frequencies away from the crossovers, with allpass correction on the mid band so phase aligns.

```c
/* test/test_lr4.c */
#include "../src/dsp/lr4.h"
#include "wav.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    /* Read a sine sweep, split into 3 bands, sum, compare to input. */
    wav_t w; if (wav_read("fixtures/sine_sweep_-12dbfs.wav", &w)) return 1;
    int n = w.frames;
    float *in_l  = malloc(sizeof(float)*n);
    float *in_r  = malloc(sizeof(float)*n);
    float *low_l = malloc(sizeof(float)*n), *low_r = malloc(sizeof(float)*n);
    float *mid_l = malloc(sizeof(float)*n), *mid_r = malloc(sizeof(float)*n);
    float *hi_l  = malloc(sizeof(float)*n), *hi_r  = malloc(sizeof(float)*n);

    for (int i = 0; i < n; ++i) {
        in_l[i] = w.samples[2*i+0] / 32768.0f;
        in_r[i] = w.samples[2*i+1] / 32768.0f;
    }

    lr4_3band_t s; lr4_3band_init(&s, 44100, 200.0f, 2871.0f);
    for (int i = 0; i < n; ++i) {
        lr4_3band_process(&s,
            in_l[i], in_r[i],
            &low_l[i], &low_r[i], &mid_l[i], &mid_r[i], &hi_l[i], &hi_r[i]);
    }

    /* Sum back. LR4 has even-order polarity that cancels naturally
       with allpass correction on mid; check sum. */
    double max_err = 0.0;
    /* skip first 1024 samples to let filters settle */
    for (int i = 1024; i < n; ++i) {
        double sum_l = (double)low_l[i] + mid_l[i] + hi_l[i];
        double err   = sum_l - in_l[i];
        if (fabs(err) > max_err) max_err = fabs(err);
    }
    fprintf(stderr, "lr4 sum max abs err: %g (%.2f dBFS)\n",
            max_err, 20.0 * log10(max_err > 0 ? max_err : 1e-30));
    /* Acceptance: ≤ -60 dBFS for now (LR4 sum is mathematically NOT flat
       in magnitude; the Linkwitz/Riley flat-magnitude property is for
       power sum, not signal sum). We test SHAPE, not bit-exactness.
       Use -60 dBFS as a sanity threshold. */
    if (max_err > 0.001) { fprintf(stderr, "FAIL\n"); return 1; }
    fprintf(stderr, "PASS\n");
    return 0;
}
```

**Note on the math:** LR4's classic "flat sum" property is for *power* (squared magnitude), not signal sum. The signal sum has an allpass response — phase rotates 360° across the crossover, magnitude is flat. So summing the bands gives the original signal **delayed/phase-rotated**, not bit-identical. The acceptance bar of `-60 dBFS` reflects "signal envelope intact, no notch at crossover." If you need a truly flat-sum split, switch to LR2 (2nd-order Butterworth squared = 4th-order LR sum is allpass, not flat) — but that's a v2 decision. Document this in `lr4.h` as a comment.

**Step 2: Run test — expect link error (lr4 functions not yet defined)**

```bash
cd test && make test_lr4 2>&1 | tail -5
```

Expected: link errors for `lr4_3band_init`, `lr4_3band_process`.

**Step 3: Implement `src/dsp/lr4.h`**

```c
#ifndef GOODERER_LR4_H
#define GOODERER_LR4_H

/* TPT-form (Zavalishin) state-variable filter, cascaded for LR4.
 * Stereo. One filter pair per band (lowpass + highpass at the same fc),
 * then squared (cascaded twice) for 24 dB/oct.
 *
 * Allpass correction: when summing low+mid+high signals, the LR4 sum is
 * an allpass (phase rotates, magnitude flat). For our purposes this is
 * acceptable — we process then sum, and the master comp stage smooths
 * any small reconstruction artefact. We do NOT add an explicit allpass
 * compensator on mid for v1.
 */

typedef struct {
    float g, R, h;          /* TPT coefficients */
    float s1_l, s2_l;       /* state, left */
    float s1_r, s2_r;       /* state, right */
} svf_t;

typedef struct {
    int sample_rate;
    svf_t lp_low_a,  lp_low_b;   /* cascaded LP at low/mid xover */
    svf_t hp_low_a,  hp_low_b;   /* cascaded HP at low/mid xover */
    svf_t lp_high_a, lp_high_b;  /* cascaded LP at mid/high xover */
    svf_t hp_high_a, hp_high_b;  /* cascaded HP at mid/high xover */
} lr4_3band_t;

void lr4_3band_init(lr4_3band_t *s, int sample_rate, float fc_low, float fc_high);

/* in_l, in_r: input samples; out parameters: per-band stereo outputs.
 * All pointers must be non-null. */
void lr4_3band_process(lr4_3band_t *s,
    float in_l, float in_r,
    float *low_l, float *low_r,
    float *mid_l, float *mid_r,
    float *hi_l,  float *hi_r);

#endif
```

**Step 4: Implement `src/dsp/lr4.c`**

```c
#include "lr4.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void svf_set(svf_t *f, int sr, float fc) {
    /* Zavalishin TPT: g = tan(pi*fc/sr), R = 1/(2*Q); for Butterworth Q=1/sqrt(2).
     * For 2nd-order Butterworth: R = 1/sqrt(2). */
    f->g = tanf((float)M_PI * fc / (float)sr);
    f->R = 0.7071067811865475f;          /* 1/sqrt(2) */
    f->h = 1.0f / (1.0f + 2.0f * f->R * f->g + f->g * f->g);
    /* states reset by caller */
}

static void svf_reset(svf_t *f) { f->s1_l = f->s2_l = f->s1_r = f->s2_r = 0.0f; }

/* Process one mono sample, returns LP and HP simultaneously. */
static inline void svf_process_lphp(svf_t *f, int channel, float x, float *lp, float *hp) {
    float *s1 = (channel == 0) ? &f->s1_l : &f->s1_r;
    float *s2 = (channel == 0) ? &f->s2_l : &f->s2_r;
    float yh = f->h * (x - (2.0f * f->R + f->g) * (*s1) - (*s2));
    float yb = f->g * yh + (*s1);
    float yl = f->g * yb + (*s2);
    *s1 = f->g * yh + yb;
    *s2 = f->g * yb + yl;
    *lp = yl;
    *hp = yh;
}

void lr4_3band_init(lr4_3band_t *s, int sample_rate, float fc_low, float fc_high) {
    s->sample_rate = sample_rate;
    svf_set(&s->lp_low_a,  sample_rate, fc_low);
    svf_set(&s->lp_low_b,  sample_rate, fc_low);
    svf_set(&s->hp_low_a,  sample_rate, fc_low);
    svf_set(&s->hp_low_b,  sample_rate, fc_low);
    svf_set(&s->lp_high_a, sample_rate, fc_high);
    svf_set(&s->lp_high_b, sample_rate, fc_high);
    svf_set(&s->hp_high_a, sample_rate, fc_high);
    svf_set(&s->hp_high_b, sample_rate, fc_high);
    svf_reset(&s->lp_low_a);  svf_reset(&s->lp_low_b);
    svf_reset(&s->hp_low_a);  svf_reset(&s->hp_low_b);
    svf_reset(&s->lp_high_a); svf_reset(&s->lp_high_b);
    svf_reset(&s->hp_high_a); svf_reset(&s->hp_high_b);
}

void lr4_3band_process(lr4_3band_t *s,
    float in_l, float in_r,
    float *low_l, float *low_r,
    float *mid_l, float *mid_r,
    float *hi_l,  float *hi_r)
{
    /* Low band: cascaded LP at fc_low (twice) */
    float t1l, t1r, dummy_l, dummy_r;
    svf_process_lphp(&s->lp_low_a, 0, in_l, &t1l, &dummy_l);
    svf_process_lphp(&s->lp_low_a, 1, in_r, &t1r, &dummy_r);
    svf_process_lphp(&s->lp_low_b, 0, t1l, low_l, &dummy_l);
    svf_process_lphp(&s->lp_low_b, 1, t1r, low_r, &dummy_r);

    /* Above-low band: cascaded HP at fc_low (twice) */
    float al_l, al_r;
    svf_process_lphp(&s->hp_low_a, 0, in_l, &dummy_l, &t1l);
    svf_process_lphp(&s->hp_low_a, 1, in_r, &dummy_r, &t1r);
    svf_process_lphp(&s->hp_low_b, 0, t1l, &dummy_l, &al_l);
    svf_process_lphp(&s->hp_low_b, 1, t1r, &dummy_r, &al_r);

    /* Mid band: LP cascaded at fc_high on the above-low signal */
    float t2l, t2r;
    svf_process_lphp(&s->lp_high_a, 0, al_l, &t2l, &dummy_l);
    svf_process_lphp(&s->lp_high_a, 1, al_r, &t2r, &dummy_r);
    svf_process_lphp(&s->lp_high_b, 0, t2l, mid_l, &dummy_l);
    svf_process_lphp(&s->lp_high_b, 1, t2r, mid_r, &dummy_r);

    /* High band: HP cascaded at fc_high on the above-low signal */
    svf_process_lphp(&s->hp_high_a, 0, al_l, &dummy_l, &t2l);
    svf_process_lphp(&s->hp_high_a, 1, al_r, &dummy_r, &t2r);
    svf_process_lphp(&s->hp_high_b, 0, t2l, &dummy_l, hi_l);
    svf_process_lphp(&s->hp_high_b, 1, t2r, &dummy_r, hi_r);
}
```

**Step 5: Add `test_lr4` to Makefile**

```make
test_lr4: test_lr4.c wav.c ../src/dsp/lr4.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@
```

**Step 6: Run test — expect PASS**

```bash
cd test && make test_lr4 && ./test_lr4
```

Expected: `lr4 sum max abs err: <small> (-NN dBFS)\nPASS`. If the error is large (>-60 dBFS), check filter coefficient signs and the input/output assignments in `process_lphp`.

**Step 7: Commit**

```bash
git add src/dsp/lr4.h src/dsp/lr4.c test/test_lr4.c test/Makefile
git commit -m "feat(dsp): TPT/SVF Linkwitz-Riley 4th-order 3-band crossover"
```

---

### Task 3.2: Peak + RMS detectors

**Files:**
- Create: `src/dsp/detector.h`, `src/dsp/detector.c`
- Create: `test/test_detector.c`
- Modify: `test/Makefile`

**Step 1: Write the failing test**

Drive a peak detector with a +6 dBFS step (full-scale square), assert the output settles to ≈1.0 within ~2 attack time-constants. Drive an RMS detector with a 60 Hz sine at -12 dBFS, assert RMS reads ≈ -15 dBFS (sin RMS is amplitude/sqrt(2) → -3 dB).

```c
/* test/test_detector.c */
#include "../src/dsp/detector.h"
#include <math.h>
#include <stdio.h>

int main(void) {
    /* Peak detector: step response */
    peak_det_t p; peak_det_init(&p, 44100, 0.001f /*1ms attack*/, 0.050f /*50ms release*/);
    float v = 0;
    for (int i = 0; i < 4410; ++i) v = peak_det_process(&p, 1.0f);
    if (fabsf(v - 1.0f) > 0.01f) { fprintf(stderr, "peak settle FAIL: %g\n", v); return 1; }

    /* RMS: 60 Hz sine at amplitude 0.25 (~ -12 dBFS) -> RMS = 0.25/sqrt(2) ~ 0.1768 */
    rms_det_t r; rms_det_init(&r, 44100, 0.010f);
    float rms = 0;
    for (int i = 0; i < 44100; ++i) {
        float x = 0.25f * sinf(2.0f * (float)M_PI * 60.0f * i / 44100.0f);
        rms = rms_det_process(&r, x);
    }
    float expected = 0.25f / sqrtf(2.0f);
    if (fabsf(rms - expected) / expected > 0.05f) {
        fprintf(stderr, "rms FAIL: got %g, want %g\n", rms, expected);
        return 1;
    }
    fprintf(stderr, "detector PASS (peak=%g, rms=%g vs %g)\n", v, rms, expected);
    return 0;
}
```

**Step 2: Run — expect link errors**

**Step 3: Implement `detector.h` + `detector.c`**

```c
/* detector.h */
#ifndef GOODERER_DETECTOR_H
#define GOODERER_DETECTOR_H

typedef struct { float a_a, a_r; float env; } peak_det_t;

void  peak_det_init(peak_det_t *d, int sr, float attack_s, float release_s);
float peak_det_process(peak_det_t *d, float x);

typedef struct { float a; float ms; } rms_det_t;

void  rms_det_init(rms_det_t *d, int sr, float window_s);
float rms_det_process(rms_det_t *d, float x);

#endif
```

```c
/* detector.c */
#include "detector.h"
#include <math.h>

void peak_det_init(peak_det_t *d, int sr, float a, float r) {
    d->a_a = expf(-1.0f / (sr * (a > 1e-6f ? a : 1e-6f)));
    d->a_r = expf(-1.0f / (sr * (r > 1e-6f ? r : 1e-6f)));
    d->env = 0.0f;
}

float peak_det_process(peak_det_t *d, float x) {
    float ax = fabsf(x);
    if (ax > d->env) d->env = d->a_a * d->env + (1.0f - d->a_a) * ax;
    else             d->env = d->a_r * d->env + (1.0f - d->a_r) * ax;
    return d->env;
}

void rms_det_init(rms_det_t *d, int sr, float w) {
    d->a = expf(-1.0f / (sr * (w > 1e-6f ? w : 1e-6f)));
    d->ms = 0.0f;
}

float rms_det_process(rms_det_t *d, float x) {
    d->ms = d->a * d->ms + (1.0f - d->a) * x * x;
    return sqrtf(d->ms);
}
```

**Step 4: Add to Makefile, run test**

```make
test_detector: test_detector.c ../src/dsp/detector.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@
```

```bash
cd test && make test_detector && ./test_detector
```

Expected: `detector PASS (peak=1.0..., rms=0.176..., vs 0.176...)`.

**Step 5: Commit**

```bash
git add src/dsp/detector.h src/dsp/detector.c test/test_detector.c test/Makefile
git commit -m "feat(dsp): peak and RMS detectors with one-pole smoothing"
```

---

### Task 3.3: Compressor curve (downward + upward) with soft knee

**Files:**
- Create: `src/dsp/comp.h`, `src/dsp/comp.c`
- Create: `test/test_comp.c`
- Modify: `test/Makefile`

**Step 1: Failing test — verify the static input/output curve**

For known input dB and threshold dB, the gain reduction must match the textbook compressor formula with a quadratic soft knee. Sample 5 levels (-40, -30, -20, -10, 0 dBFS) at threshold = -20 dB, ratio = 4:1, knee = 6 dB:
- input ≤ -23 dB: gain = 0 dB (below knee)
- input ≥ -17 dB: gain = -(input - (-20)) * (1 - 1/4)
- in knee: quadratic blend

```c
/* test/test_comp.c */
#include "../src/dsp/comp.h"
#include <math.h>
#include <stdio.h>

static float db(float lin) { return 20.0f * log10f(fmaxf(lin, 1e-30f)); }

int main(void) {
    struct { float in_db, expect_gain_db; } cases[] = {
        { -40, 0.0f },
        { -30, 0.0f },
        { -20, 0.0f - 0.5625f },   /* halfway through 6 dB knee centred at -20:
                                      with 4:1 ratio, gr = (1-1/r)*(input_above_thresh+knee/2)^2/(2*knee)
                                      at exactly thresh, input above = 0, knee/2 = 3, so gr ≈ (3/4)*9/12 = 0.5625 */
        { -10, -7.5f },             /* well above knee: gr = -(in - thresh)*(1 - 1/r) = -(10)*(0.75) = -7.5 dB */
        {   0, -15.0f },            /* gr = -(20)*(0.75) = -15 */
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        float lin = powf(10.0f, cases[i].in_db / 20.0f);
        float gr_db = comp_gain_down_db(lin, -20.0f, 4.0f, 6.0f);
        if (fabsf(gr_db - cases[i].expect_gain_db) > 0.5f) {
            fprintf(stderr, "FAIL at %.0f dBFS: got %g dB, want %g dB\n",
                    cases[i].in_db, gr_db, cases[i].expect_gain_db);
            ++fails;
        }
    }
    /* Mirror test for upward branch: */
    {
        float lin = powf(10.0f, -30.0f / 20.0f);
        float gu = comp_gain_up_db(lin, -20.0f, 3.0f, 6.0f);
        /* below threshold by 10 dB; ratio 3:1 → gain_up = (10)*(1 - 1/3) = 6.66 dB */
        if (fabsf(gu - 6.66f) > 0.5f) {
            fprintf(stderr, "upward FAIL: got %g, want ~6.66\n", gu);
            ++fails;
        }
    }
    if (fails) return 1;
    fprintf(stderr, "comp PASS\n");
    return 0;
}
```

**Step 2: Run — expect link errors**

**Step 3: Implement `comp.h` + `comp.c`**

```c
/* comp.h */
#ifndef GOODERER_COMP_H
#define GOODERER_COMP_H

/* Returns gain reduction in dB (≤ 0). */
float comp_gain_down_db(float input_lin, float threshold_db, float ratio, float knee_db);

/* Returns gain *boost* in dB (≥ 0). Symmetric mirror. */
float comp_gain_up_db(float input_lin, float threshold_db, float ratio, float knee_db);

#endif
```

```c
/* comp.c */
#include "comp.h"
#include <math.h>

static float lin_to_db(float x) {
    return 20.0f * log10f(x > 1e-30f ? x : 1e-30f);
}

float comp_gain_down_db(float input_lin, float thresh_db, float ratio, float knee_db) {
    float in_db = lin_to_db(input_lin);
    float over = in_db - thresh_db;
    float slope = 1.0f - 1.0f / ratio;
    if (over <= -0.5f * knee_db) return 0.0f;
    if (over >=  0.5f * knee_db) return -slope * over;
    /* quadratic knee */
    float k = over + 0.5f * knee_db;
    return -slope * (k * k) / (2.0f * knee_db);
}

float comp_gain_up_db(float input_lin, float thresh_db, float ratio, float knee_db) {
    float in_db = lin_to_db(input_lin);
    float under = thresh_db - in_db;        /* dB below threshold */
    float slope = 1.0f - 1.0f / ratio;
    if (under <= -0.5f * knee_db) return 0.0f;
    if (under >=  0.5f * knee_db) return slope * under;
    float k = under + 0.5f * knee_db;
    return slope * (k * k) / (2.0f * knee_db);
}
```

**Step 4: Run test — expect PASS**

```bash
cd test && make test_comp && ./test_comp
```

**Step 5: Commit**

```bash
git add src/dsp/comp.h src/dsp/comp.c test/test_comp.c test/Makefile
git commit -m "feat(dsp): compressor static curves (down + up) with soft knee"
```

---

### Task 3.4: Soft saturation

**Files:**
- Create: `src/dsp/sat.h`, `src/dsp/sat.c`
- Create: `test/test_sat.c`
- Modify: `test/Makefile`

**Step 1: Failing test — verify shape**

`sat(x, drive)` should be near-linear at low input (slope ≈ 1) and clip soft at the rails. At drive=1.0 it's pure tanh (gentle), at drive=2.0 it's harder.

```c
#include "../src/dsp/sat.h"
#include <math.h>
#include <stdio.h>

int main(void) {
    /* drive=1.0, small input: slope ≈ 1 */
    float y = sat_tanh(0.01f, 1.0f);
    if (fabsf(y - 0.01f) > 0.001f) { fprintf(stderr, "FAIL near-lin: %g\n", y); return 1; }

    /* drive=1.0, x=1.0: should be < 1.0 (clipped) */
    y = sat_tanh(1.0f, 1.0f);
    if (y >= 1.0f || y < 0.7f) { fprintf(stderr, "FAIL clip: %g\n", y); return 1; }

    /* sign symmetric */
    y = sat_tanh(-0.5f, 1.0f);
    float yp = sat_tanh(0.5f, 1.0f);
    if (fabsf(y + yp) > 1e-6f) { fprintf(stderr, "FAIL symmetry\n"); return 1; }

    fprintf(stderr, "sat PASS\n");
    return 0;
}
```

**Step 2: Run — link errors**

**Step 3: Implement**

```c
/* sat.h */
#ifndef GOODERER_SAT_H
#define GOODERER_SAT_H

/* Drive >= 1.0. drive=1.0 is gentle. Output is normalized so |y| <= 1.0
 * given |x| <= 1.0 (output of tanh(drive*x)/tanh(drive)). */
float sat_tanh(float x, float drive);

#endif
```

```c
/* sat.c */
#include "sat.h"
#include <math.h>

float sat_tanh(float x, float drive) {
    if (drive <= 1.0001f) return tanhf(x);
    return tanhf(drive * x) / tanhf(drive);
}
```

**Step 4: Run + commit**

```bash
cd test && make test_sat && ./test_sat
git add src/dsp/sat.h src/dsp/sat.c test/test_sat.c test/Makefile
git commit -m "feat(dsp): soft-tanh saturator"
```

---

### Task 3.5: Mode preset tables

**Files:**
- Create: `src/dsp/modes.h`, `src/dsp/modes.c`

**Step 1: Implement** (no failing test — this is pure data resolution; verified indirectly through the integration test in Task 4.1)

```c
/* modes.h */
#ifndef GOODERER_MODES_H
#define GOODERER_MODES_H

typedef enum { MODE_A = 0, MODE_B = 1, MODE_C = 2, MODE_D = 3 } mode_id_t;

typedef struct {
    /* Crossovers (Hz) */
    float fc_low, fc_high;
    /* Per band: low=0, mid=1, high=2 */
    float t_down_db[3];
    float spread_max_db[3];
    float ratio[3];
    float attack_s[3];          /* converted from ms */
    float release_s[3];
    float makeup_db[3];
    float band_drive[3];
    int   band_bypass[3];       /* mode B: all 3 bands bypassed */
    /* Master stage */
    float master_t_db;
    float master_ratio;
    float master_attack_s;
    float master_release_s;
    float master_drive;
} mode_preset_t;

extern const mode_preset_t MODE_PRESETS[4];

mode_id_t mode_from_string(const char *s);  /* "A".."D" or "a".."d" */

#endif
```

```c
/* modes.c */
#include "modes.h"
#include <string.h>

#define MS(x) ((x) * 0.001f)

const mode_preset_t MODE_PRESETS[4] = {
    /* A */
    { .fc_low=200.0f, .fc_high=2871.0f,
      .t_down_db    ={-18.f, -33.f, -36.f},
      .spread_max_db={ 24.f,  24.f,  24.f},
      .ratio        ={ 68.f,  16.f,   9.f},
      .attack_s     ={ MS(150), MS(121), MS(150) },
      .release_s    ={ MS(12),  MS(139), MS(1)   },   /* 0 ms → 1 ms */
      .makeup_db    ={ 3.f,    3.f,    3.f },
      .band_drive   ={ 1.4f,   1.0f,   1.4f },
      .band_bypass  ={ 0,      0,      0   },
      .master_t_db=0.f, .master_ratio=12.f,
      .master_attack_s=MS(150), .master_release_s=MS(150),
      .master_drive=1.2f },
    /* B */
    { .fc_low=200.0f, .fc_high=3000.0f,
      .t_down_db    ={ 0.f,   0.f,   0.f },     /* unused */
      .spread_max_db={ 0.f,   0.f,   0.f },
      .ratio        ={ 1.f,   1.f,   1.f },
      .attack_s     ={ MS(140), MS(145), MS(145) },
      .release_s    ={ MS(65),  MS(75),  MS(75)  },
      .makeup_db    ={ 0.f,   0.f,   0.f },
      .band_drive   ={ 1.0f,  1.0f,  1.0f },
      .band_bypass  ={ 1,     1,     1 },
      .master_t_db=-5.f, .master_ratio=12.f,
      .master_attack_s=MS(145), .master_release_s=MS(150),
      .master_drive=1.4f },
    /* C */
    { .fc_low=200.0f, .fc_high=2297.0f,
      .t_down_db    ={-36.f, -21.f, -32.f},
      .spread_max_db={ 30.f,  24.f,  30.f},
      .ratio        ={ 68.f,  13.f,  65.f},
      .attack_s     ={ MS(99), MS(1),  MS(35) },
      .release_s    ={ MS(109),MS(65), MS(99) },
      .makeup_db    ={ 5.f,    3.f,    5.f },
      .band_drive   ={ 1.6f,   1.0f,   1.6f },
      .band_bypass  ={ 0,      0,      0   },
      .master_t_db=-36.f, .master_ratio=65.f,
      .master_attack_s=MS(1), .master_release_s=MS(99),
      .master_drive=1.3f },
    /* D */
    { .fc_low=58.0f, .fc_high=9304.0f,
      .t_down_db    ={-36.f, -10.f, -15.f},
      .spread_max_db={ 36.f,  36.f,  36.f},
      .ratio        ={ 15.f,  16.f,  41.f},
      .attack_s     ={ MS(1), MS(1), MS(46) },
      .release_s    ={ MS(117),MS(107),MS(94) },
      .makeup_db    ={ 4.f,   4.f,   5.f },
      .band_drive   ={ 1.8f,  1.5f,  1.8f },
      .band_bypass  ={ 0,     0,     0   },
      .master_t_db=0.f, .master_ratio=37.f,
      .master_attack_s=MS(10), .master_release_s=MS(150),
      .master_drive=1.6f },
};

mode_id_t mode_from_string(const char *s) {
    if (!s) return MODE_A;
    char c = s[0];
    if (c >= 'a') c -= ('a' - 'A');
    switch (c) {
        case 'B': return MODE_B;
        case 'C': return MODE_C;
        case 'D': return MODE_D;
        default:  return MODE_A;
    }
}
```

**Step 2: Compile-only check (no test needed yet)**

```bash
cd test && make runner   # links modes.c into the no-op DSP — should still build clean
```

**Step 3: Commit**

```bash
git add src/dsp/modes.h src/dsp/modes.c
git commit -m "feat(dsp): per-mode preset tables (A/B/C/D from Goodizer.amxd)"
```

---

## Phase 4 — Integration

### Task 4.1: Wire LR4 + per-band processor + master into the V2 plugin

**Files:**
- Modify: `src/dsp/gooderer.c`
- Create: `test/test_passthrough.c`
- Modify: `test/Makefile`

**Step 1: Failing integration test — `amount=0` should be (close to) passthrough through master**

At `amount=0`, the band path is muted; only the master stage processes the dry input. With `mode=A` and master sat drive 1.2 + mild master comp, the output won't be byte-identical, but with input *well below* the master threshold (mode A T_master=0 dBFS), the master comp shouldn't engage and the only colouration is master saturation, which is gentle. Bound: ≤ 1 dB RMS deviation from input on a -20 dBFS pink noise input.

```c
/* test/test_passthrough.c */
#include "wav.h"
#include "../src/dsp/audio_fx_api_v2.h"
#include "../src/dsp/plugin_api_v1.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

extern const host_api_v1_t* stub_host(void);
extern audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host);

static double rms_dbfs(const int16_t *s, int frames, int channels) {
    double ss = 0;
    int n = frames * channels;
    for (int i = 0; i < n; ++i) {
        double v = s[i] / 32768.0;
        ss += v * v;
    }
    return 20.0 * log10(sqrt(ss / n));
}

int main(void) {
    wav_t in_w, out_w;
    if (wav_read("fixtures/pink_-20dbfs.wav", &in_w)) return 1;
    /* deep-copy in_w → out_w to avoid in-place mutation breaking RMS comparison */
    out_w = in_w;
    out_w.samples = malloc(in_w.frames * in_w.channels * sizeof(int16_t));
    memcpy(out_w.samples, in_w.samples, in_w.frames * in_w.channels * sizeof(int16_t));

    audio_fx_api_v2_t *api = move_audio_fx_init_v2(stub_host());
    void *inst = api->create_instance(".", NULL);
    api->set_param(inst, "amount", "0.0");
    api->set_param(inst, "ottness", "0.0");
    api->set_param(inst, "mode", "A");

    const int BLOCK = 128;
    for (int i = 0; i < out_w.frames; i += BLOCK) {
        int n = (out_w.frames - i < BLOCK) ? (out_w.frames - i) : BLOCK;
        api->process_block(inst, &out_w.samples[i*2], n);
    }
    api->destroy_instance(inst);

    double in_rms  = rms_dbfs(in_w.samples,  in_w.frames,  2);
    double out_rms = rms_dbfs(out_w.samples, out_w.frames, 2);
    fprintf(stderr, "amount=0 dry-through-master: in=%.2f dBFS, out=%.2f dBFS\n", in_rms, out_rms);
    if (fabs(out_rms - in_rms) > 1.5) { fprintf(stderr, "FAIL: deviation > 1.5 dB\n"); return 1; }
    fprintf(stderr, "PASS\n");
    return 0;
}
```

**Step 2: Run — expect FAIL (no-op DSP currently outputs the input unchanged → passes the bound trivially, but we want the test to drive the implementation)**

If it passes trivially against the no-op, that's fine for now — the *real* signal that this task works comes from the next assertion (Task 4.2 testing `amount=1`).

**Step 3: Replace `gooderer.c` with the full implementation**

This is the largest single step in the plan. The file structure:

```c
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "audio_fx_api_v2.h"
#include "plugin_api_v1.h"
#include "lr4.h"
#include "detector.h"
#include "comp.h"
#include "sat.h"
#include "modes.h"

#define SR 44100

typedef struct {
    /* Public params */
    float amount;
    float ottness;
    mode_id_t mode;

    /* Active mode preset (resolved on mode change) */
    mode_preset_t preset;

    /* Crossover */
    lr4_3band_t xover;

    /* Per-band detectors */
    rms_det_t det_low;       /* RMS for low (slow attack on bass) */
    peak_det_t det_mid;
    peak_det_t det_high;

    /* Per-band gain smoothers */
    float gain_smooth[3];
    float a_a[3], a_r[3];    /* attack/release coefs per band */

    /* Master detector + smoother */
    peak_det_t det_master;
    float master_gain_smooth;
    float master_a_a, master_a_r;

    char module_dir[256];
} gooderer_t;

static const host_api_v1_t *g_host = NULL;

static void apply_mode(gooderer_t *g, mode_id_t m) {
    g->mode = m;
    g->preset = MODE_PRESETS[m];
    lr4_3band_init(&g->xover, SR, g->preset.fc_low, g->preset.fc_high);

    rms_det_init (&g->det_low,  SR, 0.010f);
    peak_det_init(&g->det_mid,  SR, 0.001f, g->preset.release_s[1]);
    peak_det_init(&g->det_high, SR, 0.001f, g->preset.release_s[2]);

    for (int b = 0; b < 3; ++b) {
        float ta = g->preset.attack_s[b];
        float tr = g->preset.release_s[b];
        g->a_a[b] = expf(-1.0f / (SR * (ta > 1e-6f ? ta : 1e-6f)));
        g->a_r[b] = expf(-1.0f / (SR * (tr > 1e-6f ? tr : 1e-6f)));
        g->gain_smooth[b] = 1.0f;
    }

    peak_det_init(&g->det_master, SR, 0.001f, g->preset.master_release_s);
    g->master_a_a = expf(-1.0f / (SR * g->preset.master_attack_s));
    g->master_a_r = expf(-1.0f / (SR * g->preset.master_release_s));
    g->master_gain_smooth = 1.0f;
}

static inline float db_to_lin(float db) { return powf(10.0f, db * 0.05f); }

static inline float smooth_gain(float new_lin, float *state, float a_a, float a_r) {
    float c = (new_lin < *state) ? a_a : a_r;   /* dropping = attacking */
    *state = c * (*state) + (1.0f - c) * new_lin;
    return *state;
}

/* ---------- V2 API ---------- */

static void* v2_create_instance(const char *module_dir, const char *config_json) {
    (void)config_json;
    gooderer_t *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    if (module_dir) strncpy(g->module_dir, module_dir, sizeof(g->module_dir) - 1);
    g->amount = 0.5f;
    g->ottness = 0.5f;
    apply_mode(g, MODE_A);
    return g;
}

static void v2_destroy_instance(void *p) { free(p); }

static void v2_set_param(void *p, const char *k, const char *v) {
    gooderer_t *g = (gooderer_t*)p;
    if (!g || !k || !v) return;
    if      (strcmp(k, "amount")  == 0) { float x = strtof(v, NULL); if (x < 0) x = 0; if (x > 1) x = 1; g->amount = x; }
    else if (strcmp(k, "ottness") == 0) { float x = strtof(v, NULL); if (x < 0) x = 0; if (x > 1) x = 1; g->ottness = x; }
    else if (strcmp(k, "mode")    == 0) { apply_mode(g, mode_from_string(v)); }
}

static int v2_get_param(void *p, const char *k, char *buf, int n) {
    gooderer_t *g = (gooderer_t*)p;
    if (!g || !k || !buf || n <= 0) return -1;
    if      (strcmp(k, "amount")  == 0) return snprintf(buf, n, "%.4f", g->amount);
    else if (strcmp(k, "ottness") == 0) return snprintf(buf, n, "%.4f", g->ottness);
    else if (strcmp(k, "mode")    == 0) return snprintf(buf, n, "%c",  "ABCD"[g->mode]);
    else if (strcmp(k, "chain_params") == 0) {
        const char *json =
            "[{\"key\":\"amount\",\"name\":\"Amount\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"ottness\",\"name\":\"OTTness\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"A\",\"B\",\"C\",\"D\"]}]";
        int len = (int)strlen(json);
        if (len < n) { memcpy(buf, json, len + 1); return len; }
        return -1;
    }
    return -1;
}

static void v2_process_block(void *p, int16_t *audio, int frames) {
    gooderer_t *g = (gooderer_t*)p;
    if (!g) return;

    for (int i = 0; i < frames; ++i) {
        float l = audio[2*i + 0] / 32768.0f;
        float r = audio[2*i + 1] / 32768.0f;

        /* --- Band path --- */
        float ll, lr, ml, mr, hl, hr;
        lr4_3band_process(&g->xover, l, r, &ll, &lr, &ml, &mr, &hl, &hr);

        float bl = 0, br = 0;   /* band-path summed output */
        struct { float *l, *r; float det; int bidx; } bands[3] = {
            { &ll, &lr, 0, 0 },
            { &ml, &mr, 0, 1 },
            { &hl, &hr, 0, 2 },
        };

        /* Detectors */
        float lin_low = fabsf(ll) > fabsf(lr) ? fabsf(ll) : fabsf(lr);
        float lin_mid = fabsf(ml) > fabsf(mr) ? fabsf(ml) : fabsf(mr);
        float lin_hi  = fabsf(hl) > fabsf(hr) ? fabsf(hl) : fabsf(hr);
        bands[0].det = rms_det_process(&g->det_low,  lin_low);
        bands[1].det = peak_det_process(&g->det_mid,  lin_mid);
        bands[2].det = peak_det_process(&g->det_high, lin_hi);

        for (int b = 0; b < 3; ++b) {
            float bL = *bands[b].l, bR = *bands[b].r;
            if (g->preset.band_bypass[b]) {
                bl += bL; br += bR;
                continue;
            }

            float det = bands[b].det + 1e-30f;
            float gd = comp_gain_down_db(det, g->preset.t_down_db[b], g->preset.ratio[b], 6.0f);
            float t_up = g->preset.t_down_db[b] - g->preset.spread_max_db[b];
            float gu = comp_gain_up_db  (det, t_up,                     g->preset.ratio[b], 6.0f) * g->ottness;
            float total_db = gd + gu;
            if (total_db < -30.0f) total_db = -30.0f;
            if (total_db >  24.0f) total_db =  24.0f;

            float target_lin = db_to_lin(total_db);
            float gain = smooth_gain(target_lin, &g->gain_smooth[b], g->a_a[b], g->a_r[b]);

            bL *= gain; bR *= gain;
            float makeup = db_to_lin(g->preset.makeup_db[b]);
            bL *= makeup; bR *= makeup;

            float drive = 1.0f + (g->preset.band_drive[b] - 1.0f) * g->amount;
            bL = sat_tanh(bL, drive);
            bR = sat_tanh(bR, drive);

            bl += bL; br += bR;
        }

        /* --- LMH mix: dry vs band-processed → master input --- */
        float ml_in_l = (1.0f - g->amount) * l + g->amount * bl;
        float ml_in_r = (1.0f - g->amount) * r + g->amount * br;

        /* --- Master saturation (always on) --- */
        float master_drive = 1.0f + (g->preset.master_drive - 1.0f) * g->amount;
        float ms_l = sat_tanh(ml_in_l, master_drive);
        float ms_r = sat_tanh(ml_in_r, master_drive);

        /* --- Master comp (always on, downward only) --- */
        float md_in = fabsf(ms_l) > fabsf(ms_r) ? fabsf(ms_l) : fabsf(ms_r);
        float md_env = peak_det_process(&g->det_master, md_in + 1e-30f);
        float mg_db  = comp_gain_down_db(md_env, g->preset.master_t_db, g->preset.master_ratio, 6.0f);
        if (mg_db < -30.0f) mg_db = -30.0f;
        float mg_target = db_to_lin(mg_db);
        float mg = smooth_gain(mg_target, &g->master_gain_smooth, g->master_a_a, g->master_a_r);
        ms_l *= mg; ms_r *= mg;

        /* Clip + write back */
        if (ms_l >  1.0f) ms_l =  1.0f; if (ms_l < -1.0f) ms_l = -1.0f;
        if (ms_r >  1.0f) ms_r =  1.0f; if (ms_r < -1.0f) ms_r = -1.0f;
        audio[2*i + 0] = (int16_t)(ms_l * 32767.0f);
        audio[2*i + 1] = (int16_t)(ms_r * 32767.0f);
    }
}

/* Entry point */
static audio_fx_api_v2_t g_api;
audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    memset(&g_api, 0, sizeof(g_api));
    g_api.api_version     = AUDIO_FX_API_VERSION_2;
    g_api.create_instance = v2_create_instance;
    g_api.destroy_instance= v2_destroy_instance;
    g_api.process_block   = v2_process_block;
    g_api.set_param       = v2_set_param;
    g_api.get_param       = v2_get_param;
    if (g_host && g_host->log) g_host->log("[gooderer] init v2");
    return &g_api;
}
```

**Step 4: Update Makefile to include all DSP TUs**

```make
DSP_SRC := ../src/dsp/gooderer.c ../src/dsp/lr4.c ../src/dsp/detector.c \
           ../src/dsp/comp.c ../src/dsp/sat.c ../src/dsp/modes.c
TEST_SRC := wav.c stub_host.c

runner: $(TEST_SRC) runner.c $(DSP_SRC)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_passthrough: $(TEST_SRC) test_passthrough.c $(DSP_SRC)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@
```

**Step 5: Run all tests — passthrough + the prior unit tests**

```bash
cd test && make runner test_lr4 test_detector test_comp test_sat test_passthrough
./test_lr4 && ./test_detector && ./test_comp && ./test_sat && ./test_passthrough
```

Expected: all PASS. If `test_passthrough` fails (output deviates >1.5 dB at amount=0), check (a) master comp threshold isn't engaging on -20 dBFS pink (master_t_db=0 in mode A → should not engage), (b) master saturation drive at amount=0 is `1.0` (gentle tanh → -20 dBFS pink barely changes).

**Step 6: Commit**

```bash
git add src/dsp/gooderer.c test/test_passthrough.c test/Makefile
git commit -m "feat: integrated LR4 + 3-band comp + master comp/sat with V2 API"
```

---

### Task 4.2: Loudness validation — pink noise gain at amount=1, mode A, ottness=0

**Files:**
- Create: `test/test_loudness.c`
- Modify: `test/Makefile`

**Step 1: Failing test**

```c
/* test/test_loudness.c — assert mode A at amount=1, ottness=0 raises pink RMS by 5..9 dB */
#include "wav.h"
#include "../src/dsp/audio_fx_api_v2.h"
#include "../src/dsp/plugin_api_v1.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

extern const host_api_v1_t* stub_host(void);
extern audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host);
static double rms_dbfs(const int16_t *s, int n) { double ss = 0; for (int i = 0; i < n; ++i) { double v = s[i]/32768.0; ss += v*v; } return 20.0*log10(sqrt(ss/n)); }

int main(void) {
    wav_t w; if (wav_read("fixtures/pink_-20dbfs.wav", &w)) return 1;
    int16_t *out = malloc(w.frames * 2 * sizeof(int16_t));
    memcpy(out, w.samples, w.frames * 2 * sizeof(int16_t));

    audio_fx_api_v2_t *api = move_audio_fx_init_v2(stub_host());
    void *inst = api->create_instance(".", NULL);
    api->set_param(inst, "amount",  "1.0");
    api->set_param(inst, "ottness", "0.0");
    api->set_param(inst, "mode",    "A");

    for (int i = 0; i < w.frames; i += 128) {
        int n = (w.frames - i < 128) ? (w.frames - i) : 128;
        api->process_block(inst, &out[i*2], n);
    }
    api->destroy_instance(inst);

    double in_db  = rms_dbfs(w.samples, w.frames * 2);
    double out_db = rms_dbfs(out, w.frames * 2);
    fprintf(stderr, "mode A amount=1 ottness=0: in %.2f -> out %.2f dBFS (delta %+.2f)\n", in_db, out_db, out_db - in_db);
    if (out_db - in_db < 5.0 || out_db - in_db > 9.0) {
        fprintf(stderr, "FAIL: expected +5..+9 dB\n"); return 1;
    }
    fprintf(stderr, "PASS\n");
    return 0;
}
```

**Step 2: Run — may fail in either direction**

If output is *quieter*, makeup is too low or master comp is over-clamping. If output is *much louder*, drives or makeup are too high. Tune `MODE_PRESETS[MODE_A].makeup_db` first (currently +3 dB across bands; bump to +5 if needed).

**Step 3: Commit (with whatever tuning is needed)**

```bash
git add test/test_loudness.c test/Makefile src/dsp/modes.c
git commit -m "test: validate mode A loudness gain at amount=1 (5..9 dB rise on pink)"
```

---

### Task 4.3: Static-level S-curve sanity (mode A, manual inspection)

**Files:**
- Create: `test/test_curve.c` (writes processed `static_levels.wav` for visual inspection — no auto-assert)

```c
/* test/test_curve.c — process the static-level fixture, write to /tmp/curve_modeA.wav.
 * Inspect manually: per-second average should plateau above thresholds. */
#include "wav.h"
#include "../src/dsp/audio_fx_api_v2.h"
#include "../src/dsp/plugin_api_v1.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

extern const host_api_v1_t* stub_host(void);
extern audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host);

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "A";
    wav_t w; if (wav_read("fixtures/static_levels.wav", &w)) return 1;
    audio_fx_api_v2_t *api = move_audio_fx_init_v2(stub_host());
    void *inst = api->create_instance(".", NULL);
    api->set_param(inst, "amount",  "1.0");
    api->set_param(inst, "ottness", "0.0");
    api->set_param(inst, "mode",    mode);

    for (int i = 0; i < w.frames; i += 128) {
        int n = (w.frames - i < 128) ? (w.frames - i) : 128;
        api->process_block(inst, &w.samples[i*2], n);
    }
    api->destroy_instance(inst);

    char path[256]; snprintf(path, sizeof(path), "/tmp/curve_mode%s.wav", mode);
    wav_write(path, &w);

    /* Print per-second peak (rough static curve) */
    for (int sec = 0; sec < w.frames / 44100; ++sec) {
        int start = sec * 44100, end = start + 44100;
        double pk = 0;
        for (int i = start; i < end; ++i) {
            double v = fabs(w.samples[i*2] / 32768.0);
            if (v > pk) pk = v;
        }
        fprintf(stderr, "in %d dBFS -> out peak %.2f dBFS\n", -40 + sec, 20.0 * log10(pk + 1e-30));
    }
    fprintf(stderr, "wrote %s\n", path);
    return 0;
}
```

**Step 1: Run for all four modes**

```bash
cd test && make test_curve
for m in A B C D; do ./test_curve $m; done
```

Check by eye: the printed table should rise nearly linearly from -40 dBFS up to roughly the per-band threshold, then flatten or compress as levels approach -20 to 0 dBFS. Mode B should rise nearly 1:1 (master comp barely engaged at -5 dBFS threshold for sub-threshold input).

**Step 2: Commit**

```bash
git add test/test_curve.c test/Makefile
git commit -m "test: per-mode static-curve dump for manual S-curve inspection"
```

---

### Task 4.4: Mode-B sanity — bypass branch behaves correctly

**Files:**
- Create: `test/test_mode_b.c`

**Step 1: Failing test — at mode B, varying amount should change RMS by ≤ 2 dB (since bands are bypassed, only LMH-mix between dry-to-master and band-summed-to-master differs, and the band sum equals dry plus tiny LR4 reconstruction error)**

```c
/* test/test_mode_b.c */
/* For mode B, amount=0 vs amount=1 should produce nearly identical RMS
 * because the bands pass-through (bypass=1) and the band sum reconstructs
 * the dry input. Difference comes from master saturation drive scaling
 * with amount. */
#include "wav.h"
#include "../src/dsp/audio_fx_api_v2.h"
#include "../src/dsp/plugin_api_v1.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const host_api_v1_t* stub_host(void);
extern audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host);

static double rms_dbfs(const int16_t *s, int n) {
    double ss = 0; for (int i = 0; i < n; ++i) { double v = s[i]/32768.0; ss += v*v; }
    return 20.0*log10(sqrt(ss/n));
}

static double process_at(const wav_t *src, const char *amount) {
    int n = src->frames * 2;
    int16_t *buf = malloc(n * sizeof(int16_t));
    memcpy(buf, src->samples, n * sizeof(int16_t));
    audio_fx_api_v2_t *api = move_audio_fx_init_v2(stub_host());
    void *inst = api->create_instance(".", NULL);
    api->set_param(inst, "ottness", "0.0");
    api->set_param(inst, "mode", "B");
    api->set_param(inst, "amount", amount);
    for (int i = 0; i < src->frames; i += 128) {
        int k = (src->frames - i < 128) ? (src->frames - i) : 128;
        api->process_block(inst, &buf[i*2], k);
    }
    api->destroy_instance(inst);
    double db = rms_dbfs(buf, n);
    free(buf); return db;
}

int main(void) {
    wav_t w; if (wav_read("fixtures/pink_-20dbfs.wav", &w)) return 1;
    double r0 = process_at(&w, "0.0");
    double r1 = process_at(&w, "1.0");
    fprintf(stderr, "mode B amount=0: %.2f, amount=1: %.2f, delta %.2f\n", r0, r1, r1 - r0);
    if (fabs(r1 - r0) > 2.0) { fprintf(stderr, "FAIL\n"); return 1; }
    fprintf(stderr, "PASS\n");
    return 0;
}
```

**Step 2: Run, commit**

```bash
cd test && make test_mode_b && ./test_mode_b
git add test/test_mode_b.c test/Makefile
git commit -m "test: mode B amount-sweep delta ≤ 2 dB (bypass + master only)"
```

---

## Phase 5 — Module manifest + chain integration

### Task 5.1: Write `src/module.json`

**Files:**
- Replace: `src/module.json` (overwriting cloudseed's content)

**Step 1: Write the manifest**

```json
{
  "id": "gooderer",
  "name": "Gooderer",
  "abbrev": "GD",
  "version": "0.1.0",
  "description": "Soundgoodizer-flavored multiband + master comp + sat, with OTT upward branch",
  "author": "charlesvestal",
  "license": "MIT",
  "dsp": "gooderer.so",
  "api_version": 2,
  "capabilities": {
    "chainable": true,
    "component_type": "audio_fx",
    "ui_hierarchy": {
      "modes": null,
      "levels": {
        "root": {
          "name": "Gooderer",
          "knobs": ["amount", "mode"],
          "params": [
            {"key": "amount",  "label": "Amount",  "type": "float", "min": 0.0, "max": 1.0, "default": 0.5, "step": 0.01},
            {"key": "mode",    "label": "Mode",    "type": "enum",  "options": ["A","B","C","D"], "default": "A"},
            {"key": "ottness", "label": "OTTness", "type": "float", "min": 0.0, "max": 1.0, "default": 0.5, "step": 0.01}
          ]
        }
      }
    }
  }
}
```

**Step 2: Validate JSON**

```bash
python3 -c "import json; json.load(open('src/module.json'))"
```

**Step 3: Commit**

```bash
git add src/module.json
git commit -m "feat: module.json — chainable audio_fx with amount/ottness/mode"
```

---

### Task 5.2: help.json (terse module help)

**Files:**
- Replace: `src/help.json`

```json
{
  "title": "Gooderer",
  "summary": "Multiband Soundgoodizer + OTT.",
  "params": {
    "amount":  "Wet/dry between dry input and band-processed signal into the master stage.",
    "ottness": "0 = downward-only (Maximus-like). 1 = full upward+downward (OTT slam).",
    "mode":    "A = balanced. B = master glue + sat. C = aggressive/loud. D = coloration."
  }
}
```

```bash
git add src/help.json
git commit -m "docs: help.json"
```

---

## Phase 6 — Build, deploy, release

### Task 6.1: Build for ARM64 in Docker

**Files:**
- Verify: `scripts/Dockerfile`, `scripts/build.sh`

**Step 1: Inspect build.sh**

The renamed `build.sh` (Task 1.2) should already invoke `${CROSS_PREFIX}gcc` and create `dist/gooderer/` plus `dist/gooderer-module.tar.gz`. Verify this end-to-end is what the script does. The cloudseed version is the reference; the `sed` should have done the right thing.

**Step 2: Run a dry build (no Docker)**

If you have aarch64 cross-compilers locally:

```bash
CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh
```

Otherwise, run via Docker:

```bash
./scripts/build.sh
```

**Step 3: Verify the tarball**

```bash
ls -la dist/
file dist/gooderer/gooderer.so
```

Expected: `dist/gooderer-module.tar.gz` exists; `gooderer.so` is `ELF 64-bit LSB shared object, ARM aarch64`.

**Step 4: Verify exported symbol**

```bash
aarch64-linux-gnu-objdump -T dist/gooderer/gooderer.so | grep move_audio_fx_init_v2
# or, if no aarch64 binutils available:
docker run --rm -v "$PWD:/build" -w /build move-anything-builder \
  aarch64-linux-gnu-objdump -T dist/gooderer/gooderer.so | grep move_audio_fx_init_v2
```

Expected: a single matching line marking the symbol as exported.

**Step 5: Commit any build.sh fixes**

```bash
git status   # should be clean if no fixes were needed
# else: git add scripts/build.sh && git commit -m "fix: build.sh"
```

---

### Task 6.2: Deploy and test on Move device

**Files:** none modified.

**Step 1: Install**

```bash
./scripts/install.sh
```

Expected: ssh + scp succeed, files land in `/data/UserData/schwung/modules/audio_fx/gooderer/`.

**Step 2: Verify on device**

```bash
ssh ableton@move.local "ls -la /data/UserData/schwung/modules/audio_fx/gooderer/"
```

Expected: `gooderer.so`, `module.json`, `help.json`.

**Step 3: Restart Schwung host or just rescan**

In the Schwung UI, Tools → Module Store → Rescan, or restart the host service.

**Step 4: Add Gooderer to a Signal Chain slot, listen with mode A, sweep amount 0 → 1**

Manual ear test — confirm:
- amount=0: signal passes through with mild master sat colour
- amount=1: clearly more compressed, "louder" character
- mode A vs C vs D: distinguishable character differences
- mode B at amount=1 vs amount=0: subtle delta (master + sat only)
- ottness=0 vs ottness=1 at amount=1, mode A: ottness=1 should be more aggressive ("slammy")

**Step 5: If anything sounds wrong, return to Phase 4 tuning**

Document specific issues and which mode preset values to adjust before re-deploying. Each tuning iteration: edit `src/dsp/modes.c`, rebuild, install, listen.

---

### Task 6.3: Release workflow + first tag

**Files:**
- Verify: `.github/workflows/release.yml`
- Modify: `release.json` (sync to v0.1.0)

**Step 1: Bump version in module.json + release.json**

```bash
# src/module.json: version "0.1.0" (already)
# release.json: sync version to 0.1.0
cat > release.json <<'EOF'
{
  "version": "0.1.0",
  "download_url": "https://github.com/charlesvestal/schwung-gooderer/releases/download/v0.1.0/gooderer-module.tar.gz"
}
EOF
git add release.json
git commit -m "chore: bump release.json to v0.1.0"
```

**Step 2: Create the GitHub repo and push**

```bash
gh repo create charlesvestal/schwung-gooderer --public --source=. --remote=origin --push
```

**Step 3: Tag and push**

```bash
git tag v0.1.0
git push --tags
```

**Step 4: Wait for CI, verify release**

```bash
gh run watch -R charlesvestal/schwung-gooderer
gh release view v0.1.0 -R charlesvestal/schwung-gooderer
```

Expected: release exists with `gooderer-module.tar.gz` attached.

**Step 5: Add release notes**

```bash
gh release edit v0.1.0 -R charlesvestal/schwung-gooderer --notes "$(cat <<'EOF'
- Initial release: Gooderer audio FX module for Schwung
- Three controls: amount (LMH mix), ottness (upward-comp blend), mode (A/B/C/D)
- 3-band LR4 split + per-band comp + saturation + master comp
- Mode tables grounded in IL Maximus A/B/C/D presets
EOF
)"
```

---

### Task 6.4: Add Gooderer to schwung's module-catalog.json

**Files:**
- Modify: `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/module-catalog.json`

**Step 1: Read current catalog**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung
jq '.modules | map(.id)' module-catalog.json
```

**Step 2: Append the entry**

Add this object to the `.modules` array (use `jq` or hand-edit; preserve formatting):

```json
{
  "id": "gooderer",
  "name": "Gooderer",
  "description": "Soundgoodizer-flavored multiband + OTT (audio FX)",
  "author": "charlesvestal",
  "component_type": "audio_fx",
  "github_repo": "charlesvestal/schwung-gooderer",
  "default_branch": "main",
  "asset_name": "gooderer-module.tar.gz",
  "min_host_version": "0.1.0"
}
```

**Step 3: Validate**

```bash
python3 -c "import json; json.load(open('module-catalog.json'))"
```

**Step 4: Commit + push**

```bash
git add module-catalog.json
git commit -m "catalog: add Gooderer audio FX module"
git push
```

**Step 5: Verify in the Module Store on device**

In the Schwung UI: Module Store → refresh → "Gooderer" should appear with "Available 0.1.0".

---

## Phase 7 — Final validation pass on hardware

**Listening tests (on Move):**

1. Load Gooderer in a chain after a sound generator (e.g. drums or pad).
2. Mode A, amount=0: confirm passthrough character (mild glue).
3. Mode A, amount=1, ottness=0: expect Soundgoodizer A character — louder, fuller, "limited".
4. Mode A, amount=1, ottness=1: expect OTT-style pumping/slamming on top.
5. Mode B: subtle glue, master-driven.
6. Mode C: aggressive — heavy compression on lows + highs.
7. Mode D: coloration mode — heavy saturation, near-mono treatment.
8. Sweep amount and ottness slowly with both knobs in the chain UI; confirm both are smooth (no zipper noise) and audibly distinct.

**Final commit + tag-push if any tuning happened:**

If you tuned tables in Phase 4 testing, those changes should have been committed during Phase 4. If tuning happens during Phase 7, bump version (0.1.1 patch) and re-release per Task 6.3.

---

## Out of scope (v2 territory, do not implement now)

- Stereo widener (per-band M/S width)
- Per-band Input/Output trim
- Adjustable crossovers / ratios / times
- Sidechain
- Lookahead
- Oversampling
- LR2/IIR alternatives

These are noted in the Design doc's "Out of scope" section — leave them alone in v1.

---

## Acceptance criteria (whole plan)

- [ ] All test_*.c files pass on dev machine (Phase 3 + 4)
- [ ] Mode A loudness rises 5..9 dB on pink at amount=1, ottness=0 (Task 4.2)
- [ ] Mode B amount-sweep RMS delta ≤ 2 dB (Task 4.4)
- [ ] LR4 invariants pass: single-tone routing >85% in target band, crossover -6 dB-per-band split (~25%/25% at fc), sweep sum-magnitude ratio in [0.85, 1.15] (Task 3.1)
- [ ] `dist/gooderer/gooderer.so` exports `move_audio_fx_init_v2` (Task 6.1)
- [ ] Module loads in a Schwung Signal Chain slot, three params editable from chain UI (Task 6.2)
- [ ] GitHub release v0.1.0 published with tarball (Task 6.3)
- [ ] `module-catalog.json` updated and pushed (Task 6.4)
- [ ] Manual ear test passes for all four modes (Task 6.2 + Phase 7)
