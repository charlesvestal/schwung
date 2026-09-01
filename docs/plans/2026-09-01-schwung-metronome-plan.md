# Schwung Metronome Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Play a Schwung-generated metronome click under Move→Schwung, where Move's own metronome is inaudible by construction.

**Architecture:** Detect Move's metronome from its own `"Metronome On"` / `"Metronome Off"` screen-reader announcement, which `shadow_dbus.c` already receives. Generate a click on the SPI callback from `shadow_transport_pulses` (24 PPQN, already fed from cable 0), and mix it into `mailbox_audio` after the capture snapshot so it reaches the DAC but never a recording. Two settings in Global Settings → Audio, carried in `shadow_control_t` the way `recall_quantize` is.

**Tech Stack:** C11 (shim + shadow_ui host), QuickJS-hosted JS (`shadow_ui.js`), ES modules (`.mjs` contracts), bash + node `tests/host/` suite, cross-compiled for ARM64 via Docker.

**Design doc:** `docs/plans/2026-09-01-schwung-metronome-design.md`

**User decisions (already made):**
- "DAC only" — the click must not appear in resamples or Skipback. Mix after the `unity_view` snapshot.
- "Not now — code defensively" — do not block on confirming the exact announcement string; match whitespace-normalised and case-insensitively, confirm on hardware later.
- "we can move that under shortcuts" — `skipback_shortcut` moves out of Audio to make room. This plan also moves `skipback_seconds` with it, so the pair stays together.
- "settings absolutely does scroll" — sections are not capped at one page; the constraint is avoiding an orphan page, not a hard limit.
- Standing preferences: never deploy to the device without asking; work on a branch, never push to `main`.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/host/metronome_click.h` | **Create.** Pure, header-only: beat/downbeat boundary maths and the click voice. Dependency-free so `tests/host/` can compile and run it natively — the same reason `recall_quantize.h` is a header. |
| `src/host/metronome_announce.h` | **Create.** Pure, header-only: normalise an announcement string and classify it as on / off / neither. |
| `src/host/shadow_metronome.c` / `.h` | **Create.** The stateful side: owns the voice, watches `shadow_transport_pulses`, renders a block. Calls only into the two pure headers. |
| `src/host/shadow_dbus.c` | **Modify.** One call into the matcher; owns `shadow_metronome_on`. |
| `src/host/shadow_dbus.h` | **Modify.** Export `shadow_metronome_on`. |
| `src/host/shadow_constants.h` | **Modify.** Three appended `shadow_control_t` bytes. |
| `src/schwung_shim.c` | **Modify.** One call at the mix point, plus the mode gate. |
| `src/shadow/shadow_ui.c` | **Modify.** `shadow_metronome_set` binding (SHM + features.json), `shadow_metronome_beats_set`. |
| `src/shadow/shadow_ui.js` | **Modify.** State vars, startup load, the two contract switch cases, the `Song.abl` time-signature read. |
| `src/shadow/shadow_ui_global_grid.mjs` | **Modify.** Section membership, enum table, routing table. |
| `scripts/build.sh` | **Modify.** Add `shadow_metronome.c` to both source lists. |
| `tests/host/Makefile` | **Modify.** Two new C targets. |
| `tests/host/test_metronome_click.c` | **Create.** Boundary maths + voice. |
| `tests/host/test_metronome_announce.c` | **Create.** The matcher, including the near-misses. |
| `tests/host/test_global_settings_contract.sh` | **Modify.** Section counts. |

---

## Task 1: Beat and downbeat maths, plus the click voice

**Goal:** A pure, dependency-free header that answers "did a beat boundary just pass, and was it a downbeat" and synthesises the click, with a native test.

**Files:**
- Create: `src/host/metronome_click.h`
- Create: `tests/host/test_metronome_click.c`
- Modify: `tests/host/Makefile:32-48` (TARGETS list) and the rules section

**Acceptance Criteria:**
- [ ] Pulse 0 is a downbeat; pulse 24 is beat 1; pulse 96 is a downbeat at `beats_per_bar = 4`
- [ ] `beats_per_bar = 3` accents pulse 0 and 72, not 96
- [ ] A backwards pulse count (MIDI Start reset) reports a downbeat, not a missed beat
- [ ] A block spanning several pulses reports exactly one boundary, the latest one
- [ ] `beats_per_bar <= 0` is clamped to 4 rather than dividing by zero
- [ ] A triggered voice decays monotonically and reaches silence; an untriggered voice returns exactly 0.0f
- [ ] Mutating `+ div` to `+ 0` in `metronome_beat_crossed` makes the test FAIL

**Verify:** `make -C tests/host test 2>&1 | grep -E 'metronome_click|FAIL'` → `test_metronome_click: PASS`, no FAIL lines

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_metronome_click.c`:

```c
/* Beat boundaries and the click voice, run natively.
 *
 * The boundary cases here are the ones that are silent when wrong: an
 * off-by-one accent, and the transport reset that eats the first click of a
 * take. Both feel like "the metronome is a bit off" rather than a bug.
 */
#include <stdio.h>
#include <math.h>
#include "metronome_click.h"

static int failures = 0;

static void expect_beat(int prev, int now, int bpb, int want, const char *what)
{
    int got = metronome_beat_crossed(prev, now, bpb);
    if (got != want) {
        printf("FAIL: %s: crossed(%d,%d,bpb=%d) = %d, want %d\n",
               what, prev, now, bpb, got, want);
        failures++;
    }
}

int main(void)
{
    /* ---- 4/4 ---- */
    expect_beat(-1, 0, 4, -1, "no prior pulse is not a crossing, even at pulse 0");
    expect_beat(-1, 96, 4, -1, "no prior pulse is not a crossing, even mid-bar");
    expect_beat(0, 1, 4, -1, "inside a beat");
    expect_beat(23, 24, 4, 1, "pulse 24 is beat 1");
    expect_beat(47, 48, 4, 2, "pulse 48 is beat 2");
    expect_beat(71, 72, 4, 3, "pulse 72 is beat 3");
    expect_beat(95, 96, 4, 0, "pulse 96 is the downbeat of bar 2");
    expect_beat(24, 24, 4, -1, "no advance is no crossing");

    /* ---- transport reset: MIDI Start zeroes the counter ---- */
    expect_beat(95, 0, 4, 0, "a backwards count is a downbeat, not a miss");

    /* ---- 3/4 ---- */
    expect_beat(71, 72, 3, 0, "3/4 accents pulse 72");
    expect_beat(95, 96, 3, 1, "3/4 does NOT accent pulse 96");

    /* ---- degenerate beats_per_bar clamps rather than dividing by zero ---- */
    expect_beat(95, 96, 0, 0, "bpb 0 clamps to 4");
    expect_beat(95, 96, -3, 0, "negative bpb clamps to 4");

    /* ---- several boundaries in one call report the latest, once ---- */
    expect_beat(0, 96, 4, 0, "a wide span reports the latest boundary");
    expect_beat(0, 50, 4, 2, "a two-beat span reports beat 2");

    /* ---- voice ---- */
    {
        metronome_voice_t v = {0};
        if (metronome_voice_next(&v) != 0.0f) {
            printf("FAIL: an untriggered voice must return exactly 0.0f\n");
            failures++;
        }
        metronome_voice_trigger(&v, METRONOME_FREQ_BEAT_HZ, 1.0f,
                                METRONOME_DECAY_SECONDS, 44100.0f);
        if (!metronome_voice_active(&v)) {
            printf("FAIL: a triggered voice must be active\n");
            failures++;
        }
        float peak = 0.0f, prev_env = 2.0f;
        int went_silent = 0;
        for (int i = 0; i < 44100; i++) {
            float s = metronome_voice_next(&v);
            if (fabsf(s) > peak) peak = fabsf(s);
            if (s > 1.0f || s < -1.0f) {
                printf("FAIL: voice left -1..1 at sample %d (%f)\n", i, s);
                failures++;
                break;
            }
            /* Envelope must not grow. */
            if (v.amp > prev_env + 1e-6f) {
                printf("FAIL: envelope grew at sample %d\n", i);
                failures++;
                break;
            }
            prev_env = v.amp;
            if (!metronome_voice_active(&v)) { went_silent = 1; break; }
        }
        if (peak < 0.5f) {
            printf("FAIL: voice peak %f is implausibly quiet\n", peak);
            failures++;
        }
        if (!went_silent) {
            printf("FAIL: voice never decayed to silence within 1 s\n");
            failures++;
        }
        if (metronome_voice_next(&v) != 0.0f) {
            printf("FAIL: a decayed voice must return exactly 0.0f\n");
            failures++;
        }
    }

    if (failures) { printf("test_metronome_click: FAIL (%d)\n", failures); return 1; }
    printf("test_metronome_click: PASS\n");
    return 0;
}
```

