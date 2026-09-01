/*
 * metronome_click.h — beat boundaries and the click voice.
 *
 * Header-only and dependency-free for the same reason as recall_quantize.h:
 * so tests/host can compile and RUN it natively. Its caller lives in
 * shadow_metronome.c, driven from schwung_shim.c, which cannot be built on the
 * dev machine — which is how boundary maths ends up shipped untested, and
 * boundary maths is wrong SILENTLY. A metronome that accents the wrong beat
 * does not crash; it just feels wrong forever.
 *
 * Pure: no allocation, no I/O, no globals. The call site is the SPI callback.
 */
#ifndef METRONOME_CLICK_H
#define METRONOME_CLICK_H

#include <math.h>

#include "transport_grid.h"

/* Both of these are properties of shadow_transport_pulses, not of the
 * metronome — including the fact that the downbeat sits at 24N+1, which
 * quantized recall gets wrong in exactly the same way. The reasoning and the
 * hardware measurement live in transport_grid.h, next to the constants, so the
 * two consumers cannot drift. */
#define METRONOME_PULSES_PER_BEAT TRANSPORT_PULSES_PER_BEAT

#define METRONOME_DEFAULT_BEATS_PER_BAR 4

/*
 * Did a beat boundary fall in (prev, now]?
 *
 * Returns the beat's index within the bar — 0 is the downbeat — or -1 if no
 * boundary was crossed.
 *
 * `now < prev` means the transport restarted (MIDI Start zeroed the counter)
 * between the two samples. That is a DOWNBEAT, not a missed beat: reporting -1
 * there would swallow the first click of every take, which is the one click
 * that matters most.
 *
 * When several boundaries fall inside one block the LATEST is reported, once.
 * At 128 frames (2.9 ms) that needs a tempo above 10,000 BPM to happen, but
 * reporting one boundary per call is what keeps the caller a simple trigger
 * rather than a queue.
 */
static inline int metronome_beat_crossed(int prev_pulses, int now_pulses,
                                         int beats_per_bar)
{
    if (beats_per_bar <= 0) beats_per_bar = METRONOME_DEFAULT_BEATS_PER_BAR;
    if (now_pulses < 0) return -1;

    /* No previous sample. NOT a crossing: without a prior position there is
     * nothing to have crossed, and returning a downbeat here would fire a
     * click the moment the feature is switched on mid-bar. The caller guards
     * this too, and the guard belongs in both places — a pure function that
     * invents a beat from a sentinel is a trap for the next caller. */
    if (prev_pulses < 0) return -1;

    if (now_pulses < prev_pulses) {
        /* Transport restarted (MIDI Start zeroed the counter). NOT a beat yet:
         * the downbeat is the FIRST CLOCK after Start, so it arrives on the
         * next call, at pulse 1, and the grid maths below catches it there.
         * Clicking here instead would fire one pulse early on every take —
         * the very error this offset exists to remove. */
        return -1;
    }
    if (now_pulses == prev_pulses) return -1;

    const int div = METRONOME_PULSES_PER_BEAT;
    /* Shift onto the beat grid: pulse 1 is the downbeat, so pulse
     * TRANSPORT_DOWNBEAT_PULSE_OFFSET maps to grid position 0. Both ends shift, so
     * crossing detection is unchanged — only where the boundaries sit moves. */
    int p_prev = prev_pulses - TRANSPORT_DOWNBEAT_PULSE_OFFSET;
    int p_now  = now_pulses  - TRANSPORT_DOWNBEAT_PULSE_OFFSET;
    if (p_now < 0) return -1;

    /* The greatest multiple of div in (p_prev, p_now]. */
    int last = (p_now / div) * div;
    if (last <= p_prev) return -1;

    int beat = (last / div) % beats_per_bar;
    return beat;
}

/* ------------------------------------------------------------------ voice */

/*
 * A decaying sine. Deliberately not a sample: a click has to be generated on
 * the SPI callback with no file I/O and no allocation, and two multiplications
 * per frame is cheaper than a table that has to be loaded, owned and freed.
 */
typedef struct {
    float phase;      /* radians */
    float phase_inc;  /* radians per sample */
    float amp;        /* current amplitude, 0 = idle */
    float decay;      /* per-sample multiplier, < 1 */
} metronome_voice_t;

/* Downbeat sits a fifth above the offbeat, the way an acoustic click does. */
#define METRONOME_FREQ_DOWNBEAT_HZ 1500.0f
#define METRONOME_FREQ_BEAT_HZ     1000.0f
#define METRONOME_DECAY_SECONDS    0.030f

/* Amplitude below which the voice is treated as silent and stops mixing. */
#define METRONOME_SILENCE_EPS 0.0001f

static inline void metronome_voice_trigger(metronome_voice_t *v, float freq_hz,
                                           float amp, float decay_s, float sr)
{
    if (!v) return;
    if (!(sr > 0.0f)) sr = 44100.0f;
    if (!(decay_s > 0.0f)) decay_s = METRONOME_DECAY_SECONDS;
    v->phase = 0.0f;
    v->phase_inc = 2.0f * (float)M_PI * freq_hz / sr;
    v->amp = amp;
    /* Reach -60 dB in decay_s. */
    v->decay = expf(-6.907755f / (decay_s * sr));
}

/* Next sample, in -1..1. Returns exactly 0.0f once the voice has decayed out,
 * so the caller can skip the mix entirely on an idle block. */
static inline float metronome_voice_next(metronome_voice_t *v)
{
    if (!v) return 0.0f;
    if (v->amp <= METRONOME_SILENCE_EPS) { v->amp = 0.0f; return 0.0f; }
    float s = sinf(v->phase) * v->amp;
    v->phase += v->phase_inc;
    if (v->phase > 2.0f * (float)M_PI) v->phase -= 2.0f * (float)M_PI;
    v->amp *= v->decay;
    return s;
}

/* ---------------------------------------------------------------- pending */

/*
 * Advance a pending-click countdown across one block of `frames`.
 *
 * `*pending` is frames remaining before the click sounds, or negative for
 * "nothing pending". Returns the frame offset WITHIN THIS BLOCK at which the
 * click starts, or -1 if it does not start in this block. `*pending` is
 * updated either way.
 *
 * A countdown rather than a delay ring: the click is generated, not delayed
 * audio, so postponing the TRIGGER costs one int instead of a 1400-sample
 * buffer, and lands the attack on an exact sample rather than a block edge.
 */
static inline int metronome_pending_advance(int *pending, int frames)
{
    if (!pending || *pending < 0) return -1;
    if (frames <= 0) return -1;
    if (*pending < frames) { int off = *pending; *pending = -1; return off; }
    *pending -= frames;
    return -1;
}

static inline int metronome_voice_active(const metronome_voice_t *v)
{
    return v && v->amp > METRONOME_SILENCE_EPS;
}

#endif /* METRONOME_CLICK_H */
