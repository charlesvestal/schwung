/* lfo_common.h - Shared LFO types, waveforms, and division table
 * Used by both chain_host.c (slot LFOs) and shadow_chain_mgmt.c (master FX LFOs). */

#ifndef LFO_COMMON_H
#define LFO_COMMON_H

#include <math.h>

/* ============================================================================
 * LFO Constants
 * ============================================================================ */

#define LFO_COUNT 2
#define LFO_NUM_DIVISIONS 14
#define LFO_SHAPE_SINE   0
#define LFO_SHAPE_TRI    1
#define LFO_SHAPE_SAW    2
#define LFO_SHAPE_SQUARE 3
#define LFO_SHAPE_SH     4
#define LFO_SHAPE_SWISHY 5
#define LFO_NUM_SHAPES   6

/* ============================================================================
 * LFO State
 * ============================================================================ */

typedef struct {
    int enabled;          /* User toggle (on/off) */
    int active;           /* enabled AND has valid target */
    int shape;            /* LFO_SHAPE_* */
    float rate_hz;        /* Free-running rate (0.1-20.0 Hz) */
    int rate_div;         /* Tempo-synced division index */
    int sync;             /* 0=free, 1=tempo-sync */
    float depth;          /* 0.0-1.0 */
    float phase_offset;   /* 0.0-1.0 (displayed as 0-360 degrees) */
    char target[16];      /* Component key (e.g. "synth", "fx1", "fx2") */
    char param[32];       /* Parameter key within target */
    double phase;         /* 0.0-1.0, accumulates each render_block */
    float last_sh_value;  /* Held value for S&H shape */
    int prev_wrap;        /* Phase wrapped last tick (for S&H trigger) */
    float drunk_start;    /* Swishy: interpolation start value */
    float drunk_target;   /* Swishy: interpolation target value */
    int drunk_init;       /* Swishy: initialized flag */
    int retrigger;        /* Reset phase on first note-on of new phrase */
    int held_count;       /* Number of currently held notes (for retrigger) */
} lfo_state_t;

/* Process a MIDI message for retrigger: reset phase on first note-on of a phrase */
static inline void lfo_process_midi(lfo_state_t *lfos, const uint8_t *msg, int len) {
    if (len < 3) return;
    uint8_t status = msg[0] & 0xF0;
    if (status == 0x90 && msg[2] > 0) {
        /* Note on */
        for (int i = 0; i < LFO_COUNT; i++) {
            if (lfos[i].retrigger && lfos[i].held_count == 0) {
                lfos[i].phase = 0.0;
            }
            lfos[i].held_count++;
        }
    } else if (status == 0x80 || (status == 0x90 && msg[2] == 0)) {
        /* Note off */
        for (int i = 0; i < LFO_COUNT; i++) {
            if (lfos[i].held_count > 0) lfos[i].held_count--;
        }
    }
}

/* ============================================================================
 * Division Table
 * ============================================================================ */

typedef struct {
    const char *label;
    float beats;
} lfo_division_t;

static const lfo_division_t lfo_divisions[LFO_NUM_DIVISIONS] = {
    { "8bar", 32.0f  },
    { "4bar", 16.0f  },
    { "2bar",  8.0f  },
    { "1/1",   4.0f  },
    { "1/2",   2.0f  },
    { "1/4",   1.0f  },
    { "1/8",   0.5f  },
    { "1/16",  0.25f },
    { "1/32",  0.125f },
    { "1/4T",  1.333f },
    { "1/8T",  0.667f },
    { "1/16T", 0.333f },
    { "1/4D",  1.5f  },
    { "1/8D",  0.75f },
};

static const char *lfo_shape_names[LFO_NUM_SHAPES] = {
    "sine", "tri", "saw", "square", "s&h", "swishy"
};

/* ============================================================================
 * Waveform Computation
 * ============================================================================ */

/* Shared RNG for S&H and Swishy */
static inline float lfo_rand_bipolar(void) {
    static unsigned int lfo_rng_state = 12345;
    lfo_rng_state = lfo_rng_state * 1103515245 + 12345;
    return ((float)(lfo_rng_state >> 16) / 32768.0f) - 1.0f;
}

/* Compute LFO waveform from phase (0.0-1.0), returns bipolar (-1.0 to +1.0) */
static inline float lfo_compute_shape(int shape, double phase, lfo_state_t *st) {
    switch (shape) {
    case LFO_SHAPE_SINE:
        return sinf((float)(phase * 2.0 * M_PI));
    case LFO_SHAPE_TRI:
        if (phase < 0.25) return (float)(phase * 4.0);
        if (phase < 0.75) return (float)(1.0 - (phase - 0.25) * 4.0);
        return (float)(-1.0 + (phase - 0.75) * 4.0);
    case LFO_SHAPE_SAW:
        return (float)(phase * 2.0 - 1.0);
    case LFO_SHAPE_SQUARE:
        return phase < 0.5 ? 1.0f : -1.0f;
    case LFO_SHAPE_SH: {
        int wrapped = (phase < 0.05 && st->prev_wrap == 0);
        if (wrapped || st->last_sh_value == 0.0f) {
            st->last_sh_value = lfo_rand_bipolar();
        }
        st->prev_wrap = (phase < 0.05) ? 1 : 0;
        return st->last_sh_value;
    }
    case LFO_SHAPE_SWISHY: {
        /* Random walk: smoothly interpolate from start to target each cycle */
        if (!st->drunk_init) {
            st->drunk_start = 0.0f;
            st->drunk_target = lfo_rand_bipolar();
            st->drunk_init = 1;
        }
        int wrapped = (phase < 0.05 && st->prev_wrap == 0);
        if (wrapped) {
            st->drunk_start = st->drunk_target;
            float step = lfo_rand_bipolar() * 0.5f;
            st->drunk_target = st->drunk_target + step;
            if (st->drunk_target > 1.0f) st->drunk_target = 1.0f;
            if (st->drunk_target < -1.0f) st->drunk_target = -1.0f;
        }
        st->prev_wrap = (phase < 0.05) ? 1 : 0;
        return st->drunk_start + (st->drunk_target - st->drunk_start) * (float)phase;
    }
    default:
        return 0.0f;
    }
}

/* ============================================================================
 * Phase Helpers
 * ============================================================================ */

/* Compute rate in Hz for a synced LFO given BPM and division index */
static inline float lfo_sync_rate_hz(float bpm, int rate_div) {
    if (bpm < 20.0f) bpm = 120.0f;
    if (rate_div < 0) rate_div = 0;
    if (rate_div >= LFO_NUM_DIVISIONS) rate_div = LFO_NUM_DIVISIONS - 1;
    float beats = lfo_divisions[rate_div].beats;
    return (bpm / 60.0f) / beats;
}

/* Advance LFO phase by one block, return new phase (wraps at 1.0) */
static inline double lfo_advance_phase(double phase, float rate_hz, int frames, float sample_rate) {
    phase += (double)rate_hz * (double)frames / (double)sample_rate;
    if (phase >= 1.0) phase -= (int)phase;
    return phase;
}

#endif /* LFO_COMMON_H */