- [ ] **Step 2: Add the Makefile target**

In `tests/host/Makefile`, append to the `TARGETS` list (after `$(BUILD_DIR)/test_recall_quantize`):

```make
	$(BUILD_DIR)/test_metronome_click
```

Add ONLY this one now. Task 2 adds its own target when its test file exists — listing a target whose source is not written yet makes `make test` fail for a reason that has nothing to do with the code under test, and that noise is what makes a red suite easy to ignore.

Then add a rule alongside the existing single-source rules — copy the shape of the `test_recall_quantize` rule already in the file, substituting the name. It needs `-lm` for `sinf`/`expf`:

```make
$(BUILD_DIR)/test_metronome_click: test_metronome_click.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) -o $@ $< $(LDFLAGS) -lm
```

- [ ] **Step 3: Run the test and watch it fail**

The header does not exist yet. That is the point — a test that has never been
seen to fail has not been shown to be capable of failing.

Run: `make -C tests/host test 2>&1 | tail -n 20`
Expected: FAIL — `metronome_click.h: No such file or directory`

- [ ] **Step 4: Write the header**

Create `src/host/metronome_click.h`:

```c
/*
 * metronome_click.h — beat boundaries and the click voice.
 *
 * Header-only and dependency-free for the same reason as recall_quantize.h:
 * so tests/host can compile and RUN it natively. Its caller lives in
 * shadow_metronome.c, driven from schwung_shim.c, which cannot be built on the
 * dev machine — which is how boundary maths ends up shipped untested, and
 * boundary maths is wrong SILENTLY. A metronome that accents the wrong beat
 * does not crash; it just feels wrong forever.
 *
 * Pure: no allocation, no I/O, no globals. The call site is the SPI callback.
 */
#ifndef METRONOME_CLICK_H
#define METRONOME_CLICK_H

#include <math.h>

/* MIDI clock is 24 PPQN. shadow_transport_pulses counts these and resets to 0
 * on MIDI Start (0xFA), which Move sends at bar 1 beat 1 — so bar phase is
 * free once beats_per_bar is known. */
#define METRONOME_PULSES_PER_BEAT 24

#define METRONOME_DEFAULT_BEATS_PER_BAR 4

/*
 * Did a beat boundary fall in (prev, now]?
 *
 * Returns the beat's index within the bar — 0 is the downbeat — or -1 if no
 * boundary was crossed.
 *
 * `now < prev` means the transport restarted (MIDI Start zeroed the counter)
 * between the two samples. That is a DOWNBEAT, not a missed beat: reporting -1
 * there would swallow the first click of every take, which is the one click
 * that matters most.
 *
 * When several boundaries fall inside one block the LATEST is reported, once.
 * At 128 frames (2.9 ms) that needs a tempo above 10,000 BPM to happen, but
 * reporting one boundary per call is what keeps the caller a simple trigger
 * rather than a queue.
 */
static inline int metronome_beat_crossed(int prev_pulses, int now_pulses,
                                         int beats_per_bar)
{
    if (beats_per_bar <= 0) beats_per_bar = METRONOME_DEFAULT_BEATS_PER_BAR;
    if (now_pulses < 0) return -1;

    /* No previous sample. NOT a crossing: without a prior position there is
     * nothing to have crossed, and returning a downbeat here would fire a
     * click the moment the feature is switched on mid-bar. The caller guards
     * this too, and the guard belongs in both places — a pure function that
     * invents a beat from a sentinel is a trap for the next caller. */
    if (prev_pulses < 0) return -1;

    if (now_pulses < prev_pulses) {
        /* Transport restarted. Pulse 0 is bar 1 beat 1. */
        return 0;
    }
    if (now_pulses == prev_pulses) return -1;

    const int div = METRONOME_PULSES_PER_BEAT;
    /* The greatest multiple of div in (prev, now]. */
    int last = (now_pulses / div) * div;
    if (last <= prev_pulses) return -1;

    int beat = (last / div) % beats_per_bar;
    return beat;
}

/* ------------------------------------------------------------------ voice */

/*
 * A decaying sine. Deliberately not a sample: a click has to be generated on
 * the SPI callback with no file I/O and no allocation, and two sine
 * multiplications per frame is cheaper than a lookup that has to be loaded,
 * owned and freed.
 */
typedef struct {
    float phase;      /* radians */
    float phase_inc;  /* radians per sample */
    float amp;        /* current amplitude, 0 = idle */
    float decay;      /* per-sample multiplier, < 1 */
} metronome_voice_t;

/* Downbeat sits a fifth above the offbeat, the way an acoustic click does. */
#define METRONOME_FREQ_DOWNBEAT_HZ 1500.0f
#define METRONOME_FREQ_BEAT_HZ     1000.0f
#define METRONOME_DECAY_SECONDS    0.030f

static inline void metronome_voice_trigger(metronome_voice_t *v, float freq_hz,
                                           float amp, float decay_s, float sr)
{
    if (!v) return;
    if (!(sr > 0.0f)) sr = 44100.0f;
    if (!(decay_s > 0.0f)) decay_s = METRONOME_DECAY_SECONDS;
    v->phase = 0.0f;
    v->phase_inc = 2.0f * (float)M_PI * freq_hz / sr;
    v->amp = amp;
    /* Reach -60 dB in decay_s. */
    v->decay = expf(-6.907755f / (decay_s * sr));
}

/* Next sample, in -1..1. Returns exactly 0.0f once the voice has decayed out,
 * so the caller can skip the mix entirely on an idle block. */
static inline float metronome_voice_next(metronome_voice_t *v)
{
    if (!v || v->amp <= 0.0001f) { if (v) v->amp = 0.0f; return 0.0f; }
    float s = sinf(v->phase) * v->amp;
    v->phase += v->phase_inc;
    if (v->phase > 2.0f * (float)M_PI) v->phase -= 2.0f * (float)M_PI;
    v->amp *= v->decay;
    return s;
}

static inline int metronome_voice_active(const metronome_voice_t *v)
{
    return v && v->amp > 0.0001f;
}

#endif /* METRONOME_CLICK_H */
```

- [ ] **Step 5: Run the test and watch it pass**

Run: `make -C tests/host test 2>&1 | grep -E 'metronome_click|FAIL'`
Expected: `test_metronome_click: PASS` and no FAIL lines

- [ ] **Step 6: Prove the test can fail**

Temporarily change `int last = (now_pulses / div) * div;` to `int last = (now_pulses / div) * div + div;` in `metronome_click.h`.

Run: `make -C tests/host test 2>&1 | grep -E 'metronome_click|FAIL'`
Expected: FAIL lines naming the pulse-24 and pulse-96 cases.

Revert the mutation. Re-run and confirm PASS. **A probe that cannot fail reports green on broken code** — do not skip this step.

- [ ] **Step 7: Commit**

```bash
git add src/host/metronome_click.h tests/host/test_metronome_click.c tests/host/Makefile
git commit -m "metronome: beat boundary maths and the click voice, header-only

Pure and dependency-free so tests/host runs it natively, the same reason
recall_quantize.h is a header: the caller is in the shim, which cannot be
built on the dev machine, and an off-by-one accent is silent."
```

---

## Task 2: Detect Move's metronome from its own announcement

**Goal:** A pure matcher for `"Metronome On"` / `"Metronome Off"`, wired into the D-Bus text handler, exposing `shadow_metronome_on`.

**Files:**
- Create: `src/host/metronome_announce.h`
- Create: `tests/host/test_metronome_announce.c`
- Modify: `src/host/shadow_dbus.c:242` (right after the `native_sampler_update` call)
- Modify: `src/host/shadow_dbus.h` (export the flag)
- Modify: `tests/host/Makefile` (rule for the second target added in Task 1)

**Acceptance Criteria:**
- [ ] `"Metronome On"`, `"Metronome\nOn"`, `"metronome on"`, `"  Metronome   On  "` all classify as ON
- [ ] The same four shapes of `Off` classify as OFF
- [ ] `"Metronome"`, `"Metronome On Track"`, `"Onmetronome"`, `"unmuted"`, `""`, `NULL` all classify as NEITHER and leave the flag alone
- [ ] `shadow_metronome_on` starts at 0 and is never persisted to disk
- [ ] Mutating the matcher to a `strstr` prefix test makes `"Metronome On Track"` fail

