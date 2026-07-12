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

int main(void) {
    /* --- start anchor: FA then first F8 = beat 0 --- */
    shadow_transport_init(44100);
    shadow_transport_on_realtime(TRANSPORT_SRC_INTERNAL, 0xFA);
    shadow_transport_on_realtime(TRANSPORT_SRC_INTERNAL, 0xF8);
    expect_near(shadow_transport_beat_position(), 0.0, 1e-9, "beat 0 at first tick");
    if (shadow_transport_source() != TRANSPORT_SRC_INTERNAL) fail("internal source active");

    /* --- 24 ticks later = beat 1; measured bpm ~= 125 ---
     * Beat position is exact (tick-count driven, tol 0.02 beat). The measured
     * bpm is intentionally jitter-tolerant: block-quantized ticks alternate
     * 768/896 samples around the true 882 (design §6), so the instantaneous
     * EMA readout swings a couple BPM about 125. The tight beat-position
     * assertion is what pins tempo precisely; this only rules out gross error. */
    run_ticks(TRANSPORT_SRC_INTERNAL, 24);
    expect_near(shadow_transport_beat_position(), 1.0, 0.02, "beat 1 after 24 ticks");
    expect_near((double)shadow_transport_bpm(), 125.0, 2.5, "bpm measured ~125");

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

    /* --- stop: no transport = negative beat position --- */
    shadow_transport_on_realtime(TRANSPORT_SRC_INTERNAL, 0xFC);
    if (shadow_transport_beat_position() >= 0.0) fail("stopped = beat < 0");
    if (shadow_transport_source() != TRANSPORT_SRC_NONE) fail("stopped = no source");

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

    printf("PASS: test_shadow_transport\n");
    return 0;
}
