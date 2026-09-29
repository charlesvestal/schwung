/* link_audio_conceal.h — zero-latency repair of Move's Link Audio tracks.
 *
 * Two problems, measured 2026-09-29 on 1.5.0 with a set playing:
 *
 * 1. A STARVED BLOCK WAS SILENCE. Move's publisher stalls now and then --
 *    14.6 ms with a 6-block burst behind it, against a normal ~5 ms -- and a
 *    track holding ~15 ms runs dry for one 2.9 ms block. The reader returned
 *    "no data", the rebuild mixed zeros for that track, and a hard cut to
 *    silence and back is a click. The fix is NOT a deeper reserve: that is
 *    latency on Move's tracks, and 13 ms of it was rejected outright.
 *    Instead the starved block is CONCEALED: the last block played, mirrored
 *    in time so its first sample IS the last sample already heard, faded to
 *    zero across the block; the next real block fades back in. A ~3 ms dip on
 *    one track instead of an edge. Past LA_CONCEAL_MAX_BLOCKS consecutive
 *    starves it gives up and reports the starve, so a real outage (a set
 *    change) still reaches the all-starve fallback in the shim.
 *
 * 2. THE TRACKS SAT AT DIFFERENT DEPTHS. Nothing chooses a track's reserve;
 *    it is wherever startup left it, and the backlog trim only ever cuts a
 *    track DOWN to its 29 ms target. Measured: track 2 at 29 ms (trimmed after
 *    a startup backlog), tracks 1/3/4 at ~16 ms. So track 2 played ~13 ms
 *    LATE against the other three -- a flam on anything spanning them -- and
 *    carried 13 ms of latency for nothing. The aligner skips a deeper track
 *    forward to the shallowest one, crossfaded over one block. It only ever
 *    REMOVES latency; it never adds any.
 *
 * Header-only and I/O-free: both run on the SPI callback, and tests/host
 * drives them directly (test_link_audio_conceal.c).
 */
#ifndef LINK_AUDIO_CONCEAL_H
#define LINK_AUDIO_CONCEAL_H

#include <stdint.h>

/* One block of stereo-interleaved samples: 128 frames x 2. */
#define LA_CONCEAL_BLOCK_SAMPLES  256

/* Consecutive starved blocks concealed before giving up: ~11.6 ms. The first
 * is the faded mirror, the rest are silence -- still "valid", so a short stall
 * on EVERY track does not bounce the mix to the non-rebuild path and back. */
#define LA_CONCEAL_MAX_BLOCKS     4

/* A track is only aligned when it sits more than one block deeper than the
 * shallowest track: all four are written by one sidecar callback, so their
 * depths move together and a difference inside a block is read jitter. */
#define LA_ALIGN_SLACK_SAMPLES    LA_CONCEAL_BLOCK_SAMPLES

/* ...and only once that has held for a second of reads (128 frames at
 * 44.1 kHz), so a burst landing between two slots' reads never triggers it. */
#define LA_ALIGN_SUSTAIN_READS    345

typedef struct {
    int16_t  last[LA_CONCEAL_BLOCK_SAMPLES];  /* last block handed out */
    int      have_last;
    uint32_t run;       /* consecutive concealed blocks */
    int      fade_in;   /* the next real block starts from silence */
} la_conceal_t;

static inline void la_conceal_reset(la_conceal_t *c)
{
    c->have_last = 0;
    c->run = 0;
    c->fade_in = 0;
}

/*
 * A read starved. Fill `out` (frames x 2 samples) with a concealment block and
 * return 1, or return 0 when there is nothing to conceal with or the stall has
 * outlasted LA_CONCEAL_MAX_BLOCKS -- the caller then treats it as a starve.
 */
static inline int la_conceal_fill(la_conceal_t *c, int16_t *out, int frames)
{
    int n = frames * 2;
    if (frames <= 0 || n > LA_CONCEAL_BLOCK_SAMPLES) return 0;
    c->fade_in = 1;   /* whatever happens now, the recovery starts from zero */
    if (!c->have_last) return 0;
    if (c->run >= LA_CONCEAL_MAX_BLOCKS) return 0;

    if (c->run == 0) {
        /* Time-mirrored: frame 0 is the last frame heard, so there is no
         * step at the join. Gain falls from 1 to ~0 across the block. */
        for (int f = 0; f < frames; f++) {
            int src = (frames - 1 - f) * 2;
            int32_t g_num = frames - f;       /* frames .. 1 */
            out[f * 2]     = (int16_t)((int32_t)c->last[src]     * g_num / frames);
            out[f * 2 + 1] = (int16_t)((int32_t)c->last[src + 1] * g_num / frames);
        }
    } else {
        for (int i = 0; i < n; i++) out[i] = 0;
    }
    c->run++;
    return 1;
}

/* A real block was read into `out`. Fade it in if a concealment (or a
 * starve) preceded it, and remember it for the next concealment. */
static inline void la_conceal_real(la_conceal_t *c, int16_t *out, int frames)
{
    int n = frames * 2;
    if (frames <= 0 || n > LA_CONCEAL_BLOCK_SAMPLES) return;
    if (c->fade_in) {
        for (int f = 0; f < frames; f++) {
            int32_t g_num = f + 1;            /* 1 .. frames */
            out[f * 2]     = (int16_t)((int32_t)out[f * 2]     * g_num / frames);
            out[f * 2 + 1] = (int16_t)((int32_t)out[f * 2 + 1] * g_num / frames);
        }
        c->fade_in = 0;
    }
    for (int i = 0; i < n; i++) c->last[i] = out[i];
    c->have_last = 1;
    c->run = 0;
}

/*
 * How far to skip slot `s` forward to line it up with the shallowest active
 * slot, in samples (always even: one stereo frame is two samples). 0 until the
 * excess has exceeded the slack for LA_ALIGN_SUSTAIN_READS consecutive calls;
 * `run` is the caller's per-slot counter and is maintained here.
 */
static inline uint32_t la_align_skip(const uint32_t *avail, const int *active,
                                     int n, int s, uint32_t *run)
{
    if (s < 0 || s >= n || !active[s]) { *run = 0; return 0; }
    uint32_t mn = UINT32_MAX;
    int peers = 0;
    for (int i = 0; i < n; i++) {
        if (!active[i]) continue;
        peers++;
        if (avail[i] < mn) mn = avail[i];
    }
    if (peers < 2 || avail[s] <= mn || avail[s] - mn <= LA_ALIGN_SLACK_SAMPLES) {
        *run = 0;
        return 0;
    }
    if (++(*run) < LA_ALIGN_SUSTAIN_READS) return 0;
    *run = 0;
    return (avail[s] - mn) & ~1u;
}

/* Crossfade the block at the old read position (`from`) into the block at the
 * skipped-to position (`to`), so the jump is a one-block blend, not a step. */
static inline void la_align_crossfade(const int16_t *from, const int16_t *to,
                                      int16_t *out, int frames)
{
    for (int f = 0; f < frames; f++) {
        int32_t b = f + 1, a = frames - b;   /* weights sum to `frames` */
        out[f * 2]     = (int16_t)(((int32_t)from[f * 2]     * a + (int32_t)to[f * 2]     * b) / frames);
        out[f * 2 + 1] = (int16_t)(((int32_t)from[f * 2 + 1] * a + (int32_t)to[f * 2 + 1] * b) / frames);
    }
}

#endif /* LINK_AUDIO_CONCEAL_H */
