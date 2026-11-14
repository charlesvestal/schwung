/*
 * speaker_eq.c — MoveSpeakerEnhancer emulation and its jack rules.
 * Moved unchanged from schwung_shim.c; see speaker_eq.h for why.
 *
 * ----- RBJ biquad (direct form I transposed) for speaker-EQ compensation -----
 * Approximates the MoveSpeakerEnhancer response derived from on-device white-noise
 * and log-sweep measurements (2026-04-18). Applied only on the rebuild_from_la
 * DAC output path so captures/headphones stay neutral. Coefficients are computed
 * once at startup — no per-frame allocations.
 *
 * Jack-state robustness for the speaker EQ (2026-06-02).
 * The EQ must NEVER color headphone/line-out output. speaker_active by
 * itself is untrustworthy at boot and at song/set load: XMOS can broadcast a
 * transient or uncorrected CC 115 val=0 ("speaker") while headphones are
 * actually plugged, latching the EQ onto the headphone DAC path (the
 * hollow/distorted bug) until a physical replug sends a corrective val=127.
 *
 * Fix: bias hard toward EQ-OFF (per user: "less bass on speakers" is far
 * better than "hollow audio on headphones"). Only trust a speaker reading
 * (CC 115 val=0) if we have already seen a jack-inserted reading (val=127)
 * earlier this session — i.e. a genuine headphones→speaker transition while
 * running. A bare boot/song-load val=0 (no prior val=127) is NOT trusted, so
 * the EQ stays off. This needs no timers and has no timing hole: it does not
 * matter WHEN the stray val=0 arrives.
 *
 * Trade-off: a device that only ever uses the built-in speaker (never inserts
 * a jack) runs without the enhancer EQ until a jack is plugged+unplugged once
 * — no longer a practical concern now that the boot jack re-assert restores
 * the true state automatically. This whole paragraph describes AUTO, which is
 * the default; Global Settings -> Audio -> Spkr EQ can force it Off or On
 * (shadow_control_t.speaker_eq_mode), which is the escape for a device whose
 * jack reading is simply wrong in one direction or the other.
 *
 * Auto-mode stability: engage the EQ only when the jack has read speaker
 * (CC 115 val=0) continuously for SPEAKER_EQ_STABLE_SEC. A val=127 (jack
 * inserted) flips us out of speaker instantly. This rejects transients and
 * contact bounce (e.g. the 127→0→127 we observed on replug) and a transient
 * boot val=0 that's quickly corrected, while still restoring the EQ for a
 * genuine built-in-speaker session. A truly stuck wrong val=0 would still
 * engage after the window — that case is what mode "off" is for.
 */
/* clock_gettime under -std=c11 (the host unit tests build strict C). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "speaker_eq.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int speaker_eq_speaker_stable_at(const speaker_eq_t *eq, const struct timespec *now)
{
    if (!(eq->speaker_active && eq->speaker_known)) return 0;
    double el = (double)(now->tv_sec - eq->speaker_since.tv_sec)
              + (double)(now->tv_nsec - eq->speaker_since.tv_nsec) / 1e9;
    return el >= SPEAKER_EQ_STABLE_SEC;
}

int speaker_eq_speaker_stable(const speaker_eq_t *eq)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return speaker_eq_speaker_stable_at(eq, &now);
}

void speaker_eq_restart_clock(speaker_eq_t *eq)
{
    clock_gettime(CLOCK_MONOTONIC, &eq->speaker_since);
}

int speaker_eq_on_jack(speaker_eq_t *eq, uint8_t cc115_value, int mark_known)
{
    int new_speaker = (cc115_value == 0) ? 1 : 0;  /* val=0 → speaker active; val=127 → jack inserted */
    int prev_known_speaker = eq->speaker_active && eq->speaker_known;
    int changed = 0;
    if (mark_known) eq->speaker_known = 1;
    if (new_speaker != eq->speaker_active) {
        eq->speaker_active = new_speaker;
        /* Reset filter state on output switch to avoid thump */
        memset(eq->eq_state, 0, sizeof(eq->eq_state));
        changed = 1;
    }
    /* Auto-mode stability: restart the speaker stability clock on a new
     * entry into the speaker state, so only a stable speaker reading
     * engages the EQ (transients/bounce do not). */
    if (new_speaker && !prev_known_speaker) {
        speaker_eq_restart_clock(eq);
    }
    return changed;
}