**Verify:** `make -C tests/host test 2>&1 | grep -E 'metronome_announce|FAIL'` → `test_metronome_announce: PASS`, no FAIL lines

**Steps:**

- [ ] **Step 1: Write the matcher header**

Create `src/host/metronome_announce.h`:

```c
/*
 * metronome_announce.h — classify a Move screen-reader announcement.
 *
 * Move raises "Metronome\nOn" / "Metronome\nOff" (strings at 0x169474 and
 * 0x1909d8 in MoveOriginal, sitting among "Clip\ncreated" and "Notes\ndeleted")
 * and pushes them out as com.ableton.move.ScreenReader.text, which
 * shadow_dbus.c already receives.
 *
 * WHY THIS IS NOT THE MUTE BUG. The removed mute auto-correct matched any text
 * ENDING IN " muted"/" soloed", so Move's own "Lay Down Kit muted" and
 * Schwung's TTS looping back through the same handler both hit it — and it
 * PERSISTED the result, so a spurious match silenced slots across projects.
 * Here the match is exact equality on a whole normalised string, Schwung never
 * utters either string, and the result is runtime-only.
 *
 * Normalisation, not a family of literals: the binary holds the DISPLAY form
 * (with a newline) and the announcement may normalise it differently. Lowering
 * case and collapsing whitespace covers every plausible shape without widening
 * the match to a substring — which is precisely what made the mute rule unsafe.
 *
 * Pure: no allocation, no I/O, no globals.
 */
#ifndef METRONOME_ANNOUNCE_H
#define METRONOME_ANNOUNCE_H

#include <stddef.h>

typedef enum {
    METRONOME_ANNOUNCE_NONE = 0,  /* not about the metronome — change nothing */
    METRONOME_ANNOUNCE_ON   = 1,
    METRONOME_ANNOUNCE_OFF  = 2,
} metronome_announce_t;

/* Lowercase, collapse every whitespace run to one space, trim both ends.
 * Writes at most out_len-1 chars plus a NUL. */
static inline void metronome_announce_normalize(const char *in, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!in) return;

    size_t o = 0;
    int pending_space = 0;
    int seen_any = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            if (seen_any) pending_space = 1;
            continue;
        }
        if (pending_space) {
            if (o + 1 >= out_len) break;
            out[o++] = ' ';
            pending_space = 0;
        }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (o + 1 >= out_len) break;
        out[o++] = (char)c;
        seen_any = 1;
    }
    out[o] = '\0';
}

static inline int metronome_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == '\0' && *b == '\0';
}

/*
 * EXACT equality on the whole normalised string. Not a prefix, not a substring:
 * "Metronome On Track" must NOT be read as the metronome coming on.
 */
static inline metronome_announce_t metronome_announce_classify(const char *text)
{
    char norm[64];
    metronome_announce_normalize(text, norm, sizeof(norm));
    if (metronome_streq(norm, "metronome on"))  return METRONOME_ANNOUNCE_ON;
    if (metronome_streq(norm, "metronome off")) return METRONOME_ANNOUNCE_OFF;
    return METRONOME_ANNOUNCE_NONE;
}

#endif /* METRONOME_ANNOUNCE_H */
```

- [ ] **Step 2: Write the failing test**

Create `tests/host/test_metronome_announce.c`:

```c
/* The announcement matcher.
 *
 * The near-miss cases are the point. A suffix or substring match here would be
 * the drum-slot mute bug again: that rule matched any text ending in " muted",
 * so Move's own "Lay Down Kit muted" and Schwung's TTS loopback both fired it.
 */
#include <stdio.h>
#include "metronome_announce.h"

static int failures = 0;

static void expect(const char *in, metronome_announce_t want, const char *what)
{
    metronome_announce_t got = metronome_announce_classify(in);
    if (got != want) {
        printf("FAIL: %s: classify(\"%s\") = %d, want %d\n",
               what, in ? in : "(null)", (int)got, (int)want);
        failures++;
    }
}

int main(void)
{
    /* ---- every plausible shape of the wire text ---- */
    expect("Metronome On",     METRONOME_ANNOUNCE_ON,  "space form");
    expect("Metronome\nOn",    METRONOME_ANNOUNCE_ON,  "display form, newline");
    expect("metronome on",     METRONOME_ANNOUNCE_ON,  "lowercase");
    expect("METRONOME ON",     METRONOME_ANNOUNCE_ON,  "uppercase");
    expect("  Metronome   On  ", METRONOME_ANNOUNCE_ON, "padded and doubled space");
    expect("Metronome\r\nOn",  METRONOME_ANNOUNCE_ON,  "crlf");

    expect("Metronome Off",    METRONOME_ANNOUNCE_OFF, "space form");
    expect("Metronome\nOff",   METRONOME_ANNOUNCE_OFF, "display form, newline");
    expect("metronome off",    METRONOME_ANNOUNCE_OFF, "lowercase");
    expect("  Metronome\tOff", METRONOME_ANNOUNCE_OFF, "tab");

    /* ---- near misses. Each of these WOULD match a substring rule. ---- */
    expect("Metronome",             METRONOME_ANNOUNCE_NONE, "bare noun");
    expect("Metronome On Track",    METRONOME_ANNOUNCE_NONE, "longer sentence starting the same");
    expect("Turn Metronome On",     METRONOME_ANNOUNCE_NONE, "prefixed sentence");
    expect("Onmetronome",           METRONOME_ANNOUNCE_NONE, "no separator");
    expect("Metronome Onn",         METRONOME_ANNOUNCE_NONE, "trailing char");
    expect("Lay Down Kit muted",    METRONOME_ANNOUNCE_NONE, "the mute-bug shape");
    expect("unmuted",               METRONOME_ANNOUNCE_NONE, "unrelated");
    expect("",                      METRONOME_ANNOUNCE_NONE, "empty");
    expect(NULL,                    METRONOME_ANNOUNCE_NONE, "null");

    /* ---- a very long string must not overrun the normalise buffer ---- */
    {
        char big[512];
        for (int i = 0; i < 511; i++) big[i] = 'x';
        big[511] = '\0';
        expect(big, METRONOME_ANNOUNCE_NONE, "overlong input");
    }

    if (failures) { printf("test_metronome_announce: FAIL (%d)\n", failures); return 1; }
    printf("test_metronome_announce: PASS\n");
    return 0;
}
```

- [ ] **Step 3: Add the Makefile rule**

In `tests/host/Makefile`, append to the `TARGETS` list (after the Task 1 entry):

```make
	$(BUILD_DIR)/test_metronome_announce
```

and add the rule next to the Task 1 rule:

```make
$(BUILD_DIR)/test_metronome_announce: test_metronome_announce.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) -o $@ $< $(LDFLAGS)
```

- [ ] **Step 4: Run and verify**

Run: `make -C tests/host test 2>&1 | grep -E 'metronome_announce|FAIL'`
Expected: `test_metronome_announce: PASS`, no FAIL lines

- [ ] **Step 5: Prove the test can fail**

Temporarily replace the body of `metronome_announce_classify` with a prefix test:

```c
    char norm[64];
    metronome_announce_normalize(text, norm, sizeof(norm));
    if (norm[0] && norm[1] && norm[2] && norm[3]) {
        const char *p = norm;
        while (*p) { if (*p == 'o' && *(p+1) == 'n') return METRONOME_ANNOUNCE_ON; p++; }
    }
    return METRONOME_ANNOUNCE_NONE;
```

Run: `make -C tests/host test 2>&1 | grep -E 'metronome_announce|FAIL'`
Expected: FAIL naming `"Metronome On Track"` and `"Onmetronome"`.

Revert. Re-run and confirm PASS.

- [ ] **Step 6: Own the flag in shadow_dbus.c**

In `src/host/shadow_dbus.c`, add to the includes near `#include "shadow_dbus.h"`:

```c
#include "metronome_announce.h"
```

Add to the extern-globals block (next to `volatile int in_set_overview = 0;`, around line 51):

```c
/*
 * 1 = Move's metronome is on, learned from Move's own "Metronome On" /
 * "Metronome Off" announcement.
 *
 * NOT PERSISTED, deliberately. Move does not persist it either — `metronome`
 * appears in neither /data/UserData/settings/Settings.json nor Song.abl — so
 * the metronome is off at every boot and 0 is the truth, not a guess. Writing
 * it to disk would be the one thing that could make it wrong.
 *
 * Written on the D-Bus monitor thread, read on the SPI callback. A plain
 * volatile int, the same as in_set_overview above.
 */
volatile int shadow_metronome_on = 0;
```

