# Clip-Associated Automation Lanes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Record a knob turn against Move's playing clip and hear it play back in
time with that clip, every loop, on hardware.

**Architecture:** Lanes are time-addressed (breakpoints in beats from the clip's
loop start) and absolute (the lane *is* the value; the knob is the base
underneath it). The engine lives in the chain DSP beside the LFOs, where
`mod_tick` runs every block including on silent slots. Clip phase reaches it
from the shim through a **dlsym'd entry point**, not a `host_api_v1_t` field.
Playback drains through a new **override** source class in `chain_mod`, so
write throttling, the change epsilon and base tracking all come free.

**Tech Stack:** C11 (shim, chain DSP, `tests/host` units), QuickJS ES modules
(shadow UI), Docker cross-build for ARM64, `./scripts/install.sh local`.

**User decisions (already made):**
- "the lane is time-addressed — breakpoints in beats within the loop — with steps as a quantized view. It therefore never reads Move's note content."
- "Don't take the step LEDs." Move's step editor *is* those buttons; we read the page, we do not assign one.
- Lanes are **absolute** — "the lane IS the value", knob is the base, clearing returns to the knob.
- Keying is **position + fingerprint, refuse on mismatch** — a mismatched lane is STALE: retained, silent, never guessed at.
- Arm is **Move's own Record button**, read off the cable-0 LED stream, *measured before anything is built on it*.
- First gesture is **Record + knob turn**. Hold-step p-locks are the second gesture and are out of scope here.
- While the transport is **stopped the lane does not drive** — the parameter sits at the knob's value.
- First PR is a **thin vertical slice**: one lane, one param, end to end, verified by ear on hardware.

---

## Design decision changed during planning — read this before Task 2

The design said the clip-phase callback would consume `host_api_v1_t`'s
`reserved` tail "from the front", per `CLAUDE.md`. **Measured, that is wrong,
and it would resurrect the breakbeat boot-loop.**

```
$ sizeof=184  get_beat_position=+112  reserved=+120 (64 bytes)
```

`reserved` *starts at +120* — the exact offset a shipped breakbeat build calls
as `get_project_bpm()`. Consuming the front slot puts a live function pointer
there, breakbeat's `if (host->fn)` guard passes, and it calls
`int (*)(void*, double*, double*)` as `float (*)(void)` — garbage pointers
written through, on the SPI callback. `tests/host/test_host_api_reserved_tail.c`
**cannot catch this**: it inspects a `memset`-zeroed struct, so a real field at
+120 reads NULL there and passes.

So this plan does not touch `host_api_v1_t` at all. It uses the codebase's own
precedent for this exact hazard: `move_plugin_render_split` is **dlsym'd rather
than made a field on `plugin_api_v2_t`, because of breakbeat's header drift**
(`docs/CHAIN.md`). `chain_take_midi_tick_wake` is resolved the same way
(`src/host/shadow_chain_mgmt.c:2443`), and Task 2 mirrors it line for line.

Task 12 records the corrected rule in `CLAUDE.md`: **do not consume `reserved`
from the front — its front IS +120.**

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/host/lane_store.h` / `.c` | **Pure** lane model: breakpoints, write+thinning, eval with wrap, fingerprint compare. No I/O, no globals, no chain types. Compiled into `dsp.so` and into `tests/host`. | 1 |
| `tests/host/test_lane_store.c` / `.sh` | Unit tests for the above. | 1 |
| `src/modules/chain/dsp/chain_host.c` | dlsym'd `chain_set_clip_phase`; `lane_tick` call sites; `lanes:` param surface; recorder hook. | 2, 4, 5 |
| `src/host/shadow_chain_mgmt.{c,h}` | Resolve `chain_set_clip_phase`; `shadow_slot_clip_phase()` (slot→track→phase). | 2 |
| `src/schwung_shim.c` | Push phase per slot per block; decode Move's Record LED; push arm state. | 2, 7 |
| `src/modules/chain/dsp/chain_mod.c` | Override source class. | 3 |
| `src/modules/chain/dsp/chain_lanes.c` | Lane playback + recording glue against `chain_instance_t`. | 4, 5 |
| `src/host/clip_regions.{c,h}` | `note_count` / `first_note` for the fingerprint. | 6 |
| `src/shadow/shadow_ui.js` | `lanes_<i>.json` persistence; driven mark; Clear Lane; refusal message. | 8, 9 |

---

## Task 1: `lane_store` — the pure lane model

**Goal:** A dependency-free breakpoint store with write/thinning, wrapped
evaluation and fingerprint comparison, unit-tested off-device.

**Files:**
- Create: `src/host/lane_store.h`, `src/host/lane_store.c`
- Create: `tests/host/test_lane_store.c`, `tests/host/test_lane_store.sh`
- Modify: `tests/host/Makefile` (add target), `scripts/build.sh:506-530` (add
  `src/host/lane_store.c` to both the `needs_rebuild` dependency list and the
  compile line for `build/modules/chain/dsp.so`)

**Acceptance Criteria:**
- [ ] An empty lane evaluates to "no value" (returns 0), never to 0.0
- [ ] A point at phase >= `loop_len` is ignored by eval and **retained** in the store
- [ ] Raising `loop_len` reveals that same point without rewriting anything
- [ ] Float lanes interpolate linearly; int/enum lanes hold the previous value
- [ ] Before the first point and after the last, the value is held (no wrap-around interpolation)
- [ ] Two writes closer than `LANE_MIN_POINT_BEATS` collapse to one point carrying the newer value
- [ ] Writes arriving out of order leave the list sorted by phase
- [ ] A full lane replaces its nearest point and increments `full_hits` rather than dropping the write
- [ ] Fingerprint comparison rejects a differing note count even when the loop geometry matches

**Verify:** `bash tests/host/test_lane_store.sh` → `PASS: lane_store`

**Steps:**

- [ ] **Step 1: Write the header**

Create `src/host/lane_store.h`:

```c
/*
 * lane_store — clip-associated automation lanes: the pure part.
 *
 * A lane is (clip position) x (target, param). Its content is breakpoints in
 * BEATS FROM THE CLIP'S LOOP START. Steps are a quantized view of this; the
 * lane itself never knows about steps and never reads Move's note content.
 *
 * THREE RULES THAT ARE NOT OBVIOUS
 * --------------------------------
 * 1. A LANE HAS NO LENGTH OF ITS OWN. Clip length is mutable from Move's step
 *    editor, so points are stored UNBOUNDED and evaluation wraps at whatever
 *    loop_len the clip currently has. Extending a clip reveals what was
 *    recorded there; shrinking it makes the tail dormant. Nothing is rescaled
 *    -- stretching a lane turns a filter sweep into a different filter sweep,
 *    which is the musically wrong answer even though it looks tidy.
 *
 * 2. EVAL CONSIDERS ONLY POINTS BELOW loop_len, and holds at both ends rather
 *    than interpolating across the wrap. Otherwise a dormant point past the
 *    end bends the audible curve while appearing nowhere on screen.
 *
 * 3. MOVE'S CLIPS HAVE NO IDENTITY. A clip in Song.abl carries name (usually
 *    ""), color, region, grooveId, notes -- no id, no uuid. So a lane is bound
 *    to a POSITION plus a FINGERPRINT of what was there when it was recorded.
 *    A mismatch makes the lane STALE: retained, silent, never guessed at. A
 *    lane playing the wrong clip's automation is worse than no lane at all.
 */
#ifndef LANE_STORE_H
#define LANE_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#define LANE_MAX          16   /* lanes per chain slot */
#define LANE_POINTS_MAX   64   /* breakpoints per lane */

/* Two writes closer together than this collapse into one. ~5 ms at 120 BPM:
 * below a knob detent's spacing, above the jitter of sampling phase on the
 * callback. It is also what makes a second pass REPLACE rather than layer. */
#define LANE_MIN_POINT_BEATS 0.01

typedef struct { double phase; float value; } lane_point_t;

/* What the clip looked like when the lane was recorded. Cheap, and each field
 * discriminates something the others do not: geometry catches a re-cut clip,
 * the note count catches a copy of a same-length clip, the first note catches
 * a same-length same-density different clip. */
typedef struct {
    double loop_start;
    double loop_len;
    int    note_count;
    int    first_note;   /* noteNumber of the earliest note, or -1 */
} lane_fingerprint_t;

typedef struct {
    int  used;
    char target[16];     /* "synth", "fx3", "midi_fx1" -- chain component addr */
    char param[32];
    int  track;          /* Move track 0..3 */
    int  slot;           /* clip slot 0..7 */
    lane_fingerprint_t fp;
    int  stale;          /* fingerprint mismatch: retained, silent */
    int  orphaned;       /* the clip was deleted; retained, silent */
    int  n;
    int  full_hits;      /* writes that had to replace a neighbour */
    /* Runtime, not content. `driving` is "we currently hold an override on
     * this target", so losing the phase can RELEASE it exactly once instead of
     * leaving the parameter stuck where the clip stopped. The punch pair is an
     * unarmed knob turn taking over until the loop comes round -- without it,
     * under an absolute lane, turning a knob does nothing audible. */
    int    driving;
    int    punch_until_wrap;
    double punch_phase;
    lane_point_t pts[LANE_POINTS_MAX];
} lane_t;

typedef struct { lane_t lanes[LANE_MAX]; } lane_store_t;

void   lane_store_reset(lane_store_t *st);

/* Find the lane for (target, param), or NULL. */
lane_t *lane_find(lane_store_t *st, const char *target, const char *param);

/* Find, else take a free slot and bind it. NULL when the store is full. */
lane_t *lane_alloc(lane_store_t *st, const char *target, const char *param,
                   int track, int slot, const lane_fingerprint_t *fp);

/* Insert or replace a breakpoint. Keeps pts[] sorted by phase. A write within
 * LANE_MIN_POINT_BEATS of an existing point overwrites that point's value --
 * which is both the thinning rule and the second-pass replace rule. */
void lane_write(lane_t *ln, double phase, float value);

/* Value at `phase`, considering only points below loop_len.
 * `stepped` = 1 for int/enum params (hold), 0 for float (linear).
 * Returns 1 and writes *out, or 0 for "this lane has nothing to say" --
 * which is NOT 0.0, and the caller must not treat it as a value. */
int lane_eval(const lane_t *ln, double phase, double loop_len, int stepped,
              float *out);

/* Does this lane still describe the clip that is there now? */
int lane_fingerprint_matches(const lane_t *ln, const lane_fingerprint_t *now);

#ifdef __cplusplus
}
#endif
#endif /* LANE_STORE_H */
```

- [ ] **Step 2: Write the failing tests**

Create `tests/host/test_lane_store.c`:

```c
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lane_store.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); fails++; } } while (0)

static lane_t *mk(lane_store_t *st) {
    lane_fingerprint_t fp = { 0.0, 8.0, 14, 41 };
    lane_store_reset(st);
    return lane_alloc(st, "synth", "cutoff", 2, 3, &fp);
}

