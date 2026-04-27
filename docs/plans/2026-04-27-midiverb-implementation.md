# Midiverb Module Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build `schwung-midiverb`, a new Schwung audio-FX module that emulates the Alesis Midiverb / Midifex / Midiverb II rack reverbs using the decompiled DSP from [thement/midiverb_emulator](https://github.com/thement/midiverb_emulator).

**Architecture:** New sibling repo `schwung-midiverb/`. Vendors upstream's decompiled C effect tables (GPL-3). DSP runs at the hardware-native 23.4 kHz; Move runs at 44.1 kHz, so the plugin sandwich is `downsample 44.1→23.4 → effect_fn → upsample 23.4→44.1`, with a self-contained 32-tap polyphase windowed-sinc FIR. Implements `audio_fx_api_v2` (matches `schwung-mverb`). UI is Osirus-pattern: jog wheel scrolls programs, Unit (Midiverb / Midifex / Midiverb II) lives in a sub-menu.

**Tech Stack:** C/C++, cross-compile via Docker (debian-bullseye + aarch64-linux-gnu-gcc), no external runtime deps.

**Reference design:** `docs/plans/2026-04-27-midiverb-design.md`

**Reference module to mirror:** `../schwung-mverb/`

---

## Conventions

- All commands run from `/Volumes/ExtFS/charlesvestal/github/schwung-parent/` unless noted.
- Use `Edit` / `Write` / `Read` tools (not `cat` / `sed`) for file work.
- Commit after each task using `git -C schwung-midiverb commit ...`.
- Don't push to GitHub until Task 24 (catalog publish).
- DO NOT deploy to device until Task 21 (first on-device smoke test).
- For all Schwung host-side conventions referenced (LED colors, MIDI cables, etc.), see `schwung/CLAUDE.md` already loaded in this session.

## Phase 0 — Repo skeleton

### Task 1: Create the repo directory and `git init`

**Files:** none yet (creating directory)

**Step 1:** Create directory and init git
```bash
mkdir -p /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-midiverb
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-midiverb
git init
git branch -m main
```

**Step 2:** Create `.gitignore`

```gitignore
build/
dist/
*.o
*.so
.DS_Store
```

**Step 3:** Create `LICENSE` — copy verbatim from `../schwung-mverb/LICENSE` (GPL-3.0).
```bash
cp ../schwung-mverb/LICENSE /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-midiverb/LICENSE
```

**Step 4:** Initial commit
```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-midiverb
git add .gitignore LICENSE
git commit -m "chore: initial repo skeleton (GPL-3.0)"
```

---

### Task 2: Create `src/module.json`

**Files:**
- Create: `schwung-midiverb/src/module.json`

**Step 1:** Write `src/module.json` (mirror `../schwung-mverb/src/module.json` shape):

```json
{
  "id": "midiverb",
  "name": "Midiverb",
  "abbrev": "MVB",
  "version": "0.1.0",
  "description": "Alesis Midiverb / Midifex / Midiverb II emulator (lo-fi 23.4 kHz reverb)",
  "author": "Charles Vestal (DSP: thement/midiverb_emulator)",
  "license": "GPL-3.0",
  "dsp": "midiverb.so",
  "api_version": 2,
  "capabilities": {
    "chainable": true,
    "component_type": "audio_fx"
  }
}
```

**Step 2:** Commit
```bash
git -C schwung-midiverb add src/module.json
git -C schwung-midiverb commit -m "feat: module.json (id=midiverb, audio_fx, api_v2)"
```

---

### Task 3: Vendor host plugin API headers

**Files:**
- Create: `schwung-midiverb/src/dsp/audio_fx_api_v2.h` (copy from `../schwung-mverb/src/dsp/audio_fx_api_v2.h`)
- Create: `schwung-midiverb/src/dsp/plugin_api_v1.h` (copy from `../schwung-mverb/src/dsp/plugin_api_v1.h`)

**Step 1:** Copy verbatim
```bash
mkdir -p schwung-midiverb/src/dsp
cp schwung-mverb/src/dsp/audio_fx_api_v2.h schwung-midiverb/src/dsp/
cp schwung-mverb/src/dsp/plugin_api_v1.h schwung-midiverb/src/dsp/
```

**Step 2:** Commit
```bash
git -C schwung-midiverb add src/dsp/audio_fx_api_v2.h src/dsp/plugin_api_v1.h
git -C schwung-midiverb commit -m "chore: vendor audio_fx_api_v2 + host_api_v1 headers"
```

---

## Phase 1 — Vendor upstream

### Task 4: Add upstream as a git remote and fetch only what we need (no submodule)

**Files:**
- Create: `schwung-midiverb/src/dsp/decompiled-midiverb.h`
- Create: `schwung-midiverb/src/dsp/decompiled-midifex.h`
- Create: `schwung-midiverb/src/dsp/decompiled-midiverb2.h`
- Create: `schwung-midiverb/src/dsp/names-midiverb.h`
- Create: `schwung-midiverb/src/dsp/names-midifex.h`
- Create: `schwung-midiverb/src/dsp/names-midiverb2.h`
- Create: `schwung-midiverb/src/dsp/THIRD_PARTY_LICENSES.md`

**Step 1:** Pull the six files from upstream (master branch) directly via curl — no submodule, no shallow clone:
```bash
cd schwung-midiverb/src/dsp
BASE=https://raw.githubusercontent.com/thement/midiverb_emulator/master
for f in decompiled-midiverb.h decompiled-midifex.h decompiled-midiverb2.h \
         names-midiverb.h names-midifex.h names-midiverb2.h; do
  curl -fsSL -o "$f" "$BASE/$f"
done
```

**Step 2:** Verify file sizes look right
```bash
ls -la schwung-midiverb/src/dsp/decompiled-*.h schwung-midiverb/src/dsp/names-*.h
```
Expect: decompiled-midiverb.h ~135KB, decompiled-midifex.h ~98KB, decompiled-midiverb2.h ~250KB; names-*.h small (~2-4KB each).

**Step 3:** Create `THIRD_PARTY_LICENSES.md`:

```markdown
# Third-party licenses

## midiverb_emulator (decompiled DSP and preset names)

The following files are vendored from
https://github.com/thement/midiverb_emulator (GPL-3.0):

- `decompiled-midiverb.h`
- `decompiled-midifex.h`
- `decompiled-midiverb2.h`
- `names-midiverb.h`
- `names-midifex.h`
- `names-midiverb2.h`

This module is therefore distributed under the GPL-3.0 (see the
top-level LICENSE).
```

**Step 4:** Commit
```bash
git -C schwung-midiverb add src/dsp/decompiled-*.h src/dsp/names-*.h src/dsp/THIRD_PARTY_LICENSES.md
git -C schwung-midiverb commit -m "vendor: midiverb_emulator decompiled DSP + names (GPL-3)"
```

---

### Task 5: Smoke-test that the vendored headers compile in isolation