In `shadow_dbus_handle_text()`, immediately after the `host.native_sampler_update(text);` line (currently `shadow_dbus.c:243`):

```c
    /* Move's metronome, from Move's own notification. Exact equality on the
     * whole normalised string — see metronome_announce.h for why this is not
     * the mute auto-correct that had to be removed. A NONE result changes
     * nothing, so unrelated announcements can never clear the flag. */
    {
        metronome_announce_t m = metronome_announce_classify(text);
        if (m != METRONOME_ANNOUNCE_NONE) {
            int now_on = (m == METRONOME_ANNOUNCE_ON);
            if (now_on != shadow_metronome_on) {
                shadow_metronome_on = now_on;
                char msg[64];
                snprintf(msg, sizeof(msg), "Metronome: Move reports %s",
                         now_on ? "ON" : "OFF");
                host.log(msg);
            }
        }
    }
```

- [ ] **Step 7: Export it**

In `src/host/shadow_dbus.h`, in the extern block alongside `extern volatile int in_set_overview;`:

```c
/* 1 = Move's metronome is on. Set from Move's own announcement in
 * shadow_dbus_handle_text; read by the shim on the SPI callback. Never
 * persisted — Move does not persist it either, so 0 at boot is correct. */
extern volatile int shadow_metronome_on;
```

- [ ] **Step 8: Commit**

```bash
git add src/host/metronome_announce.h src/host/shadow_dbus.c src/host/shadow_dbus.h \
        tests/host/test_metronome_announce.c tests/host/Makefile
git commit -m "metronome: detect Move's metronome from its own announcement

Move raises \"Metronome On\"/\"Metronome Off\" and pushes it as a
ScreenReader.text signal shadow_dbus.c already receives. Exact equality on
the whole normalised string, not the suffix match that made the mute
auto-correct fire on 'Lay Down Kit muted' and on Schwung's own TTS."
```

---

## Task 3: Carry mode, level and bar length in `shadow_control_t`

**Goal:** Three appended control bytes plus the JS bindings that write them, following the `recall_quantize` template exactly.

**Files:**
- Modify: `src/host/shadow_constants.h` (append after `stay_in_shadow`, ~line 365)
- Modify: `src/shadow/shadow_ui.c` (new bindings next to `js_shadow_recall_quantize_set`, ~line 235, and registration at ~line 2904)
- Modify: `src/shadow/shadow_ui.js` (state + loader next to `loadRecallQuantize`, ~line 8772; startup call at ~line 20907)

**Acceptance Criteria:**
- [ ] `sizeof(shadow_control_t) <= CONTROL_BUFFER_SIZE` still holds (the existing `shadow_control_size_check` static assert proves it at compile time)
- [ ] The three fields are appended AFTER `stay_in_shadow`, so no existing field moves and `schwung-manager`'s raw-offset read of `stay_in_shadow` (`shmconfig.go`) is untouched
- [ ] `shadow_metronome_set(mode, level)` clamps mode to 0..2 and level to 0..100 and writes both SHM and `features.json`
- [ ] `loadMetronome()` restores from `features.json` at startup and pushes the register down
- [ ] `grep -c stay_in_shadow` in `schwung-manager/shmconfig.go` is unchanged by this task

**Verify:** `./scripts/build.sh 2>&1 | tail -n 5` → build succeeds (the static assert is the test); `node --check src/shadow/shadow_ui.js` is NOT sufficient — see Step 5

**Steps:**

- [ ] **Step 1: Append the control fields**

In `src/host/shadow_constants.h`, immediately after `volatile uint8_t stay_in_shadow;`:

```c
    /*
     * Metronome, all three APPENDED after stay_in_shadow so nothing behind
     * them moves. sizeof is a contract between two binaries, and
     * schwung-manager reads stay_in_shadow at a RAW OFFSET (shmconfig.go) —
     * appending is free, inserting is not.
     *
     * Free in the other sense too: CONTROL_BUFFER_SIZE is 256 for a struct
     * that uses ~86, and /dev/shm allocates by page, so three bytes cost
     * nothing. Only SHRINKING the container fails the build.
     *
     * These live here rather than in features.json because
     * load_feature_config() runs ONCE at init — a value parsed there would
     * need a reboot to change. Same reason recall_quantize is a field.
     */

    /* 0 = off, 1 = follow Move's metronome, 2 = always on while playing. */
    volatile uint8_t metronome_mode;

    /* 0-100 %. */
    volatile uint8_t metronome_level;

    /*
     * Beats per bar, for the downbeat accent. From timeSignature.upper in the
     * current set's Song.abl, read by the shadow UI on SET_CHANGED — never on
     * the SPI callback. 0 means "not yet known"; the consumer clamps to 4.
     */
    volatile uint8_t metronome_beats_per_bar;
```

- [ ] **Step 2: Add the C bindings**

In `src/shadow/shadow_ui.c`, after `js_shadow_recall_quantize_set` (which ends around line 258):

```c
/* shadow_metronome_set(mode, level) -> void   (mode 0=off, 1=follow, 2=on)
 *
 * Writes shadow_control_t.metronome_mode / metronome_level, which the shim
 * reads on the SPI callback, and persists both to features.json — the register
 * lives in SHM and does not survive a reboot, exactly as for recall_quantize.
 */
static JSValue js_shadow_metronome_set(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 2) return JS_UNDEFINED;
    int mode = 0, level = 50;
    if (JS_ToInt32(ctx, &mode, argv[0])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &level, argv[1])) return JS_UNDEFINED;
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    shadow_control->metronome_mode = (uint8_t)mode;
    shadow_control->metronome_level = (uint8_t)level;

    static const char *NAMES[3] = { "off", "follow", "on" };
    char quoted[16];
    snprintf(quoted, sizeof(quoted), "\"%s\"", NAMES[mode]);
    features_json_set("metronome_mode", quoted);
    snprintf(quoted, sizeof(quoted), "%d", level);
    features_json_set("metronome_level", quoted);
    return JS_UNDEFINED;
}

/* shadow_metronome_beats_set(n) -> void
 *
 * Bar length for the downbeat accent, from the set's time signature. NOT
 * persisted: it belongs to the set, and the shadow UI re-reads it on every
 * SET_CHANGED. 0 means unknown and the shim clamps to 4.
 */
static JSValue js_shadow_metronome_beats_set(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int n = 0;
    if (JS_ToInt32(ctx, &n, argv[0])) return JS_UNDEFINED;
    if (n < 0) n = 0;
    if (n > 32) n = 32;
    shadow_control->metronome_beats_per_bar = (uint8_t)n;
    return JS_UNDEFINED;
}
```

Register both next to the `shadow_recall_quantize_set` registration (~line 2904):

```c
    JS_SetPropertyStr(ctx, global_obj, "shadow_metronome_set", JS_NewCFunction(ctx, js_shadow_metronome_set, "shadow_metronome_set", 2));
    JS_SetPropertyStr(ctx, global_obj, "shadow_metronome_beats_set", JS_NewCFunction(ctx, js_shadow_metronome_beats_set, "shadow_metronome_beats_set", 1));
```

- [ ] **Step 3: JS state and startup load**

In `src/shadow/shadow_ui.js`, after `loadRecallQuantize()` (which ends around line 8797):

```js
/*
 * Metronome. Mirrors recallQuantizeValue above: the setting persists in
 * features.json, the register in SHM does not, so JS reads the file at startup
 * and pushes the value down.
 */
let metronomeMode = 0;                 /* 0 off, 1 follow, 2 on */
let metronomeLevel = 50;               /* percent */
const METRONOME_MODE_NAMES = ["off", "follow", "on"];

function setMetronome(mode, level) {
    metronomeMode = (mode >= 0 && mode <= 2) ? mode : 0;
    metronomeLevel = (level >= 0 && level <= 100) ? level : 50;
    if (typeof shadow_metronome_set === "function") {
        shadow_metronome_set(metronomeMode, metronomeLevel);
    }
}

function loadMetronome() {
    let mode = 0, level = 50;
    try {
        const raw = host_read_file("/data/UserData/schwung/config/features.json");
        if (raw) {
            const mm = /"metronome_mode"\s*:\s*"([^"]*)"/.exec(raw);
            if (mm) {
                const i = METRONOME_MODE_NAMES.indexOf(mm[1]);
                if (i >= 0) mode = i;
            }
            const ml = /"metronome_level"\s*:\s*([0-9]+)/.exec(raw);
            if (ml && ml[1]) {
                const v = parseInt(ml[1], 10);
                if (v >= 0 && v <= 100) level = v;
            }
        }
    } catch (e) { debugLog("metronome read failed: " + e); }
    setMetronome(mode, level);
}
```