int main(void) {
    lane_store_t st;
    float v;

    /* 1. An empty lane says nothing -- it does not say 0.0. */
    lane_t *ln = mk(&st);
    CHECK(ln != NULL, "alloc returned NULL");
    CHECK(lane_eval(ln, 0.0, 8.0, 0, &v) == 0, "empty lane produced a value");

    /* 2. One point is that value everywhere (hold at both ends). */
    lane_write(ln, 2.0, 0.5f);
    CHECK(lane_eval(ln, 0.0, 8.0, 0, &v) == 1 && fabsf(v - 0.5f) < 1e-6f,
          "single point before: %f", v);
    CHECK(lane_eval(ln, 7.9, 8.0, 0, &v) == 1 && fabsf(v - 0.5f) < 1e-6f,
          "single point after: %f", v);

    /* 3. Linear between two float points. */
    lane_write(ln, 6.0, 1.0f);
    CHECK(lane_eval(ln, 4.0, 8.0, 0, &v) == 1 && fabsf(v - 0.75f) < 1e-6f,
          "midpoint interp: %f", v);

    /* 4. Stepped (int/enum) holds the previous value instead. */
    CHECK(lane_eval(ln, 4.0, 8.0, 1, &v) == 1 && fabsf(v - 0.5f) < 1e-6f,
          "stepped should hold 0.5, got %f", v);

    /* 5. A point past the CURRENT loop end is ignored -- and retained. */
    ln = mk(&st);
    lane_write(ln, 1.0, 0.2f);
    lane_write(ln, 12.0, 0.9f);       /* beyond an 8-beat loop */
    CHECK(lane_eval(ln, 7.0, 8.0, 0, &v) == 1 && fabsf(v - 0.2f) < 1e-6f,
          "dormant point leaked into the curve: %f", v);
    CHECK(ln->n == 2, "dormant point was dropped (n=%d)", ln->n);

    /* 6. Extending the clip reveals it, with no rewrite. */
    CHECK(lane_eval(ln, 12.0, 16.0, 0, &v) == 1 && fabsf(v - 0.9f) < 1e-6f,
          "extended loop did not reveal the point: %f", v);

    /* 7. Thinning: a second write inside the window replaces, not appends. */
    ln = mk(&st);
    lane_write(ln, 1.000f, 0.1f);
    lane_write(ln, 1.005f, 0.4f);     /* < LANE_MIN_POINT_BEATS away */
    CHECK(ln->n == 1, "thinning failed (n=%d)", ln->n);
    CHECK(fabsf(ln->pts[0].value - 0.4f) < 1e-6f,
          "thinning kept the OLD value: %f", ln->pts[0].value);

    /* 8. Out-of-order writes leave the list sorted. */
    ln = mk(&st);
    lane_write(ln, 4.0, 0.4f);
    lane_write(ln, 1.0, 0.1f);
    lane_write(ln, 2.0, 0.2f);
    CHECK(ln->n == 3 && ln->pts[0].phase < ln->pts[1].phase &&
          ln->pts[1].phase < ln->pts[2].phase, "points are not sorted");

    /* 9. A full lane degrades resolution; it never drops the gesture. */
    ln = mk(&st);
    for (int i = 0; i < LANE_POINTS_MAX + 8; i++)
        lane_write(ln, 0.1 + i * 0.5, (float)i / 100.0f);
    CHECK(ln->n == LANE_POINTS_MAX, "overflowed (n=%d)", ln->n);
    CHECK(ln->full_hits == 8, "full_hits=%d, want 8", ln->full_hits);

    /* 10. The fingerprint refuses a same-geometry clip with other notes. */
    ln = mk(&st);
    lane_fingerprint_t same = { 0.0, 8.0, 14, 41 };
    lane_fingerprint_t copy = { 0.0, 8.0, 9,  41 };
    CHECK(lane_fingerprint_matches(ln, &same) == 1, "identical fp rejected");
    CHECK(lane_fingerprint_matches(ln, &copy) == 0,
          "different note count accepted");

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: lane_store\n");
    return 0;
}
```

Create `tests/host/test_lane_store.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
bin="build/tests/host/test_lane_store"
mkdir -p "$(dirname "$bin")"
cc -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
  -Isrc/host tests/host/test_lane_store.c src/host/lane_store.c -lm -o "$bin"
"$bin"
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `chmod +x tests/host/test_lane_store.sh && bash tests/host/test_lane_store.sh`
Expected: FAIL — linker errors, `undefined symbol: lane_store_reset` etc.

- [ ] **Step 4: Implement `src/host/lane_store.c`**

```c
#include "lane_store.h"
#include <string.h>

void lane_store_reset(lane_store_t *st) {
    if (st) memset(st, 0, sizeof(*st));
}

lane_t *lane_find(lane_store_t *st, const char *target, const char *param) {
    if (!st || !target || !param) return 0;
    for (int i = 0; i < LANE_MAX; i++) {
        lane_t *ln = &st->lanes[i];
        if (!ln->used) continue;
        if (strcmp(ln->target, target) == 0 && strcmp(ln->param, param) == 0)
            return ln;
    }
    return 0;
}

lane_t *lane_alloc(lane_store_t *st, const char *target, const char *param,
                   int track, int slot, const lane_fingerprint_t *fp) {
    if (!st || !target || !param) return 0;
    lane_t *ln = lane_find(st, target, param);
    if (ln) return ln;
    for (int i = 0; i < LANE_MAX; i++) {
        ln = &st->lanes[i];
        if (ln->used) continue;
        memset(ln, 0, sizeof(*ln));
        ln->used = 1;
        snprintf(ln->target, sizeof(ln->target), "%s", target);
        snprintf(ln->param, sizeof(ln->param), "%s", param);
        ln->track = track;
        ln->slot = slot;
        if (fp) ln->fp = *fp;
        return ln;
    }
    return 0;   /* full: the caller reports it, never silently discards */
}

static int lane_nearest(const lane_t *ln, double phase) {
    int best = 0;
    double bestd = 1e30;
    for (int i = 0; i < ln->n; i++) {
        double d = ln->pts[i].phase - phase;
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

void lane_write(lane_t *ln, double phase, float value) {
    if (!ln || !ln->used || phase < 0.0) return;

    /* Inside the window of an existing point: replace it. This is the thinning
     * rule AND the second-pass replace rule -- recording over a region
     * overwrites the points you pass rather than layering a second curve. */
    for (int i = 0; i < ln->n; i++) {
        double d = ln->pts[i].phase - phase;
        if (d < 0) d = -d;
        if (d < LANE_MIN_POINT_BEATS) { ln->pts[i].value = value; return; }
    }

    if (ln->n >= LANE_POINTS_MAX) {
        /* Degrade resolution rather than drop the gesture -- a lost write in
         * the middle of a sweep is a hole the user cannot see or fix. */
        int i = lane_nearest(ln, phase);
        ln->pts[i].phase = phase;
        ln->pts[i].value = value;
        ln->full_hits++;
        /* Re-sort the single moved element. */
        while (i > 0 && ln->pts[i - 1].phase > ln->pts[i].phase) {
            lane_point_t t = ln->pts[i - 1];
            ln->pts[i - 1] = ln->pts[i]; ln->pts[i] = t; i--;
        }
        while (i + 1 < ln->n && ln->pts[i + 1].phase < ln->pts[i].phase) {
            lane_point_t t = ln->pts[i + 1];
            ln->pts[i + 1] = ln->pts[i]; ln->pts[i] = t; i++;
        }
        return;
    }

    int at = ln->n;
    for (int i = 0; i < ln->n; i++) {
        if (ln->pts[i].phase > phase) { at = i; break; }
    }
    for (int i = ln->n; i > at; i--) ln->pts[i] = ln->pts[i - 1];
    ln->pts[at].phase = phase;
    ln->pts[at].value = value;
    ln->n++;
}

int lane_eval(const lane_t *ln, double phase, double loop_len, int stepped,
              float *out) {
    if (!ln || !ln->used || !out || ln->n <= 0) return 0;
    if (ln->stale || ln->orphaned) return 0;
    if (loop_len <= 0.0) return 0;

    /* Only points that are INSIDE the clip as it is right now. */
    int last = -1;
    for (int i = 0; i < ln->n; i++) {
        if (ln->pts[i].phase >= loop_len) break;
        last = i;
    }
    if (last < 0) return 0;

    if (phase <= ln->pts[0].phase) { *out = ln->pts[0].value; return 1; }
    if (phase >= ln->pts[last].phase) { *out = ln->pts[last].value; return 1; }

    for (int i = 0; i < last; i++) {
        const lane_point_t *a = &ln->pts[i];
        const lane_point_t *b = &ln->pts[i + 1];
        if (phase < a->phase || phase > b->phase) continue;
        if (stepped) { *out = a->value; return 1; }
        double span = b->phase - a->phase;
        if (span <= 0.0) { *out = b->value; return 1; }
        double t = (phase - a->phase) / span;
        *out = (float)(a->value + t * (b->value - a->value));
        return 1;
    }
    *out = ln->pts[last].value;
    return 1;
}

int lane_fingerprint_matches(const lane_t *ln, const lane_fingerprint_t *now) {
    if (!ln || !now) return 0;
    const double eps = 1e-6;
    double ds = ln->fp.loop_start - now->loop_start;
    double dl = ln->fp.loop_len - now->loop_len;
    if (ds < 0) ds = -ds;
    if (dl < 0) dl = -dl;
    /* loop_len is deliberately NOT compared: a clip that grew is the same
     * clip, and that is the routine case this whole design tolerates. The
     * length lives in the fingerprint for diagnostics only. */
    (void)dl;
    if (ds > eps) return 0;
    if (ln->fp.note_count != now->note_count) return 0;
    if (ln->fp.first_note != now->first_note) return 0;
    return 1;
}
```

Add `#include <stdio.h>` at the top for `snprintf`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `bash tests/host/test_lane_store.sh`
Expected: `PASS: lane_store`

- [ ] **Step 6: Wire into the chain build**

In `scripts/build.sh`, the `build/modules/chain/dsp.so` rule (around line 505):
add `src/host/lane_store.c` to **both** the `needs_rebuild` dependency list and
the compile command, next to the existing `src/host/unified_log.c`.

Run: `./scripts/build.sh 2>&1 | tail -5`
Expected: build completes, `build/modules/chain/dsp.so` rebuilt.

- [ ] **Step 7: Commit**

```bash
git add src/host/lane_store.h src/host/lane_store.c \
        tests/host/test_lane_store.c tests/host/test_lane_store.sh scripts/build.sh
git commit -m "lanes: the pure breakpoint store — unbounded, wrapped at the clip's current length"
```

---

## Task 2: Clip phase reaches the chain — a dlsym'd entry point

**Goal:** Every block, each active chain slot is told its Move track's clip
phase and loop length, or told that it is unknown.

**Files:**
- Modify: `src/modules/chain/dsp/chain_mod.c` (exported `chain_set_clip_phase`)
  — **not `chain_host.c`**: `tests/host/test_chain_host_file_split.sh` pins that
  file under 2900 lines and it sits at 2871, so the function plus its comment
  does not fit. It belongs beside the modulation bus that consumes it anyway.
- Modify: `src/modules/chain/dsp/chain_internal.h` (three fields on the instance)
- Modify: `src/host/shadow_chain_mgmt.c:2443` (dlsym), `src/host/shadow_chain_mgmt.h:207` (extern)
- Modify: `src/schwung_shim.c:1995` (push it at the top of the per-slot loop)
- Create: `tests/host/test_lane_phase_seam.sh`

**Acceptance Criteria:**
- [ ] `host_api_v1_t` is **byte-identical** — `sizeof` still 184, `reserved` still 64 bytes at +120
- [ ] `chain_set_clip_phase` is resolved by dlsym exactly as `chain_take_midi_tick_wake` is, and a NULL resolution degrades to "phase unknown" rather than crashing
- [ ] Slot *N* is given Move track *N*'s phase
- [ ] `identity_valid && !anchor_valid` reaches the chain as **unknown**, not as phase 0
- [ ] The push happens before the idle gate, so a silent slot is told too

**Verify:** `bash tests/host/test_lane_phase_seam.sh && ./scripts/build.sh 2>&1 | tail -3`

**Steps:**

- [ ] **Step 1: Add the receiving state to the chain instance**

In `src/modules/chain/dsp/chain_internal.h`, inside `chain_instance_t` (beside
the other per-instance runtime state, near `mod_targets`):