static inline float waveshaper_poly(float x)
{
    /* y = c1*x + c2*x^2 + c3*x^3 + c4*x^4 + c5*x^5 using Horner's method. */
    return x * (2.4f + x * (-1.2f + x * (-5.6f + x * (1.2f + x * 4.48f))));
}

static void biquad_hp(speaker_eq_biquad_t *f, float fs, float fc, float q)
{
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * q);
    float b0 = (1.0f + cw) * 0.5f;
    float b1 = -(1.0f + cw);
    float b2 = (1.0f + cw) * 0.5f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cw;
    float a2 = 1.0f - alpha;
    f->b0 = b0 / a0; f->b1 = b1 / a0; f->b2 = b2 / a0;
    f->a1 = a1 / a0; f->a2 = a2 / a0;
}

static inline void biquad_assign(speaker_eq_biquad_t *f, float b0, float b1, float b2, float a1, float a2)
{
    f->b0 = b0; f->b1 = b1; f->b2 = b2; f->a1 = a1; f->a2 = a2;
}

void speaker_eq_init(speaker_eq_t *eq, float fs)
{
    /* Coefficients copied verbatim from live MoveOriginal DSP state memory
     * (2026-04-18). SpeakerEq = Set 1 (4 biquads at 0x5592ef8060+0x0c onwards),
     * Bandpass = Set 2 (4 biquads at 0x5592ef80e0 onwards). The cascades
     * produce the measured speaker-voicing curve (-7 dB @ 50, +3.6 dB @ 200,
     * -4.6 dB @ 800, +3 dB @ 16k) and a steep bandpass at 131-200 Hz for the
     * harmonic exciter path.
     *
     * Signal flow per sample:
     *   1. hb = crossover_hp(x)           high band (> 203 Hz)
     *   2. bp = bandpass_cascade(x)       131-200 Hz extracted
     *   3. wsbp = waveshaper(bp/norm)*norm  harmonic exciter
     *   4. mix = hb + 1.365 × wsbp
     *   5. out = speaker_eq_cascade(mix)  final speaker tuning */
    (void)fs;

    /* SpeakerEq cascade (from 0x5592ef8060+0x0c) — format (b0,b1,b2,a1,a2) */
    biquad_assign(&eq->eq_coefs[0], 0.992062628f, -1.98412526f,  0.992062628f, -1.98406231f, 0.984188318f);
    biquad_assign(&eq->eq_coefs[1], 1.01622748f,  -1.9452281f,   0.92980653f,  -1.9452281f,  0.946034133f);
    biquad_assign(&eq->eq_coefs[2], 0.962782085f, -1.83911681f,  0.888346255f, -1.83911681f, 0.851128399f);
    biquad_assign(&eq->eq_coefs[3], 1.33173597f,  -1.80474436f,  0.611439228f, -1.25587428f, 0.394305021f);

    /* Bandpass cascade (from 0x5592ef80e0) — first 2 LP, then 2 HP (131-200 Hz) */
    biquad_assign(&eq->bandpass_coefs[0], 0.000200790499f, 0.000401580997f, 0.000200790499f, -1.97762573f,  0.9784289f);
    biquad_assign(&eq->bandpass_coefs[1], 0.000197773843f, 0.000395547686f, 0.000197773843f, -1.94791412f,  0.948705196f);
    biquad_assign(&eq->bandpass_coefs[2], 0.992822111f,   -1.98564422f,    0.992822111f,    -1.98547125f,  0.985817075f);
    biquad_assign(&eq->bandpass_coefs[3], 0.982964098f,   -1.9659282f,     0.982964098f,    -1.96575701f,  0.966099381f);

    /* Upstream subsonic HP (Move's master bus processing, not in MoveSpeakerEnhancer itself
     * but in the Move→DAC chain we're matching). Numerically fit to digital resample of
     * pink noise through Move's native path: fc=135 Hz, Q=1.5 matches observed curve with
     * 1.5 dB RMS error across 40 Hz–8 kHz. */
    biquad_hp(&eq->subsonic_hp_coefs, 44100.0f, 135.0f, 1.5f);
    biquad_hp(&eq->crossover_hp_coefs, 44100.0f, 203.0f, 0.707f); /* unused but kept for future */

    memset(eq->eq_state, 0, sizeof(eq->eq_state));
    memset(eq->bandpass_state, 0, sizeof(eq->bandpass_state));
    memset(eq->subsonic_hp_state, 0, sizeof(eq->subsonic_hp_state));
    memset(eq->crossover_hp_state, 0, sizeof(eq->crossover_hp_state));
    eq->initialized = 1;
}