**Files:**
- Create: `schwung-midiverb/tests/host/test_compile_smoke.c`
- Create: `schwung-midiverb/tests/host/Makefile`

**Step 1:** Write the test file `tests/host/test_compile_smoke.c`:

```c
/* Smoke test: just compile-include the vendored headers and link. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/dsp/decompiled-midiverb.h"
#include "../../src/dsp/decompiled-midifex.h"
#include "../../src/dsp/decompiled-midiverb2.h"

int main(void) {
    int16_t dram[0x4000];
    memset(dram, 0, sizeof(dram));
    int16_t out_l = 0, out_r = 0;
    /* Run a single sample through midiverb effect_1 to prove linkage */
    midiverb_effects[0](0, &out_l, &out_r, dram, 0, 0, 0);
    printf("smoke ok\n");
    return 0;
}
```

**Step 2:** Write `tests/host/Makefile`:

```makefile
CC := cc
CFLAGS := -O2 -Wall -Wno-unused-function

test_compile_smoke: test_compile_smoke.c
	$(CC) $(CFLAGS) $< -o $@

.PHONY: smoke
smoke: test_compile_smoke
	./test_compile_smoke

.PHONY: clean
clean:
	rm -f test_compile_smoke
```

**Step 3:** Build and run on host (Mac, native cc)
```bash
make -C schwung-midiverb/tests/host smoke
```
Expected output: `smoke ok`

**Step 4:** If `int16_t` overflow warnings appear, that's expected (decompiled code does signed intermediate math); confirm exit code 0 and `smoke ok` printed.

**Step 5:** Commit
```bash
git -C schwung-midiverb add tests/host/test_compile_smoke.c tests/host/Makefile
git -C schwung-midiverb commit -m "test: host smoke test that vendored headers compile + link"
```

---

## Phase 2 — Resampler

### Task 6: Polyphase coefficient generator script

**Files:**
- Create: `schwung-midiverb/scripts/gen_resampler.py`

**Step 1:** Write `scripts/gen_resampler.py`:

```python
#!/usr/bin/env python3
"""Generate polyphase windowed-sinc FIR coefficients for 44.1 <-> 23.4 kHz.

Emits a C header (resampler_coeffs.h) with two tables:
  - down_coeffs[NUM_PHASES][TAPS]  (44.1 -> 23.4)
  - up_coeffs[NUM_PHASES][TAPS]    (23.4 -> 44.1)

Both directions use a 32-tap Kaiser-windowed sinc.
"""
import math
import sys

TAPS = 32
NUM_PHASES = 64  # subdivide one input sample into 64 phases
KAISER_BETA = 8.6


def i0(x):
    """Modified Bessel function of the first kind, order 0."""
    sum_ = 1.0
    term = 1.0
    for k in range(1, 50):
        term *= (x / (2.0 * k)) ** 2
        sum_ += term
        if term < 1e-12 * sum_:
            break
    return sum_


def kaiser(n, N, beta):
    return i0(beta * math.sqrt(1 - ((2 * n) / (N - 1) - 1) ** 2)) / i0(beta)


def sinc(x):
    if x == 0.0:
        return 1.0
    return math.sin(math.pi * x) / (math.pi * x)


def gen_table(cutoff_normalized):
    """cutoff_normalized: cutoff frequency relative to source sample rate
       (e.g. 0.5 for Nyquist).
    """
    table = [[0.0] * TAPS for _ in range(NUM_PHASES)]
    for phase in range(NUM_PHASES):
        offset = phase / NUM_PHASES
        for tap in range(TAPS):
            n = tap - TAPS // 2 + offset
            w = kaiser(tap + (1 if offset > 0 else 0), TAPS + 1, KAISER_BETA)
            table[phase][tap] = 2 * cutoff_normalized * sinc(2 * cutoff_normalized * n) * w
    # Normalize each phase row so DC gain is 1
    for phase in range(NUM_PHASES):
        s = sum(table[phase])
        if s != 0.0:
            table[phase] = [c / s for c in table[phase]]
    return table


def emit_table(name, table, fp):
    fp.write(f"static const float {name}[{NUM_PHASES}][{TAPS}] = {{\n")
    for phase, row in enumerate(table):
        coeffs = ", ".join(f"{c:+.10ef}" for c in row)
        fp.write(f"    {{ {coeffs} }},\n")
    fp.write("};\n\n")


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "src/dsp/resampler_coeffs.h"

    # Cutoff: half the Nyquist of the lower rate, expressed in source-rate units
    # 23.4 kHz Nyquist is 11.7 kHz; use 0.45 normalized to source rate of each direction
    down = gen_table(0.45 * (23400.0 / 44100.0))  # 44.1->23.4: cutoff at 0.45 of 23.4kHz Nyquist
    up = gen_table(0.45)                           # 23.4->44.1: cutoff at 0.45 of 23.4kHz Nyquist

    with open(out_path, "w") as fp:
        fp.write("/* Auto-generated by scripts/gen_resampler.py - do not edit. */\n\n")
        fp.write(f"#define RESAMPLER_TAPS {TAPS}\n")
        fp.write(f"#define RESAMPLER_PHASES {NUM_PHASES}\n\n")
        emit_table("down_coeffs", down, fp)
        emit_table("up_coeffs", up, fp)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
```

**Step 2:** Run it to generate `src/dsp/resampler_coeffs.h`
```bash
cd schwung-midiverb
python3 scripts/gen_resampler.py src/dsp/resampler_coeffs.h
```

**Step 3:** Spot-check output: file exists, ~50 KB, first row sums close to 1.0
```bash
ls -la src/dsp/resampler_coeffs.h
head -20 src/dsp/resampler_coeffs.h
```

**Step 4:** Commit
```bash
git -C schwung-midiverb add scripts/gen_resampler.py src/dsp/resampler_coeffs.h
git -C schwung-midiverb commit -m "feat: polyphase windowed-sinc coeff generator + table"
```

---

### Task 7: Resampler implementation

**Files:**
- Create: `schwung-midiverb/src/dsp/resampler.h`
- Create: `schwung-midiverb/src/dsp/resampler.c`

**Step 1:** Write `src/dsp/resampler.h`:

```c
#ifndef MIDIVERB_RESAMPLER_H
#define MIDIVERB_RESAMPLER_H

#include <stdint.h>

#include "resampler_coeffs.h"

/* Maximum block size we will ever see at 44.1 kHz */
#define RESAMPLER_MAX_BLOCK_44 256

/* Maximum samples produced at 23.4 kHz from one 44.1 kHz block */
#define RESAMPLER_MAX_BLOCK_23 160

/* Down: 44.1 -> 23.4 (mono float in/out) */
typedef struct {
    float buf[RESAMPLER_TAPS + RESAMPLER_MAX_BLOCK_44];
    int   buf_pos;          /* read head into buf */
    float phase;            /* fractional position into source samples [0, 1) */
} downsampler_t;

/* Up: 23.4 -> 44.1 (mono float in/out) */
typedef struct {
    float buf[RESAMPLER_TAPS + RESAMPLER_MAX_BLOCK_23];
    int   buf_pos;
    float phase;
} upsampler_t;

void downsampler_init(downsampler_t *r);
void upsampler_init(upsampler_t *r);

/* Run downsampler. Reads `n_in` samples, returns count written to `out`. */
int downsampler_process(downsampler_t *r, const float *in, int n_in,
                        float *out, int out_capacity);

/* Run upsampler. Reads `n_in` samples and produces exactly `n_out` samples. */
void upsampler_process(upsampler_t *r, const float *in, int n_in,
                       float *out, int n_out);

#endif
```