```c
    /* Clip phase, pushed by the shim once per block. NOT read from the host
     * api struct: its `reserved` tail begins at +120, the exact offset a
     * shipped breakbeat build calls as get_project_bpm(), so a live pointer
     * there boot-loops the device. Same reason move_plugin_render_split is
     * dlsym'd rather than a field on plugin_api_v2_t. */
    int    clip_phase_valid;      /* 0 = UNKNOWN. Not zero. Unknown. */
    double clip_phase_beats;      /* beats from the clip's loop start */
    double clip_loop_len;         /* beats */
    /* Which clip the phase belongs to, and what it looks like right now. All
     * pushed together in ONE call, deliberately: these five are facts about
     * one clip at one instant, and splitting them across calls lets a lane
     * bind a fingerprint to a position it did not come from. */
    int    lane_track;            /* Move track 0..3 (== the slot index) */
    int    lane_clip_slot;        /* 0..7 */
    int    clip_fp_valid;
    lane_fingerprint_t clip_fp;   /* content fingerprint; see Task 6 */
```

`lane_fingerprint_t` comes from `lane_store.h`, which this header now includes
(Task 4 adds the include for `lane_store_t`; add it here in Task 2 instead, so
the struct compiles from this task onward).

Task 6 is what *fills* `clip_fp` with real note data — until then the shim
passes `clip_fp_valid = 0`, and nothing compares against an invalid
fingerprint, because **an unknown clip is not a mismatched one.**

- [ ] **Step 2: Export the entry point from the chain**

In `src/modules/chain/dsp/chain_host.c`, beside `chain_take_midi_tick_wake`
(around line 2871):

```c
/* Pushed by the shim once per block, per slot, BEFORE the idle gate.
 *
 * valid == 0 means "we could not tell where in the clip we are" -- which is
 * not phase 0 and must never be used as one. A lane on an unanchored track
 * stays silent and refuses to record; see clip_state.h. */
void chain_set_clip_phase(void *instance, int valid, double phase_beats,
                          double loop_len, int track, int clip_slot,
                          int fp_valid, const double *fp /* 4 doubles */) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst) return;
    inst->clip_phase_valid = valid ? 1 : 0;
    inst->clip_phase_beats = phase_beats;
    inst->clip_loop_len = loop_len;
    inst->lane_track = track;
    inst->lane_clip_slot = clip_slot;
    inst->clip_fp_valid = (fp_valid && fp) ? 1 : 0;
    if (fp_valid && fp) {
        inst->clip_fp.loop_start = fp[0];
        inst->clip_fp.loop_len   = fp[1];
        inst->clip_fp.note_count = (int)fp[2];
        inst->clip_fp.first_note = (int)fp[3];
    }
}
```

**The signature is final here and is not grown again.** It carries the
fingerprint from the first commit even though Task 6 is what fills it, because
every later task would otherwise have to re-edit the `dlsym` cast, the shim
call and the pin test in step 6 — three places, twice, for no behaviour. The
fingerprint crosses as four `double`s rather than the struct so the shim does
not need `lane_store.h`'s layout to agree with the chain's.

- [ ] **Step 3: Resolve it in the shim**

`src/host/shadow_chain_mgmt.c` — beside line 78:

```c
void (*shadow_chain_set_clip_phase)(void *, int, double, double, int, int,
                                    int, const double *) = NULL;
```

beside line 2443:

```c
    shadow_chain_set_clip_phase =
        (void (*)(void *, int, double, double, int, int, int, const double *))
        dlsym(shadow_dsp_handle, "chain_set_clip_phase");
```

and add it to the `unified_log` line below so a failed resolution is visible
rather than silent. `src/host/shadow_chain_mgmt.h` beside line 207:

```c
extern void (*shadow_chain_set_clip_phase)(void *, int, double, double,
                                           int, int, int, const double *);
```

- [ ] **Step 4: Resolve slot → track → phase, in the shim**

Add to `src/host/shadow_chain_mgmt.c`, near `shadow_chain_slot_recv_channel`:

```c
/* Slot N rides Move track N. That binding is not new: the shim already builds
 * a slot as move_track[s] + synth[s], so the four slot stems ARE the four
 * tracks. Returns 1 and fills both outputs, else 0 for UNKNOWN. */
int shadow_slot_clip_phase(int slot, double *phase_beats, double *loop_len,
                           int *clip_slot, int *fp_valid, double *fp /* [4] */) {
    if (slot < 0 || slot >= CLIP_TRACKS || !phase_beats || !loop_len ||
        !clip_slot || !fp_valid || !fp) return 0;
    *clip_slot = -1;
    *fp_valid = 0;
    const clip_state_t *cs = clip_state_current();
    if (!cs) return 0;
    const clip_track_state_t *tr = &cs->tracks[slot];
    if (!tr->identity_valid || tr->clip_slot < 0) return 0;
    *clip_slot = tr->clip_slot;
    const clip_regions_t *rg = shadow_clip_regions();   /* existing accessor */
    if (!rg || !rg->valid) return 0;
    const clip_region_t *r = &rg->slots[slot][tr->clip_slot];
    if (!r->exists || r->loop_len <= 0.0) return 0;

    /* The fingerprint is valid as soon as the CLIP is known, independently of
     * whether the phase is. Identity and anchor are separately valid
     * (clip_state.h), and a lane still needs to know whether it is bound to
     * the right clip while it waits for an anchor. */
    fp[0] = r->loop_start;
    fp[1] = r->loop_len;
    fp[2] = (double)r->note_count;   /* Task 6; 0 until then */
    fp[3] = (double)r->first_note;   /* Task 6; -1 until then */
    *fp_valid = 1;

    if (!tr->anchor_valid) return 0;   /* clip known, phase UNKNOWN */
    double ph = 0.0;
    if (!clip_phase_beats(tr, (uint32_t)shadow_transport_pulses,
                          r->loop_start, r->loop_len, &ph)) return 0;
    *phase_beats = ph - r->loop_start;   /* beats from the LOOP START */
    *loop_len = r->loop_len;
    return 1;
}
```

Note the return value answers **phase**, while `*fp_valid` and `*clip_slot`
answer **identity** — they are filled even when the function returns 0. That
split is Project 1's rule carried across the seam: a refresh gives identity
with no anchor, and collapsing the two is how a lane binds to the right clip at
the wrong phase.

If `shadow_clip_regions()` does not exist, expose the existing `g_regions`
from `src/host/shim_worker.c` with an accessor of that name rather than making
a second copy of the parse.

- [ ] **Step 5: Push it, before the idle gate**

`src/schwung_shim.c`, immediately inside the per-slot loop at line 1996 (after
the `continue` guard, before the fade/idle handling):

```c
            /* Tell the slot where its Move track's clip is. BEFORE the idle
             * gate on purpose: a silent slot still ticks its modulation via
             * mod:tick, and a lane must keep playing through silence. */
            if (shadow_chain_set_clip_phase) {
                double lane_phase = 0.0, lane_loop = 0.0, lane_fp[4] = {0,0,0,-1};
                int lane_clip = -1, lane_fp_ok = 0;
                int lane_ok = shadow_slot_clip_phase(s, &lane_phase, &lane_loop,
                                                     &lane_clip, &lane_fp_ok,
                                                     lane_fp);
                shadow_chain_set_clip_phase(shadow_chain_slots[s].instance,
                                            lane_ok, lane_phase, lane_loop,
                                            s, lane_clip, lane_fp_ok, lane_fp);
            }
```

- [ ] **Step 6: Pin the seam**

Create `tests/host/test_lane_phase_seam.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
fail() { echo "FAIL: $1"; exit 1; }

# 1. The host ABI is untouched. The front of `reserved` IS +120 -- the offset a
#    shipped breakbeat build calls as get_project_bpm() -- so consuming it puts
#    a live pointer there and boot-loops the device. test_host_api_reserved_tail
#    cannot see that (it inspects a zeroed struct), so the rule is pinned here.
grep -q 'void \*reserved\[8\];' src/host/plugin_api_v1.h \
  || fail "host_api_v1_t's reserved tail changed — see the plan's ABI note"

# 2. The phase seam is a dlsym, like chain_take_midi_tick_wake and
#    move_plugin_render_split, and NOT a host_api field.
grep -q 'dlsym(shadow_dsp_handle, "chain_set_clip_phase")' \
  src/host/shadow_chain_mgmt.c || fail "chain_set_clip_phase is not dlsym'd"
grep -q 'clip_phase' src/host/plugin_api_v1.h \
  && fail "clip phase leaked into host_api_v1_t"

# 3. The push precedes the idle gate, or a silent slot's lane freezes.
push=$(grep -n 'shadow_chain_set_clip_phase(' src/schwung_shim.c | head -1 | cut -d: -f1)
gate=$(grep -n 'if (shadow_slot_idle\[s\]) {' src/schwung_shim.c | head -1 | cut -d: -f1)
[ -n "$push" ] && [ -n "$gate" ] && [ "$push" -lt "$gate" ] \
  || fail "clip phase is pushed after the idle gate (push=$push gate=$gate)"

echo "PASS: lane phase seam"
```

- [ ] **Step 7: Run it and build**

Run: `chmod +x tests/host/test_lane_phase_seam.sh && bash tests/host/test_lane_phase_seam.sh && make -C tests/host test 2>&1 | tail -3`
Expected: `PASS: lane phase seam`, and `test_host_api_reserved_tail` still passes.

- [ ] **Step 8: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c src/modules/chain/dsp/chain_internal.h \
        src/host/shadow_chain_mgmt.c src/host/shadow_chain_mgmt.h \
        src/schwung_shim.c tests/host/test_lane_phase_seam.sh
git commit -m "lanes: push clip phase to each slot through a dlsym'd entry point, not the host ABI"
```

---

## Task 3: The override source class in `chain_mod`

**Goal:** A source can set a target's **absolute** value; LFOs still sum on top;
clearing it returns the parameter to the user's knob.

**Files:**
- Modify: `src/modules/chain/dsp/chain_internal.h:187-192` (one field on `mod_source_contribution_t`), `:1176` (declaration)
- Modify: `src/modules/chain/dsp/chain_mod.c:107-166` (recompute), new `chain_mod_emit_override`
- Create: `tests/host/test_chain_mod_override.c`, `tests/host/test_chain_mod_override.sh`

**Acceptance Criteria:**
- [ ] With an override active, the plugin receives the override's value, not `base + contribution`
- [ ] An LFO offset on the same target sums **on top of** the override
- [ ] Removing the override restores the base to the plugin (one write, `force_write`)
- [ ] The result is clamped to the parameter's `min_val`/`max_val`
- [ ] Existing behaviour is unchanged when no override is present — `test_chain_mod_plain_read_base.sh` still passes

**Verify:** `bash tests/host/test_chain_mod_override.sh && bash tests/host/test_chain_mod_plain_read_base.sh`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_chain_mod_override.c`. Copy the `#include`s and the
`chain_log` / `v2_*` stub block from `tests/host/test_chain_mod_plain_read_base.c`
(lines 24-41) verbatim — `chain_mod.c` will not link without them — then add
these two helpers, which the next three tasks also use:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chain_internal.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); fails++; } } while (0)

/* ------------------------------------------------- fake synth (the module) */
static char fake_value[64] = "10";
static void fake_set_param(void *inst, const char *key, const char *val) {
    (void)inst; (void)key;
    snprintf(fake_value, sizeof(fake_value), "%s", val);
}
static int fake_get_param(void *inst, const char *key, char *buf, int len) {
    (void)inst; (void)key;
    return snprintf(buf, len, "%s", fake_value);
}
static plugin_api_v2_t fake_api = {
    .api_version = 2,
    .set_param = fake_set_param,
    .get_param = fake_get_param,
};

/* What the module actually received -- the DESTINATION side. No param read can
 * answer this: a plain read serves the BASE by design (#276) and ':effective'
 * serves chain_mod's own table. Both are the chain's numbers. Reading
 * ':effective' and calling it verified is exactly what made the parked
 * feat/slot-mod-routes verification hollow -- reported 18/18, parameter never
 * moved. */
static float fake_last_value(void) { return (float)atof(fake_value); }

