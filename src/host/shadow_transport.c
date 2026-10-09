/* Single authority for transport state: which clock source is running, its
 * tempo, and an interpolated beat position. Fed system-realtime bytes from
 * the shim's cable-0 tap (Move native) and from overtake-DSP internal sends.
 * The writers (shadow_transport_on_realtime, shadow_transport_advance_block)
 * run on the shim's audio thread: fixed-size state, no locks, no I/O, no
 * allocation. The BPM/source readers (shadow_transport_bpm/source/last_*) may
 * also be called off the audio thread via sampler_get_bpm; like the existing
 * unlocked sampler BPM reads, a torn double read on 32-bit ARM yields at worst
 * a transient wrong BPM, never a crash, and no reader mutates state. */
#include "shadow_transport.h"

#include <math.h>

#define TRANSPORT_PPQN 24
#define TRANSPORT_STALE_SEC 0.5
/* EMA weight: converges in ~10 ticks while absorbing the ±1-block (~2.9 ms)
 * jitter of block-quantized clock senders. Phase interpolation only -- see
 * THE STEADY TEMPO below for why the EMA is never reported as a tempo. */
#define TRANSPORT_EMA_ALPHA 0.25

/* THE STEADY TEMPO -- what get_bpm() reports while a clock runs.
 *
 * A tick is only ever SEEN at block granularity: it lands in whichever
 * 128-frame block it falls in, so at 120 BPM (918.75 samples per tick) the
 * observed intervals are 896 or 1024 samples and no single interval is a
 * tempo. Reporting one -- or an EMA of them, or one beat timed by the wall
 * clock, which is what get_bpm() used to serve -- hands modules a number that
 * changes every beat (120.19, 119.49, 120.19 ...) on a perfectly steady
 * clock. A tempo-synced delay resizes its line on every change, and resizing
 * a delay line is a pitch bend: up to an octave per beat on a 1-bar echo.
 *
 * So the tempo is measured across a WINDOW of ticks in SAMPLE time (a late
 * SPI frame replays as a burst, so wall time adds error that sample time
 * does not), and HELD: the reported value moves only when the window
 * disagrees with it by more than the window can be wrong. Each end of the
 * span is late by under one block, so the span's error is under one block;
 * the deadband is TRANSPORT_BPM_DEADBAND_BLOCKS of them, the margin covering a
 * tick that arrives a block late. On a steady clock the held value therefore
 * never moves, and a real tempo change still moves it. The window is long
 * because the bound is blocks over the span: 16 beats resolve ~0.1 % at 120
 * BPM and ~0.3 % at 300, where 8 beats (1.6 s) left 0.5 %. */
#define TRANSPORT_BPM_WINDOW 384          /* ticks: 16 beats */
#define TRANSPORT_BPM_MIN_TICKS 24        /* one beat before a first estimate */
#define TRANSPORT_TICK_QUANTUM 128.0      /* samples: a tick is seen per block */
#define TRANSPORT_BPM_DEADBAND_BLOCKS 3.0

typedef struct {
    int running;
    int awaiting_first_tick;  /* set by 0xFA; the next 0xF8 is tick 0 */
    unsigned long long tick_count;
    unsigned long long last_tick_at;  /* sample time of last 0xF8 */
    double tick_interval;             /* EMA, samples per tick; 0 = unknown */
    /* THE STEADY TEMPO: the last TRANSPORT_BPM_WINDOW + 1 tick times of an
     * unbroken run (reset by anything that breaks the stream), and the tempo
     * held from them. steady_bpm survives a stop, so a restart at the same
     * tempo reports it at once. */
    unsigned long long tick_at[TRANSPORT_BPM_WINDOW + 1];
    int ring_len, ring_head;
    int settled;                      /* snapped to a FULL window since the last reset */
    double steady_bpm;                /* 0 = not measured yet */
} transport_source_state_t;

static transport_source_state_t g_src[3];
static unsigned long long g_now;
static uint32_t g_sample_rate = 44100;
static unsigned long long g_stale_samples;
/* Last actively-measured tempo + its source, retained after stop. */
static float g_last_bpm;
static int g_last_bpm_source;

static transport_source_state_t *transport_active(int *which);

/* Anything that breaks the tick stream (start, continue, stop, staleness, a
 * clock picked up mid-song) breaks the window: an interval across a gap is
 * not a tempo. steady_bpm is kept. */
static void steady_reset(transport_source_state_t *s) {
    s->ring_len = 0;
    s->ring_head = 0;
    s->settled = 0;
}

