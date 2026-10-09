#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "host/shadow_transport.h"

static void fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); }
static void expect_near(double got, double want, double tol, const char *msg) {
    if (fabs(got - want) > tol) {
        fprintf(stderr, "FAIL: %s (got %f want %f)\n", msg, got, want);
        exit(1);
    }
}

/* 125 BPM at 24 PPQN and 44100 Hz = exactly 882 samples per tick. */
#define TICK_SAMPLES 882

/* Advance in 128-frame blocks, firing a tick each time the boundary passes. */
static void run_ticks(transport_src_t src, int ticks) {
    static long long carry = 0;
    for (int t = 0; t < ticks; t++) {
        carry += TICK_SAMPLES;
        while (carry > 0) { shadow_transport_advance_block(128); carry -= 128; }
        shadow_transport_on_realtime(src, 0xF8);
    }
}

/* A clock at any tempo, ticks seen per 128-frame block the way the shim sees
 * them: a tick whose true time falls inside a block is delivered at that
 * block's boundary. late_every > 0 delivers every Nth tick one block late, as
 * a crowded MIDI_OUT would. Returns the bpm read after each tick into out[]. */
static double g_clock_t, g_clock_now;
static void clock_reset(void) { g_clock_t = 0.0; g_clock_now = 0.0; }
static void run_clock(transport_src_t src, double bpm, int ticks, int late_every, float *out) {
    const double spt = 44100.0 * 60.0 / (bpm * 24.0);
    for (int t = 0; t < ticks; t++) {
        g_clock_t += spt;
        while (g_clock_now < g_clock_t) { shadow_transport_advance_block(128); g_clock_now += 128; }
        if (late_every > 0 && t % late_every == late_every - 1) {
            shadow_transport_advance_block(128); g_clock_now += 128;
        }
        shadow_transport_on_realtime(src, 0xF8);
        if (out) out[t] = shadow_transport_bpm();
    }
}

/* THE STEADY TEMPO: a steady clock must report a value that NEVER MOVES once
 * the window is full. The per-beat measurement it replaced read 120.19,
 * 119.49, 120.19 ... on this exact input, and a tempo-synced delay bent its
 * pitch on every change. Asserted as "no change at all", not "within a
 * tolerance": a tolerance is what that measurement passed. */
static void steady_case(double bpm, int late_every) {
    static float seen[64 * 24];
    char msg[160];
    shadow_transport_init(44100);
    clock_reset();
    shadow_transport_on_realtime(TRANSPORT_SRC_MOVE, 0xFA);
    run_clock(TRANSPORT_SRC_MOVE, bpm, 64 * 24, late_every, seen);
    /* From the tick the 16-beat window is full: settled for good. */
    for (int t = 17 * 24; t < 64 * 24; t++) {
        if (seen[t] != seen[17 * 24]) {
            snprintf(msg, sizeof msg, "steady %.2f BPM (late_every %d): moved %.4f -> %.4f at tick %d",
                     bpm, late_every, (double)seen[17 * 24], (double)seen[t], t);
            fail(msg);
        }
    }
    snprintf(msg, sizeof msg, "steady %.2f BPM (late_every %d) is accurate", bpm, late_every);
    expect_near((double)seen[64 * 24 - 1], bpm, bpm * 0.001, msg);
}

