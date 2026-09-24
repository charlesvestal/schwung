/*
 * master_filter.h -- the one-knob master filter (a "DJ filter").
 *
 * One value, x in [-1, 1]: left of centre a low-pass closing from 20 kHz to
 * 60 Hz, right of centre a high-pass opening from 20 Hz to 8 kHz, mild fixed
 * resonance. Driven from Master FX Settings and the E16 Mixer.
 *
 * TRANSPARENT AT CENTRE, BY CONSTRUCTION. Inside the dead band the filter is
 * not running at all -- the buffer is returned untouched, bit for bit -- not a
 * filter parked at 20 kHz, which still shifts phase. Leaving the band fades
 * the filtered signal in over RAMP_MS; returning fades it out, and only once
 * the fade reaches zero does it stop and forget its state. Crossing straight
 * from low-pass to high-pass fades out, resets, and fades back in, so the
 * filter never switches type with signal on it.
 *
 * Pure C on int16 stereo interleaved, no allocation, no I/O: it runs on the
 * SPI callback. One instance per buffer (the DAC mailbox and the capture view
 * each keep their own state).
 */
#ifndef MASTER_FILTER_H
#define MASTER_FILTER_H

#include <math.h>
#include <stdint.h>

#define MASTER_FILTER_DEADBAND  0.02f
#define MASTER_FILTER_RAMP_MS   10.0f
#define MASTER_FILTER_Q         0.9f
#define MASTER_FILTER_LP_TOP    20000.0f
#define MASTER_FILTER_LP_BOTTOM 60.0f
#define MASTER_FILTER_HP_BOTTOM 20.0f
#define MASTER_FILTER_HP_TOP    8000.0f

typedef struct {
    float ic1[2], ic2[2];  /* TPT SVF state, per channel */
    float mix;             /* 0 = dry (not running) .. 1 = fully filtered */
    int mode;              /* -1 low-pass, +1 high-pass, 0 none */
    float x;               /* smoothed position within the running mode */
} master_filter_t;

static inline int master_filter_mode_of(float x) {
    if (x < -MASTER_FILTER_DEADBAND) return -1;
    if (x > MASTER_FILTER_DEADBAND) return 1;
    return 0;
}

/* Cutoff (Hz) for a position x within its mode. */
static inline float master_filter_cutoff(float x) {
    float t = (fabsf(x) - MASTER_FILTER_DEADBAND) / (1.0f - MASTER_FILTER_DEADBAND);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    if (x < 0.0f)
        return MASTER_FILTER_LP_TOP * powf(MASTER_FILTER_LP_BOTTOM / MASTER_FILTER_LP_TOP, t);
    return MASTER_FILTER_HP_BOTTOM * powf(MASTER_FILTER_HP_TOP / MASTER_FILTER_HP_BOTTOM, t);
}

static inline void master_filter_reset(master_filter_t *f) {
    f->ic1[0] = f->ic1[1] = f->ic2[0] = f->ic2[1] = 0.0f;
    f->mix = 0.0f;
    f->mode = 0;
    f->x = 0.0f;
}

/* Process one block in place. `target` is the knob, [-1, 1]. */
static inline void master_filter_process(master_filter_t *f, int16_t *buf, int frames,
                                         float target, float sample_rate) {
    if (!(target >= -1.0f)) target = -1.0f;   /* also catches NaN */
    if (target > 1.0f) target = 1.0f;
    const int want = master_filter_mode_of(target);

    /* Idle and asked to stay idle: untouched, bit-exact. */
    if (f->mix <= 0.0f && want == 0) {
        if (f->mode != 0) master_filter_reset(f);
        return;
    }
    /* Starting from rest: take the requested mode, from a clean state. */
    if (f->mix <= 0.0f && f->mode != want) {
        master_filter_reset(f);
        f->mode = want;
        f->x = target;
    }
    /* Fade toward 1 only while running the requested mode; toward 0 when the
     * request is centre OR the other mode (it switches once at rest). */
    const float mix_target = (want != 0 && want == f->mode) ? 1.0f : 0.0f;
    if (want == f->mode) f->x += (target - f->x) * 0.3f;   /* per-block smoothing */

    const float fc = master_filter_cutoff(f->x);
    float wc = (float)M_PI * fc / sample_rate;
    if (wc > 1.5f) wc = 1.5f;                             /* stay below Nyquist */
    const float g = tanf(wc), k = 1.0f / MASTER_FILTER_Q;
    const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const float step = 1.0f / (MASTER_FILTER_RAMP_MS * 0.001f * sample_rate);

    for (int i = 0; i < frames; i++) {
        if (f->mix < mix_target) { f->mix += step; if (f->mix > mix_target) f->mix = mix_target; }
        else if (f->mix > mix_target) { f->mix -= step; if (f->mix < mix_target) f->mix = mix_target; }
        for (int c = 0; c < 2; c++) {
            const float v0 = (float)buf[2 * i + c];
            const float v3 = v0 - f->ic2[c];
            const float v1 = a1 * f->ic1[c] + a2 * v3;
            const float v2 = f->ic2[c] + a2 * f->ic1[c] + a3 * v3;
            f->ic1[c] = 2.0f * v1 - f->ic1[c];
            f->ic2[c] = 2.0f * v2 - f->ic2[c];
            const float wet = (f->mode < 0) ? v2 : (v0 - k * v1 - v2);
            float out = v0 + f->mix * (wet - v0);
            if (out > 32767.0f) out = 32767.0f;
            if (out < -32768.0f) out = -32768.0f;
            buf[2 * i + c] = (int16_t)lrintf(out);
        }
    }
    /* Faded out completely: stop, and forget, so the next start is clean and
     * the idle path above is bit-exact again. */
    if (f->mix <= 0.0f) master_filter_reset(f);
}

#endif /* MASTER_FILTER_H */