/* Least-squares samples-per-tick over the NEWEST n ticks of the window. A fit
 * rather than (newest - oldest) / ticks: with only the ends, ONE late tick at
 * either end shifts the estimate by a whole block over the span; in a fit it
 * carries ~1/n of that. Times are taken relative to the oldest so the doubles
 * stay small. */
static double steady_fit(const transport_source_state_t *s, int n) {
    const int cap = TRANSPORT_BPM_WINDOW + 1;
    int idx = (s->ring_head + cap - n) % cap;
    const unsigned long long t0 = s->tick_at[idx];
    /* One pass: the index sums are closed-form, so only the time sums are
     * accumulated. This runs on the SPI callback, once per tick. */
    double st = 0.0, sit = 0.0;
    for (int k = 0; k < n; k++) {
        const double t = (double)(s->tick_at[idx] - t0);
        st += t;
        sit += k * t;
        if (++idx == cap) idx = 0;
    }
    const double si = n * (n - 1) / 2.0;
    const double sii = (double)(n - 1) * n * (2 * n - 1) / 6.0;
    return (n * sit - si * st) / (n * sii - si * si);
}

/* Does a tempo (as samples per tick, over n ticks) disagree with the held one
 * by more than n ticks can be wrong? */
static int steady_disagrees(const transport_source_state_t *s, double spt, int n) {
    const double bpm = (60.0 * g_sample_rate) / (spt * TRANSPORT_PPQN);
    const double deadband = TRANSPORT_BPM_DEADBAND_BLOCKS * TRANSPORT_TICK_QUANTUM / (spt * (n - 1));
    return fabs(bpm - s->steady_bpm) > deadband * s->steady_bpm;
}

static void steady_on_tick(transport_source_state_t *s) {
    s->tick_at[s->ring_head] = g_now;
    s->ring_head = (s->ring_head + 1) % (TRANSPORT_BPM_WINDOW + 1);
    if (s->ring_len < TRANSPORT_BPM_WINDOW + 1) s->ring_len++;
    const int beat = TRANSPORT_BPM_MIN_TICKS + 1;
    if (s->ring_len < beat) return;
    /* A REAL CHANGE: the last beat on its own disagrees with the held tempo by
     * more than one beat can be wrong. The ticks before it belong to the old
     * tempo, so the window restarts there -- the new tempo is then taken
     * within about a beat instead of glided to across the whole window, which
     * on a delay is that many beats of smeared pitch. */
    if (s->steady_bpm > 0.0 && s->ring_len > beat) {
        const double spt_beat = steady_fit(s, beat);
        if (spt_beat > 0.0 && steady_disagrees(s, spt_beat, beat)) {
            s->ring_len = beat;
            s->settled = 0;
        }
    }
    const double spt = steady_fit(s, s->ring_len);
    if (!(spt > 0.0)) return;
    const double bpm = (60.0 * g_sample_rate) / (spt * TRANSPORT_PPQN);
    if (bpm < 20.0 || bpm > 999.0) return;
    /* While the window fills, the held value only has to be as good as the
     * window so far, and the deadband shrinks as it grows -- so it can lag the
     * fit by a little more than a FULL window's deadband, and cross it at some
     * arbitrary later tick: one stray step, minutes in. Snapping to the fit
     * once, when the window first fills, puts the held value within the fit's
     * own error, which the deadband exceeds; nothing moves it after that but a
     * real change. */
    const int full = s->ring_len == TRANSPORT_BPM_WINDOW + 1;
    if (s->steady_bpm <= 0.0 || steady_disagrees(s, spt, s->ring_len) || (full && !s->settled))
        s->steady_bpm = bpm;
    if (full) s->settled = 1;
    /* Retained here as well as per block: a Stop can follow the very tick
     * that measured the tempo, before another block runs. */
    int which = TRANSPORT_SRC_NONE;
    if (transport_active(&which) == s) {
        g_last_bpm = (float)s->steady_bpm;
        g_last_bpm_source = which;
    }
}

void shadow_transport_init(uint32_t sample_rate) {
    for (int i = 0; i < 3; i++) {
        g_src[i].running = 0;
        g_src[i].awaiting_first_tick = 0;
        g_src[i].tick_count = 0;
        g_src[i].last_tick_at = 0;
        g_src[i].tick_interval = 0.0;
        steady_reset(&g_src[i]);
        g_src[i].steady_bpm = 0.0;
    }
    g_now = 0;
    g_sample_rate = sample_rate ? sample_rate : 44100;
    g_stale_samples = (unsigned long long)(TRANSPORT_STALE_SEC * g_sample_rate);
    g_last_bpm = 0.0f;
    g_last_bpm_source = TRANSPORT_SRC_NONE;
}

