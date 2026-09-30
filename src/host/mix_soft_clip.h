/* mix_soft_clip.h — the ONE place a summed mix becomes int16.
 *
 * Under Move->Schwung the shim rebuilds the whole master from Move's four
 * Link Audio tracks, the slots, the overtake DSP and the send returns. It used
 * to sum them straight into the int16 mailbox, clamping after EVERY add, and
 * a busy set went over full scale several times a second: measured 2026-09-30,
 * 3272 hard-clipped samples in 30 s (131 separate overs), each one a pop, with
 * every stage feeding the sum clean. The overs landed on a 100%-wet CloudSeed
 * return stacking onto the rest, which is why they were heard "in the reverb".
 *
 * Two defects, fixed together:
 *
 *  1. A per-add clamp is ORDER-DEPENDENT. A sum that goes over and comes back
 *     (a positive slot, then a negative track) keeps the clipped intermediate,
 *     so the result is wrong even when the true total fits. The rebuild now
 *     accumulates in int32 and converts once.
 *
 *  2. A hard clamp is a click. The single conversion goes through the curve
 *     below instead: IDENTITY up to the knee, so anything that fits today is
 *     bit-for-bit unchanged, then a tanh shoulder that approaches full scale
 *     without reaching it. Value and slope are both continuous at the knee
 *     (tanh'(0) = 1), so there is no corner for an over to ring off.
 *
 * Header-only and allocation-free: runs on the SPI callback. tanhf is paid
 * only by samples above the knee.
 */
#ifndef MIX_SOFT_CLIP_H
#define MIX_SOFT_CLIP_H

#include <stdint.h>
#include <math.h>

/* -1 dBFS. Below this the curve is the identity. */
#define MIX_SOFT_CLIP_KNEE 29204

static inline int16_t mix_soft_clip(int32_t v)
{
    if (v <= MIX_SOFT_CLIP_KNEE && v >= -MIX_SOFT_CLIP_KNEE) return (int16_t)v;
    const float knee = (float)MIX_SOFT_CLIP_KNEE;
    const float room = 32767.0f - knee;
    const float a = (float)(v < 0 ? -(int64_t)v : (int64_t)v);
    float y = knee + room * tanhf((a - knee) / room);
    int32_t q = (int32_t)lroundf(y);
    if (q > 32767) q = 32767;          /* tanh rounds to 1.0f far out */
    return (int16_t)(v < 0 ? -q : q);
}

/* n is SAMPLES (frames * 2). */
static inline void mix_soft_clip_block(const int32_t *src, int16_t *dst, int n)
{
    for (int i = 0; i < n; i++) dst[i] = mix_soft_clip(src[i]);
}

#endif /* MIX_SOFT_CLIP_H */