/* One float param, range 0..127, default 10 -- matching fake_value above. */
static void setup_fake_synth(chain_instance_t *inst) {
    snprintf(fake_value, sizeof(fake_value), "10");
    inst->synth_plugin_v2 = &fake_api;
    inst->synth_instance = (void *)0x1;
    inst->synth_chain_param_count = 1;
    chain_param_info_t *p = &inst->synth_chain_params[0];
    snprintf(p->key, sizeof(p->key), "cutoff");
    p->type = KNOB_TYPE_FLOAT;
    p->min_val = 0.0f;
    p->max_val = 127.0f;
    p->default_val = 10.0f;
}
```

Check the exact member names for the param cache against
`chain_internal.h` and `find_param_by_key` in `chain_params.c` before running —
if they differ, follow the header, not this snippet. Then:

```c
int main(void) {
    chain_instance_t *inst = calloc(1, sizeof(*inst));
    setup_fake_synth(inst);            /* from the copied scaffolding */

    /* A lane sets an absolute value. */
    chain_mod_emit_override(inst, "lane", "synth", "cutoff", 90.0f, 1);
    CHECK(fake_last_value() == 90.0f,
          "override did not reach the plugin: %f", fake_last_value());

    /* An LFO offset sums ON TOP of it, rather than replacing or being lost. */
    chain_mod_emit_value(inst, "lfo1", "synth", "cutoff",
                         1.0f /*signal*/, 0.1f /*depth*/, 0.0f /*offset*/,
                         1 /*bipolar*/, 1 /*enabled*/);
    CHECK(fake_last_value() > 90.0f,
          "LFO did not sum on top of the override: %f", fake_last_value());

    /* Dropping the override returns the parameter to the user's knob. */
    chain_mod_emit_value(inst, "lfo1", "synth", "cutoff", 0, 0, 0, 1, 0);
    chain_mod_emit_override(inst, "lane", "synth", "cutoff", 0.0f, 0);
    CHECK(fake_last_value() == 10.0f,
          "clearing the override did not restore the base: %f",
          fake_last_value());
    ...
}
```

Create `tests/host/test_chain_mod_override.sh` as a copy of
`tests/host/test_chain_mod_plain_read_base.sh` with the source file and binary
name changed (it compiles `chain_mod.c`, `chain_params.c`, `chain_json.c` with
the `malloc.h` shim for macOS).

- [ ] **Step 2: Run it, verify it fails**

Run: `chmod +x tests/host/test_chain_mod_override.sh && bash tests/host/test_chain_mod_override.sh`
Expected: FAIL — `undefined symbol: chain_mod_emit_override`

- [ ] **Step 3: Add the field**

`src/modules/chain/dsp/chain_internal.h`, in `mod_source_contribution_t`:

```c
typedef struct mod_source_contribution {
    int active;
    char source_id[32];
    float contribution;
    /* An OVERRIDE carries an absolute value in `contribution` and replaces the
     * base rather than adding to it. That is what an automation lane is: the
     * lane IS the value, the knob is the base underneath it. Offsets from
     * LFOs still sum on top, so the two compose. */
    int is_override;
} mod_source_contribution_t;
```

and declare beside line 1176:

```c
CHAIN_INTERNAL int chain_mod_emit_override(void *ctx, const char *source_id,
                                           const char *target, const char *param,
                                           float value, int enabled);
```

- [ ] **Step 4: Teach recompute about it**

Replace `chain_mod_recompute_effective` (`chain_mod.c:156`):

```c
static void chain_mod_recompute_effective(mod_target_state_t *entry) {
    float base = entry->base_value;
    float sum = 0.0f;
    for (int i = 0; i < MAX_MOD_SOURCES_PER_TARGET; i++) {
        const mod_source_contribution_t *s = &entry->sources[i];
        if (!s->active) continue;
        if (s->is_override) base = s->contribution;   /* absolute: replaces */
        else                sum += s->contribution;   /* relative: sums */
    }
    entry->effective_value = chain_mod_clampf(base + sum,
                                              entry->min_val, entry->max_val);
}
```

`chain_mod_sum_contributions` keeps its current meaning and is left alone; only
this function decides the effective value.

- [ ] **Step 5: Add the emitter**

In `chain_mod.c`, after `chain_mod_emit_value`:

```c
/* Absolute modulation: the source dictates the value outright.
 *
 * Shares every guard, the param-info lookup, the throttle and the base capture
 * with chain_mod_emit_value -- the only difference is that the contribution is
 * the value itself and is flagged as replacing the base. Disabling it goes
 * through the ordinary clear path, so the parameter returns to the knob with a
 * forced write rather than sticking wherever the lane stopped. */
int chain_mod_emit_override(void *ctx, const char *source_id,
                            const char *target, const char *param,
                            float value, int enabled) {
    chain_instance_t *inst = (chain_instance_t *)ctx;
    if (!inst || !source_id || !target || !param) return -1;
    if (!enabled) { chain_mod_clear_source(inst, source_id); return 0; }

    chain_param_info_t *pinfo = find_param_by_key(inst, target, param);
    if (!pinfo) return -1;

    mod_target_state_t *entry = chain_mod_alloc_target_entry(inst, target, param);
    if (!entry) return -1;
    mod_source_contribution_t *src =
        chain_mod_find_or_alloc_source_contribution(entry, source_id);
    if (!src) return -1;

    if (!entry->enabled) {
        float base = pinfo->default_val;
        char val_buf[64];
        if (chain_mod_get_param_string(inst, target, param, val_buf,
                                       sizeof(val_buf)) > 0)
            base = dsp_value_to_float(val_buf, pinfo, base);
        entry->base_value = chain_mod_clampf(base, pinfo->min_val, pinfo->max_val);
    }
    entry->type = pinfo->type;
    entry->min_val = pinfo->min_val;
    entry->max_val = pinfo->max_val;

    src->is_override = 1;
    src->contribution = chain_mod_clampf(value, pinfo->min_val, pinfo->max_val);
    entry->enabled = chain_mod_has_active_sources(entry);
    chain_mod_apply_effective_value(inst, entry, 0);
    return 0;
}
```

In `chain_mod_remove_source_contribution`, clear `is_override` along with the
rest when a slot is freed (it is `memset` there already — verify it is, and add
`entry->sources[i].is_override = 0;` if the clear is field-by-field).

- [ ] **Step 6: Run both tests**

Run: `bash tests/host/test_chain_mod_override.sh && bash tests/host/test_chain_mod_plain_read_base.sh`
Expected: `PASS` from both.

- [ ] **Step 7: Commit**

```bash
git add src/modules/chain/dsp/chain_mod.c src/modules/chain/dsp/chain_internal.h \
        tests/host/test_chain_mod_override.c tests/host/test_chain_mod_override.sh
git commit -m "chain_mod: an override source sets the value outright; offsets still sum on top"
```

---

## Task 4: Lane playback in the chain

**Goal:** Every block, each lane with a valid phase evaluates and drives its
parameter; with no phase, nothing is driven and nothing is guessed.

**Files:**
- Create: `src/modules/chain/dsp/chain_lanes.c`
- Modify: `src/modules/chain/dsp/chain_internal.h` (the store on the instance + declarations)
- Modify: `src/modules/chain/dsp/chain_host.c:937` and `:2387` (call `lane_tick` beside `lfo_tick`)
- Modify: `scripts/build.sh` (add `chain_lanes.c` to the dsp.so rule, both lists)
- Create: `tests/host/test_chain_lanes_playback.c` / `.sh`

**Acceptance Criteria:**
- [ ] `lane_tick` runs on **both** paths — `render_block` and the silent-slot `mod:tick` — so a lane keeps playing through silence
- [ ] `clip_phase_valid == 0` drives nothing at all (no override emitted, no value written)
- [ ] A lane whose phase becomes invalid **releases** its override once, so the parameter returns to the knob rather than sticking
- [ ] The lane store is a member of `chain_instance_t`, **not** of `patch_info_t`
- [ ] int/enum params are evaluated stepped, floats linear (the type comes from `find_param_by_key`)

**Verify:** `bash tests/host/test_chain_lanes_playback.sh`

**Steps:**

- [ ] **Step 1: Put the store on the instance — and nowhere near the patch**

`src/modules/chain/dsp/chain_internal.h`, in `chain_instance_t` beside
`mod_targets`:

```c
#include "lane_store.h"
...
    /* ON THE INSTANCE, never inside patch_info_t. That struct is a STACK LOCAL
     * on the SPI callback (v2_set_param's load_file) and sits MAX_PATCHES deep
     * in this instance -- which is why raising SLOT_BUSES from 4 to 8 took the
     * callback frame from 194 KB to 232 KB. 16 lanes x 64 points is ~9 KB and
     * must not land in either multiplier. */
    lane_store_t lanes;
    int lane_armed;        /* pushed from the shim: Move's Record button */
```

Declare in the same header:

```c
CHAIN_INTERNAL void lane_tick(chain_instance_t *inst);
CHAIN_INTERNAL void lane_release_all(chain_instance_t *inst);
```

- [ ] **Step 2: Write the failing test**

Create `tests/host/test_chain_lanes_playback.c`, reusing the stub + fake-synth
scaffolding from `tests/host/test_chain_mod_plain_read_base.c` (copy it
verbatim), then:

```c
int main(void) {
    chain_instance_t *inst = calloc(1, sizeof(*inst));
    setup_fake_synth(inst);

    lane_fingerprint_t fp = { 0.0, 8.0, 14, 41 };
    lane_t *ln = lane_alloc(&inst->lanes, "synth", "cutoff", 0, 0, &fp);
    lane_write(ln, 0.0, 20.0f);
    lane_write(ln, 4.0, 80.0f);

    /* 1. Phase unknown drives NOTHING -- not 20.0, not the midpoint, nothing. */
    inst->clip_phase_valid = 0;
    lane_tick(inst);
    CHECK(fake_last_value() == 10.0f,
          "a lane drove the parameter with no phase: %f", fake_last_value());

    /* 2. With a phase, it plays. */
    inst->clip_phase_valid = 1;
    inst->clip_phase_beats = 2.0;
    inst->clip_loop_len = 8.0;
    lane_tick(inst);
    CHECK(fake_last_value() == 50.0f,
          "midpoint should be 50, got %f", fake_last_value());

    /* 3. Losing the phase RELEASES, so the knob comes back. */
    inst->clip_phase_valid = 0;
    lane_tick(inst);
    CHECK(fake_last_value() == 10.0f,
          "lost phase left the parameter stuck at %f", fake_last_value());
    ...
}
```

Create the `.sh` as a copy of `tests/host/test_chain_mod_override.sh` with the
source names changed, adding `src/modules/chain/dsp/chain_lanes.c` and
`src/host/lane_store.c` to the compile line.

- [ ] **Step 3: Run it, verify it fails**

Run: `chmod +x tests/host/test_chain_lanes_playback.sh && bash tests/host/test_chain_lanes_playback.sh`
Expected: FAIL — `undefined symbol: lane_tick`

- [ ] **Step 4: Implement `src/modules/chain/dsp/chain_lanes.c`**

```c
/*
 * chain_lanes — clip-associated automation lanes, the chain-side half.
 *
 * The pure model is src/host/lane_store.c; this file is the glue that knows
 * about chain_instance_t, parameter types and the mod bus.
 *
 * RT: lane_tick runs on the SPI callback (render_block, and mod:tick on a
 * silent slot). No allocation, no I/O, no locks.
 */
#include "chain_internal.h"
#include <string.h>

/* One source id per lane, so the mod bus can tell them apart and clear one
 * without touching the others. */
static void lane_source_id(const lane_t *ln, char *buf, int len) {
    snprintf(buf, len, "lane:%s:%s", ln->target, ln->param);
}

void lane_release_all(chain_instance_t *inst) {
    if (!inst) return;
    for (int i = 0; i < LANE_MAX; i++) {
        lane_t *ln = &inst->lanes.lanes[i];
        if (!ln->used || !ln->driving) continue;
        char sid[64];
        lane_source_id(ln, sid, sizeof(sid));
        chain_mod_emit_override(inst, sid, ln->target, ln->param, 0.0f, 0);
        ln->driving = 0;
    }
}

