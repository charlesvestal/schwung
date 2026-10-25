/*
 * speaker_eq: the jack rules that decide when the EQ may colour the output,
 * and the properties of the filter itself. The coefficients are not pinned
 * by a golden hash: float results differ across compilers (FMA contraction),
 * so a hash would fail on whichever machine did not make it.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "speaker_eq.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static struct timespec after(const struct timespec *t, double sec)
{
    struct timespec r = *t;
    long ns = (long)(sec * 1e9);
    r.tv_sec += ns / 1000000000L;
    r.tv_nsec += ns % 1000000000L;
    if (r.tv_nsec >= 1000000000L) { r.tv_sec++; r.tv_nsec -= 1000000000L; }
    return r;
}

static void fill_noise(int16_t *buf, int n, uint32_t *seed)
{
    for (int i = 0; i < n; i++) {
        *seed = *seed * 1664525u + 1013904223u;
        buf[i] = (int16_t)(((int32_t)(*seed >> 16) - 32768) / 2);
    }
}

static void test_jack_rules(void)
{
    speaker_eq_t eq = SPEAKER_EQ_INIT;
    speaker_eq_init(&eq, 44100.0f);
    speaker_eq_restart_clock(&eq);
    struct timespec t0 = eq.speaker_since;

    CHECK(eq.speaker_active == 1 && eq.speaker_known == 0, "boot state is speaker, unknown");
    CHECK(!speaker_eq_speaker_stable_at(&eq, &(struct timespec){ t0.tv_sec + 60, 0 }),
          "an unobserved jack never engages the EQ, however long it waits");

    /* The gated handler does not count as observing the jack. */
    CHECK(speaker_eq_on_jack(&eq, 0, 0) == 0, "speaker -> speaker is no change");
    CHECK(eq.speaker_known == 0, "mark_known=0 leaves the jack unknown");

    /* The early scan does. A speaker reading must then HOLD for the window. */
    CHECK(speaker_eq_on_jack(&eq, 0, 1) == 0, "still no change");
    struct timespec t1 = eq.speaker_since;
    CHECK(eq.speaker_known == 1, "mark_known=1 marks the jack observed");
    struct timespec early = after(&t1, SPEAKER_EQ_STABLE_SEC - 0.1);
    struct timespec late = after(&t1, SPEAKER_EQ_STABLE_SEC + 0.1);
    CHECK(!speaker_eq_speaker_stable_at(&eq, &early), "not engaged inside the stability window");
    CHECK(speaker_eq_speaker_stable_at(&eq, &late), "engaged once the speaker reading held");

    /* A jack insert flips out at once and clears the EQ state (no thump). */
    eq.eq_state[0][0].z1 = 123.0f;
    CHECK(speaker_eq_on_jack(&eq, 127, 1) == 1, "speaker -> jack is a change");
    CHECK(eq.speaker_active == 0, "jack inserted reads as not-speaker");
    CHECK(eq.eq_state[0][0].z1 == 0.0f, "an output switch resets the EQ state");
    CHECK(!speaker_eq_speaker_stable_at(&eq, &late), "never engaged with a jack in");

    /* Back to speaker: the clock restarts, so the window applies again. A
     * stale clock planted first would read as long-stable if it did not. */
    eq.speaker_since = (struct timespec){ 0, 0 };
    CHECK(speaker_eq_on_jack(&eq, 0, 1) == 1, "jack -> speaker is a change");
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct timespec soon = after(&now, 0.5);
    CHECK(!speaker_eq_speaker_stable_at(&eq, &soon),
          "re-entering speaker restarts the clock: a fresh reading waits out the window");
}

static void test_filter(void)
{
    speaker_eq_t a = SPEAKER_EQ_INIT, b = SPEAKER_EQ_INIT;
    speaker_eq_init(&a, 44100.0f);
    speaker_eq_init(&b, 44100.0f);
    CHECK(a.initialized, "init marks the EQ built");

    int16_t silence[256] = {0};
    speaker_eq_process(&a, silence, 128);
    int quiet = 1;
    for (int i = 0; i < 256; i++) if (silence[i] != 0) quiet = 0;
    CHECK(quiet, "silence in, silence out");

    /* Two instances are independent and deterministic. */
    int16_t x[256], y[256];
    uint32_t s1 = 7, s2 = 7;
    int same = 1, changed = 0;
    for (int blk = 0; blk < 32; blk++) {
        fill_noise(x, 256, &s1);
        fill_noise(y, 256, &s2);
        int16_t in0 = x[0];
        speaker_eq_process(&a, x, 128);
        speaker_eq_process(&b, y, 128);
        if (memcmp(x, y, sizeof(x)) != 0) same = 0;
        if (x[0] != in0) changed = 1;
    }
    CHECK(same, "same input through two instances gives the same output");
    CHECK(changed, "the EQ actually changes the signal");

    /* The subsonic HP rejects DC: a constant input settles near zero. */
    speaker_eq_t c = SPEAKER_EQ_INIT;
    speaker_eq_init(&c, 44100.0f);
    int16_t dc[256];
    for (int blk = 0; blk < 400; blk++) {
        for (int i = 0; i < 256; i++) dc[i] = 8000;
        speaker_eq_process(&c, dc, 128);
    }
    CHECK(abs(dc[254]) < 50 && abs(dc[255]) < 50, "DC is filtered out");

    /* Full-scale input clips to int16 instead of wrapping. The cascade's
     * first-sample gain is above 1 (the b0 product), so a full-scale step
     * from rest overshoots: it must land on +32767, not wrap negative. */
    speaker_eq_t d = SPEAKER_EQ_INIT;
    speaker_eq_init(&d, 44100.0f);
    int16_t loud[256];
    for (int i = 0; i < 256; i++) loud[i] = 32767;
    speaker_eq_process(&d, loud, 128);
    CHECK(loud[0] == 32767 && loud[1] == 32767, "a full-scale step clips instead of wrapping");
}

int main(void)
{
    test_jack_rules();
    test_filter();
    if (failures) { printf("test_speaker_eq: %d failure(s)\n", failures); return 1; }
    printf("test_speaker_eq: all passed\n");
    return 0;
}