At startup, next to `try { loadRecallQuantize(); } ...` (~line 20907):

```js
    try { loadMetronome(); } catch (e) { debugLog("metronome load failed: " + e); }
```

- [ ] **Step 4: Build**

Run: `./scripts/build.sh 2>&1 | tail -n 5`
Expected: build succeeds. If `shadow_control_size_check` fires, the struct exceeded `CONTROL_BUFFER_SIZE` — bump the container, never shrink it.

- [ ] **Step 5: Confirm the JS actually parses as the host loads it**

`node --check` on a `.js` file silently passes source the QuickJS host rejects, so it is not sufficient on its own. Confirm the file is syntactically whole by loading it the way a test does:

`node --check` on a `.js` file treats it as CommonJS and silently passes source
the host rejects. `new Function(src)` is no better here — `shadow_ui.js` has
top-level `import` statements, so it must be parsed AS A MODULE:

Run: `node --input-type=module --check < src/shadow/shadow_ui.js && echo "PARSE OK"`
Expected: `PARSE OK`

And prove that check can fail, once:

Run: `(cat src/shadow/shadow_ui.js; echo "function {{{ broken") | node --input-type=module --check`
Expected: a SyntaxError, non-zero exit

- [ ] **Step 6: Confirm no existing field moved**

Run: `git diff src/host/shadow_constants.h | grep -E '^[-]' | grep -v '^---'`
Expected: no removed lines inside `shadow_control_t` — the change must be pure addition, appended last.

- [ ] **Step 7: Commit**

```bash
git add src/host/shadow_constants.h src/shadow/shadow_ui.c src/shadow/shadow_ui.js
git commit -m "metronome: carry mode, level and bar length in shadow_control_t

Appended after stay_in_shadow so nothing behind them moves and
schwung-manager's raw-offset read is untouched. In SHM rather than
features.json because load_feature_config() runs once at init, the same
reason recall_quantize is a field."
```

---

## Task 4: Global Settings — move the Skipback pair, add the metronome

**Goal:** `metronome_mode` and `metronome_level` appear in Audio, `skipback_shortcut` and `skipback_seconds` move to Shortcuts, and the contract still plans to exactly seven pages.

**Files:**
- Modify: `src/shadow/shadow_ui_global_grid.mjs` — `AUDIO_PARAMS` (~372), `SHORTCUTS_PARAMS` (~496), `GLOBAL_ENUM_VALUES` (~109), `GLOBAL_ROUTING` (~200)
- Modify: `src/shadow/shadow_ui.js` — `readParam` switch (~12210), `writeParam` switch (~12331)
- Modify: `tests/host/test_global_settings_contract.sh` — `WANT_COUNT` (~124)

**Acceptance Criteria:**
- [ ] Audio holds exactly 8 params: the 6 survivors plus `metronome_mode` and `metronome_level`
- [ ] Shortcuts holds exactly 4: `shadow_ui_trigger`, `recall_quantize`, `skipback_shortcut`, `skipback_seconds`
- [ ] `planPages` returns exactly 7 pages — no `Audio - 2`
- [ ] `metronome_mode`'s options all fit the 3-char enum square via `short_options`
- [ ] Every new key resolves to declared metadata (no guessed `float 0..1`)
- [ ] The contract still builds with no `io` and no host globals

**Verify:** `bash tests/host/test_global_settings_contract.sh` → `PASS`, exit 0

**Steps:**

- [ ] **Step 1: Move the Skipback pair out of Audio**

In `src/shadow/shadow_ui_global_grid.mjs`, DELETE these two entries from `AUDIO_PARAMS` (keeping their comments, which move with them):

```js
    { key: "skipback_shortcut", name: "Skipback", type: "enum",
      options: ["Cap", "Vol+Cap"], short_options: ["S+C", "SVC"], default: 0 },
    { key: "skipback_seconds", name: "Skipback Len", type: "enum",
      options: ["30s", "1m", "2m", "3m", "4m", "5m"], default: 0 },
```

- [ ] **Step 2: Add the metronome to Audio**

Append to `AUDIO_PARAMS`, after `usbc_out_persist`:

```js
    /*
     * THREE OPTIONS, NOT A BOOL, and the third one is load-bearing.
     *
     * "Follow" tracks Move's own metronome, learned from Move's "Metronome On"
     * / "Metronome Off" announcement. "On" ignores that and clicks whenever the
     * transport runs — it is the hedge for a firmware whose announcement text
     * is shaped differently, so the feature stays usable while detection is
     * fixed rather than silently doing nothing.
     *
     * In EVERY mode the click sounds only under Move->Schwung. Outside it
     * Move's own metronome is audible, so this rule prevents doubling by
     * construction rather than by a second condition someone can forget.
     */
    { key: "metronome_mode", name: "Metronome", type: "enum",
      options: ["Off", "Follow", "On"], short_options: ["OFF", "FOL", "ON"], default: 0 },
    { key: "metronome_level", name: "Click Vol", type: "int",
      min: 0, max: 100, step: 5, default: 50, unit: "%" },
```

- [ ] **Step 3: Add the Skipback pair to Shortcuts**

In `SHORTCUTS_PARAMS`, after the existing entries, append:

```js
    /*
     * Both Skipback rows moved here from Audio to make room for the metronome.
     * They move as a PAIR: one names the button combo and the other its
     * length, and splitting them across two sections would be worse than
     * leaving both in Audio. Skipback is a shortcut feature — Shift+Capture —
     * so this is where the combo belonged anyway.
     */
    { key: "skipback_shortcut", name: "Skipback", type: "enum",
      options: ["Cap", "Vol+Cap"], short_options: ["S+C", "SVC"], default: 0 },
    { key: "skipback_seconds", name: "Skipback Len", type: "enum",
      options: ["30s", "1m", "2m", "3m", "4m", "5m"], default: 0 },
```

- [ ] **Step 4: Register the enum's stored values**

In `GLOBAL_ENUM_VALUES`, add:

```js
    metronome_mode: [0, 1, 2],
```

(`skipback_shortcut` and `skipback_seconds` entries stay exactly where they are — the table is keyed by param, not by section.)

- [ ] **Step 5: Route the two new keys**

In `GLOBAL_ROUTING`, in the Audio block:

```js
    /* persist: null — shadow_metronome_set writes features.json itself, the
     * same way shadow_recall_quantize_set does, because the register it also
     * writes lives in SHM and does not survive a reboot. */
    metronome_mode:         { read: "metronome.get_mode",   write: "metronome.set_mode",   persist: null,   cache: null,                     modal: null },
    metronome_level:        { read: "metronome.get_level",  write: "metronome.set_level",  persist: null,   cache: null,                     modal: null },
```

- [ ] **Step 6: Read and write branches**

In `src/shadow/shadow_ui.js`, in the `readParam` switch, in the audio group:

```js
            case "metronome_mode":
                return String(metronomeMode);
            case "metronome_level":
                return String(metronomeLevel);
```

In the `writeParam` switch:

```js
            case "metronome_mode":
                setMetronome(parseInt(value, 10) || 0, metronomeLevel);
                return;
            case "metronome_level":
                setMetronome(metronomeMode, Math.round(parseFloat(value)));
                return;
```

- [ ] **Step 7: Update the contract test's counts**

In `tests/host/test_global_settings_contract.sh`, change:

```js
  const WANT_COUNT = { display: 7, audio: 8, accessibility: 6, set_pages: 1, shortcuts: 2, services: 2 };
```

to:

```js
  /* Audio is still 8: the two Skipback rows moved to Shortcuts to make room
   * for the metronome pair. The "exactly 7 pages" and "Audio is at
   * KNOBS_PER_PAGE exactly" assertions below are UNCHANGED — they are what
   * catches a spill, and a spill is the thing this move exists to avoid. */
  const WANT_COUNT = { display: 7, audio: 8, accessibility: 6, set_pages: 1, shortcuts: 4, services: 2 };
```

- [ ] **Step 8: Run the contract test**

Run: `bash tests/host/test_global_settings_contract.sh`
Expected: `PASS` (exit 0). A `FAIL: expected exactly 7 pages … got 8` means a section spilled — recount rather than raising the expectation.