void shadow_transport_advance_block(int frames) {
    if (frames > 0) g_now += (unsigned long long)frames;
    /* Staleness safety net: Move normally sends 0xFC on stop, but a wedged
     * sender must not leave LFOs frozen on a dead beat position. */
    for (int i = 1; i < 3; i++) {
        if (g_src[i].running && g_src[i].last_tick_at &&
            g_now - g_src[i].last_tick_at > g_stale_samples) {
            g_src[i].running = 0;
            steady_reset(&g_src[i]);
        }
    }
    /* Capture the active transport's tempo each block; it survives stop so a
     * free-running LFO keeps the rate it was locked to. */
    int which = TRANSPORT_SRC_NONE;
    transport_source_state_t *a = transport_active(&which);
    if (a && a->steady_bpm > 0.0) {
        g_last_bpm = (float)a->steady_bpm;
        g_last_bpm_source = which;
    }
}

void shadow_transport_on_realtime(transport_src_t src, uint8_t status) {
    if (src != TRANSPORT_SRC_MOVE && src != TRANSPORT_SRC_INTERNAL) return;
    transport_source_state_t *s = &g_src[src];
    switch (status) {
    case 0xFA:
        s->running = 1;
        s->awaiting_first_tick = 1;
        s->tick_count = 0;
        s->last_tick_at = g_now;
        steady_reset(s);
        break;
    case 0xFB:
        s->running = 1;
        /* Continue resumes mid-bar (tick_count is kept, unlike 0xFA). Refresh
         * last_tick_at so advance_block's staleness net can't flip us back off
         * before the first post-Continue 0xF8 — that would re-anchor the next
         * tick to beat 0 instead of resuming. (Movy emits only FA/F8/FC, so
         * this hardens the Move-native source, which can send Continue.) */
        s->last_tick_at = g_now;
        steady_reset(s);
        break;
    case 0xFC:
        s->running = 0;
        steady_reset(s);
        break;
    case 0xF8:
        if (!s->running) {
            /* Clock without Start (we attached mid-song): run unanchored so
             * the tempo is right; bar alignment arrives with the next 0xFA. */
            s->running = 1;
            s->awaiting_first_tick = 1;
            steady_reset(s);
        }
        if (s->awaiting_first_tick) {
            s->awaiting_first_tick = 0;
            s->tick_count = 0;
        } else {
            s->tick_count++;
            double delta = (double)(g_now - s->last_tick_at);
            /* Accept only intervals inside 20-999 BPM at 24 PPQN. */
            double min_d = (60.0 * g_sample_rate) / (999.0 * TRANSPORT_PPQN);
            double max_d = (60.0 * g_sample_rate) / (20.0 * TRANSPORT_PPQN);
            if (delta >= min_d && delta <= max_d) {
                s->tick_interval = (s->tick_interval <= 0.0)
                    ? delta
                    : s->tick_interval + TRANSPORT_EMA_ALPHA * (delta - s->tick_interval);
            }
        }
        s->last_tick_at = g_now;
        steady_on_tick(s);
        break;
    default:
        break;
    }
}

static transport_source_state_t *transport_active(int *which) {
    if (g_src[TRANSPORT_SRC_MOVE].running) {
        if (which) *which = TRANSPORT_SRC_MOVE;
        return &g_src[TRANSPORT_SRC_MOVE];
    }
    if (g_src[TRANSPORT_SRC_INTERNAL].running) {
        if (which) *which = TRANSPORT_SRC_INTERNAL;
        return &g_src[TRANSPORT_SRC_INTERNAL];
    }
    if (which) *which = TRANSPORT_SRC_NONE;
    return 0;
}

double shadow_transport_beat_position(void) {
    transport_source_state_t *s = transport_active(0);
    if (!s) return -1.0;
    double frac = 0.0;
    if (s->tick_interval > 0.0) {
        frac = (double)(g_now - s->last_tick_at) / s->tick_interval;
        /* Never run past the next expected tick: a late tick freezes phase
         * instead of overshooting and snapping back. */
        if (frac > 1.0) frac = 1.0;
        if (frac < 0.0) frac = 0.0;
    }
    return ((double)s->tick_count + frac) / (double)TRANSPORT_PPQN;
}

float shadow_transport_bpm(void) {
    transport_source_state_t *s = transport_active(0);
    if (!s || s->steady_bpm <= 0.0) return 0.0f;
    return (float)s->steady_bpm;
}

int shadow_transport_source(void) {
    int which = TRANSPORT_SRC_NONE;
    (void)transport_active(&which);
    return which;
}

float shadow_transport_last_bpm(void) {
    return g_last_bpm;
}

int shadow_transport_last_source(void) {
    return g_last_bpm_source;
}
