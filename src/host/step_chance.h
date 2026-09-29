/*
 * Step Chance: Elektron-style trig conditions for a step's notes.
 *
 * One list, walked by the jog in the step menu (hold a step, press Menu):
 *   index 0          100%   -- always; the default, and what no entry means
 *   index 1..21      99% .. 1%, Elektron's own ladder, strictly descending
 *   index 22..       A:B, B-major (1:2 2:2 1:3 2:3 3:3 1:4 ... 8:8)
 *
 * A percentage rolls; A:B is deterministic -- it plays on pass A of every B
 * passes of the clip's loop, counting the first pass after launch as pass 0
 * (i.e. "1" of 1:B). The pass is DERIVED from the live model's clock, never
 * counted here, so it cannot drift and restarts with the clip.
 *
 * Pure and header-only so tests/host can run it (schwung_shim.c cannot be
 * built on the dev machine). Safe on the SPI callback: no allocation, no I/O.
 */
#ifndef STEP_CHANCE_H
#define STEP_CHANCE_H

#include <stdint.h>
#include <stdio.h>
#include <math.h>

#define SC_ALWAYS 0
#define SC_N_PERCENT 21
#define SC_MAX_B 8

static const uint8_t sc_percent_ladder[SC_N_PERCENT] = {
    99, 98, 96, 94, 91, 87, 81, 75, 67, 59, 50, 41, 33, 25, 19, 13, 9, 6, 4, 3, 1
};

/* 1 + 21 + (2+3+...+8) */
static inline int sc_count(void)
{
    return 1 + SC_N_PERCENT + (SC_MAX_B * (SC_MAX_B + 1) / 2 - 1);
}

static inline int sc_valid(int idx) { return idx >= 0 && idx < sc_count(); }

/* The percentage an index plays at; -1 for an A:B index. Out of range is
 * 100: an index stored by a build with a longer list must not silence a note. */
static inline int sc_percent(int idx)
{
    if (!sc_valid(idx) || idx == SC_ALWAYS) return 100;
    if (idx <= SC_N_PERCENT) return sc_percent_ladder[idx - 1];
    return -1;
}

/* 1 and A/B filled for an A:B index, else 0. */
static inline int sc_ratio(int idx, int *a, int *b)
{
    if (!sc_valid(idx) || idx <= SC_N_PERCENT) return 0;
    int k = idx - (SC_N_PERCENT + 1);
    for (int bb = 2; bb <= SC_MAX_B; bb++) {
        if (k < bb) { if (a) *a = k + 1; if (b) *b = bb; return 1; }
        k -= bb;
    }
    return 0;
}

static inline void sc_name(int idx, char *buf, size_t n)
{
    int a, b;
    if (sc_ratio(idx, &a, &b)) snprintf(buf, n, "%d:%d", a, b);
    else snprintf(buf, n, "%d%%", sc_percent(idx));
}

/* Does a note under condition `idx` play on loop pass `pass`, given a uniform
 * 32-bit random number? A percentage rolls against the FULL 32-bit range; a
 * ratio ignores `rnd`. An unknown pass (< 0) plays -- a note is never dropped
 * for want of a clock. */
static inline int sc_should_play(int idx, long pass, uint32_t rnd)
{
    int a, b;
    if (sc_ratio(idx, &a, &b)) {
        if (pass < 0) return 1;
        return (pass % b) == (a - 1);
    }
    int p = sc_percent(idx);
    if (p >= 100) return 1;
    return ((uint64_t)rnd * 100u) >> 32 < (uint64_t)p;
}

/* Which pass of the loop is `beats_since_launch` in? -1 when unknown. A clock
 * a hair short of a boundary (a note ON the downbeat of pass N, read a few
 * nanobeats early) belongs to pass N, not N-1. */
#define SC_PASS_EPS 1e-6
static inline long sc_pass_index(double beats_since_launch, double loop_len)
{
    if (!(loop_len > 0.0) || !(beats_since_launch >= 0.0)) return -1;
    return (long)floor((beats_since_launch + SC_PASS_EPS) / loop_len);
}

/* The loop pass a clip is on, from Move's launch beat and the transport --
 * the same arithmetic as mm_clip_position, without the wrap. The FIRST pass
 * runs from the region start to the loop end (longer than one loop when the
 * clip starts before its loop), then every pass is one loop long. -1 when
 * unknown or when an unlooped clip has ended. */
static inline long sc_clip_pass(double region_start, double loop_start, double loop_end,
                                int loop_on, double start_beats, double now)
{
    if (!(now >= start_beats)) return -1;
    const double pos = region_start + (now - start_beats);
    if (!loop_on) return 0;
    const double len = loop_end - loop_start;
    if (!(len > 1e-9)) return -1;
    if (pos + SC_PASS_EPS < loop_end) return 0;
    return 1 + sc_pass_index(pos - loop_end, len);
}

#endif /* STEP_CHANCE_H */
