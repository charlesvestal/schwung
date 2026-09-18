/*
 * playhead_anchor.h — a phase for a clip that has never been launched.
 *
 * WHY THIS EXISTS. Starting a loop from an EMPTY CLIP is the normal way to
 * work on Move, and until now automation recorded into one could not be heard
 * for about three loops. Measured on hardware 2026-09-15, from `lanes_trace`:
 *
 *     f=127024  ph=nan     row=-2  pend=1  drv=0   <- 4.7 s with no phase
 *     f=128656  ph=3.0417  row=0   pend=0  drv=0   <- Song.abl lands
 *     f=133552  ph=2.2917  row=0   pend=0  drv=1   <- first playback
 *
 * The lock lands instantly; it cannot PLAY, because playback needs a phase and
 * a phase needs an anchor. The anchor normally comes from a ch-9 ON in Move's
 * LED stream -- a clip PLAYING -- and a clip you just made in the step editor
 * has never played. So there is nothing to anchor to until Move writes the
 * file, ~8-12 s later.
 *
 * MOVE IS ALREADY TELLING US WHERE IT IS. The step playhead (a step note with
 * d2=126) is emitted in Note view, which is exactly where this flow happens,
 * and clip_state.h already documents the relationship it relies on for its
 * phase CHECK:
 *
 *     playhead_idx == floor(pos_in_loop / step_resolution) mod 16
 *
 * Read backwards, that is an anchor: a playhead at step `idx` observed at
 * pulse P means the clip was at `idx * step_resolution` beats then, so its
 * step 0 was `idx * step_resolution * 24` pulses earlier. The check becomes a
 * source.
 *
 * ONE PAGE ONLY, and that restriction is the whole reason this is safe. The
 * relation is `mod 16` -- the index is the step within whatever page is
 * DISPLAYED -- so on a multi-page clip an index of 3 could be beat 0.75 of any
 * page and there is no honest way to choose. A clip just created is one page,
 * which is the case this exists for; anything longer waits for the file, as it
 * does today. Refusing is correct there, not a gap.
 *
 * Pure: no allocation, no I/O, no globals. Callers run on the SPI callback.
 */
#ifndef PLAYHEAD_ANCHOR_H
#define PLAYHEAD_ANCHOR_H

#include <math.h>
#include <stdint.h>

/* Move's clock: 24 pulses per quarter note. Named rather than spelled, because
 * clip_state.c's own phase maths uses the same 24 and the two must agree. */
#define PLAYHEAD_PULSES_PER_BEAT 24.0

/* How stale a playhead observation may be and still anchor, in pulses.
 * 48 = two beats. The playhead is re-emitted every step while the transport
 * runs (six pulses at 1/16), so a live one is never near this; anything older
 * means playback stopped, and an anchor from before a stop places the clip
 * wherever it would have been had it kept running -- confidently wrong.
 */
#define PLAYHEAD_ANCHOR_MAX_AGE_PULSES 48u

/*
 * Derive the clip phase now, from the last step-playhead observation.
 *
 *   idx          the lit step, 0..15
 *   ev_pulses    shadow_transport_pulses when it was seen
 *   now_pulses   shadow_transport_pulses now
 *   step_res     beats per step (clip_regions_t.step_resolution)
 *   loop_len     the clip's length in beats (from Move's bar strip)
 *   segments     pages the strip reports; MUST be 1, see above
 *
 * Returns 1 and writes *out_phase (loop-relative, 0 <= phase < loop_len), or
 * 0 for "cannot tell" -- which every caller must treat as no phase at all
 * rather than as phase zero. A clip sitting at 0.0 and a clip we cannot place
 * are the same number and must not be the same decision.
 */
static inline int playhead_phase_now(uint8_t idx, uint32_t ev_pulses,
                                     uint32_t now_pulses, double step_res,
                                     double loop_len, int segments,
                                     double *out_phase)
{
    if (!out_phase) return 0;
    if (segments != 1) return 0;                 /* multi-page: ambiguous */
    if (idx > 15) return 0;
    if (!isfinite(step_res) || step_res <= 0.0) return 0;
    if (!isfinite(loop_len) || loop_len <= 0.0) return 0;
    /* Time only runs forward here. A `now` before the observation means the
     * transport was restarted, which resets the pulse counter -- the anchor
     * belongs to a timeline that no longer exists. */
    if (now_pulses < ev_pulses) return 0;
    if (now_pulses - ev_pulses > PLAYHEAD_ANCHOR_MAX_AGE_PULSES) return 0;

    const double at_event = (double)idx * step_res;
    /* A playhead index past the end of the loop is not this clip's -- the
     * editor can display sixteen steps for a clip shorter than sixteen. */
    if (at_event >= loop_len) return 0;

    const double elapsed =
        (double)(now_pulses - ev_pulses) / PLAYHEAD_PULSES_PER_BEAT;
    double phase = at_event + elapsed;
    phase = fmod(phase, loop_len);
    if (!isfinite(phase)) return 0;
    if (phase < 0.0) phase = 0.0;
    *out_phase = phase;
    return 1;
}

#endif /* PLAYHEAD_ANCHOR_H */
