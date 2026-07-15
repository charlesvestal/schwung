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

#define TRANSPORT_PPQN 24
#define TRANSPORT_STALE_SEC 0.5
/* EMA weight: converges in ~10 ticks while absorbing the ±1-block (~2.9 ms)
 * jitter of block-quantized clock senders. */
#define TRANSPORT_EMA_ALPHA 0.25

typedef struct {
    int running;
    int awaiting_first_tick;  /* set by 0xFA; the next 0xF8 is tick 0 */
    unsigned long long tick_count;
    unsigned long long last_tick_at;  /* sample time of last 0xF8 */
    double tick_interval;             /* EMA, samples per tick; 0 = unknown */
} transport_source_state_t;

static transport_source_state_t g_src[3];
static unsigned long long g_now;
static uint32_t g_sample_rate = 44100;
static unsigned long long g_stale_samples;
/* Last actively-measured tempo + its source, retained after stop. */
static float g_last_bpm;
static int g_last_bpm_source;

static transport_source_state_t *transport_active(int *which);

void shadow_transport_init(uint32_t sample_rate) {
    for (int i = 0; i < 3; i++) {
        g_src[i].running = 0;
        g_src[i].awaiting_first_tick = 0;
        g_src[i].tick_count = 0;
        g_src[i].last_tick_at = 0;
        g_src[i].tick_interval = 0.0;
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
        }
    }
    /* Capture the active transport's tempo each block; it survives stop so a
     * free-running LFO keeps the rate it was locked to. */
    int which = TRANSPORT_SRC_NONE;
    transport_source_state_t *a = transport_active(&which);
    if (a && a->tick_interval > 0.0) {
        g_last_bpm = (float)((60.0 * g_sample_rate) / (a->tick_interval * TRANSPORT_PPQN));
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
        break;
    case 0xFB:
        s->running = 1;
        /* Continue resumes mid-bar (tick_count is kept, unlike 0xFA). Refresh
         * last_tick_at so advance_block's staleness net can't flip us back off
         * before the first post-Continue 0xF8 — that would re-anchor the next
         * tick to beat 0 instead of resuming. (Movy emits only FA/F8/FC, so
         * this hardens the Move-native source, which can send Continue.) */
        s->last_tick_at = g_now;
        break;
    case 0xFC:
        s->running = 0;
        break;
    case 0xF8:
        if (!s->running) {
            /* Clock without Start (we attached mid-song): run unanchored so
             * the tempo is right; bar alignment arrives with the next 0xFA. */
            s->running = 1;
            s->awaiting_first_tick = 1;
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
    if (!s || s->tick_interval <= 0.0) return 0.0f;
    return (float)((60.0 * g_sample_rate) / (s->tick_interval * TRANSPORT_PPQN));
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