- [ ] **Step 9: Prove the count assertion can still fail**

Temporarily add a ninth param to `AUDIO_PARAMS`:

```js
    bool("metronome_debug_probe", "Probe", 0),
```

Run: `bash tests/host/test_global_settings_contract.sh`
Expected: FAIL, naming both the 7-page assertion and Audio's count.

Remove the probe. Re-run and confirm PASS.

- [ ] **Step 10: Commit**

```bash
git add src/shadow/shadow_ui_global_grid.mjs src/shadow/shadow_ui.js tests/host/test_global_settings_contract.sh
git commit -m "settings: metronome in Audio, Skipback pair to Shortcuts

The knob grid holds 8 per page; a 9th does not error, it plans an
'Audio - 2' page with one lonely knob. Moving the two Skipback rows to
Shortcuts (where the combo belonged) keeps Audio at 8 and the total at 7."
```

---

## Task 5: The click generator

**Goal:** `shadow_metronome.c` — watches the transport, triggers the voice, renders a block. Nothing else knows about pulses or sines.

**Files:**
- Create: `src/host/shadow_metronome.h`
- Create: `src/host/shadow_metronome.c`
- Modify: `scripts/build.sh:241` and `scripts/build.sh:271-274` (both source lists)

**Acceptance Criteria:**
- [ ] `shadow_metronome_render()` is a no-op returning 0 when mode is off, the transport is stopped, or the voice is idle
- [ ] No allocation, no file I/O, no locks, no `unified_log()` anywhere in the file
- [ ] Mode 1 requires `shadow_metronome_on`; mode 2 ignores it
- [ ] `shadow_metronome_reset()` silences the voice and forgets the pulse position
- [ ] `grep -nE 'malloc|calloc|free|fopen|fprintf|unified_log|pthread_mutex' src/host/shadow_metronome.c` returns nothing

**Verify:** `./scripts/build.sh 2>&1 | tail -n 5` → build succeeds; the grep above → empty

**Steps:**

- [ ] **Step 1: Write the header**

Create `src/host/shadow_metronome.h`:

```c
/*
 * shadow_metronome.h — Schwung's own metronome click.
 *
 * WHY THIS EXISTS. Under Move->Schwung (rebuild_from_la) the shim zeroes the
 * mailbox and rebuilds it from Link Audio slots 0-3, the four per-track
 * channels, so it can insert per-slot FX. Move mixes its metronome at MASTER,
 * not into a track, and Move's Main channel is deliberately unsubscribed
 * (link_subscriber.cpp) — so the metronome is absent from the reconstruction
 * by construction, not by a bug. Nothing recovers it but generating our own.
 *
 * ALL OF THIS RUNS ON THE SPI CALLBACK. No allocation, no file I/O, no locks,
 * no unified_log().
 */
#ifndef SHADOW_METRONOME_H
#define SHADOW_METRONOME_H

#include <stdint.h>

/* Mode values, matching shadow_control_t.metronome_mode. */
#define SHADOW_METRONOME_OFF    0
#define SHADOW_METRONOME_FOLLOW 1
#define SHADOW_METRONOME_ON     2

/*
 * Render one block of click into `out_lr` (stereo interleaved int16), MIXING
 * rather than overwriting. Returns 1 if anything was added, 0 if the block was
 * left untouched — so the caller can skip the work on an idle block.
 *
 *   mode          shadow_control_t.metronome_mode
 *   move_on       shadow_metronome_on (only consulted in FOLLOW)
 *   playing       sampler_transport_playing
 *   pulses        shadow_transport_pulses (24 PPQN, reset on MIDI Start)
 *   beats_per_bar shadow_control_t.metronome_beats_per_bar (0 = clamp to 4)
 *   level_pct     shadow_control_t.metronome_level, 0-100
 */
int shadow_metronome_render(int16_t *out_lr, int frames,
                            int mode, int move_on, int playing,
                            int pulses, int beats_per_bar, int level_pct);

/* Silence the voice and forget the pulse position. Called when the metronome
 * path is left, so re-entering cannot replay a stale boundary. */
void shadow_metronome_reset(void);

#endif /* SHADOW_METRONOME_H */
```

- [ ] **Step 2: Write the implementation**

Create `src/host/shadow_metronome.c`:

```c
/* See shadow_metronome.h. Everything here runs on the SPI callback. */

#include "shadow_metronome.h"
#include "metronome_click.h"

#define METRONOME_SAMPLE_RATE 44100.0f

/*
 * Full-scale amplitude at level 100. -12 dBFS: the click sits over a full mix
 * without being the loudest thing in it, and leaves headroom so a click landing
 * on a peak cannot be what clips the mailbox.
 */
#define METRONOME_FULL_SCALE 8192.0f

static metronome_voice_t g_voice;
static int g_last_pulses = -1;

void shadow_metronome_reset(void)
{
    g_voice.amp = 0.0f;
    g_last_pulses = -1;
}

int shadow_metronome_render(int16_t *out_lr, int frames,
                            int mode, int move_on, int playing,
                            int pulses, int beats_per_bar, int level_pct)
{
    if (!out_lr || frames <= 0) return 0;

    if (mode == SHADOW_METRONOME_OFF ||
        (mode == SHADOW_METRONOME_FOLLOW && !move_on)) {
        shadow_metronome_reset();
        return 0;
    }

    /* A queue with no clock never fires. Stopped means silent, and it also
     * means forgetting where we were: the next Start zeroes the pulse count,
     * and a stale g_last_pulses would swallow that first downbeat. */
    if (!playing) {
        shadow_metronome_reset();
        return 0;
    }

    if (beats_per_bar <= 0) beats_per_bar = METRONOME_DEFAULT_BEATS_PER_BAR;

    if (g_last_pulses >= 0) {
        int beat = metronome_beat_crossed(g_last_pulses, pulses, beats_per_bar);
        if (beat >= 0) {
            if (level_pct < 0) level_pct = 0;
            if (level_pct > 100) level_pct = 100;
            float amp = (float)level_pct / 100.0f;
            metronome_voice_trigger(&g_voice,
                                    beat == 0 ? METRONOME_FREQ_DOWNBEAT_HZ
                                              : METRONOME_FREQ_BEAT_HZ,
                                    amp, METRONOME_DECAY_SECONDS,
                                    METRONOME_SAMPLE_RATE);
        }
    }
    g_last_pulses = pulses;

    if (!metronome_voice_active(&g_voice)) return 0;

    for (int i = 0; i < frames; i++) {
        float s = metronome_voice_next(&g_voice) * METRONOME_FULL_SCALE;
        for (int ch = 0; ch < 2; ch++) {
            int idx = i * 2 + ch;
            int32_t mixed = (int32_t)out_lr[idx] + (int32_t)s;
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            out_lr[idx] = (int16_t)mixed;
        }
    }
    return 1;
}
```

- [ ] **Step 3: Add to both build source lists**

In `scripts/build.sh`, add `src/host/shadow_metronome.c` to the list at line 241 (alongside `src/host/shadow_dbus.c`) AND to the second list at lines 271-274. **Both**: a source in one list and not the other links in one target and not the other, and the failure is a link error in whichever target is built second — easy to fix in the wrong place.

- [ ] **Step 4: Build**

Run: `./scripts/build.sh 2>&1 | tail -n 5`
Expected: build succeeds.

- [ ] **Step 5: Assert the realtime contract mechanically**

Run: `grep -nE 'malloc|calloc|realloc|free\(|fopen|fprintf|fwrite|unified_log|pthread_mutex|pthread_create' src/host/shadow_metronome.c`
Expected: no output. Any hit is a realtime violation on the SPI callback.

- [ ] **Step 6: Commit**

```bash
git add src/host/shadow_metronome.c src/host/shadow_metronome.h scripts/build.sh
git commit -m "metronome: the click generator

Watches shadow_transport_pulses, triggers a decaying sine, mixes a block.
No allocation, no file I/O, no locks — it runs on the SPI callback."
```

---

## Task 6: Mix it into the DAC path, and nowhere else

**Goal:** The click reaches the speakers and never a recording.

**Files:**
- Modify: `src/schwung_shim.c` — include (~line 100 block), the mix call after `native_capture_total_mix_snapshot_from_buffer(unity_view);` (~line 2806)

