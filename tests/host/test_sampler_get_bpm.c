/* What every module's host->get_bpm() returns: sampler_get_bpm(), linked for
 * real with shadow_transport.c and fed Move's clock the way the shim feeds it
 * (ticks seen per 128-frame block).
 *
 * The defect this exists to fail on: while Move played, get_bpm() re-timed
 * each beat on the wall clock and so read 120.19, 119.49, 120.19 ... at a
 * steady 120. A tempo-synced delay resizes its line on every change, which is
 * a pitch bend -- up to an octave per beat on a 1-bar echo (Munchi Delay; War
 * Bells and Tape Echo 2 the same way). So the assertion is that the value
 * does not MOVE, not that it is near 120: "near 120" is what the broken
 * measurement passed. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "host/shadow_sampler.h"
#include "host/shadow_transport.h"

/* ---- stubs for what shadow_sampler.c reaches that a tempo query never does */
typedef struct clip_state clip_state_t;
clip_state_t *clip_state_mutable(void) { return NULL; }
void clip_state_on_transport_start(clip_state_t *st) { (void)st; }
void shim_worker_post(uint8_t evt) { (void)evt; }

static void quiet(const char *m) { (void)m; }
static void quiet0(void) {}
static int no_cmd(const char *const argv[]) { (void)argv; return 0; }

/* Move's live model, as move_model_sync_tempo() answers it. */
static float g_model_bpm;   /* 0 = no model / Move follows an external clock */
static int model_tempo(float *bpm)
{
    if (g_model_bpm <= 0.0f) return 0;
    *bpm = g_model_bpm;
    return 1;
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* Move's clock at `bpm` for `beats`, as the shim sees it. Returns how many
 * times get_bpm() changed after `settle_beats`. */
static double g_t, g_now;
static int play(transport_src_t src, double bpm, int beats, int settle_beats, float *last)
{
    const double spt = 44100.0 * 60.0 / (bpm * 24.0);
    int changes = 0;
    float ref = -1.0f;
    for (int k = 0; k < beats * 24; k++) {
        g_t += spt;
        while (g_now < g_t) { shadow_transport_advance_block(128); g_now += 128; }
        if (src == TRANSPORT_SRC_MOVE) sampler_on_clock(0xF8);      /* schwung_shim.c, cable-0 tap */
        shadow_transport_on_realtime(src, 0xF8);
        const float b = sampler_get_bpm(NULL);
        if (k >= settle_beats * 24) {
            if (ref >= 0.0f && b != ref) changes++;
            ref = b;
        }
        if (last) *last = b;
    }
    return changes;
}

static void start(transport_src_t src)
{
    shadow_transport_on_realtime(src, 0xFA);
}

static void stop(transport_src_t src)
{
    shadow_transport_on_realtime(src, 0xFC);
    shadow_transport_advance_block(128);
    g_now += 128; g_t = g_now;
}

int main(void)
{
    sampler_host_t host = {
        .log = quiet, .announce = quiet, .overlay_sync = quiet0, .run_command = no_cmd,
        .move_tempo = model_tempo,
    };
    sampler_init(&host, NULL);
    shadow_transport_init(44100);
    tempo_source_t src;
    float last;

    /* 1. Move plays, model live: Move's own number, every beat, exactly. */
    g_model_bpm = 120.0f;
    start(TRANSPORT_SRC_MOVE);
    CHECK(play(TRANSPORT_SRC_MOVE, 120.0, 32, 0, &last) == 0);
    CHECK(last == 120.0f);
    CHECK(sampler_get_bpm(&src) == 120.0f && src == TEMPO_SOURCE_CLOCK);

    /* 2. No model (or Move follows an external MIDI clock): the clock,
     * measured -- and steady once the window is full. */
    g_model_bpm = 0.0f;
    stop(TRANSPORT_SRC_MOVE);
    shadow_transport_init(44100);
    start(TRANSPORT_SRC_MOVE);
    CHECK(play(TRANSPORT_SRC_MOVE, 120.0, 64, 17, &last) == 0);
    CHECK(fabsf(last - 120.0f) < 0.12f);
    CHECK(sampler_get_bpm(&src) == last && src == TEMPO_SOURCE_CLOCK);
    CHECK(play(TRANSPORT_SRC_MOVE, 133.0, 64, 17, &last) == 0);    /* a tempo change, then steady */
    CHECK(fabsf(last - 133.0f) < 0.15f);

    /* 3. Stopped without a model: the last measured tempo, held. */
    stop(TRANSPORT_SRC_MOVE);
    CHECK(sampler_get_bpm(&src) == last && src == TEMPO_SOURCE_LAST_CLOCK);

    /* 4. Stopped with a model: the set's tempo, and a tempo changed while
     * stopped is followed at once rather than when the clock next runs. */
    g_model_bpm = 90.0f;
    CHECK(sampler_get_bpm(&src) == 90.0f && src == TEMPO_SOURCE_SET);
    g_model_bpm = 95.5f;
    CHECK(sampler_get_bpm(NULL) == 95.5f);

    /* 5. An overtake sequencer's clock (movy) wins while Move is stopped --
     * its tempo, not the set's -- and its last tempo is kept after it stops,
     * so a synced LFO free-runs at the rate it was locked to. */
    g_model_bpm = 120.0f;
    start(TRANSPORT_SRC_INTERNAL);
    CHECK(play(TRANSPORT_SRC_INTERNAL, 100.0, 40, 17, &last) == 0);
    CHECK(fabsf(last - 100.0f) < 0.1f);
    stop(TRANSPORT_SRC_INTERNAL);
    CHECK(sampler_get_bpm(&src) == last && src == TEMPO_SOURCE_LAST_CLOCK);

    /* ...until Move plays: then Move's tempo. */
    start(TRANSPORT_SRC_MOVE);
    CHECK(play(TRANSPORT_SRC_MOVE, 120.0, 4, 0, &last) == 0);
    CHECK(last == 120.0f);

    if (fails) { printf("test_sampler_get_bpm: %d FAILED\n", fails); return 1; }
    printf("PASS: test_sampler_get_bpm\n");
    return 0;
}