void lane_tick(chain_instance_t *inst) {
    if (!inst) return;

    /* No phase means UNKNOWN, and unknown is not zero. Release anything we are
     * driving -- once -- so the parameter returns to the user's knob rather
     * than freezing wherever the clip happened to stop. */
    if (!inst->clip_phase_valid || inst->clip_loop_len <= 0.0) {
        lane_release_all(inst);
        return;
    }

    for (int i = 0; i < LANE_MAX; i++) {
        lane_t *ln = &inst->lanes.lanes[i];
        if (!ln->used) continue;

        /* An unarmed knob turn punches through until the loop comes round --
         * otherwise, under an absolute lane, turning a knob does nothing
         * audible and reads as a broken encoder. Set in Task 5. */
        if (ln->punch_until_wrap) {
            if (inst->clip_phase_beats < ln->punch_phase) ln->punch_until_wrap = 0;
            else continue;
        }

        chain_param_info_t *pinfo = find_param_by_key(inst, ln->target, ln->param);
        if (!pinfo) continue;      /* module swapped out from under the lane */
        const int stepped = (pinfo->type == KNOB_TYPE_INT ||
                             pinfo->type == KNOB_TYPE_ENUM);

        float v = 0.0f;
        if (!lane_eval(ln, inst->clip_phase_beats, inst->clip_loop_len,
                       stepped, &v)) {
            if (ln->driving) {
                char sid[64];
                lane_source_id(ln, sid, sizeof(sid));
                chain_mod_emit_override(inst, sid, ln->target, ln->param, 0.0f, 0);
                ln->driving = 0;
            }
            continue;
        }

        char sid[64];
        lane_source_id(ln, sid, sizeof(sid));
        chain_mod_emit_override(inst, sid, ln->target, ln->param, v, 1);
        ln->driving = 1;
    }
}
```

`driving`, `punch_until_wrap` and `punch_phase` are already on `lane_t` from
Task 1; nothing new is added to the header here.

- [ ] **Step 5: Call it beside `lfo_tick`, at both sites**

`src/modules/chain/dsp/chain_host.c` line 937 (the `mod:tick` branch) and line
2387 (inside `render_block`) — immediately after each `lfo_tick(inst, frames);`:

```c
        lane_tick(inst);
```

Placing it inside the same conditional as `lfo_tick` is deliberate: it inherits
the existing double-tick discipline in `chain_idle_tick.h`, where ticking twice
in one frame is the bug that guard exists to prevent.

- [ ] **Step 6: Add to the build and run the test**

Add `src/modules/chain/dsp/chain_lanes.c` to both lists in the dsp.so rule in
`scripts/build.sh`.

Run: `bash tests/host/test_chain_lanes_playback.sh && ./scripts/build.sh 2>&1 | tail -3`
Expected: `PASS: chain lanes playback`, clean build.

- [ ] **Step 7: Commit**

```bash
git add src/modules/chain/dsp/chain_lanes.c src/modules/chain/dsp/chain_internal.h \
        src/modules/chain/dsp/chain_host.c src/host/lane_store.h \
        tests/host/test_chain_lanes_playback.c tests/host/test_chain_lanes_playback.sh \
        scripts/build.sh
git commit -m "lanes: play back against clip phase; unknown phase releases rather than guesses"
```

---

## Task 5: Recording, and the unarmed-turn punch

**Goal:** While armed and the phase is known, a knob write appends a breakpoint
at that phase; while unarmed, the knob takes over until the loop comes round.

**Files:**
- Modify: `src/modules/chain/dsp/chain_lanes.c` (`lane_on_set_param`)
- Modify: `src/modules/chain/dsp/chain_host.c:1236`, `:1283`, `:1327` (three call sites)
- Modify: `tests/host/test_chain_lanes_playback.c` (extend)

**Acceptance Criteria:**
- [ ] Armed + phase valid: a write to `synth:cutoff` appends a point at the current phase with that value
- [ ] Armed + phase **invalid**: nothing is recorded (no point at a guessed 0.0)
- [ ] Unarmed: nothing is recorded, and the lane yields to the knob until `clip_phase_beats` wraps past where the turn happened
- [ ] **Playback does not record itself** — 200 `lane_tick` calls after a one-point recording leave `ln->n == 1`
- [ ] A lane is created implicitly on the first armed write to a parameter that resolves through `find_param_by_key`

**Verify:** `bash tests/host/test_chain_lanes_playback.sh`

**Steps:**

- [ ] **Step 1: Extend the test first**

Append to `tests/host/test_chain_lanes_playback.c`:

```c
    /* 4. Armed + phase valid records at the phase, and creates the lane. */
    chain_instance_t *rec = calloc(1, sizeof(*rec));
    setup_fake_synth(rec);
    rec->lane_armed = 1;
    rec->clip_phase_valid = 1;
    rec->clip_loop_len = 8.0;
    rec->clip_phase_beats = 2.0;
    lane_on_set_param(rec, "synth", "cutoff", "55");
    lane_t *made = lane_find(&rec->lanes, "synth", "cutoff");
    CHECK(made && made->n == 1, "armed write did not create a point");
    CHECK(made && made->pts[0].phase == 2.0, "recorded at the wrong phase: %f",
          made ? made->pts[0].phase : -1.0);

    /* 5. PLAYBACK MUST NOT RECORD ITSELF. chain_mod writes straight to the
     *    plugin and never re-enters v2_set_param, so this is structural --
     *    and it is pinned here because if that ever changes, the lane
     *    compounds its own curve every loop, silently and worse each bar. */
    for (int i = 0; i < 200; i++) {
        rec->clip_phase_beats = (double)(i % 8);
        lane_tick(rec);
    }
    CHECK(made->n == 1, "playback recorded itself (n=%d)", made->n);

    /* 6. Armed with NO phase records nothing. */
    rec->clip_phase_valid = 0;
    lane_on_set_param(rec, "synth", "cutoff", "70");
    CHECK(made->n == 1, "recorded with no phase (n=%d)", made->n);

    /* 7. Unarmed, the knob punches through until the loop wraps. */
    rec->lane_armed = 0;
    rec->clip_phase_valid = 1;
    rec->clip_phase_beats = 6.0;
    lane_on_set_param(rec, "synth", "cutoff", "33");
    CHECK(made->n == 1, "an unarmed turn was recorded");
    CHECK(made->punch_until_wrap == 1, "unarmed turn did not punch through");
    lane_tick(rec);                       /* still in the punch */
    CHECK(made->driving == 0, "lane kept driving during the punch");
    rec->clip_phase_beats = 1.0;          /* wrapped */
    lane_tick(rec);
    CHECK(made->driving == 1, "lane did not resume after the wrap");
```

- [ ] **Step 2: Run it, verify it fails**

Run: `bash tests/host/test_chain_lanes_playback.sh`
Expected: FAIL — `undefined symbol: lane_on_set_param`

- [ ] **Step 3: Implement the recorder**

Add to `src/modules/chain/dsp/chain_lanes.c`:

```c
/* A parameter write arrived from the UI.
 *
 * THIS IS NOT CALLED BY PLAYBACK. chain_mod_set_param_string writes the
 * sub-plugin's set_param directly and never re-enters v2_set_param, so a lane
 * cannot record its own output. That is structural rather than a flag, and the
 * playback-does-not-record assertion in the unit test is what keeps it true.
 *
 * Phase is sampled HERE -- on the callback, at the moment of the write --
 * rather than at UI frame time, because a frame is ~23 ms and a knob sweep is
 * faster than that. */
void lane_on_set_param(chain_instance_t *inst, const char *target,
                       const char *param, const char *val) {
    if (!inst || !target || !param || !val) return;

    chain_param_info_t *pinfo = find_param_by_key(inst, target, param);
    if (!pinfo) return;
    const float v = dsp_value_to_float(val, pinfo, pinfo->default_val);

    if (inst->lane_armed && inst->clip_phase_valid && inst->clip_loop_len > 0.0) {
        lane_fingerprint_t fp;
        lane_current_fingerprint(inst, &fp);     /* Task 6 fills this in */
        lane_t *ln = lane_alloc(&inst->lanes, target, param,
                                inst->lane_track, inst->lane_clip_slot, &fp);
        if (!ln) return;                          /* store full: no silent drop */
        lane_write(ln, inst->clip_phase_beats, v);
        ln->punch_until_wrap = 0;                 /* an armed turn IS the lane */
        return;
    }

    /* Unarmed, under an absolute lane: hand the parameter to the knob until
     * the loop comes round. Without this the knob is inaudible and reads as
     * broken -- the lane is writing the same target every block. */
    lane_t *ln = lane_find(&inst->lanes, target, param);
    if (!ln || !ln->used) return;
    if (inst->clip_phase_valid) {
        ln->punch_until_wrap = 1;
        ln->punch_phase = inst->clip_phase_beats;
    }
    if (ln->driving) {
        char sid[64];
        lane_source_id(ln, sid, sizeof(sid));
        chain_mod_emit_override(inst, sid, ln->target, ln->param, 0.0f, 0);
        ln->driving = 0;
    }
}
```

`lane_track` and `lane_clip_slot` are already on the instance and already
pushed by Task 2 — nothing is added to the seam here.

`lane_current_fingerprint` is defined in this task, in `chain_lanes.c`, as a
one-line read of what Task 2 pushes:

```c
/* The clip's fingerprint as it is RIGHT NOW. Task 6 is what fills it with real
 * note data; until then clip_fp_valid is 0 and a lane records with an
 * all-zero fingerprint, which nothing compares against -- an unknown clip is
 * not a mismatched one. */
void lane_current_fingerprint(chain_instance_t *inst, lane_fingerprint_t *out) {
    if (!inst || !out) return;
    memset(out, 0, sizeof(*out));
    if (inst->clip_fp_valid) *out = inst->clip_fp;
}
```

Declare both in `chain_internal.h`:

```c
CHAIN_INTERNAL void lane_on_set_param(chain_instance_t *inst, const char *target,
                                      const char *param, const char *val);
CHAIN_INTERNAL void lane_current_fingerprint(chain_instance_t *inst,
                                             lane_fingerprint_t *out);
```

- [ ] **Step 4: Hook the three `v2_set_param` branches**

In `src/modules/chain/dsp/chain_host.c`, immediately after each existing
`chain_mod_update_base_from_set_param(...)` call — lines 1236, 1283 and 1327:

```c
                lane_on_set_param(inst, "synth", subkey, val);     /* :1236 */
                lane_on_set_param(inst, fx_id, subkey, val);       /* :1283 */
                lane_on_set_param(inst, mfx_id, subkey, val);      /* :1327 */
```

- [ ] **Step 5: Run the test**

Run: `bash tests/host/test_chain_lanes_playback.sh`
Expected: `PASS: chain lanes playback`

- [ ] **Step 6: Commit**

```bash
git add src/modules/chain/dsp/chain_lanes.c src/modules/chain/dsp/chain_host.c \
        src/modules/chain/dsp/chain_internal.h tests/host/test_chain_lanes_playback.c