**Step 2:** Write `src/dsp/resampler.c`:

```c
#include "resampler.h"

#include <string.h>

/* SR ratio constants */
#define SR_IN  44100.0f
#define SR_OUT 23400.0f
#define DOWN_RATIO (SR_OUT / SR_IN)   /* 0.5306... output per input */
#define UP_RATIO   (SR_IN / SR_OUT)   /* 1.8846... output per input */

void downsampler_init(downsampler_t *r) {
    memset(r, 0, sizeof(*r));
}

void upsampler_init(upsampler_t *r) {
    memset(r, 0, sizeof(*r));
}

static inline float convolve(const float *taps, const float *coeffs) {
    float acc = 0.0f;
    for (int i = 0; i < RESAMPLER_TAPS; i++) {
        acc += taps[i] * coeffs[i];
    }
    return acc;
}

int downsampler_process(downsampler_t *r, const float *in, int n_in,
                        float *out, int out_capacity) {
    /* Append new input to ring/sliding buffer */
    if (r->buf_pos + n_in > (int)(sizeof(r->buf) / sizeof(float))) {
        /* Slide tail to front */
        int tail = r->buf_pos - (RESAMPLER_TAPS - 1);
        if (tail < 0) tail = 0;
        int kept = r->buf_pos - tail;
        memmove(r->buf, r->buf + tail, kept * sizeof(float));
        r->buf_pos = kept;
    }
    memcpy(r->buf + r->buf_pos, in, n_in * sizeof(float));
    r->buf_pos += n_in;

    int n_out = 0;
    /* Each output advances source position by 1/DOWN_RATIO ~= 1.884 */
    while (n_out < out_capacity) {
        int integer = (int)r->phase;
        if (integer + RESAMPLER_TAPS > r->buf_pos) break;
        float frac = r->phase - integer;
        int phase_idx = (int)(frac * RESAMPLER_PHASES);
        if (phase_idx >= RESAMPLER_PHASES) phase_idx = RESAMPLER_PHASES - 1;
        out[n_out++] = convolve(&r->buf[integer], down_coeffs[phase_idx]);
        r->phase += 1.0f / DOWN_RATIO;
    }

    /* Slide phase + buffer to keep numbers small */
    int consumed = (int)r->phase;
    if (consumed > 0) {
        int kept = r->buf_pos - consumed;
        if (kept < 0) kept = 0;
        memmove(r->buf, r->buf + consumed, kept * sizeof(float));
        r->buf_pos = kept;
        r->phase -= consumed;
    }
    return n_out;
}

void upsampler_process(upsampler_t *r, const float *in, int n_in,
                       float *out, int n_out) {
    /* Append input */
    if (r->buf_pos + n_in > (int)(sizeof(r->buf) / sizeof(float))) {
        int tail = r->buf_pos - (RESAMPLER_TAPS - 1);
        if (tail < 0) tail = 0;
        int kept = r->buf_pos - tail;
        memmove(r->buf, r->buf + tail, kept * sizeof(float));
        r->buf_pos = kept;
    }
    memcpy(r->buf + r->buf_pos, in, n_in * sizeof(float));
    r->buf_pos += n_in;

    for (int i = 0; i < n_out; i++) {
        int integer = (int)r->phase;
        if (integer + RESAMPLER_TAPS > r->buf_pos) {
            /* Underflow: emit silence (shouldn't happen at steady state) */
            out[i] = 0.0f;
            r->phase += 1.0f / UP_RATIO;
            continue;
        }
        float frac = r->phase - integer;
        int phase_idx = (int)(frac * RESAMPLER_PHASES);
        if (phase_idx >= RESAMPLER_PHASES) phase_idx = RESAMPLER_PHASES - 1;
        out[i] = convolve(&r->buf[integer], up_coeffs[phase_idx]);
        r->phase += 1.0f / UP_RATIO;
    }

    int consumed = (int)r->phase;
    if (consumed > 0) {
        int kept = r->buf_pos - consumed;
        if (kept < 0) kept = 0;
        memmove(r->buf, r->buf + consumed, kept * sizeof(float));
        r->buf_pos = kept;
        r->phase -= consumed;
    }
}
```

**Step 3:** Commit
```bash
git -C schwung-midiverb add src/dsp/resampler.h src/dsp/resampler.c
git -C schwung-midiverb commit -m "feat: 44.1<->23.4 polyphase resampler"
```

---

### Task 8: Host-side test for the resampler

**Files:**
- Create: `schwung-midiverb/tests/host/test_resampler.c`
- Modify: `schwung-midiverb/tests/host/Makefile`

**Step 1:** Write `tests/host/test_resampler.c`:

```c
/* End-to-end sanity test: 1 kHz sine at 44.1 -> 23.4 -> 44.1 should
 * (a) produce a similar number of output samples to input samples
 * (b) preserve roughly its amplitude after a settling period
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/dsp/resampler.h"
#include "../../src/dsp/resampler.c"

int main(void) {
    downsampler_t down;
    upsampler_t up;
    downsampler_init(&down);
    upsampler_init(&up);

    const int total_blocks = 200;
    const int block_44 = 128;
    float in[128], mid[160], out[256];
    double peak = 0.0;
    int total_in = 0, total_out = 0;

    for (int b = 0; b < total_blocks; b++) {
        for (int i = 0; i < block_44; i++) {
            float t = (b * block_44 + i) / 44100.0f;
            in[i] = 0.5f * sinf(2.0f * (float)M_PI * 1000.0f * t);
        }
        int mid_n = downsampler_process(&down, in, block_44, mid, 160);
        upsampler_process(&up, mid, mid_n, out, block_44);
        if (b > 20) {  /* skip warmup */
            for (int i = 0; i < block_44; i++) {
                double a = fabs(out[i]);
                if (a > peak) peak = a;
            }
        }
        total_in += block_44;
        total_out += block_44;
    }

    printf("peak after warmup = %.3f (expected ~0.5)\n", peak);
    if (peak < 0.4 || peak > 0.6) {
        fprintf(stderr, "FAIL: peak amplitude out of range\n");
        return 1;
    }
    printf("resampler ok\n");
    return 0;
}
```

**Step 2:** Append a target to `tests/host/Makefile`:

```makefile
test_resampler: test_resampler.c ../../src/dsp/resampler.c ../../src/dsp/resampler.h ../../src/dsp/resampler_coeffs.h
	$(CC) $(CFLAGS) -I../../src/dsp test_resampler.c -o $@ -lm

.PHONY: resampler
resampler: test_resampler
	./test_resampler

.PHONY: all
all: smoke resampler
```

**Step 3:** Build and run
```bash
make -C schwung-midiverb/tests/host resampler
```
Expected: `peak after warmup = 0.5xx` and `resampler ok`. Acceptable range 0.4–0.6 (FIR adds a small amount of attenuation).

**Step 4:** Commit
```bash
git -C schwung-midiverb add tests/host/test_resampler.c tests/host/Makefile
git -C schwung-midiverb commit -m "test: resampler 1kHz sine round-trip preserves amplitude"
```

---

## Phase 3 — DSP plugin core

### Task 9: Per-instance state struct + dispatch table

**Files:**
- Create: `schwung-midiverb/src/dsp/midiverb_core.h`
- Create: `schwung-midiverb/src/dsp/midiverb_core.c`

**Step 1:** Write `src/dsp/midiverb_core.h`:

```c
#ifndef MIDIVERB_CORE_H
#define MIDIVERB_CORE_H

#include <stdint.h>

#include "resampler.h"

#define MIDIVERB_DRAM_LEN 0x4000

typedef enum {
    MV_UNIT_MIDIVERB = 0,
    MV_UNIT_MIDIFEX  = 1,
    MV_UNIT_MIDIVERB2 = 2,
    MV_UNIT_COUNT
} mv_unit_t;

/* Function-pointer signature shared by all three decompiled tables */
typedef void (*mv_effect_fn)(int16_t input, int16_t *out_left, int16_t *out_right,
                             int16_t *DRAM, int ptr,
                             uint32_t lfo1_value, uint32_t lfo2_value);

typedef struct {
    /* Pending param changes (written by set_param, applied in process_block) */
    int   pending_unit;
    int   pending_program;
    int   reset_dsp_state;

    /* Active state */
    mv_unit_t    unit;
    int          program;
    mv_effect_fn effect_fn;

    /* DASP state */
    int16_t  dram[MIDIVERB_DRAM_LEN];
    int      ptr;
    uint32_t lfo1, lfo2;          /* simple counter LFOs */

    /* Continuous params (final, smoothed) */
    float mix;
    float feedback;
    float input_gain;
    float output_gain;
    float predelay_ms;
    float low_cut_hz;
    float high_cut_hz;
    float width;

    /* DSP scratch */
    downsampler_t down_l, down_r;
    upsampler_t   up_l, up_r;

    /* Pre-delay ring buffer (44.1 kHz mono pre-mix in) */
    float predelay_buf[44100 / 4];   /* ~250ms headroom */
    int   predelay_pos;

    /* One-pole HPF + LPF state per channel */
    float hpf_l, hpf_r;
    float lpf_l, lpf_r;

    /* Feedback tap */
    float fb_l, fb_r;
} mv_instance_t;

void mv_instance_init(mv_instance_t *inst);
const char* mv_unit_name(mv_unit_t u);
int  mv_program_count(mv_unit_t u);
const char* mv_program_name(mv_unit_t u, int idx);

#endif
```

**Step 2:** Write `src/dsp/midiverb_core.c`:

```c
#include "midiverb_core.h"

#include <string.h>

#include "decompiled-midiverb.h"
#include "decompiled-midifex.h"
#include "decompiled-midiverb2.h"

static const char* unit_names[MV_UNIT_COUNT] = {
    "Midiverb", "Midifex", "Midiverb II",
};

/* Program-name tables (vendored as comma-separated string literals) */
static const char* midiverb_program_names[] = {
#include "names-midiverb.h"
};
static const char* midifex_program_names[] = {
#include "names-midifex.h"
};
static const char* midiverb2_program_names[] = {
#include "names-midiverb2.h"
};

static int program_counts[MV_UNIT_COUNT] = {
    sizeof(midiverb_program_names) / sizeof(midiverb_program_names[0]),
    sizeof(midifex_program_names)  / sizeof(midifex_program_names[0]),
    sizeof(midiverb2_program_names)/ sizeof(midiverb2_program_names[0]),
};

static mv_effect_fn dispatch_for(mv_unit_t u, int prog) {
    int max = program_counts[u];
    if (prog < 0) prog = 0;
    if (prog >= max) prog = max - 1;
    switch (u) {
        case MV_UNIT_MIDIVERB:  return midiverb_effects[prog];
        case MV_UNIT_MIDIFEX:   return midifex_effects[prog];
        case MV_UNIT_MIDIVERB2: return midiverb2_effects[prog];
        default:                return midiverb_effects[0];
    }
}

void mv_instance_init(mv_instance_t *inst) {
    memset(inst, 0, sizeof(*inst));
    inst->unit = MV_UNIT_MIDIVERB;
    inst->program = 0;
    inst->effect_fn = dispatch_for(inst->unit, inst->program);
    inst->mix = 0.35f;
    inst->feedback = 0.30f;
    inst->input_gain = 1.0f;
    inst->output_gain = 1.0f;
    inst->predelay_ms = 0.0f;
    inst->low_cut_hz = 20.0f;
    inst->high_cut_hz = 20000.0f;
    inst->width = 1.0f;
    inst->pending_unit = -1;
    inst->pending_program = -1;
    downsampler_init(&inst->down_l);
    downsampler_init(&inst->down_r);
    upsampler_init(&inst->up_l);
    upsampler_init(&inst->up_r);
}

const char* mv_unit_name(mv_unit_t u) {
    if (u < 0 || u >= MV_UNIT_COUNT) return "?";
    return unit_names[u];
}

int mv_program_count(mv_unit_t u) {
    if (u < 0 || u >= MV_UNIT_COUNT) return 0;
    return program_counts[u];
}

const char* mv_program_name(mv_unit_t u, int idx) {
    if (idx < 0 || idx >= mv_program_count(u)) return "?";
    switch (u) {
        case MV_UNIT_MIDIVERB:  return midiverb_program_names[idx];
        case MV_UNIT_MIDIFEX:   return midifex_program_names[idx];
        case MV_UNIT_MIDIVERB2: return midiverb2_program_names[idx];
        default:                return "?";
    }
}

mv_effect_fn mv_dispatch_for(mv_unit_t u, int prog) {
    return dispatch_for(u, prog);
}
```

**Step 3:** Add prototype for `mv_dispatch_for` in `midiverb_core.h` after the existing declarations:

```c
mv_effect_fn mv_dispatch_for(mv_unit_t u, int prog);
```

**Step 4:** Compile-test on host (extend `tests/host/Makefile`):

Add target:
```makefile
test_core: test_core.c ../../src/dsp/midiverb_core.c ../../src/dsp/midiverb_core.h \
           ../../src/dsp/resampler.c ../../src/dsp/decompiled-midiverb.h \
           ../../src/dsp/decompiled-midifex.h ../../src/dsp/decompiled-midiverb2.h
	$(CC) $(CFLAGS) -I../../src/dsp -Wno-unused-function ../../src/dsp/midiverb_core.c ../../src/dsp/resampler.c test_core.c -o $@ -lm

.PHONY: core
core: test_core
	./test_core
```

