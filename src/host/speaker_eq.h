/*
 * speaker_eq.h — the built-in speaker's tone, without Move.
 *
 * When Schwung rebuilds the DAC output itself (rebuild_from_la), Move's own
 * MoveSpeakerEnhancer is no longer in the path, so the shim runs this
 * emulation of it instead (coefficients extracted from live DSP state,
 * 2026-04-18). A standalone tool that owns SPI has the same problem and no
 * shim, so the EQ and the CC 115 jack rules that gate it live here, where any
 * host can compile them, instead of as statics inside schwung_shim.c.
 *
 * Not thread-safe: one speaker_eq_t belongs to the thread that renders audio
 * and reads the jack CCs (the SPI callback in the shim). No allocation.
 */
#ifndef SPEAKER_EQ_H
#define SPEAKER_EQ_H

#include <stdint.h>
#include <time.h>

#define SPEAKER_EQ_NUM_STAGES 4      /* SpeakerEq 4-biquad cascade */
#define SPEAKER_EQ_BANDPASS_STAGES 4 /* Processed-band 4-biquad cascade */
#define SPEAKER_EQ_NUM_CHANNELS 2

/* Auto-mode stability window: see speaker_eq_speaker_stable(). */
#define SPEAKER_EQ_STABLE_SEC 3.0

typedef struct {
    float b0, b1, b2, a1, a2;    /* normalized biquad coefficients */
    float z1, z2;                /* state (per channel) */
} speaker_eq_biquad_t;

typedef struct {
    speaker_eq_biquad_t eq_coefs[SPEAKER_EQ_NUM_STAGES];
    speaker_eq_biquad_t eq_state[SPEAKER_EQ_NUM_STAGES][SPEAKER_EQ_NUM_CHANNELS];
    speaker_eq_biquad_t bandpass_coefs[SPEAKER_EQ_BANDPASS_STAGES];
    speaker_eq_biquad_t bandpass_state[SPEAKER_EQ_BANDPASS_STAGES][SPEAKER_EQ_NUM_CHANNELS];
    speaker_eq_biquad_t subsonic_hp_coefs;
    speaker_eq_biquad_t subsonic_hp_state[SPEAKER_EQ_NUM_CHANNELS];
    speaker_eq_biquad_t crossover_hp_coefs;
    speaker_eq_biquad_t crossover_hp_state[SPEAKER_EQ_NUM_CHANNELS];
    int initialized;

    /* Jack state from CC 115 (cable 0): 1 = built-in speaker, 0 = headphones
     * or line out. Starts as a GUESS of 1 that is not trusted until known. */
    int speaker_active;
    int speaker_known;               /* 1 once any CC 115 has been observed */
    struct timespec speaker_since;   /* when speaker state last became true */
} speaker_eq_t;

/* Static initializer: speaker assumed, nothing known, filters not built. */
#define SPEAKER_EQ_INIT { .speaker_active = 1 }

/* Build the coefficients (fs is accepted for the API; the extracted
 * coefficients are for Move's 44.1 kHz) and clear the filter state.
 * Leaves the jack state alone. */
void speaker_eq_init(speaker_eq_t *eq, float fs);

/* Run the EQ in place over interleaved stereo int16. Clips to int16. */
void speaker_eq_process(speaker_eq_t *eq, int16_t *audio, int frames);

/* Start the auto-mode stability clock now (the shim does this at boot). */
void speaker_eq_restart_clock(speaker_eq_t *eq);

/* Feed one CC 115 value (0 = speaker, 127 = jack inserted). mark_known says
 * whether this reading counts as having observed the jack: the shim's early,
 * ungated scan says yes; its later gated handler says no, as it always has.
 * Resets the EQ state on an output switch (no thump) and restarts the
 * stability clock on a new entry into the speaker state.
 * Returns 1 if speaker_active changed, so the caller can mirror it. */
int speaker_eq_on_jack(speaker_eq_t *eq, uint8_t cc115_value, int mark_known);

/* Auto mode: engage only when a KNOWN speaker reading has held for
 * SPEAKER_EQ_STABLE_SEC. The EQ must never colour headphone or line-out
 * output, and XMOS can send a transient CC 115 val=0 at boot or Set load
 * with headphones plugged, so a bare reading is not enough. The _at form
 * takes the monotonic time, for tests. */
int speaker_eq_speaker_stable(const speaker_eq_t *eq);
int speaker_eq_speaker_stable_at(const speaker_eq_t *eq, const struct timespec *now);

#endif /* SPEAKER_EQ_H */