git commit -m "lanes: record on an armed knob write; an unarmed turn punches through to the wrap"
```

---

## Task 6: The fingerprint — note count and first note from `Song.abl`

**Goal:** `clip_region_t` carries enough of the clip's content to tell a
replacement clip from the one a lane was recorded against.

**Files:**
- Modify: `src/host/clip_regions.h` (two fields), `src/host/clip_regions.c` (parse)
- Modify: `src/modules/chain/dsp/chain_lanes.c` (`lane_current_fingerprint`)
- Modify: `src/host/shadow_chain_mgmt.c` (pass the fingerprint with the phase push)
- Modify: `tests/host/test_clip_regions.c`

**Acceptance Criteria:**
- [ ] `note_count` and `first_note` are parsed for every clip in `tests/fixtures/song_abl_sample.json`
- [ ] A clip with no notes reports `note_count == 0`, `first_note == -1` — not 0
- [ ] The parser's existing outputs (`loop_start`, `loop_len`, `scroll_beats`, `exists`, `is_playing`) are unchanged — `test_clip_regions` still passes on every existing assertion
- [ ] A lane whose fingerprint no longer matches is marked `stale` and stops driving
- [ ] A lane whose clip was **deleted** is marked `orphaned`, stops driving, and is **still present** in `lanes:state` afterwards — deleting it would be destroying user data on a file-diff heuristic
- [ ] A clip that reappears (undo) un-orphans its lanes via the fingerprint match

**Verify:** `make -C tests/host test 2>&1 | grep clip_regions` → PASS

**Steps:**

- [ ] **Step 1: Extend the test first**

In `tests/host/test_clip_regions.c`, after the existing geometry assertions for
the sample set:

```c
    /* The fingerprint's content half. A clip copied into another slot has the
     * same geometry and different notes -- geometry alone cannot tell them
     * apart, which is the whole reason these two fields exist. */
    CHECK(rg.slots[0][0].note_count > 0,
          "note_count not parsed (%d)", rg.slots[0][0].note_count);
    CHECK(rg.slots[0][0].first_note == 41,
          "first_note is %d, want 41", rg.slots[0][0].first_note);

    /* An empty slot reports -1, never 0 -- 0 is a real note number. */
    CHECK(rg.slots[3][7].first_note == -1,
          "empty slot reported first_note %d", rg.slots[3][7].first_note);
```

- [ ] **Step 2: Run it, verify it fails**

Run: `make -C tests/host test 2>&1 | grep -A3 clip_regions`
Expected: FAIL — `note_count` is not a member of `clip_region_t`

- [ ] **Step 3: Add the fields and parse them**

`src/host/clip_regions.h`, in `clip_region_t`:

```c
    /* The CONTENT half of a lane's fingerprint. Geometry alone cannot tell a
     * copied clip from the original -- same loop, different notes -- and
     * binding a lane to the wrong clip is the confidently-wrong answer this
     * whole pair of projects exists to refuse.
     *
     * first_note is -1 when there are no notes. NOT 0: note 0 is a real note. */
    int note_count;
    int first_note;
```

In `src/host/clip_regions.c`, inside the per-clip parse, count `"noteNumber"`
occurrences within the clip's span and take the `noteNumber` of the entry with
the smallest `startTime`. Initialise `first_note = -1` for every slot when the
struct is zeroed, so an unparsed clip cannot read as note 0.

- [ ] **Step 4: Feed it across the existing seam**

Nothing about the seam changes: `shadow_slot_clip_phase` already reads
`r->note_count` and `r->first_note` into `fp[2]` / `fp[3]` (Task 2), so filling
them in the parser is all it takes for the real values to arrive.

In `lane_tick`, before evaluating, mark a mismatched lane rather than playing it:

```c
        if (inst->clip_fp_valid && !lane_fingerprint_matches(ln, &inst->clip_fp))
            ln->stale = 1;
        else if (inst->clip_fp_valid)
            ln->stale = 0;    /* the clip came back: re-bind rather than strand */
```

`lane_eval` already returns 0 for a stale lane (Task 1), so a stale lane stops
driving through the same release path as a lost phase.

**THE PLACEHOLDER FINGERPRINT MATCHES EVERYTHING, AND MUST NOT.** Until this
task lands, Task 2 pushes `note_count = 0` and `first_note = -1` for every
clip. `lane_fingerprint_matches` deliberately does not compare `loop_len` (a
grown clip is the same clip), so with the other two fields constant it
degenerates to a single test: `loop_start` within 1e-6. **Every clip whose loop
starts at 0.0 then fingerprints identically**, so a lane recorded in that
window matches the wrong clip and *plays* — the confidently-wrong answer this
design exists to refuse — instead of going stale.

So treat the placeholder as **absent, not matching**: `note_count == 0 &&
first_note == -1` is "no fingerprint was ever recorded", and a lane carrying it
is STALE until re-recorded. Add the assertion for that to
`tests/host/test_lane_store.c` in this task, and note it in Task 8 — a
`lanes_<i>.json` written before this task must not come back as a live lane.

- [ ] **Step 5: Orphan the lanes of a DELETED clip — keep them, never delete them**

`clip_regions_forget_deleted(before, after, st)` already distinguishes deleted
from not-yet-saved by **history**: a clip present in the previous parse and
absent from this one was deleted, while one that never existed may simply be
new and unsaved. Its `before`/`after` pair is the only place that knows the
difference, so the lane side has to be told from there rather than inferring it
from a single parse.

In `src/host/clip_regions.c`, extend `clip_regions_forget_deleted` to also
report which `(track, slot)` pairs were deleted — an out-parameter bitmask is
enough.

**Do NOT push it with `set_param` from the worker.** The re-parse runs on the
worker thread, and `v2_set_param` is a module entry point — i.e. the SPI
callback. Calling it from the worker races every reader the callback owns, and
the chain instance is only safe because RT is its single writer.

Use the **same shape the regions table already uses to cross that boundary**: the
worker publishes, the callback reads and pushes. Concretely — the worker stores
the deleted mask beside `g_regions` and bumps a **generation counter**; the
shim's per-slot loop notices a generation it has not seen and calls a second
`dlsym`'d entry point once:

```c
/* A deleted clip ORPHANS its lanes. It does not delete them.
 *
 * Move saves Song.abl about 35 s after an edit, so "absent from the file" is a
 * statement about the last save, not about the user's intent. Deleting recorded
 * automation on the strength of a file diff inside that window is the wrong
 * direction to fail in, and a lane is small. Pruning is only ever an explicit
 * user action (Clear Lanes, Task 9).
 *
 * RT: called from the per-slot loop, like chain_set_clip_phase. The worker only
 * ever publishes the mask -- it never reaches into the instance. */
void chain_set_clip_deleted(void *instance, int track, int slot);
```

A generation counter rather than a boolean, for the reason `chain_bus.c` records:
a flag can be resurrected by a preempted worker, and a counter cannot. `lane_eval` already refuses an orphaned lane (Task 1),
so it goes silent through the same release path as a stale one — and if the clip
comes back (an undo), the fingerprint match in step 4 is what un-orphans it.

Add to the acceptance criteria check: a lane whose clip is deleted still exists
in `lanes:state` afterwards. **If it does not, the feature deletes user data on
a heuristic and the task is not done.**

- [ ] **Step 6: Run the tests**

Run: `make -C tests/host test 2>&1 | tail -5 && bash tests/host/test_chain_lanes_playback.sh`
Expected: all PASS.

- [ ] **Step 7: Commit**

```bash
git add src/host/clip_regions.h src/host/clip_regions.c \
        src/modules/chain/dsp/chain_lanes.c src/modules/chain/dsp/chain_host.c \
        src/host/shadow_chain_mgmt.c src/schwung_shim.c \
        tests/host/test_clip_regions.c tests/host/test_lane_phase_seam.sh
git commit -m "lanes: fingerprint a clip by its notes, and refuse a lane that no longer matches"
```

---

## Task 7: Arm — Move's Record button reaches the chain

> **USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

**Goal:** Establish, **by measurement on the device**, how Move reports its
Record state on the cable-0 LED stream, then push that state to every chain slot.

**Files:**
- Modify: `src/host/shadow_led_queue.c:395-410` (log the candidate events, then decode)
- Modify: `src/host/shim_worker.c` (arming-file-gated log, mirroring `clip_state_tick`)
- Modify: `src/host/shadow_chain_mgmt.c` / `src/schwung_shim.c` (push `lane_armed` on change)
- Modify: `src/modules/chain/dsp/chain_host.c` (`lanes:armed` via `chain_set_clip_phase`'s sibling, or a set_param)

**Acceptance Criteria:**
- [ ] A capture file exists showing the exact cable-0 events Move emits when Record is pressed and released, with note/CC number, channel and value, captured with the transport both stopped and running
- [ ] The decode distinguishes **armed** from **not armed**; if Move blinks the LED while recording, the blink does not read as disarm (a flashing LED must not produce an arm/disarm oscillation)
- [ ] `lane_armed` reaching the chain is pushed **on change only**, never per block
- [ ] If the LED turns out not to be cleanly decodable, the task closes by implementing the agreed fallback — a Schwung-side arm toggle in Slot Settings — and recording that finding in the design doc
- [ ] The finding (either way) is written into `docs/plans/2026-09-12-automation-lanes-design.md`

**Verify:** `ssh ableton@move.local "cat /data/UserData/schwung/rec_led.log"` → shows distinct armed/disarmed lines matching the button presses, with timestamps

**Steps:**

- [ ] **Step 1: Add the instrument**

In `src/host/shadow_led_queue.c`, inside the existing cable-0 scan (beside the
`clip_state_on_led` call at line 406), record candidate events into a small ring
for the worker to print. Candidates are **every** cable-0 event that is not a
grid pad (68-99) and not a step (16-31) — do not pre-guess the number:

```c
                if (!(d1 >= 68 && d1 <= 99) && !(d1 >= 16 && d1 <= 31))
                    rec_led_record(midi_out[i+1], d1, d2,
                                   (uint32_t)shadow_transport_pulses);
```

In `src/host/shim_worker.c`, add `rec_led_tick()` modelled **exactly** on
`clip_state_tick` (`shim_worker.c:759`) — gated on
`/data/UserData/schwung/rec_led_on`, appending to
`/data/UserData/schwung/rec_led.log`, and **opening and closing the file per
line**. That last part is not style: a `FILE*` held across an `rm` of the log
sends every later write to an unlinked inode, and the readout goes silent in a
way that looks exactly like a dead worker. `led_capture_logger_thread` still has
that bug; do not copy it.

- [ ] **Step 2: Deploy and capture**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
ssh ableton@move.local "touch /data/UserData/schwung/rec_led_on"
```

Ask the user to, in order, with a few seconds between each: press Record
(transport stopped), press Record again, press Play, press Record, press Record
again, press Stop. Then:

```bash
ssh ableton@move.local "cat /data/UserData/schwung/rec_led.log"
```

- [ ] **Step 3: Decode, and say what was found**

Identify the event whose value changes with the button state. Write the finding
into the design doc under a new "Measured: Move's Record LED" heading — the
number, the channel, the values for armed and not-armed, and whether it blinks.

**If it blinks while recording**, the decode must latch on the *presence* of the
armed colour within a window rather than on each event, or the arm will
oscillate at the blink rate and record a lane's worth of punch-ins.

- [ ] **Step 4: Push it to the slots, on change**

In the shim, when the decoded arm state changes, write it to every active chain
slot once:

```c
    if (rec_armed != prev_rec_armed) {
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            if (!shadow_chain_slots[s].active || !shadow_chain_slots[s].instance)
                continue;
            shadow_plugin_v2->set_param(shadow_chain_slots[s].instance,
                                        "lanes:armed", rec_armed ? "1" : "0");
        }
        prev_rec_armed = rec_armed;
    }
```

Handle `lanes:armed` in `v2_set_param` by assigning `inst->lane_armed`.

- [ ] **Step 5: Disarm the instrument**

```bash
ssh ableton@move.local "rm -f /data/UserData/schwung/rec_led_on"
```

A diagnostic left armed is a cost on every later measurement — and one of them
has already caused the dropouts it was measuring.

- [ ] **Step 6: Commit**

```bash
git add src/host/shadow_led_queue.c src/host/shim_worker.c \
        src/host/shadow_chain_mgmt.c src/schwung_shim.c \
        src/modules/chain/dsp/chain_host.c docs/plans/2026-09-12-automation-lanes-design.md
git commit -m "lanes: arm from Move's Record button, decoded off the cable-0 LED stream"
```