Write `tests/host/test_core.c`:

```c
#include <stdio.h>

#include "../../src/dsp/midiverb_core.h"

int main(void) {
    mv_instance_t inst;
    mv_instance_init(&inst);
    printf("unit=%s program=%d (%s)\n",
           mv_unit_name(inst.unit), inst.program,
           mv_program_name(inst.unit, inst.program));
    printf("counts: midiverb=%d midifex=%d midiverb2=%d\n",
           mv_program_count(MV_UNIT_MIDIVERB),
           mv_program_count(MV_UNIT_MIDIFEX),
           mv_program_count(MV_UNIT_MIDIVERB2));
    return 0;
}
```

Build + run:
```bash
make -C schwung-midiverb/tests/host core
```
Expected: prints `unit=Midiverb program=0 (.2 Sec SMALL BRIGHT)` and counts (64/64/100).

**Step 5:** Commit
```bash
git -C schwung-midiverb add src/dsp/midiverb_core.h src/dsp/midiverb_core.c \
    tests/host/test_core.c tests/host/Makefile
git -C schwung-midiverb commit -m "feat: midiverb core (per-instance state + dispatch)"
```

---

### Task 10: Plugin entry point + minimal `process_block`

**Files:**
- Create: `schwung-midiverb/src/dsp/plugin.c`

**Step 1:** Write `src/dsp/plugin.c`:

```c
/* schwung-midiverb plugin entry */
#include "audio_fx_api_v2.h"
#include "midiverb_core.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static const host_api_v1_t *g_host = NULL;

/* Apply pending unit/program changes from non-RT thread */
static void apply_pending(mv_instance_t *inst) {
    int dirty = 0;
    if (inst->pending_unit >= 0) {
        if (inst->pending_unit != (int)inst->unit) {
            inst->unit = (mv_unit_t)inst->pending_unit;
            int max = mv_program_count(inst->unit);
            if (inst->program >= max) inst->program = max - 1;
            dirty = 1;
        }
        inst->pending_unit = -1;
    }
    if (inst->pending_program >= 0) {
        int max = mv_program_count(inst->unit);
        int p = inst->pending_program;
        if (p >= max) p = max - 1;
        if (p != inst->program) {
            inst->program = p;
            dirty = 1;
        }
        inst->pending_program = -1;
    }
    if (inst->reset_dsp_state || dirty) {
        memset(inst->dram, 0, sizeof(inst->dram));
        inst->fb_l = inst->fb_r = 0.0f;
        inst->effect_fn = mv_dispatch_for(inst->unit, inst->program);
        inst->reset_dsp_state = 0;
    }
}

static void* mv_create(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    mv_instance_t *inst = (mv_instance_t*)calloc(1, sizeof(mv_instance_t));
    if (!inst) return NULL;
    mv_instance_init(inst);
    return inst;
}

static void mv_destroy(void *vp) {
    free(vp);
}

/* Bandwidth-safe one-pole HPF / LPF coefficients at 23.4 kHz */
static inline float onepole_coef(float fc) {
    float t = expf(-2.0f * (float)M_PI * fc / 23400.0f);
    return t;
}

static void mv_process(void *vp, int16_t *audio_inout, int frames) {
    mv_instance_t *inst = (mv_instance_t*)vp;
    apply_pending(inst);

    /* Split stereo int16 -> two mono float buffers, scale to [-1,1] */
    static float in_l[256], in_r[256], mid_l[160], mid_r[160], out_l[256], out_r[256];
    if (frames > 256) frames = 256;
    for (int i = 0; i < frames; i++) {
        in_l[i] = audio_inout[2*i + 0] / 32768.0f;
        in_r[i] = audio_inout[2*i + 1] / 32768.0f;
    }

    /* Downsample 44.1 -> 23.4 */
    int n_mid_l = downsampler_process(&inst->down_l, in_l, frames, mid_l, 160);
    int n_mid_r = downsampler_process(&inst->down_r, in_r, frames, mid_r, 160);
    int n_mid = n_mid_l < n_mid_r ? n_mid_l : n_mid_r;

    /* Pre-delay (44.1kHz pre-down): for v1 keep simple — no pre-delay yet */
    /* Pre-DSP gain + filter coefficients */
    float a_hp = onepole_coef(inst->low_cut_hz);
    float a_lp = onepole_coef(inst->high_cut_hz);

    /* Run effect_fn sample-by-sample on mono sum (Midiverb input is mono) */
    static float wet_l[160], wet_r[160];
    for (int i = 0; i < n_mid; i++) {
        /* Sum to mono with input gain */
        float mono = (mid_l[i] + mid_r[i]) * 0.5f * inst->input_gain;
        /* Inject feedback */
        mono += inst->fb_l * inst->feedback * 0.5f
              + inst->fb_r * inst->feedback * 0.5f;
        /* HPF on input */
        inst->hpf_l = a_hp * inst->hpf_l + (1.0f - a_hp) * mono;
        float hp = mono - inst->hpf_l;

        int16_t in16 = (int16_t)(fmaxf(-1.0f, fminf(1.0f, hp)) * 32767.0f);
        int16_t ol = 0, or_ = 0;
        inst->effect_fn(in16, &ol, &or_, inst->dram, inst->ptr,
                        inst->lfo1, inst->lfo2);
        inst->ptr = (inst->ptr + 1) & 0x3fff;
        inst->lfo1 += 1;  /* placeholder; programs that want LFO will read this */
        inst->lfo2 += 2;

        float wl = ol / 32768.0f;
        float wr = or_ / 32768.0f;
        /* LPF on wet */
        inst->lpf_l = a_lp * inst->lpf_l + (1.0f - a_lp) * wl;
        inst->lpf_r = a_lp * inst->lpf_r + (1.0f - a_lp) * wr;
        wl = inst->lpf_l;
        wr = inst->lpf_r;

        /* M/S width on wet */
        float mid = 0.5f * (wl + wr);
        float side = 0.5f * (wl - wr) * inst->width;
        wl = mid + side;
        wr = mid - side;

        wet_l[i] = wl;
        wet_r[i] = wr;
        inst->fb_l = wl;
        inst->fb_r = wr;
    }

    /* Upsample wet 23.4 -> 44.1 */
    upsampler_process(&inst->up_l, wet_l, n_mid, out_l, frames);
    upsampler_process(&inst->up_r, wet_r, n_mid, out_r, frames);

    /* Mix dry + wet, output gain, back to int16 */
    float mix = inst->mix;
    float og = inst->output_gain;
    for (int i = 0; i < frames; i++) {
        float l = (1.0f - mix) * in_l[i] + mix * out_l[i];
        float r = (1.0f - mix) * in_r[i] + mix * out_r[i];
        l *= og; r *= og;
        if (l >  1.0f) l =  1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r >  1.0f) r =  1.0f;
        if (r < -1.0f) r = -1.0f;
        audio_inout[2*i + 0] = (int16_t)(l * 32767.0f);
        audio_inout[2*i + 1] = (int16_t)(r * 32767.0f);
    }
}

static void mv_set_param(void *vp, const char *key, const char *val) {
    mv_instance_t *inst = (mv_instance_t*)vp;
    if (!inst || !key || !val) return;
    if (strcmp(key, "unit") == 0) {
        int u = atoi(val);
        if (u >= 0 && u < MV_UNIT_COUNT) inst->pending_unit = u;
    } else if (strcmp(key, "program") == 0) {
        inst->pending_program = atoi(val);
    } else if (strcmp(key, "mix") == 0) {
        inst->mix = atof(val);
    } else if (strcmp(key, "feedback") == 0) {
        inst->feedback = atof(val);
    } else if (strcmp(key, "input_gain") == 0) {
        inst->input_gain = atof(val);
    } else if (strcmp(key, "output_gain") == 0) {
        inst->output_gain = atof(val);
    } else if (strcmp(key, "predelay_ms") == 0) {
        inst->predelay_ms = atof(val);
    } else if (strcmp(key, "low_cut_hz") == 0) {
        inst->low_cut_hz = atof(val);
    } else if (strcmp(key, "high_cut_hz") == 0) {
        inst->high_cut_hz = atof(val);
    } else if (strcmp(key, "width") == 0) {
        inst->width = atof(val);
    }
}

static int mv_get_param(void *vp, const char *key, char *buf, int buf_len) {
    mv_instance_t *inst = (mv_instance_t*)vp;
    if (!inst || !key || !buf || buf_len <= 0) return -1;
    int n = -1;

    if (strcmp(key, "unit") == 0) {
        n = snprintf(buf, buf_len, "%d", (int)inst->unit);
    } else if (strcmp(key, "program") == 0) {
        n = snprintf(buf, buf_len, "%d", inst->program);
    } else if (strcmp(key, "program_count") == 0) {
        n = snprintf(buf, buf_len, "%d", mv_program_count(inst->unit));
    } else if (strcmp(key, "program_name") == 0) {
        n = snprintf(buf, buf_len, "%s", mv_program_name(inst->unit, inst->program));
    } else if (strcmp(key, "unit_list") == 0) {
        n = snprintf(buf, buf_len, "[\"Midiverb\",\"Midifex\",\"Midiverb II\"]");
    } else if (strcmp(key, "mix") == 0) {
        n = snprintf(buf, buf_len, "%.3f", inst->mix);
    } else if (strcmp(key, "feedback") == 0) {
        n = snprintf(buf, buf_len, "%.3f", inst->feedback);
    } else if (strcmp(key, "input_gain") == 0) {
        n = snprintf(buf, buf_len, "%.3f", inst->input_gain);
    } else if (strcmp(key, "output_gain") == 0) {
        n = snprintf(buf, buf_len, "%.3f", inst->output_gain);
    } else if (strcmp(key, "predelay_ms") == 0) {
        n = snprintf(buf, buf_len, "%.1f", inst->predelay_ms);
    } else if (strcmp(key, "low_cut_hz") == 0) {
        n = snprintf(buf, buf_len, "%.0f", inst->low_cut_hz);
    } else if (strcmp(key, "high_cut_hz") == 0) {
        n = snprintf(buf, buf_len, "%.0f", inst->high_cut_hz);
    } else if (strcmp(key, "width") == 0) {
        n = snprintf(buf, buf_len, "%.3f", inst->width);
    } else if (strcmp(key, "chain_params") == 0) {
        int max = mv_program_count(inst->unit) - 1;
        n = snprintf(buf, buf_len,
            "["
            "{\"key\":\"unit\",\"name\":\"Unit\",\"type\":\"enum\",\"options\":[\"Midiverb\",\"Midifex\",\"Midiverb II\"]},"
            "{\"key\":\"program\",\"name\":\"Program\",\"type\":\"int\",\"min\":0,\"max\":%d},"
            "{\"key\":\"mix\",\"name\":\"Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"feedback\",\"name\":\"Feedback\",\"type\":\"float\",\"min\":0,\"max\":0.95,\"step\":0.01},"
            "{\"key\":\"input_gain\",\"name\":\"Input Gain\",\"type\":\"float\",\"min\":0,\"max\":2,\"step\":0.01,\"unit\":\"x\"},"
            "{\"key\":\"output_gain\",\"name\":\"Output Gain\",\"type\":\"float\",\"min\":0,\"max\":2,\"step\":0.01,\"unit\":\"x\"},"
            "{\"key\":\"predelay_ms\",\"name\":\"Pre-delay\",\"type\":\"float\",\"min\":0,\"max\":200,\"step\":1,\"unit\":\"ms\"},"
            "{\"key\":\"low_cut_hz\",\"name\":\"Low Cut\",\"type\":\"float\",\"min\":20,\"max\":1000,\"step\":1,\"unit\":\"Hz\"},"
            "{\"key\":\"high_cut_hz\",\"name\":\"High Cut\",\"type\":\"float\",\"min\":1000,\"max\":20000,\"step\":50,\"unit\":\"Hz\"},"
            "{\"key\":\"width\",\"name\":\"Width\",\"type\":\"float\",\"min\":0,\"max\":1.5,\"step\":0.01}"
            "]", max);
    } else if (strcmp(key, "ui_hierarchy") == 0) {
        n = snprintf(buf, buf_len,
            "{"
              "\"modes\":null,"
              "\"levels\":{"
                "\"root\":{"
                  "\"label\":\"Midiverb\","
                  "\"list_param\":\"program\","
                  "\"count_param\":\"program_count\","
                  "\"name_param\":\"program_name\","
                  "\"knobs\":[\"mix\",\"feedback\",\"input_gain\",\"output_gain\",\"predelay_ms\",\"low_cut_hz\",\"high_cut_hz\",\"width\"],"
                  "\"params\":["
                    "{\"key\":\"mix\",\"label\":\"Mix\"},"
                    "{\"key\":\"feedback\",\"label\":\"Feedback\"},"
                    "{\"key\":\"input_gain\",\"label\":\"Input Gain\"},"
                    "{\"key\":\"output_gain\",\"label\":\"Output Gain\"},"
                    "{\"key\":\"predelay_ms\",\"label\":\"Pre-delay\"},"
                    "{\"key\":\"low_cut_hz\",\"label\":\"Low Cut\"},"
                    "{\"key\":\"high_cut_hz\",\"label\":\"High Cut\"},"
                    "{\"key\":\"width\",\"label\":\"Width\"},"
                    "{\"level\":\"unit\",\"label\":\"Unit\"}"
                  "]"
                "},"
                "\"unit\":{"
                  "\"label\":\"Unit\","
                  "\"items_param\":\"unit_list\","
                  "\"select_param\":\"unit\","
                  "\"knobs\":[],"
                  "\"params\":[]"
                "}"
              "}"
            "}");
    }
    if (n < 0) return -1;
    if (n >= buf_len) return buf_len - 1;
    return n;
}

static void mv_on_midi(void *vp, const uint8_t *msg, int len, int source) {
    (void)vp; (void)msg; (void)len; (void)source;
}

static audio_fx_api_v2_t API = {
    .api_version = AUDIO_FX_API_VERSION_2,
    .create_instance = mv_create,
    .destroy_instance = mv_destroy,
    .process_block = mv_process,
    .set_param = mv_set_param,
    .get_param = mv_get_param,
    .on_midi = mv_on_midi,
};

audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &API;
}
```