**Acceptance Criteria:**
- [ ] The call sits AFTER `native_capture_total_mix_snapshot_from_buffer(unity_view)` and BEFORE the `rebuild_from_la && mv < 0.9999f` volume scaling
- [ ] It is guarded by `rebuild_from_la`, so it cannot double with Move's own metronome
- [ ] `unity_view` is not touched, so the sampler, Skipback and the resample bridge never see the click
- [ ] Leaving `rebuild_from_la` resets the generator

**Verify:** `./scripts/build.sh 2>&1 | tail -n 5` → succeeds; the source-order check in Step 3 → PASS

**Steps:**

- [ ] **Step 1: Include the header**

In `src/schwung_shim.c`, alongside the other `src/host` includes:

```c
#include "host/shadow_metronome.h"
```

(Match the include style already used for `shadow_dbus.h` in that file — use the same prefix form as its neighbours.)

- [ ] **Step 2: Add the mix call**

Immediately after `native_capture_total_mix_snapshot_from_buffer(unity_view);` and BEFORE the `if (rebuild_from_la && mv < 0.9999f)` block:

```c
    /*
     * Schwung's metronome. Move mixes its own at master, which rebuild_from_la
     * discards along with everything else outside the four per-track channels,
     * so under Move->Schwung there is no click unless we make one.
     *
     * POSITION IS THE WHOLE DESIGN. unity_view was snapshotted above, so the
     * click is absent from the Quantized Sampler, Skipback and the native
     * resample bridge — a resample stays clean. It goes in before the master
     * volume scaling below, so it tracks the knob and gets speaker EQ like
     * everything else on the DAC.
     *
     * Gated on rebuild_from_la in EVERY mode, including "On": outside it
     * Move's own metronome is audible and this would double it.
     */
    if (rebuild_from_la && shadow_control) {
        shadow_metronome_render(mailbox_audio, FRAMES_PER_BLOCK,
                                shadow_control->metronome_mode,
                                shadow_metronome_on,
                                sampler_transport_playing,
                                shadow_transport_pulses,
                                shadow_control->metronome_beats_per_bar,
                                shadow_control->metronome_level);
    } else {
        shadow_metronome_reset();
    }
```

- [ ] **Step 3: Assert the ordering in the source**

The design doc asked for a test asserting the click is present in
`mailbox_audio` and absent from `unity_view` in the same frame. That needs the
shim built and running, and the shim cannot be built on the dev machine — so it
is split in two: a STATIC pin on the source order here, and the on-device
resample check in Task 9 Step 6. Both are required; neither alone shows it.

The position is the design, so pin it rather than trusting the diff:

Run:
```bash
awk '/native_capture_total_mix_snapshot_from_buffer\(unity_view\)/{snap=NR}
     /shadow_metronome_render\(mailbox_audio/{mix=NR}
     /rebuild_from_la && mv < 0\.9999f/{vol=NR}
     END{ if (snap && mix && vol && snap < mix && mix < vol)
            print "PASS: snapshot(" snap ") < metronome(" mix ") < volume(" vol ")";
          else print "FAIL: snapshot=" snap " metronome=" mix " volume=" vol }' src/schwung_shim.c
```
Expected: `PASS: snapshot(...) < metronome(...) < volume(...)`

- [ ] **Step 4: Build**

Run: `./scripts/build.sh 2>&1 | tail -n 5`
Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/schwung_shim.c
git commit -m "metronome: mix the click into the DAC path, after the capture split

