/*
 * The master filter (src/host/master_filter.h): transparent at centre BIT FOR
 * BIT, a real low-pass / high-pass either side, and no click engaging,
 * leaving, or swinging straight from one side to the other.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "master_filter.h"

#define FS 44100.0f
#define N 128
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)

static void noise(int16_t *b, unsigned *seed) {
    for (int i = 0; i < N * 2; i++) { *seed = *seed * 1103515245u + 12345u; b[i] = (int16_t)((*seed >> 16) - 16384); }
}
static void sine(int16_t *b, float hz, long *n0, float amp) {
    for (int i = 0; i < N; i++) { float v = amp * sinf(2.0f * (float)M_PI * hz * (float)(*n0 + i) / FS);
        b[2 * i] = b[2 * i + 1] = (int16_t)lrintf(v); }
    *n0 += N;
}
/* RMS of a sine at `hz` through the filter held at x, after settling. */
static float rms_through(float x, float hz) {
    master_filter_t f; master_filter_reset(&f);
    int16_t b[N * 2]; long n0 = 0; double acc = 0; int cnt = 0;
    for (int blk = 0; blk < 400; blk++) {
        sine(b, hz, &n0, 10000.0f);
        master_filter_process(&f, b, N, x, FS);
        if (blk >= 300) for (int i = 0; i < N * 2; i++) { acc += (double)b[i] * b[i]; cnt++; }
    }
    return (float)sqrt(acc / cnt);
}

int main(void) {
    const float dry = 10000.0f / sqrtf(2.0f);

    /* 1. Centre is untouched, bit for bit. */
    {
        master_filter_t f; master_filter_reset(&f);
        unsigned seed = 1; int same = 1;
        for (int blk = 0; blk < 200; blk++) {
            int16_t b[N * 2], ref[N * 2]; noise(b, &seed); memcpy(ref, b, sizeof b);
            master_filter_process(&f, b, N, (blk % 2) ? 0.01f : -0.015f, FS);   /* inside the dead band */
            if (memcmp(b, ref, sizeof b)) same = 0;
        }
        CHECK(same, "centre (inside the dead band) is bit-exact");
    }
    /* 2. Engage, then return: once faded out it is bit-exact again. */
    {
        master_filter_t f; master_filter_reset(&f);
        unsigned seed = 7; int16_t b[N * 2], ref[N * 2];
        for (int blk = 0; blk < 50; blk++) { noise(b, &seed); master_filter_process(&f, b, N, -0.8f, FS); }
        for (int blk = 0; blk < 10; blk++) { noise(b, &seed); master_filter_process(&f, b, N, 0.0f, FS); }
        int same = 1;
        for (int blk = 0; blk < 50; blk++) { noise(b, &seed); memcpy(ref, b, sizeof b);
            master_filter_process(&f, b, N, 0.0f, FS); if (memcmp(b, ref, sizeof b)) same = 0; }
        CHECK(same, "back at centre, bit-exact again (it stopped, not idling at 20 kHz)");
        CHECK(f.mix == 0.0f && f.mode == 0, "...and its state is forgotten");
    }
    /* 3. It filters. */
    {
        float lp_hi = rms_through(-1.0f, 5000.0f), lp_lo = rms_through(-1.0f, 30.0f);
        float hp_lo = rms_through(1.0f, 100.0f), hp_hi = rms_through(1.0f, 15000.0f);
        CHECK(lp_hi < dry * 0.01f, "hard left: 5 kHz is cut (below -40 dB)");
        CHECK(lp_lo > dry * 0.7f, "hard left: 30 Hz passes");
        CHECK(hp_lo < dry * 0.05f, "hard right: 100 Hz is cut");
        CHECK(hp_hi > dry * 0.7f, "hard right: 15 kHz passes");
        CHECK(rms_through(-0.1f, 1000.0f) > dry * 0.9f, "a touch left of centre barely colours 1 kHz");
    }
    /* 4. No click engaging, and 5. swinging straight across: the largest
     * sample-to-sample step stays near the dry signal own. */
    {
        master_filter_t f; master_filter_reset(&f);
        int16_t b[N * 2]; long n0 = 0; int prev = 0, maxstep = 0, bad = 0;
        const float xs[] = { 0.0f, 0.0f, -0.7f, -0.7f, -0.7f, 0.9f, 0.9f, 0.9f, -1.0f, 0.0f, 0.0f };
        for (int j = 0; j < (int)(sizeof xs / sizeof xs[0]); j++) {
            for (int blk = 0; blk < 20; blk++) {
                sine(b, 220.0f, &n0, 12000.0f);
                master_filter_process(&f, b, N, xs[j], FS);
                for (int i = 0; i < N; i++) { int d = abs(b[2 * i] - prev); if (d > maxstep) maxstep = d; prev = b[2 * i];
                    if (b[2 * i] != b[2 * i]) bad = 1; }
            }
        }
        /* A 220 Hz, 12000 sine steps at most ~376 per sample. */
        CHECK(maxstep < 1500, "no click engaging, leaving, or swinging LP -> HP in one move");
        CHECK(!bad, "...and never a NaN");
    }
    printf(fails ? "FAILED %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