```json:metadata
{"userGate": true, "tags": ["user-gate"], "requiresUserSpecification": false, "verifyCommand": "ssh ableton@move.local \"cat /data/UserData/schwung/rec_led.log\"", "acceptanceCriteria": ["capture file shows the cable-0 events for Record press/release, stopped and running, with note-or-CC, channel and value", "armed is distinguishable from not-armed and a blink does not oscillate the state", "lane_armed is pushed on change only", "if undecodable, the Slot Settings fallback is implemented instead", "the finding is written into the design doc"], "modelTier": "standard"}
```

---

## Task 8: Persistence — `lanes_<i>.json` in the set directory

**Goal:** Lanes survive a reboot and travel with the set.

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c` (get/set of the `lanes:state` blob)
- Modify: `src/modules/chain/dsp/chain_lanes.c` (serialise/deserialise)
- Modify: `src/shadow/shadow_ui.js:10162` (`autosaveOneSlot` region) and the set-load path
- Create: `tests/host/test_lane_state_roundtrip.c` / `.sh`

**Acceptance Criteria:**
- [ ] `lanes:state` round-trips: serialise → reset → deserialise reproduces every lane, point, position and fingerprint **bit-exactly** for the values that matter (phases and values to 6 decimal places)
- [ ] A slot with no lanes serves `""` and writes **no file**
- [ ] A failed or empty read never truncates an existing `lanes_<i>.json` (the same bail-if-empty guard `autosaveOneSlot` already carries)
- [ ] Lanes load into the chain on set load, and a lane whose fingerprint no longer matches loads as **stale**
- [ ] The file lives at `activeSlotStateDir + "/lanes_" + i + ".json"` — per set, so it travels with the set and is deleted with it

**Verify:** `bash tests/host/test_lane_state_roundtrip.sh`

**Steps:**

- [ ] **Step 1: Write the round-trip test**

Create `tests/host/test_lane_state_roundtrip.c`:

```c
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lane_store.h"
#include "lane_serial.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); fails++; } } while (0)

int main(void) {
    lane_store_t a, b;
    lane_fingerprint_t fp = { 4.0, 8.0, 14, 41 };
    lane_store_reset(&a);
    lane_t *ln = lane_alloc(&a, "fx3", "feedback", 2, 5, &fp);
    lane_write(ln, 0.0, 0.10f);
    lane_write(ln, 3.5, 0.75f);
    lane_write(ln, 7.25, 0.33f);

    char buf[8192];
    int n = lane_store_serialize(&a, buf, sizeof(buf));
    CHECK(n > 0, "serialize returned %d", n);

    lane_store_reset(&b);
    CHECK(lane_store_deserialize(&b, buf) == 1, "deserialize failed");

    lane_t *r = lane_find(&b, "fx3", "feedback");
    CHECK(r != NULL, "lane did not survive the round trip");
    CHECK(r && r->n == 3, "point count %d, want 3", r ? r->n : -1);
    CHECK(r && r->track == 2 && r->slot == 5, "position lost");
    CHECK(r && fabs(r->fp.loop_start - 4.0) < 1e-9, "loop_start lost");
    CHECK(r && r->fp.note_count == 14 && r->fp.first_note == 41, "fingerprint lost");
    for (int i = 0; r && i < r->n; i++) {
        CHECK(fabs(r->pts[i].phase - a.lanes[0].pts[i].phase) < 1e-6,
              "phase %d drifted: %f vs %f", i, r->pts[i].phase,
              a.lanes[0].pts[i].phase);
        CHECK(fabsf(r->pts[i].value - a.lanes[0].pts[i].value) < 1e-6f,
              "value %d drifted", i);
    }

    /* An empty store serialises to nothing, so no file is ever written for a
     * slot that has no automation. */
    lane_store_reset(&a);
    CHECK(lane_store_serialize(&a, buf, sizeof(buf)) == 0,
          "an empty store produced a document");

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: lane state round-trip\n");
    return 0;
}
```

with a `.sh` that compiles `tests/host/test_lane_state_roundtrip.c`,
`src/host/lane_store.c` and `src/host/lane_serial.c`.

- [ ] **Step 2: Run it, verify it fails**

Run: `chmod +x tests/host/test_lane_state_roundtrip.sh && bash tests/host/test_lane_state_roundtrip.sh`
Expected: FAIL — no such file `lane_serial.h`

- [ ] **Step 3: Implement `src/host/lane_serial.{h,c}`**

A flat, line-oriented document — no JSON parser needed on either side, and the
shadow UI only ever carries it as an opaque string:

```
L <target> <param> <track> <slot> <loop_start> <loop_len> <note_count> <first_note> <n>
P <phase> <value>
P <phase> <value>
```

`lane_store_serialize` returns the byte count written, or **0 for an empty
store** — which is what stops an empty document being written over a good file.
`lane_store_deserialize` returns 1 on success, 0 on a malformed document, and
must leave the store **untouched** on failure rather than half-loaded.

Add both files to the dsp.so rule in `scripts/build.sh`.

- [ ] **Step 4: Serve the blob from the chain**

In `v2_get_param`, answer `lanes:state` with `lane_store_serialize`; in
`v2_set_param`, handle `lanes:state` with `lane_store_deserialize`. Both go
beside the existing `<prefix>:state` handling so they follow the same shape the
UI already knows.

- [ ] **Step 5: Persist from the shadow UI**

In `src/shadow/shadow_ui.js`, in the `autosaveOneSlot(i)` body, after the
existing slot state write:

```javascript
    /* Lanes ride with the SET, not with the slot's sound: they are keyed to a
     * clip position, and a clip position means nothing in another set. Same
     * argument that put the recall snapshot in set_state/ rather than in a
     * global dir. An empty answer writes NOTHING — a timed-out read must never
     * truncate a good file. */
    const lanes = getSlotStateWithRetry(i, "lanes:state");
    const lanePath = activeSlotStateDir + "/lanes_" + i + ".json";
    if (lanes && lanes.length > 0) {
        host_write_file(lanePath, lanes);
    }
```

and in the set-load path (beside where `slot_<i>.json` is read back), read
`lanes_<i>.json` and write it to `lanes:state`.

- [ ] **Step 6: Run the test and build**

Run: `bash tests/host/test_lane_state_roundtrip.sh && ./scripts/build.sh 2>&1 | tail -3`
Expected: `PASS: lane state round-trip`, clean build.

- [ ] **Step 7: Commit**

```bash
git add src/host/lane_serial.h src/host/lane_serial.c \
        src/modules/chain/dsp/chain_host.c src/shadow/shadow_ui.js scripts/build.sh \
        tests/host/test_lane_state_roundtrip.c tests/host/test_lane_state_roundtrip.sh
git commit -m "lanes: persist per slot in the set directory, and never write an empty document"
```

---

## Task 9: The minimum surface — a driven mark, Clear Lane, and the refusal

**Goal:** You can see that a parameter is lane-driven, clear its lane, and be
told when recording is refused because the phase is unknown.

**Files:**
- Modify: `src/shadow/shadow_ui.js` (Slot Settings row + the refusal announcement)
- Modify: `src/modules/chain/dsp/chain_host.c` (`lanes:clear`)
- Create: `tests/host/test_lane_ui_surface.sh`

**Acceptance Criteria:**
- [ ] A parameter with an active lane is drawn with the **existing** modulated/base mark — no new glyph is invented
- [ ] Slot Settings carries a `Clear Lanes` action that writes `lanes:clear` and announces how many were cleared
- [ ] Clearing returns every affected parameter to its knob value (through `chain_mod`'s restore path, not a second one)
- [ ] Turning a knob while armed with **phase unknown** announces the refusal rather than failing silently
- [ ] The count is read from the DSP's answer, never assumed — a partial clear that reports success is the failure mode here

**Verify:** `bash tests/host/test_lane_ui_surface.sh && node --check src/shadow/shadow_ui.js`

**Steps:**

- [ ] **Step 1: Serve `lanes:clear` from the chain**

In `v2_set_param`:

```c
    /* Returns the number cleared through get_param("lanes:cleared") so the UI
     * can announce a NUMBER. A clear that reports success without a count is
     * indistinguishable from one that cleared nothing. */
    if (strcmp(key, "lanes:clear") == 0) {
        lane_release_all(inst);
        int n = 0;
        for (int i = 0; i < LANE_MAX; i++) if (inst->lanes.lanes[i].used) n++;
        lane_store_reset(&inst->lanes);
        inst->lanes_last_cleared = n;
        return;
    }
```

Add `int lanes_last_cleared;` to `chain_instance_t`, and serve it from
`v2_get_param` as `lanes:cleared`. The count exists because a clear that
reports success without one is indistinguishable from a clear that cleared
nothing — the same reason the recall snapshot counts its skipped positions.

- [ ] **Step 2: Add the Slot Settings row**

Add `Clear Lanes` to **both** `SLOT_SETTINGS` (the list form) and
`SLOT_GRID_ACTIONS` (the knob-grid form). The grid is the default surface — a
row present only on the lists is unreachable for most users, which
`docs/SHADOW_UI.md` records as having happened before.

```javascript
    {
        label: "Clear Lanes",
        action: (slot) => {
            setSlotParam(slot, "lanes:clear", "1");
            const n = getSlotParam(slot, "lanes:cleared");
            /* null is a FAILED read, "" is served-but-empty, and neither is a
             * count. Announcing "0 cleared" for a read that never completed is
             * the confidently-wrong answer. */
            if (n === null) announce("Clear lanes: no answer");
            else announce("Cleared " + n + " lane" + (n === "1" ? "" : "s"));
        },
    },
```

- [ ] **Step 3: Announce the refusal**

Where the grid writes a parameter, if the slot reports `lanes:armed == 1` and
`lanes:phase_valid == 0`, announce `"Armed — clip phase unknown"` once per
gesture (not per detent). Serve `lanes:phase_valid` from `v2_get_param` as
`inst->clip_phase_valid`.

- [ ] **Step 4: Pin the surface**

Create `tests/host/test_lane_ui_surface.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
fail() { echo "FAIL: $1"; exit 1; }

# The knob grid is the DEFAULT slot-settings surface, so a row present only on
# the list forms is unreachable for most users (docs/SHADOW_UI.md).
grep -q 'Clear Lanes' src/shadow/shadow_ui.js || fail "no Clear Lanes row"
for sym in SLOT_SETTINGS SLOT_GRID_ACTIONS; do
  awk "/$sym/,/^\];/" src/shadow/shadow_ui.js | grep -q 'Clear Lanes' \
    || fail "Clear Lanes missing from $sym"
done

# A failed read must not be announced as a count of zero.
grep -q 'Clear lanes: no answer' src/shadow/shadow_ui.js \
  || fail "no null-read branch on the clear announcement"

node --check src/shadow/shadow_ui.js || fail "shadow_ui.js does not parse"
echo "PASS: lane UI surface"
```

- [ ] **Step 5: Run and commit**

Run: `chmod +x tests/host/test_lane_ui_surface.sh && bash tests/host/test_lane_ui_surface.sh`
Expected: `PASS: lane UI surface`

```bash
git add src/shadow/shadow_ui.js src/modules/chain/dsp/chain_host.c \
        tests/host/test_lane_ui_surface.sh