int main(void) {
    /* --- start anchor: FA then first F8 = beat 0 --- */
    shadow_transport_init(44100);
    shadow_transport_on_realtime(TRANSPORT_SRC_INTERNAL, 0xFA);
    shadow_transport_on_realtime(TRANSPORT_SRC_INTERNAL, 0xF8);
    expect_near(shadow_transport_beat_position(), 0.0, 1e-9, "beat 0 at first tick");
    if (shadow_transport_source() != TRANSPORT_SRC_INTERNAL) fail("internal source active");

    /* --- 24 ticks later = beat 1; measured bpm ~= 125 ---
     * Beat position is exact (tick-count driven, tol 0.02 beat). Block-
     * quantized ticks alternate 768/896 samples around the true 882 (design
     * §6), which is why the bpm is a fit over the window rather than any one
     * interval -- see THE STEADY TEMPO cases below for what it guarantees. */
    run_ticks(TRANSPORT_SRC_INTERNAL, 24);
    expect_near(shadow_transport_beat_position(), 1.0, 0.02, "beat 1 after 24 ticks");
    expect_near((double)shadow_transport_bpm(), 125.0, 1.0, "bpm measured ~125");

    /* --- interpolation: half a tick of silence advances ~half a tick --- */
    double before = shadow_transport_beat_position();
    shadow_transport_advance_block(TICK_SAMPLES / 2);
    double mid = shadow_transport_beat_position();
    expect_near(mid - before, 0.5 / 24.0, 0.01, "interpolated half tick");

    /* --- interpolation clamps: a very late tick never overshoots --- */
    shadow_transport_advance_block(TICK_SAMPLES * 3);
    double late = shadow_transport_beat_position();
    if (late > before + 1.5 / 24.0) fail("interpolation must clamp at one tick");

    /* --- arbitration: Move start takes over; Move stop hands back --- */
    shadow_transport_init(44100);
    shadow_transport_on_realtime(TRANSPORT_SRC_INTERNAL, 0xFA);
    shadow_transport_on_realtime(TRANSPORT_SRC_INTERNAL, 0xF8);
    shadow_transport_on_realtime(TRANSPORT_SRC_MOVE, 0xFA);
    shadow_transport_on_realtime(TRANSPORT_SRC_MOVE, 0xF8);
    if (shadow_transport_source() != TRANSPORT_SRC_MOVE) fail("Move wins while running");
    shadow_transport_on_realtime(TRANSPORT_SRC_MOVE, 0xFC);
    if (shadow_transport_source() != TRANSPORT_SRC_INTERNAL) fail("falls back to internal");

    /* --- last-known tempo survives stop (LFO free-run keeps movy's rate) --- */
    shadow_transport_init(44100);
    shadow_transport_on_realtime(TRANSPORT_SRC_INTERNAL, 0xFA);
    shadow_transport_on_realtime(TRANSPORT_SRC_INTERNAL, 0xF8);
    run_ticks(TRANSPORT_SRC_INTERNAL, 24);
    shadow_transport_on_realtime(TRANSPORT_SRC_INTERNAL, 0xFC);
    shadow_transport_advance_block(128);  /* stop takes effect */
    if (shadow_transport_beat_position() >= 0.0) fail("stopped = beat < 0");
    if (shadow_transport_source() != TRANSPORT_SRC_NONE) fail("stopped = no active source");
    if (shadow_transport_last_source() != TRANSPORT_SRC_INTERNAL) fail("last source = internal after stop");
    expect_near((double)shadow_transport_last_bpm(), 125.0, 2.5, "last bpm retained after stop");

    /* --- staleness: ticks stop arriving -> transport flips off --- */
    shadow_transport_init(44100);
    shadow_transport_on_realtime(TRANSPORT_SRC_INTERNAL, 0xFA);
    run_ticks(TRANSPORT_SRC_INTERNAL, 4);
    shadow_transport_advance_block(44100);  /* 1 s of silence > 0.5 s staleness */
    if (shadow_transport_beat_position() >= 0.0) fail("stale clock = beat < 0");

    /* --- unanchored clock (tool opened mid-song): F8 without FA still runs --- */
    shadow_transport_init(44100);
    run_ticks(TRANSPORT_SRC_MOVE, 26);
    if (shadow_transport_beat_position() < 0.0) fail("bare clock runs unanchored");
    expect_near((double)shadow_transport_bpm(), 125.0, 2.5, "bpm from bare clock");

    /* --- THE STEADY TEMPO --- */
    {
        static const double tempos[] = { 60.0, 87.3, 120.0, 133.33, 174.0, 300.0 };
        for (unsigned i = 0; i < sizeof tempos / sizeof tempos[0]; i++) {
            steady_case(tempos[i], 0);
            steady_case(tempos[i], 7);     /* a tick a block late, often */
        }
    }

    /* --- a real tempo change is still followed --- */
    {
        static float seen[16 * 24];
        shadow_transport_init(44100);
        clock_reset();
        shadow_transport_on_realtime(TRANSPORT_SRC_MOVE, 0xFA);
        run_clock(TRANSPORT_SRC_MOVE, 120.0, 16 * 24, 0, NULL);
        run_clock(TRANSPORT_SRC_MOVE, 140.0, 16 * 24, 0, seen);
        /* A step change is taken within about a beat, not glided to across
         * the whole window (which is 16 beats of smeared delay pitch). */
        expect_near((double)seen[24], 140.0, 1.5, "a 120 -> 140 step lands within a beat");
        expect_near((double)seen[16 * 24 - 1], 140.0, 0.15, "and settles on it");
        run_clock(TRANSPORT_SRC_MOVE, 140.5, 32 * 24, 0, NULL);
        expect_near((double)shadow_transport_bpm(), 140.5, 0.15, "follows a half-BPM nudge");
    }

    /* --- a restart at the same tempo answers at once, and before any clock
     *     has run there is no tempo at all (never a guess) --- */
    {
        shadow_transport_init(44100);
        clock_reset();
        if (shadow_transport_bpm() != 0.0f) fail("no clock yet = 0");
        shadow_transport_on_realtime(TRANSPORT_SRC_MOVE, 0xFA);
        run_clock(TRANSPORT_SRC_MOVE, 120.0, 4, 0, NULL);
        if (shadow_transport_bpm() != 0.0f) fail("under one beat of ticks = 0, not an estimate");
        run_clock(TRANSPORT_SRC_MOVE, 120.0, 12 * 24, 0, NULL);
        float held = shadow_transport_bpm();
        shadow_transport_on_realtime(TRANSPORT_SRC_MOVE, 0xFC);
        if (shadow_transport_last_bpm() != held) fail("last bpm is the steady value");
        shadow_transport_on_realtime(TRANSPORT_SRC_MOVE, 0xFA);
        run_clock(TRANSPORT_SRC_MOVE, 120.0, 1, 0, NULL);
        if (shadow_transport_bpm() != held) fail("restart reports the held tempo at once");
        run_clock(TRANSPORT_SRC_MOVE, 120.0, 12 * 24, 0, NULL);
        if (shadow_transport_bpm() != held) fail("restart at the same tempo never moves it");
    }

    printf("PASS: test_shadow_transport\n");
    return 0;
}