/* Process one sample through a single biquad (transposed direct form II). */
static inline float biquad_step(speaker_eq_biquad_t *c, speaker_eq_biquad_t *st, float x)
{
    float y = c->b0 * x + st->z1;
    st->z1 = c->b1 * x - c->a1 * y + st->z2;
    st->z2 = c->b2 * x - c->a2 * y;
    return y;
}

/* MoveSpeakerEnhancer emulation using exact biquad coefficients + exact polynomial
 * waveshaper extracted from live DSP state. SpeakerEq cascade already contains
 * a HP component (Biquad 1 ≈ HP @ 85 Hz) that handles the LowBandVolume=0
 * sub-bass cut — no separate crossover HP needed. */
void speaker_eq_process(speaker_eq_t *eq, int16_t *audio, int frames)
{
    const float proc_band_volume = 1.365f;
    const float inv_norm = 1.0f / 32768.0f;
    const float norm = 32768.0f;
    for (int i = 0; i < frames; i++) {
        for (int ch = 0; ch < SPEAKER_EQ_NUM_CHANNELS; ch++) {
            float x = (float)audio[i * 2 + ch];

            /* Bandpass cascade (131-200 Hz via 4-biquad cascade from DSP state) */
            float bp = x;
            for (int s = 0; s < SPEAKER_EQ_BANDPASS_STAGES; s++) {
                bp = biquad_step(&eq->bandpass_coefs[s], &eq->bandpass_state[s][ch], bp);
            }
            /* Waveshape in normalized [-1, 1] range, generates 4th/5th harmonics */
            float bpn = bp * inv_norm;
            if (bpn > 1.0f) bpn = 1.0f;
            if (bpn < -1.0f) bpn = -1.0f;
            float wsbp = waveshaper_poly(bpn) * norm;

            /* Apply upstream subsonic HP to main signal (Move's master-bus filter;
             * not part of MoveSpeakerEnhancer itself but sits in the actual
             * Move→DAC path we're matching). */
            float x_sub = biquad_step(&eq->subsonic_hp_coefs, &eq->subsonic_hp_state[ch], x);

            /* Mix original signal + processed band (× ProcessedBandVolume) */
            float mix = x_sub + proc_band_volume * wsbp;

            /* Apply SpeakerEq cascade (4 biquads from DSP state) */
            float out = mix;
            for (int s = 0; s < SPEAKER_EQ_NUM_STAGES; s++) {
                out = biquad_step(&eq->eq_coefs[s], &eq->eq_state[s][ch], out);
            }

            if (out > 32767.0f) out = 32767.0f;
            if (out < -32768.0f) out = -32768.0f;
            audio[i * 2 + ch] = (int16_t)lroundf(out);
        }
    }
}

