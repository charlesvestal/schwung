/*
 * 11025 -> 44100 Hz, exactly 4x, for the openevv (Eloquence) backend.
 *
 * Header-only so tests/host can measure it without the engine.
 *
 * Why this exists: openevv synthesises at 11025 Hz whatever it is asked for
 * and raises anything higher with a 192-tap windowed sinc evaluated in
 * double precision, per output sample, on the engine's own thread
 * (libs/openevv/src/eci/sound/eci_pcm.c). Measured on x86 that conversion
 * cost ELEVEN times the speech itself: 262 ms for a sentence the engine
 * synthesises in 22 ms at its native rate. Everything it costs sits in
 * front of the first sample and inside every interruption, so the backend
 * now asks for 11025 and upsamples here.
 *
 * A fixed ratio is a polyphase FIR, not a resampler: the prototype lowpass
 * is split into four phases of TTS_UP4_PHASE_TAPS each, and every output
 * sample is one float dot product over the newest input samples -- no
 * per-tap weight interpolation, no division, contiguous memory. The
 * coefficients are designed ONCE (tts_up4_design, off the audio path).
 *
 * Tuned to keep what the engine's sinc keeps: its report was that cutting
 * the 5.0-5.5 kHz band (where Eloquence still has speech, ~27 dB down) is
 * audible as a duller voice, so the passband runs flat to ~5 kHz, and the
 * images of the 11025 stream are held >= 60 dB down.
 * tests/host/test_tts_openevv.c holds both numbers.
 */

#ifndef TTS_UPSAMPLE4_H
#define TTS_UPSAMPLE4_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define TTS_UP4_PHASE_TAPS 96                       /* input samples per output */
#define TTS_UP4_TAPS (4 * TTS_UP4_PHASE_TAPS)       /* prototype length, output rate */
#define TTS_UP4_CUTOFF 0.955                        /* of the INPUT's Nyquist */
#define TTS_UP4_BETA 7.0                            /* Kaiser: ~70 dB stopband */

typedef struct {
    float coef[4][TTS_UP4_PHASE_TAPS];
} tts_up4_filter_t;

typedef struct {
    /* Doubled so the newest TTS_UP4_PHASE_TAPS samples are always contiguous
     * at hist[pos ..], newest first, with no wrap inside the dot product. */
    float hist[2 * TTS_UP4_PHASE_TAPS];
    int pos;
} tts_up4_state_t;

static inline double tts_up4_i0(double x) {
    double term = 1.0, sum = 1.0, half = x / 2.0;
    for (int k = 1; k < 64; k++) {
        term *= (half / k) * (half / k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

/* Not realtime-safe (transcendentals over TTS_UP4_TAPS points): call once. */
static inline void tts_up4_design(tts_up4_filter_t *f) {
    const double c = (TTS_UP4_TAPS - 1) / 2.0;
    const double edge = tts_up4_i0(TTS_UP4_BETA);
    double proto[TTS_UP4_TAPS];

    for (int n = 0; n < TTS_UP4_TAPS; n++) {
        double t = (n - c) / 4.0;                   /* in input samples */
        double x = (n - c) / c;
        double w = tts_up4_i0(TTS_UP4_BETA * sqrt(1.0 - x * x)) / edge;
        double a = 3.14159265358979323846 * TTS_UP4_CUTOFF * t;
        proto[n] = w * (t == 0.0 ? TTS_UP4_CUTOFF : TTS_UP4_CUTOFF * sin(a) / a);
    }
    /* Output m*4+p reads input m-j through proto[p + 4j]. Each phase is
     * normalised to unity on its own: a phase that summed differently from
     * its neighbours would put a tone at 11025 Hz under steady input. */
    for (int p = 0; p < 4; p++) {
        double sum = 0.0;
        for (int j = 0; j < TTS_UP4_PHASE_TAPS; j++) sum += proto[p + 4 * j];
        for (int j = 0; j < TTS_UP4_PHASE_TAPS; j++)
            f->coef[p][j] = (float)(proto[p + 4 * j] / sum);
    }
}

static inline void tts_up4_reset(tts_up4_state_t *s) {
    memset(s, 0, sizeof(*s));
}

static inline int16_t tts_up4_clamp(float v) {
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return (int16_t)lrintf(v);
}

/* n input samples in, 4*n out. Streaming: chunk boundaries leave no seam. */
static inline void tts_up4_run(const tts_up4_filter_t *f, tts_up4_state_t *s,
                               const int16_t *in, int n, int16_t *out) {
    for (int i = 0; i < n; i++) {
        s->pos = (s->pos == 0) ? TTS_UP4_PHASE_TAPS - 1 : s->pos - 1;
        s->hist[s->pos] = s->hist[s->pos + TTS_UP4_PHASE_TAPS] = (float)in[i];
        const float *h = &s->hist[s->pos];
        for (int p = 0; p < 4; p++) {
            const float *c = f->coef[p];
            float acc = 0.0f;
            for (int j = 0; j < TTS_UP4_PHASE_TAPS; j++) acc += c[j] * h[j];
            out[4 * i + p] = tts_up4_clamp(acc);
        }
    }
}

#endif /* TTS_UPSAMPLE4_H */