unity_view is snapshotted first, so the sampler, Skipback and the resample
bridge never record the click; it goes in before master volume so it tracks
the knob. Gated on rebuild_from_la so it cannot double with Move's own."
```

---

## Task 7: Bar length from the set's time signature

**Goal:** The downbeat accent follows the set's time signature instead of assuming 4/4, read off the SPI thread.

**Files:**
- Modify: `src/shadow/shadow_ui.js:21646-21665` (the SET_CHANGED block that already reads `Song.abl`)

**Acceptance Criteria:**
- [ ] `timeSignature.upper` is parsed from the SAME `songJson` string the tempo override already reads — one file read, not two
- [ ] A missing or unparseable value pushes 0, and the shim clamps that to 4
- [ ] Values outside 1..32 are rejected rather than clamped silently at the JS end
- [ ] The read stays inside the SET_CHANGED handler, which is the shadow UI thread — never the SPI callback

**Verify:** `node --input-type=module -e '...'` parse check (Step 3) → parses; on-device check deferred to Task 9

**Steps:**

- [ ] **Step 1: Extend the existing Song.abl block**

In `src/shadow/shadow_ui.js`, inside the `if (songJson) {` block that currently matches `"tempo"`, after the tempo handling and before the closing brace:

```js
                        /*
                         * Bar length for the metronome's downbeat accent.
                         * Parsed from the SAME songJson the tempo override
                         * above already read — a second host_read_file for the
                         * same file on the same event would be two reads that
                         * can disagree.
                         *
                         * This is the shadow UI thread (SCHED_OTHER), which is
                         * where every file op on SET_CHANGED belongs:
                         * shadow_handle_set_loaded runs on the audio thread and
                         * deliberately does no I/O at all.
                         */
                        const ts = /"timeSignature"\s*:\s*\{[^}]*?"upper"\s*:\s*([0-9]+)/.exec(songJson);
                        let upper = 0;
                        if (ts && ts[1]) {
                            const n = parseInt(ts[1], 10);
                            if (n >= 1 && n <= 32) upper = n;
                        }
                        if (typeof shadow_metronome_beats_set === "function") {
                            shadow_metronome_beats_set(upper);
                        }
                        debugLog("SET_CHANGED: metronome beats_per_bar = " +
                                 (upper || "unknown (shim clamps to 4)"));
```

- [ ] **Step 2: Check the regex against a real Song.abl shape**

The device's `Song.abl` opens with:

```json
{
  "$schema": "http://tech.ableton.com/schema/song/1.8.3/song.json",
  "stepEditorResolution": "1/16",
  "tempo": 120.0,
  "globalGrooveAmount": 0.0,
  "timeSignature": {
    "upper": 4,
    "lower": 4
  },
```

Run:
```bash
node -e '
const s = `{"tempo": 120.0, "timeSignature": {\n    "upper": 3,\n    "lower": 4\n  },`;
const m = /"timeSignature"\s*:\s*\{[^}]*?"upper"\s*:\s*([0-9]+)/.exec(s);
console.log(m ? m[1] : "NO MATCH");
const bad = `{"timeSignature": {}, "upper": 9}`;
const m2 = /"timeSignature"\s*:\s*\{[^}]*?"upper"\s*:\s*([0-9]+)/.exec(bad);
console.log(m2 ? "FAIL: matched an upper OUTSIDE the object: " + m2[1] : "PASS: did not match outside");
'
```
Expected:
```
3
PASS: did not match outside
```

- [ ] **Step 3: Parse check**

Run: `node --input-type=module --check < src/shadow/shadow_ui.js && echo "PARSE OK"`
Expected: `PARSE OK`

- [ ] **Step 4: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "metronome: take the downbeat accent from the set's time signature

Parsed from the same Song.abl string the tempo override already reads, on
SET_CHANGED, which is the shadow UI thread — shadow_handle_set_loaded runs
on the audio thread and does no I/O by design. Unknown pushes 0 and the
shim clamps to 4."
```

---

## Task 8: Documentation

**Goal:** Every doc the Release Checklist names is current, and the war story lands in the subsystem file rather than inline in `CLAUDE.md`.

**Files:**
- Modify: `docs/SHADOW_UI.md` (new section)
- Modify: `CLAUDE.md` (ONE bullet under the Shadow Mode hook — not the prose)
- Modify: `src/shared/help_content.json`
- Modify: `../schwung-catalog-site/manual.html` (skip silently if the sibling repo is not checked out)

**Acceptance Criteria:**
- [ ] `CLAUDE.md` gains one bullet, not a section — the file is an INDEX
- [ ] `docs/SHADOW_UI.md` carries the reasoning: why the metronome is missing, why the announcement match is safe where the mute one was not, and why the mix point is where it is
- [ ] `help_content.json` documents the setting and states that the click only sounds under Move→Schwung
- [ ] Every help entry uses `children` — an entry without it is DISCARDED and never displayed

**Verify:** `node -e 'JSON.parse(require("fs").readFileSync("src/shared/help_content.json","utf8")); console.log("valid JSON")'` → `valid JSON`

**Steps:**

- [ ] **Step 1: `docs/SHADOW_UI.md`**

Add a section. Cover, in this order: that `rebuild_from_la` composites only slots 0–3 so a master-bus metronome is gone by construction; that Main is unsubscribed for measured reasons and `Main − Σ(tracks)` is not the metronome because of `returnTracks` and `masterTrack`; that detection is exact equality on a whole normalised announcement and why that differs from the removed mute auto-correct; that the state is never persisted because Move does not persist it either, which is what makes 0-at-boot correct; that the mix point sits between the `unity_view` snapshot and the master-volume scaling, and that this is the entire reason a resample stays clean; and that the count-in click is a known gap.

- [ ] **Step 2: `CLAUDE.md` — one bullet**

Under the Shadow Mode hook, add exactly one bullet:

```markdown
- **A master-bus metronome is gone under Move→Schwung by CONSTRUCTION**, not by
  a bug: `rebuild_from_la` composites only the four per-track Link Audio slots,
  and Move mixes its click at master. Schwung plays its own, detected from
  Move's `"Metronome On"` / `"Metronome Off"` announcement — **exact equality on
  the whole normalised string**, which is what separates it from the removed
  mute auto-correct that matched a suffix and fired on Move's own drum-kit
  names. Never persisted, because **Move does not persist it either** — that is
  what makes off-at-boot the truth rather than a guess. The click mixes
  **between the `unity_view` snapshot and the master-volume scaling**, so it is
  on the DAC and in no recording.
```

- [ ] **Step 3: `src/shared/help_content.json`**

Add an entry under the Global Settings help. **Every entry must have `children`** — an entry without it is discarded and never displayed, which is how twelve modules' help went unseen. Text to convey: Metronome is Off / Follow / On; Follow tracks Move's own metronome (Shift+Step 6); the click is heard only when Move→Schwung is on, because that is the only time Move's own click is missing; Click Vol sets its level; the one-bar count-in before recording is not covered.

- [ ] **Step 4: The manual**

If `../schwung-catalog-site/` is checked out, add the same user-facing description to `manual.html` beside the other Global Settings → Audio rows. If it is not checked out, skip — this is safe on any machine, and note the skip in the commit message.

No knob-grid widget changed, so **do not** regenerate the widget sheet.

- [ ] **Step 5: Validate**

Run: `node -e 'const h=JSON.parse(require("fs").readFileSync("src/shared/help_content.json","utf8")); console.log("valid JSON")'`
Expected: `valid JSON`

Run: `bash tests/host/test_widget_sheet.sh`
Expected: PASS (unchanged — no widget changed)

- [ ] **Step 6: Commit**

```bash
git add CLAUDE.md docs/SHADOW_UI.md src/shared/help_content.json
git commit -m "docs: the Schwung metronome

Reasoning in docs/SHADOW_UI.md, one bullet in CLAUDE.md — the file is an
index, and adding prose inline is how it reached 151 KB."
```

---

## Task 9: Verify on hardware

**Goal:** Confirm on the device what the host tests cannot: that the announcement arrives in the shape we match, that the click is audible under Move→Schwung, and that it is absent from a resample.

**Files:** none — this is a device session.

**Acceptance Criteria:**
- [ ] The debug log shows `Metronome: Move reports ON` when Shift+Step 6 is pressed, and `OFF` on the second press
- [ ] With Move→Schwung on, mode Follow and Move's metronome on, a click is audible and accents every 4th beat
- [ ] A Quantized Sampler resample taken while the click is audible contains **no** click
- [ ] With Move→Schwung OFF, Schwung's click is silent in every mode (Move's own is audible, undoubled)
- [ ] `debug_log_on` is removed at the end of the session

**Verify:** the log lines and the listening checks below

**Steps:**

- [ ] **Step 1: Ask before deploying**

`install.sh` restarts the service under whatever the user is doing. **Ask first.** Do not deploy on your own initiative.

- [ ] **Step 2: Deploy**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

Never scp individual files — the script handles setuid, symlinks, feature config and the service restart.

- [ ] **Step 3: Arm the log, and plan to disarm it**

```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log" &
```

`debug_log_on` has itself caused the dropouts it was being used to hunt. Disarm it in Step 7 whatever the outcome.

- [ ] **Step 4: Confirm the announcement string**

Ask the user to press Shift+Step 6, wait, and press it again.

Expected in the log:
```
Metronome: Move reports ON
Metronome: Move reports OFF
```

If neither line appears, the wire text is not what we match. Capture the real text before changing the matcher:
```bash
ssh ableton@move.local "taskset 0x7 dbus-monitor --system \"type='signal',interface='com.ableton.move.ScreenReader'\"" 
```
Ask the user to toggle again, read the actual string, and widen `metronome_announce.h` to the observed form — **exactly**, still as whole-string equality, never as a substring.

- [ ] **Step 5: Listen**

Ask the user to: enable Global Settings → Audio → Move→Schwung; set Metronome to Follow; turn Move's metronome on; press Play.

Expected: a click on every beat, accented every 4th. Confirm Click Vol changes its level and that Off silences it.

- [ ] **Step 6: Confirm the click is out of captures**

Ask the user to take a Quantized Sampler resample (Shift+Sample) while the click is audible, then play the resulting file back from `Samples/Schwung/Resampler/<date>/`.

Expected: the recording contains the music and **no click**. A click in the file means the mix landed before the `unity_view` snapshot — re-check the Task 6 ordering assertion.

- [ ] **Step 7: Disarm**

```bash
ssh ableton@move.local "rm -f /data/UserData/schwung/debug_log_on"
```

- [ ] **Step 8: Record the result**

If anything differed from the plan — especially the announcement text — amend `docs/plans/2026-09-01-schwung-metronome-design.md` with what was actually observed, and commit.

---

## Task 10: Open the PR

**Goal:** The branch lands through the required checks, on `main`'s terms.

**Acceptance Criteria:**
- [ ] All of `tests/host/` is green locally
- [ ] The PR is opened from `schwung-metronome`; nothing is pushed to `main`
- [ ] All three required checks pass: `host-tests`, `go`, `cross-compile`

**Verify:** `gh pr view --json state,statusCheckRollup`

**Steps:**

- [ ] **Step 1: Run the gating suite**

```bash
make -C tests/host test
for t in tests/host/*.sh; do bash "$t" || echo "FAILED: $t"; done
```
Expected: every target PASS, no `FAILED:` lines. Only `tests/host/` gates CI; `tests/{shadow,store,build}` carry ~20 known-stale failures and are not run here.

- [ ] **Step 2: Push and open the PR**

```bash
git push -u origin schwung-metronome
gh pr create --title "metronome: play our own click under Move>Schwung" --body "$(cat <<'EOF'
Under Move->Schwung, `rebuild_from_la` composites the mailbox from the four
per-track Link Audio slots. Move mixes its metronome at master, so the click
is gone by construction — and Main is deliberately unsubscribed, with
`Main - sum(tracks)` not being the metronome anyway (`returnTracks`,
`masterTrack`).

So Schwung plays its own, detected from Move's own "Metronome On" /
"Metronome Off" announcement, which `shadow_dbus.c` already receives. The
match is exact equality on the whole normalised string — unlike the removed
mute auto-correct, which matched a suffix and fired on Move's drum-kit names
and on Schwung's own TTS. The state is never persisted, because Move does
not persist it either, which is what makes off-at-boot correct rather than a
guess.

The click mixes between the `unity_view` snapshot and the master-volume
scaling: on the DAC, in no recording.

Settings live in Global Settings -> Audio. The two Skipback rows moved to
Shortcuts to make room — the grid holds 8 per page and a 9th plans an
"Audio - 2" page with one lonely knob.

Known gap, stated not hidden: the one-bar count-in click plays even with the
metronome off, is equally silent, and has no announcement to detect.

Design: `docs/plans/2026-09-01-schwung-metronome-design.md`
Plan: `docs/plans/2026-09-01-schwung-metronome-plan.md`

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01WrYKWXA7rzpgh3DDt5jx3n
EOF
)"
```

- [ ] **Step 3: Wait for the checks**

Run: `gh pr view --json statusCheckRollup --jq '.statusCheckRollup[] | "\(.name): \(.conclusion)"'`
Expected: `host-tests: SUCCESS`, `go: SUCCESS`, `cross-compile: SUCCESS`

- [ ] **Step 4: Merging**

Do not merge without asking. And note: `gh pr merge` reports a failure it did not have when `main` is checked out in a worktree — the merge lands, then `gh` dies updating the local checkout. Confirm with `gh pr view <n> --json state,mergeCommit` rather than the exit status, delete the remote branch by hand, and **do not re-run the merge on the strength of that error.**