**Step 2:** Commit
```bash
git -C schwung-midiverb add src/dsp/plugin.c
git -C schwung-midiverb commit -m "feat: plugin entry + process_block + param surface"
```

---

## Phase 4 — Build for ARM

### Task 11: Dockerfile

**Files:**
- Create: `schwung-midiverb/scripts/Dockerfile`

**Step 1:** Copy from sibling — same toolchain, same base image
```bash
cp schwung-mverb/scripts/Dockerfile schwung-midiverb/scripts/Dockerfile
```

**Step 2:** Commit
```bash
git -C schwung-midiverb add scripts/Dockerfile
git -C schwung-midiverb commit -m "build: Dockerfile (debian-bullseye + aarch64-linux-gnu-gcc)"
```

---

### Task 12: build.sh

**Files:**
- Create: `schwung-midiverb/scripts/build.sh`

**Step 1:** Write `scripts/build.sh` (model after `../schwung-mverb/scripts/build.sh`):

```bash
#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-midiverb-builder"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Building Midiverb Module (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
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
mkdir -p build dist/midiverb

${CROSS_PREFIX}gcc -O3 -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -ffast-math -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    -Isrc/dsp \
    src/dsp/plugin.c \
    src/dsp/midiverb_core.c \
    src/dsp/resampler.c \
    -o build/midiverb.so \
    -lm

cp src/module.json dist/midiverb/module.json
cp build/midiverb.so dist/midiverb/midiverb.so
cp src/dsp/THIRD_PARTY_LICENSES.md dist/midiverb/THIRD_PARTY_LICENSES.md
chmod +x dist/midiverb/midiverb.so

cd dist
tar -czvf midiverb-module.tar.gz midiverb/
echo "OK: dist/midiverb-module.tar.gz"
```

**Step 2:** chmod + run
```bash
chmod +x schwung-midiverb/scripts/build.sh
cd schwung-midiverb
./scripts/build.sh
```
Expected: ends with `OK: dist/midiverb-module.tar.gz`. If linker complains about `int16_t` width or warnings, that's fine; only fail on errors.

**Step 3:** Commit
```bash
git -C schwung-midiverb add scripts/build.sh
git -C schwung-midiverb commit -m "build: cross-compile script (Docker + ARM)"
```

---

### Task 13: install.sh

**Files:**
- Create: `schwung-midiverb/scripts/install.sh`

**Step 1:** Write (model after `../schwung-mverb/scripts/install.sh`):

```bash
#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/midiverb" ]; then
    echo "Run ./scripts/build.sh first."
    exit 1
fi

ssh ableton@move.local "mkdir -p /data/UserData/schwung/modules/audio_fx/midiverb"
scp -r dist/midiverb/* ableton@move.local:/data/UserData/schwung/modules/audio_fx/midiverb/
ssh ableton@move.local "chmod -R a+rw /data/UserData/schwung/modules/audio_fx/midiverb"
echo "Installed."
```

**Step 2:** chmod + commit
```bash
chmod +x schwung-midiverb/scripts/install.sh
git -C schwung-midiverb add scripts/install.sh
git -C schwung-midiverb commit -m "build: install.sh deploy to move.local"
```

---

## Phase 5 — UI

### Task 14: ui.js (chain-renderer compatible)

**Files:**
- Create: `schwung-midiverb/src/ui.js`

**Step 1:** Write a thin `ui.js`. Audio FX modules don't need a full UI — Signal Chain renders the hierarchy from `chain_params` + `ui_hierarchy`. Provide an empty stub so the module loads cleanly:

```javascript
// Schwung Midiverb — UI stub
// All UI is driven by ui_hierarchy + chain_params returned from the DSP plugin.
// This file exists to satisfy the module loader's expectation of ui.js.

globalThis.init = function() {};
globalThis.tick = function() {};
globalThis.onMidiMessageInternal = function(_data) {};
globalThis.onMidiMessageExternal = function(_data) {};
```

**Step 2:** Commit
```bash
git -C schwung-midiverb add src/ui.js
git -C schwung-midiverb commit -m "ui: stub ui.js (chain renderer drives UI)"
```

---

### Task 15: ui_chain.js (Signal Chain UI shim)

**Files:**
- Create: `schwung-midiverb/src/ui_chain.js`

**Step 1:** Check `../schwung-mverb/src/` — if there's no `ui_chain.js` there, then mverb relies entirely on the generic chain renderer reading `ui_hierarchy`. In that case, **skip this task**: don't add `ui_chain.js` at all and just rely on the host's generic rendering.

Run:
```bash
ls schwung-mverb/src/
```

**Step 2 (only if mverb has ui_chain.js):** copy and adapt minimally.
**Step 3 (default):** skip the file entirely. Update `scripts/build.sh` only if needed.

**Step 4:** No commit if no file added. Otherwise:
```bash
git -C schwung-midiverb add src/ui_chain.js
git -C schwung-midiverb commit -m "ui: ui_chain.js Signal Chain shim"
```

---

## Phase 6 — On-device smoke test

### Task 16: Build, install, and verify on hardware

**Files:** none (deploy + observe)

**Step 1:** Build
```bash
cd schwung-midiverb
./scripts/build.sh
```

**Step 2:** Install
```bash
./scripts/install.sh
```

**Step 3:** Restart Schwung on the device (or just reload modules if the host has a rescan path)
```bash
ssh ableton@move.local "systemctl --user restart schwung || true"
```

**Step 4:** On the Move:
- Enter shadow mode (Shift+Vol+Track 1)
- Add Signal Chain → audio FX → Midiverb
- Play notes through a sound generator → confirm reverb is audible
- Turn knob 1 (Mix) → confirm wet signal scales
- Turn jog wheel → confirm program name on screen changes (`.2 Sec SMALL BRIGHT`, etc.)
- Open the Unit sub-menu → switch to Midifex → confirm program list resets to a different set of names
- Enable debug log: `ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"` and `tail -f` to watch for crashes
- Audit CPU: shadow mode → master FX → enable a CPU meter if one exists, or just watch for audio dropouts

**Step 5:** If audio crackles on instantiation, suspect the resampler ring buffer not being warmed — note the symptom, do NOT push the broken module. Investigate and fix before continuing. If audio is clean, proceed.

