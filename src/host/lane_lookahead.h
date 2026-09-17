/*
 * lane_lookahead.h — how far AHEAD of the transport a lane is evaluated.
 *
 * Inside one SPI frame the shim renders first and delivers Move's MIDI second
 * (shadow_mix_audio, then shadow_inprocess_process_midi, both in
 * shim_pre_transfer). `clip_phase_beats` has not advanced to the step when the
 * render runs, so a p-lock whose rectangle starts exactly on a step was
 * applied ONE BLOCK AFTER the note for that step reached the synth. A drum
 * voice latches its pitch at note-on, so it read the value the lock was meant
 * to replace, and the lock appeared on the NEXT hit.
 *
 * Measured 2026-09-17 rather than assumed: Move's notes were stamped arriving
 * at ph = 0.000000, 1.000000, 1.500000, 3.000000 — EXACTLY on the boundaries.
 * There is no timing lag to compensate for, which is why this is one block of
 * lead and not a tuned constant.
 *
 * The lead is the phase travelled since the previous tick, REMEMBERED rather
 * than computed from tempo: this code does not own a BPM, and a derived one
 * would be wrong the moment the clock changed.
 *
 * Extracted so tests/host can drive it — the same reason recall_quantize.h and
 * chain_idle_tick.h exist.
 */
#ifndef LANE_LOOKAHEAD_H
#define LANE_LOOKAHEAD_H

#include <math.h>

#include "lane_store.h"   /* LANE_LOOKAHEAD_MAX_BEATS */

/*
 * The phase a lane's VALUE should be read at this tick.
 *
 * `prev_valid` 0 (first tick, or the phase was lost) yields no lead at all,
 * which is exactly the old behaviour for that one tick — late, never wrong.
 *
 * A WRAP gives a negative delta and a RE-ANCHOR can give a large one. Both are
 * refused rather than clamped: a bad lead would play the NEXT step's lock on
 * this step, which is the same defect this fixes, pointing the other way.
 */
static inline double lane_lookahead_phase(double phase, double prev,
                                          int prev_valid,
                                          double loop_start, double loop_len)
{
    if (!isfinite(phase) || !(loop_len > 0.0)) return phase;

    double out = phase;
    if (prev_valid && isfinite(prev)) {
        const double d = phase - prev;
        if (d > 0.0 && d < LANE_LOOKAHEAD_MAX_BEATS) out += d;
    }

    /* lane_eval takes a clip-relative phase against a WINDOW and does not
     * wrap, so a lookahead past the loop end would land outside it and answer
     * "nothing to say" — releasing every lane for one block at each loop
     * boundary, a click once a bar. */
    const double hi = loop_start + loop_len;
    if (out >= hi) out -= loop_len;
    return out;
}

#endif /* LANE_LOOKAHEAD_H */