git commit -m "lanes: a Clear Lanes action on both slot-settings surfaces, and a refusal you can hear"
```

---

## Task 10: HARDWARE GATE — record a knob, hear it play back

> **USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

**Goal:** Prove on the device that the whole chain works, **by ear** —
destination-side evidence, which no parameter read can give.

**Files:** none (verification only; any defect found is fixed in the task it belongs to)

**Acceptance Criteria:**
- [ ] **before:** with no lane recorded, sweeping the knob during playback changes the sound and *nothing repeats* on the next loop
- [ ] **after:** with Record on, a cutoff sweep on beat 2 of the clip, Record off — the sweep repeats on beat 2 **every loop**, unattended, for at least 4 loops
- [ ] `Clear Lanes` stops it and the parameter returns to where the knob is set
- [ ] Stopping the transport returns the parameter to the knob (the lane does not drive while stopped)
- [ ] Switching to Note mode and back does not disturb playback (Project 1's playhead is Note-mode-only; the *anchor* must not be)
- [ ] Relaunching a different clip on that track stops the lane; relaunching the original resumes it
- [ ] `/clip-state` shows the track anchored throughout — a lane failure with an unanchored track is a Project 1 regression, not a lane bug, and must be told apart

**Verify:** Deploy, then drive the device through the script below. The evidence is the user's own report of what they heard, plus `ssh ableton@move.local "cat /data/UserData/schwung/clip_state.log"` showing the track anchored for the duration.

**Steps:**

- [ ] **Step 1: Deploy**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Ask before deploying — the device may be in use.

- [ ] **Step 2: Arm the clip-state readout as the control**

```bash
ssh ableton@move.local "touch /data/UserData/schwung/clip_state_on"
```

This is the positive control: it separates "the lane is broken" from "the track
was never anchored", which look identical from the sound.

- [ ] **Step 3: Walk the user through it, one behaviour at a time**

Ask for each step and wait for the answer before the next. Do not batch them —
about a dozen defects in Project 1 were found exactly this way, one gesture at a
time.

1. Load a set, put a synth in slot 1, launch a clip on track 1, press Play.
2. **before:** sweep cutoff. Confirm it moves the sound and does *not* repeat next loop.
3. Press Record. Sweep cutoff on beat 2. Press Record again.
4. **after:** listen for four loops without touching anything.
5. Turn cutoff (unarmed) — confirm it takes over, and that the lane comes back at the top of the next loop.
6. Stop. Confirm the parameter is back at the knob.
7. Note mode and back. Play. Confirm the lane still plays in the right place.
8. Launch a different clip, then the original.
9. `Clear Lanes`. Confirm silence from the lane and that the knob works normally.

- [ ] **Step 3b: MEASURE THE COST — the estimate is not an answer**

The estimate for `lane_tick` is **~50 us of a 2370 us frame (~2%) at four
lanes per slot, and ~8% at the 16-lane ceiling** — arithmetic, not a
measurement, and this project's rule is to measure. Two known inefficiencies
are deliberately NOT fixed yet, so that the measurement decides which (if
either) is worth fixing:

- `find_param_by_key` is a linear scan with a `strcmp` per element, called
  **twice per lane per block** (once in `lane_tick`, again inside
  `chain_mod_emit_override`). Fix: cache the `chain_param_info_t *` on the
  lane, invalidated on module swap.
- `lane_eval` rescans its points from index 0 every block, although phase
  advances monotonically. Fix: remember the last segment index.

Take three readings — **no lanes, one lane driving, and as many lanes as the
session produced** — and do not add them together: the CPU page's two numbers
(frame budget vs process CPU) measure different things and a module's cost
already sits inside `MoveOriginal`'s `/proc` percentage.

```bash
# frame budget + per-serve cost. The tally is a 1 Hz aggregate and CANNOT see a
# single blown frame; spi_timing's param=avg/max is what does.
ssh ableton@move.local "touch /data/UserData/schwung/spi_tally_on"
ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log | grep -E 'spi-tally|spi_timing|param='"
```

Also open the **CPU usage page** (`/system/cpu` in schwung-manager, always on —
only its 1 Hz polling is armed, by a button) and read the frame-budget figure
per reading.

**`param-slow` needs no flag and is the one to watch**: any parameter serve
past 1000 us is logged **with its key**, so a lane driving a parameter whose
`set_param` does blocking work names itself. That is not hypothetical — dr32
re-reading its kit inside `set_param` blew a frame and cost a session. A lane
writes its target ~344 times a second, so it is a good way to *find* such a
parameter.

```bash
ssh ableton@move.local "grep param-slow /data/UserData/schwung/debug.log | tail -20"
```

Disarm `spi_tally_on` afterwards — an armed diagnostic has itself caused the
dropouts it was measuring.

Record the three readings in the design doc beside the estimate, and say
plainly whether either optimisation is now warranted.

- [ ] **Step 4: Disarm the diagnostic**

```bash
ssh ableton@move.local "rm -f /data/UserData/schwung/clip_state_on"
```

- [ ] **Step 5: Record what was found**

Every defect found here gets fixed in the task that owns it, and this gate is
re-run afterwards — not patched forward from here.

```json:metadata
{"userGate": true, "tags": ["user-gate"], "requiresUserSpecification": false, "requireEvidenceTokens": [["before", "no-lane"], ["after", "lane-plays"]], "verifyCommand": "ssh ableton@move.local \"cat /data/UserData/schwung/clip_state.log\"", "acceptanceCriteria": ["before: a sweep with no lane does not repeat next loop", "after: the recorded sweep repeats on beat 2 for at least 4 unattended loops", "Clear Lanes stops it and restores the knob value", "stopping the transport returns the parameter to the knob", "Note mode and back does not disturb playback", "relaunching another clip stops the lane; the original resumes it", "/clip-state shows the track anchored throughout"], "modelTier": "standard"}
```

---

## Task 11: Full test sweep and CI

**Goal:** The CI-gated suite is green and the ARM64 cross-build succeeds.

**Files:** whatever the sweep turns up

**Acceptance Criteria:**
- [ ] `make -C tests/host test` passes
- [ ] Every `tests/host/*.sh` passes
- [ ] `./scripts/build.sh` (Docker, ARM64) succeeds and the tarball carries `dsp.so` and the shim
- [ ] No new test is added that passes against code it does not exercise — each new `.sh` was seen to FAIL before its implementation landed

**Verify:** `make -C tests/host test && for t in tests/host/*.sh; do bash "$t" || echo "FAILED: $t"; done`

**Steps:**

- [ ] **Step 1: Run the CI-gated subset**

Run: `make -C tests/host test 2>&1 | tail -20`
Expected: all PASS.

- [ ] **Step 2: Run every host shell test**

Run: `for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAILED: $t"; done`
Expected: no output.

Note `tests/{shadow,store,build}` are **not** CI-gated and carry ~20 known stale
failures against since-moved code. Do not chase them; do not "fix" them by
deleting assertions.

- [ ] **Step 3: Cross-build**

Run: `./scripts/build.sh 2>&1 | tail -5`
Expected: success, with `build/modules/chain/dsp.so` and `build/schwung-shim.so`
rebuilt.

- [ ] **Step 4: Commit any fixes**

```bash
git add -A && git commit -m "lanes: fix what the full sweep turned up"
```

---

## Task 12: Documentation and the PR

**Goal:** The next session can find all of this, and `main` gets it through a
green PR.

**Files:**
- Modify: `CLAUDE.md` (two bullets), `docs/CHAIN.md` (the lane contract),
  `docs/plans/2026-09-12-automation-lanes-design.md` (fold in what was measured)
- Modify: `docs/MODULES.md` only if a module-facing contract changed (it should not have)

**Acceptance Criteria:**
- [ ] `CLAUDE.md` gains a bullet under the chain hook naming the lane contract and pointing at `docs/CHAIN.md` — the *surprising* claims, not the prose
- [ ] `CLAUDE.md`'s `host_api_v1_t` section is **corrected**: consuming `reserved` from the front puts a live pointer at **+120**, the breakbeat over-read offset; the current advice is wrong and the existing test cannot catch it
- [ ] `docs/CHAIN.md` documents the lane contract: absolute override, keying + fingerprint, the unknown-phase refusal, and the dlsym'd phase seam
- [ ] The design doc records what the hardware actually said about Move's Record LED
- [ ] PR opened against `main`, all three CI checks green

**Verify:** `gh pr view --json state,statusCheckRollup -q '.state, (.statusCheckRollup[]|.name+" "+.conclusion)'`

**Steps:**

- [ ] **Step 1: Correct the ABI advice in `CLAUDE.md`**

Under the `host_api_v1_t ends in a run of NULLs` section, after the "consume
`reserved` from the front" sentence:

```markdown
**Consuming it from the FRONT is wrong, and the test cannot see it.**
`reserved` begins at **+120** — measured — which is exactly the offset
breakbeat reads as `get_project_bpm()`. A real field there is a live pointer
at the crash site, and `test_host_api_reserved_tail.c` inspects a zeroed
struct, so it reads NULL and passes. Add a callback the way
`move_plugin_render_split` and `chain_take_midi_tick_wake` are added: **dlsym
an optional symbol**, and leave the struct alone.
```

- [ ] **Step 2: Add the lane bullets**

To `CLAUDE.md`'s chain hook:

```markdown
- **An automation lane is ABSOLUTE and TIME-ADDRESSED, and it has no length of
  its own.** Breakpoints are beats from the clip's `loop_start`; evaluation
  wraps at whatever `loop_len` the clip has *now*, considering only points
  below it — so extending a clip reveals what was recorded there, shrinking it
  makes the tail dormant, and nothing is ever rescaled or deleted. It drains
  through a new **override** source class in `chain_mod` (`effective =
  (override ? override : base) + Σ offsets`), so LFOs still sum on top and
  clearing returns the parameter to the knob. **Move's clips carry no
  identity** — no id, no uuid — so a lane is keyed to a grid POSITION plus a
  fingerprint of the clip's notes, and a mismatch makes it STALE: retained,
  silent, never guessed at. Phase comes from Project 1 and **unknown phase
  refuses** both playback and recording. See `docs/CHAIN.md`.
```

- [ ] **Step 3: Write the contract into `docs/CHAIN.md`**

A full section covering: the address `(target, param)`, the override class and
why it is not an offset, keying and the fingerprint, the unarmed-turn punch, the
dlsym'd phase seam and why it is not a host-API field, and the `lanes:` param
surface (`armed`, `state`, `clear`, `cleared`, `phase_valid`).

- [ ] **Step 4: Open the PR**

```bash
git push -u origin feat/automation-lanes
gh pr create --title "Automation lanes: record a knob against Move's clip, play it back every loop" \
  --body "$(cat <<'EOF'
Project 2 of the clip-awareness pair. Records a knob turn against the playing
clip and plays it back in time with it, every loop.

- Time-addressed, absolute lanes; no length of their own, wrapped at the clip's
  current loop length. Nothing is rescaled and nothing is deleted when a clip
  grows or shrinks.
- Keyed to a grid position plus a fingerprint of the clip's notes, because
  Move's clips carry no identity. A mismatch is STALE: retained, silent.
- Drains through a new override source class in chain_mod, so LFOs still sum on
  top and clearing returns the parameter to the knob.
- Clip phase reaches the chain through a dlsym'd entry point. It is NOT a
  host_api_v1_t field: `reserved` begins at +120, the breakbeat over-read
  offset, so consuming it from the front would put a live pointer at the crash
  site — and the existing test cannot see that, because it inspects a zeroed
  struct. CLAUDE.md is corrected.
- Arm is Move's own Record button, decoded off the cable-0 LED stream.

Verified by ear on hardware (Task 10): the recorded sweep repeats on the beat it
was played, every loop, unattended. That is destination-side evidence —
`<key>:effective` reads chain_mod's own table and could not have shown it.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_012k1GEztTRvMQujEzJSm3AA
EOF
)"
```

- [ ] **Step 5: Wait for CI and merge**

Run: `gh pr checks --watch`
Expected: `host-tests`, `go`, `cross-compile` all pass.

`gh pr merge` reports a failure it did not have when `main` is checked out in a
worktree — the merge lands on GitHub and `gh` then dies updating the local
checkout, skipping `--delete-branch`. Confirm with
`gh pr view <n> --json state,mergeCommit` rather than the exit status, and
delete the branch by hand. Re-running the merge on the strength of that error is
the actual hazard.

---

## Dependencies

```
1 ──┬─> 4 ──> 5 ──┬─> 8 ──> 9 ──> 10 ──> 11 ──> 12
2 ──┤             │
3 ──┘             │
6 ────────────────┤
7 ────────────────┘
```

Tasks 1, 2 and 3 are independent and can run in parallel. Task 7 (the hardware
measurement) is independent of everything except the deploy, so it can run early
— but its *finding* is what Task 5's arm depends on.