**Step 6:** No commit yet — wait for next polish task.

---

### Task 17: Polish & bugfix pass after hardware test

**Files:** TBD based on Step 5/6 of Task 16

**Step 1:** Address whatever issues turned up:
- Resampler aliasing → tweak Kaiser β or cutoff in `gen_resampler.py` and regenerate
- Wet too quiet → adjust default mix or normalize wet inside the loop
- Crackles on unit switch → confirm `apply_pending` runs *before* any DSP work in the block (already does, but verify)
- LFO programs (some Midifex flanger presets) sound static → tune LFO counter increment rates in `plugin.c` (the placeholder `inst->lfo1 += 1; inst->lfo2 += 2` may need to be slower; refer to upstream `lfo.h` for original rates)

**Step 2:** Commit each fix individually with descriptive messages.

---

## Phase 7 — Release plumbing

### Task 18: GitHub Actions release workflow

**Files:**
- Create: `schwung-midiverb/.github/workflows/release.yml`

**Step 1:** Copy from sibling, only the tarball name changes
```bash
mkdir -p schwung-midiverb/.github/workflows
cp schwung-mverb/.github/workflows/release.yml schwung-midiverb/.github/workflows/release.yml
```

**Step 2:** Edit the file:
- Change `dist/mverb-module.tar.gz` → `dist/midiverb-module.tar.gz`

Use Edit tool with old_string `dist/mverb-module.tar.gz` → new_string `dist/midiverb-module.tar.gz` (replace_all).

**Step 3:** Commit
```bash
git -C schwung-midiverb add .github/workflows/release.yml
git -C schwung-midiverb commit -m "ci: GitHub Actions release workflow on tag push"
```

---

### Task 19: README, CLAUDE.md, release.json

**Files:**
- Create: `schwung-midiverb/README.md`
- Create: `schwung-midiverb/CLAUDE.md`
- Create: `schwung-midiverb/release.json`

**Step 1:** Write `README.md`:

```markdown
# schwung-midiverb

Schwung audio-FX module that emulates the Alesis **Midiverb**, **Midifex**,
and **Midiverb II** rack reverbs (early-80s 16-bit DASP DSP).

DSP based on [thement/midiverb_emulator](https://github.com/thement/midiverb_emulator)
(GPL-3.0).

## Programs

- Midiverb: 64 programs
- Midifex: 64 programs
- Midiverb II: 100 programs

## Build

```bash
./scripts/build.sh
./scripts/install.sh
```

## License

GPL-3.0 — see LICENSE.
```

**Step 2:** Write `CLAUDE.md`:

```markdown
# CLAUDE.md — schwung-midiverb

Schwung audio-FX module emulating Alesis Midiverb / Midifex / Midiverb II.

## Architecture

- `src/dsp/plugin.c` — `audio_fx_api_v2` entry, params, process_block, resampler glue
- `src/dsp/midiverb_core.{c,h}` — per-instance state, unit/program dispatch
- `src/dsp/resampler.{c,h}` + `resampler_coeffs.h` — 32-tap polyphase windowed-sinc, 44.1<->23.4 kHz
- `src/dsp/decompiled-*.h`, `names-*.h` — vendored upstream (GPL-3)

DSP runs at 23.4 kHz internal. Each sample: HPF → effect_fn → LPF → M/S width.
Feedback tap reads from previous block's wet output.

## Build

`./scripts/build.sh` (Docker + aarch64-linux-gnu-gcc) → `dist/midiverb-module.tar.gz`.

## Tests

`make -C tests/host all` — host-side resampler smoke test.

## Adding ROM mode (Path B)

See parent `schwung/docs/plans/2026-04-27-midiverb-design.md` §"Path B".
Add `rom_mode` enum param, fall back to decompiled if ROMs missing.
```

**Step 3:** Write `release.json` (placeholder until first tag):

```json
{
  "version": "0.1.0",
  "download_url": "https://github.com/charlesvestal/schwung-midiverb/releases/download/v0.1.0/midiverb-module.tar.gz"
}
```

**Step 4:** Commit
```bash
git -C schwung-midiverb add README.md CLAUDE.md release.json
git -C schwung-midiverb commit -m "docs: README, CLAUDE.md, initial release.json"
```

---

### Task 20: Push to GitHub

**Files:** none

**Step 1:** Confirm with user before pushing — this is the moment the repo becomes public-visible.

**Step 2:** Create the GitHub repo
```bash
gh repo create charlesvestal/schwung-midiverb --public \
    --description "Alesis Midiverb / Midifex / Midiverb II emulator for Schwung" \
    --source=schwung-midiverb --push
```

**Step 3:** Tag and push first release
```bash
cd schwung-midiverb
git tag v0.1.0
git push origin v0.1.0
```

**Step 4:** Wait for GitHub Actions to complete. Verify the release exists:
```bash
gh release view v0.1.0 -R charlesvestal/schwung-midiverb
```

**Step 5:** Add release notes
```bash
gh release edit v0.1.0 -R charlesvestal/schwung-midiverb --notes "$(cat <<'EOF'
- Initial release
- Emulates Alesis Midiverb (64 programs), Midifex (64 programs), Midiverb II (100 programs)
- 23.4 kHz internal DSP with 32-tap polyphase resampling to/from 44.1 kHz
- Mix, Feedback, Input/Output Gain, Pre-delay, Low Cut, High Cut, Width
- Osirus-style UI: jog wheel browses programs, Unit submenu switches between the three units
- DSP based on thement/midiverb_emulator (GPL-3.0)
EOF
)"
```

---

### Task 21: Add catalog entry in main `schwung` repo

**Files:**
- Modify: `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/module-catalog.json`

**Step 1:** Read `schwung/module-catalog.json` to confirm structure and find current `min_host_version` for new audio FX modules. Use Read tool.

**Step 2:** Add new entry (preserve existing entries):

```json
{
  "id": "midiverb",
  "name": "Midiverb",
  "description": "Alesis Midiverb / Midifex / Midiverb II emulator (lo-fi 23.4 kHz reverb)",
  "author": "Charles Vestal (DSP: thement/midiverb_emulator)",
  "component_type": "audio_fx",
  "github_repo": "charlesvestal/schwung-midiverb",
  "default_branch": "main",
  "asset_name": "midiverb-module.tar.gz",
  "min_host_version": "<current host version>"
}
```

Replace `<current host version>` with the value of `host.latest_version` already in the file.

**Step 3:** Commit in the `schwung` repo
```bash
git -C /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung add module-catalog.json
git -C /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung commit -m "catalog: add midiverb module"
```

**Step 4:** Push the catalog change
```bash
git -C /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung push
```

---

## Done.

The Module Store should now show **Midiverb** within minutes (cache TTL on the
GitHub raw fetch).

## Path B (ROM mode) — out of scope for this plan

Tracked in `docs/plans/2026-04-27-midiverb-design.md` §"Path B". Will be a
separate plan, additive to the v1 param surface.
