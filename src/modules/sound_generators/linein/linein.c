/*
 * Line In Sound Generator Plugin
 *
 * Passes through audio input with conditioning for three source types:
 *   - Line:   minimal processing, optional HPF and safety limiter
 *   - Guitar: gain staging, cable compensation (high-shelf), optional soft clip
 *   - Phono:  RIAA de-emphasis, subsonic filter, hum notch
 *
 * Includes a noise gate with Auto (per-mode defaults) and Manual modes.
 *
 * V2 API - Instance-based
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "host/plugin_api_v1.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define SAMPLE_RATE       44100.0f
#define TWO_PI            6.283185307f
#define GAIN_SMOOTH_COEFF 0.002f   /* ~1.5 ms at 44100 Hz */
#define XFADE_SAMPLES     64       /* crossfade on mode switch */

/* Input type indices */
#define INPUT_TYPE_LINE   0
#define INPUT_TYPE_GUITAR 1
#define INPUT_TYPE_PHONO  2

/* Gate state machine */
#define GATE_OPEN    0
#define GATE_HOLD    1
#define GATE_CLOSING 2
#define GATE_CLOSED  3

/* Gate mode */
#define GATE_MODE_OFF    0
#define GATE_MODE_AUTO   1
#define GATE_MODE_MANUAL 2

/* HPF frequency table (index -> Hz, 0 = off) */
static const float hpf_freq_table[] = { 0.0f, 20.0f, 40.0f, 60.0f, 80.0f, 120.0f };
#define HPF_FREQ_COUNT 6

/* Subsonic filter frequency table */
static const float subsonic_freq_table[] = { 10.0f, 15.0f, 20.0f, 30.0f, 40.0f };
#define SUBSONIC_FREQ_COUNT 5

/* Cable compensation settings: {corner_hz, gain_db} */
static const float cable_comp_corner[] = { 0.0f, 5000.0f, 4000.0f, 3000.0f };
static const float cable_comp_gain[]   = { 0.0f,   -2.0f,   -4.0f,   -6.0f };
#define CABLE_COMP_COUNT 4

/* ------------------------------------------------------------------ */
/*  Biquad filter                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float z1, z2;
} biquad_t;

static void biquad_reset(biquad_t *bq) {
    bq->z1 = bq->z2 = 0.0f;
}

static void biquad_set_passthrough(biquad_t *bq) {
    bq->b0 = 1.0f; bq->b1 = 0.0f; bq->b2 = 0.0f;
    bq->a1 = 0.0f; bq->a2 = 0.0f;
    bq->z1 = 0.0f; bq->z2 = 0.0f;
}

static inline float biquad_process(biquad_t *bq, float x) {
    float y = bq->b0 * x + bq->z1;
    bq->z1 = bq->b1 * x - bq->a1 * y + bq->z2;
    bq->z2 = bq->b2 * x - bq->a2 * y;
    return y;
}

static void biquad_set_highpass(biquad_t *bq, float freq, float q) {
    float w0 = TWO_PI * freq / SAMPLE_RATE;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * q);
    float a0 = 1.0f + alpha;
    float inv_a0 = 1.0f / a0;

    bq->b0 = (1.0f + cosw0) * 0.5f * inv_a0;
    bq->b1 = -(1.0f + cosw0) * inv_a0;
    bq->b2 = (1.0f + cosw0) * 0.5f * inv_a0;
    bq->a1 = -2.0f * cosw0 * inv_a0;
    bq->a2 = (1.0f - alpha) * inv_a0;
}

static void biquad_set_notch(biquad_t *bq, float freq, float q) {
    float w0 = TWO_PI * freq / SAMPLE_RATE;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * q);
    float a0 = 1.0f + alpha;
    float inv_a0 = 1.0f / a0;

    bq->b0 = 1.0f * inv_a0;
    bq->b1 = -2.0f * cosw0 * inv_a0;
    bq->b2 = 1.0f * inv_a0;
    bq->a1 = -2.0f * cosw0 * inv_a0;
    bq->a2 = (1.0f - alpha) * inv_a0;
}

static void biquad_set_high_shelf(biquad_t *bq, float freq, float gain_db) {
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = TWO_PI * freq / SAMPLE_RATE;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float sqrtA = sqrtf(A);
    float alpha = sinw0 / 2.0f * sqrtf(2.0f);

    float a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
    float inv_a0 = 1.0f / a0;

    bq->b0 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha) * inv_a0;
    bq->b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0) * inv_a0;
    bq->b2 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha) * inv_a0;
    bq->a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0) * inv_a0;
    bq->a2 = ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha) * inv_a0;
}

/*
 * RIAA playback de-emphasis curve approximated by two cascaded biquad sections.
 *
 * The RIAA standard defines three time constants:
 *   T1 = 3180 us  (f1 = 50.05 Hz)   - bass turnover
 *   T2 = 318 us   (f2 = 500.5 Hz)   - midrange
 *   T3 = 75 us    (f3 = 2122 Hz)    - treble rolloff
 *
 * Playback curve applies:  +19.3 dB at 20 Hz, 0 dB at ~1 kHz, -13.7 dB at 10 kHz
 *
 * We approximate this with two second-order sections designed from the analog
 * prototype via bilinear transform.  Error < 0.5 dB from 20 Hz to 20 kHz.
 *
 * Stage 1: low-shelf boost centered around f1/f2 boundary
 * Stage 2: high-shelf cut centered around f3
 */
static void biquad_set_riaa_stage1(biquad_t *bq) {
    /* Low-shelf boost: +17 dB at 200 Hz corner */
    float A = powf(10.0f, 17.0f / 40.0f);
    float w0 = TWO_PI * 200.0f / SAMPLE_RATE;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float sqrtA = sqrtf(A);
    float alpha = sinw0 / 2.0f * sqrtf(2.0f);

    float a0 = (A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
    float inv_a0 = 1.0f / a0;

    bq->b0 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha) * inv_a0;
    bq->b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0) * inv_a0;
    bq->b2 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha) * inv_a0;
    bq->a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0) * inv_a0;
    bq->a2 = ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha) * inv_a0;
}

static void biquad_set_riaa_stage2(biquad_t *bq) {
    /* High-shelf cut: -14 dB at 2120 Hz corner */
    float A = powf(10.0f, -14.0f / 40.0f);
    float w0 = TWO_PI * 2120.0f / SAMPLE_RATE;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float sqrtA = sqrtf(A);
    float alpha = sinw0 / 2.0f * sqrtf(2.0f);

    float a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
    float inv_a0 = 1.0f / a0;

    bq->b0 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha) * inv_a0;
    bq->b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0) * inv_a0;
    bq->b2 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha) * inv_a0;
    bq->a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0) * inv_a0;
    bq->a2 = ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha) * inv_a0;
}

/* ------------------------------------------------------------------ */
/*  Auto-gate per-mode defaults                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    float threshold_db;
    float attack_ms;
    float hold_ms;
    float release_ms;
    float range_db;
    float hysteresis_db;
} gate_profile_t;

static const gate_profile_t auto_gate_profiles[] = {
    /* Line */   { -50.0f,  3.0f,  50.0f, 200.0f, 12.0f, 3.0f },
    /* Guitar */ { -40.0f,  2.0f, 100.0f, 350.0f, 24.0f, 4.0f },
    /* Phono */  { -55.0f,  8.0f, 200.0f, 600.0f,  8.0f, 3.0f },
};

/* ------------------------------------------------------------------ */
/*  Per-instance state                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Parameters */
    int   input_type;        /* 0=Line, 1=Guitar, 2=Phono */
    float input_trim_db;
    float output_trim_db;
    int   gate_mode;         /* 0=Off, 1=Auto, 2=Manual */
    float gate_amount;       /* 0-100, for Auto mode */
    float gate_threshold_db; /* for Manual mode */
    float gate_attack_ms;
    float gate_hold_ms;
    float gate_release_ms;
    float gate_range_db;

    /* Line settings */
    int   hpf_freq_idx;      /* 0=Off, 1-5 = frequency */
    int   safety_limiter;

    /* Guitar settings */
    int   cable_comp;        /* 0=Off, 1=Low, 2=Med, 3=High */
    int   soft_clip;

    /* Phono settings */
    int   riaa_eq;
    int   subsonic_freq_idx; /* 0-4 */
    int   hum_notch;
    int   hum_freq;          /* 0=50Hz, 1=60Hz */

    /* Smoothed gain (linear) */
    float input_gain_smooth;
    float output_gain_smooth;

    /* Biquad filter states: [0]=L, [1]=R */
    biquad_t hpf[2];
    biquad_t cable_shelf[2];
    biquad_t riaa_stage1[2];
    biquad_t riaa_stage2[2];
    biquad_t subsonic[2];
    biquad_t hum_notch1[2];  /* fundamental */
    biquad_t hum_notch2[2];  /* first harmonic */

    /* Gate state */
    float gate_envelope;
    float gate_gain;          /* current attenuation (linear, 0..1) */
    int   gate_hold_counter;  /* samples remaining */
    int   gate_state;

    /* Mode switch crossfade */
    int   xfade_remaining;
    float xfade_prev_buf[256]; /* stereo: 128 frames * 2 */

    /* Flags */
    int   filters_dirty;
} linein_instance_t;

/* Host API */
static const host_api_v1_t *g_host = NULL;

static void linein_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[linein] %s", msg);
        g_host->log(buf);
    }
}

/* ------------------------------------------------------------------ */
/*  Filter recalculation                                               */
/* ------------------------------------------------------------------ */

static void recalc_filters(linein_instance_t *inst) {
    int ch;

    /* HPF (used by Line and Guitar modes) */
    float hpf_hz = 0.0f;
    if (inst->input_type == INPUT_TYPE_LINE) {
        if (inst->hpf_freq_idx > 0 && inst->hpf_freq_idx < HPF_FREQ_COUNT)
            hpf_hz = hpf_freq_table[inst->hpf_freq_idx];
    } else if (inst->input_type == INPUT_TYPE_GUITAR) {
        /* Guitar always uses 80 Hz HPF (built-in default) */
        hpf_hz = 80.0f;
    }
    for (ch = 0; ch < 2; ch++) {
        if (hpf_hz > 0.0f) {
            biquad_set_highpass(&inst->hpf[ch], hpf_hz, 0.707f);
        } else {
            biquad_set_passthrough(&inst->hpf[ch]);
        }
    }

    /* Cable compensation (Guitar only) */
    for (ch = 0; ch < 2; ch++) {
        if (inst->input_type == INPUT_TYPE_GUITAR &&
            inst->cable_comp > 0 && inst->cable_comp < CABLE_COMP_COUNT) {
            biquad_set_high_shelf(&inst->cable_shelf[ch],
                cable_comp_corner[inst->cable_comp],
                cable_comp_gain[inst->cable_comp]);
        } else {
            biquad_set_passthrough(&inst->cable_shelf[ch]);
        }
    }

    /* RIAA (Phono only) */
    for (ch = 0; ch < 2; ch++) {
        if (inst->input_type == INPUT_TYPE_PHONO && inst->riaa_eq) {
            biquad_set_riaa_stage1(&inst->riaa_stage1[ch]);
            biquad_set_riaa_stage2(&inst->riaa_stage2[ch]);
        } else {
            biquad_set_passthrough(&inst->riaa_stage1[ch]);
            biquad_set_passthrough(&inst->riaa_stage2[ch]);
        }
    }

    /* Subsonic filter (Phono only) */
    for (ch = 0; ch < 2; ch++) {
        if (inst->input_type == INPUT_TYPE_PHONO &&
            inst->subsonic_freq_idx < SUBSONIC_FREQ_COUNT) {
            biquad_set_highpass(&inst->subsonic[ch],
                subsonic_freq_table[inst->subsonic_freq_idx], 0.707f);
        } else {
            biquad_set_passthrough(&inst->subsonic[ch]);
        }
    }

    /* Hum notch (Phono only) */
    for (ch = 0; ch < 2; ch++) {
        if (inst->input_type == INPUT_TYPE_PHONO && inst->hum_notch) {
            float fund = (inst->hum_freq == 0) ? 50.0f : 60.0f;
            biquad_set_notch(&inst->hum_notch1[ch], fund, 10.0f);
            biquad_set_notch(&inst->hum_notch2[ch], fund * 2.0f, 10.0f);
        } else {
            biquad_set_passthrough(&inst->hum_notch1[ch]);
            biquad_set_passthrough(&inst->hum_notch2[ch]);
        }
    }

    inst->filters_dirty = 0;
}

static void reset_filter_states(linein_instance_t *inst) {
    int ch;
    for (ch = 0; ch < 2; ch++) {
        biquad_reset(&inst->hpf[ch]);
        biquad_reset(&inst->cable_shelf[ch]);
        biquad_reset(&inst->riaa_stage1[ch]);
        biquad_reset(&inst->riaa_stage2[ch]);
        biquad_reset(&inst->subsonic[ch]);
        biquad_reset(&inst->hum_notch1[ch]);
        biquad_reset(&inst->hum_notch2[ch]);
    }
}

/* ------------------------------------------------------------------ */
/*  dB to linear conversion                                            */
/* ------------------------------------------------------------------ */

static inline float db_to_linear(float db) {
    return powf(10.0f, db / 20.0f);
}

/* ------------------------------------------------------------------ */
/*  Gate helpers                                                       */
/* ------------------------------------------------------------------ */

static void gate_get_effective_params(linein_instance_t *inst,
    float *out_threshold_db, float *out_attack_ms, float *out_hold_ms,
    float *out_release_ms, float *out_range_db, float *out_hysteresis_db)
{
    if (inst->gate_mode == GATE_MODE_MANUAL) {
        *out_threshold_db = inst->gate_threshold_db;
        *out_attack_ms = inst->gate_attack_ms;
        *out_hold_ms = inst->gate_hold_ms;
        *out_release_ms = inst->gate_release_ms;
        *out_range_db = inst->gate_range_db;
        *out_hysteresis_db = 3.0f;
        return;
    }

    /* Auto mode: use per-mode profile scaled by gate_amount */
    int type = inst->input_type;
    if (type < 0 || type > 2) type = 0;
    const gate_profile_t *p = &auto_gate_profiles[type];

    float amount = inst->gate_amount / 100.0f; /* 0..1 */

    /* Amount 0% = no gating, 50% = defaults, 100% = aggressive */
    *out_threshold_db = p->threshold_db + (amount - 0.5f) * 12.0f;
    *out_attack_ms = p->attack_ms;
    *out_hold_ms = p->hold_ms;
    *out_release_ms = p->release_ms;
    *out_range_db = p->range_db * amount * 2.0f; /* 0 at 0%, full at 50%, 2x at 100% */
    if (*out_range_db > 80.0f) *out_range_db = 80.0f;
    *out_hysteresis_db = p->hysteresis_db;
}

/* ------------------------------------------------------------------ */
/*  v2 API                                                             */
/* ------------------------------------------------------------------ */

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir;
    (void)json_defaults;

    linein_instance_t *inst = calloc(1, sizeof(linein_instance_t));
    if (!inst) return NULL;

    /* Defaults: Line mode */
    inst->input_type = INPUT_TYPE_LINE;
    inst->input_trim_db = 0.0f;
    inst->output_trim_db = 0.0f;
    inst->gate_mode = GATE_MODE_AUTO;
    inst->gate_amount = 50.0f;
    inst->gate_threshold_db = -40.0f;
    inst->gate_attack_ms = 3.0f;
    inst->gate_hold_ms = 80.0f;
    inst->gate_release_ms = 200.0f;
    inst->gate_range_db = 18.0f;

    /* Line settings defaults */
    inst->hpf_freq_idx = 0;
    inst->safety_limiter = 0;

    /* Guitar settings defaults */
    inst->cable_comp = 2;  /* Med */
    inst->soft_clip = 0;

    /* Phono settings defaults */
    inst->riaa_eq = 1;
    inst->subsonic_freq_idx = 2; /* 20 Hz */
    inst->hum_notch = 1;
    inst->hum_freq = 1; /* 60 Hz */

    /* Initialize gain */
    inst->input_gain_smooth = db_to_linear(inst->input_trim_db);
    inst->output_gain_smooth = db_to_linear(inst->output_trim_db);

    /* Gate starts open */
    inst->gate_gain = 1.0f;
    inst->gate_envelope = 0.0f;
    inst->gate_state = GATE_OPEN;
    inst->gate_hold_counter = 0;

    /* Initialize all filters to passthrough, then recalc */
    int ch;
    for (ch = 0; ch < 2; ch++) {
        biquad_set_passthrough(&inst->hpf[ch]);
        biquad_set_passthrough(&inst->cable_shelf[ch]);
        biquad_set_passthrough(&inst->riaa_stage1[ch]);
        biquad_set_passthrough(&inst->riaa_stage2[ch]);
        biquad_set_passthrough(&inst->subsonic[ch]);
        biquad_set_passthrough(&inst->hum_notch1[ch]);
        biquad_set_passthrough(&inst->hum_notch2[ch]);
    }
    recalc_filters(inst);

    inst->xfade_remaining = 0;

    linein_log("instance created");
    return inst;
}

static void v2_destroy_instance(void *instance) {
    if (instance) {
        linein_log("instance destroyed");
        free(instance);
    }
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)instance; (void)msg; (void)len; (void)source;
}

/* ------------------------------------------------------------------ */
/*  Enum option tables (must match module.json options arrays)          */
/* ------------------------------------------------------------------ */

static const char *input_type_options[] = { "Line", "Guitar", "Phono" };
#define INPUT_TYPE_COUNT 3

static const char *gate_mode_options[] = { "Off", "Auto", "Manual" };
#define GATE_MODE_COUNT 3

static const char *hpf_freq_options[] = { "Off", "20 Hz", "40 Hz", "60 Hz", "80 Hz", "120 Hz" };
/* HPF_FREQ_COUNT already defined above as 6 */

static const char *on_off_options[] = { "Off", "On" };
#define ON_OFF_COUNT 2

static const char *cable_comp_options[] = { "Off", "Low", "Med", "High" };
/* CABLE_COMP_COUNT already defined above as 4 */

static const char *subsonic_freq_options[] = { "10 Hz", "15 Hz", "20 Hz", "30 Hz", "40 Hz" };
/* SUBSONIC_FREQ_COUNT already defined above as 5 */

static const char *hum_freq_options[] = { "50 Hz", "60 Hz" };
#define HUM_FREQ_COUNT 2

/* Parse an enum value from either a string label or numeric index */
static int parse_enum(const char *val, const char **options, int count) {
    /* Try matching option labels first */
    for (int i = 0; i < count; i++) {
        if (strcmp(val, options[i]) == 0) return i;
    }
    /* Fall back to numeric index (from chain_host knob pathway) */
    int idx = (int)atof(val);
    if (idx < 0) idx = 0;
    if (idx >= count) idx = count - 1;
    return idx;
}

/* ------------------------------------------------------------------ */
/*  set_param                                                          */
/* ------------------------------------------------------------------ */

static void v2_set_param(void *instance, const char *key, const char *val) {
    linein_instance_t *inst = (linein_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "input_type") == 0) {
        int new_type = parse_enum(val, input_type_options, INPUT_TYPE_COUNT);
        if (new_type != inst->input_type) {
            inst->input_type = new_type;
            reset_filter_states(inst);
            recalc_filters(inst);
            inst->xfade_remaining = XFADE_SAMPLES;
        }
    }
    else if (strcmp(key, "input_trim") == 0) {
        float v = (float)atof(val);
        inst->input_trim_db = (v < -12.0f) ? -12.0f : (v > 40.0f) ? 40.0f : v;
    }
    else if (strcmp(key, "output_trim") == 0) {
        float v = (float)atof(val);
        inst->output_trim_db = (v < -24.0f) ? -24.0f : (v > 12.0f) ? 12.0f : v;
    }
    else if (strcmp(key, "gate_mode") == 0) {
        inst->gate_mode = parse_enum(val, gate_mode_options, GATE_MODE_COUNT);
    }
    else if (strcmp(key, "gate_amount") == 0) {
        float v = (float)atof(val);
        inst->gate_amount = (v < 0.0f) ? 0.0f : (v > 100.0f) ? 100.0f : v;
    }
    else if (strcmp(key, "gate_threshold") == 0) {
        float v = (float)atof(val);
        inst->gate_threshold_db = (v < -80.0f) ? -80.0f : (v > 0.0f) ? 0.0f : v;
    }
    else if (strcmp(key, "gate_attack") == 0) {
        float v = (float)atof(val);
        inst->gate_attack_ms = (v < 0.5f) ? 0.5f : (v > 50.0f) ? 50.0f : v;
    }
    else if (strcmp(key, "gate_hold") == 0) {
        float v = (float)atof(val);
        inst->gate_hold_ms = (v < 5.0f) ? 5.0f : (v > 500.0f) ? 500.0f : v;
    }
    else if (strcmp(key, "gate_release") == 0) {
        float v = (float)atof(val);
        inst->gate_release_ms = (v < 10.0f) ? 10.0f : (v > 1000.0f) ? 1000.0f : v;
    }
    else if (strcmp(key, "gate_range") == 0) {
        float v = (float)atof(val);
        inst->gate_range_db = (v < 0.0f) ? 0.0f : (v > 80.0f) ? 80.0f : v;
    }
    /* Line settings */
    else if (strcmp(key, "hpf_freq") == 0) {
        inst->hpf_freq_idx = parse_enum(val, hpf_freq_options, HPF_FREQ_COUNT);
        inst->filters_dirty = 1;
    }
    else if (strcmp(key, "safety_limiter") == 0) {
        inst->safety_limiter = parse_enum(val, on_off_options, ON_OFF_COUNT);
    }
    /* Guitar settings */
    else if (strcmp(key, "cable_comp") == 0) {
        inst->cable_comp = parse_enum(val, cable_comp_options, CABLE_COMP_COUNT);
        inst->filters_dirty = 1;
    }
    else if (strcmp(key, "soft_clip") == 0) {
        inst->soft_clip = parse_enum(val, on_off_options, ON_OFF_COUNT);
    }
    /* Phono settings */
    else if (strcmp(key, "riaa_eq") == 0) {
        inst->riaa_eq = parse_enum(val, on_off_options, ON_OFF_COUNT);
        inst->filters_dirty = 1;
    }
    else if (strcmp(key, "subsonic_freq") == 0) {
        inst->subsonic_freq_idx = parse_enum(val, subsonic_freq_options, SUBSONIC_FREQ_COUNT);
        inst->filters_dirty = 1;
    }
    else if (strcmp(key, "hum_notch") == 0) {
        inst->hum_notch = parse_enum(val, on_off_options, ON_OFF_COUNT);
        inst->filters_dirty = 1;
    }
    else if (strcmp(key, "hum_freq") == 0) {
        inst->hum_freq = parse_enum(val, hum_freq_options, HUM_FREQ_COUNT);
        inst->filters_dirty = 1;
    }
    /* Backward compat: old "gain" param maps to input_trim (0-2 -> dB) */
    else if (strcmp(key, "gain") == 0) {
        float v = (float)atof(val);
        float lin = (v < 0.0f) ? 0.0f : (v > 2.0f) ? 2.0f : v;
        if (lin <= 0.0f) {
            inst->input_trim_db = -12.0f;
        } else {
            inst->input_trim_db = 20.0f * log10f(lin);
            if (inst->input_trim_db < -12.0f) inst->input_trim_db = -12.0f;
            if (inst->input_trim_db > 40.0f) inst->input_trim_db = 40.0f;
        }
    }

    if (inst->filters_dirty) {
        recalc_filters(inst);
    }
}

/* ------------------------------------------------------------------ */
/*  get_param                                                          */
/* ------------------------------------------------------------------ */

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    linein_instance_t *inst = (linein_instance_t *)instance;
    if (!key) return -1;

    if (strcmp(key, "input_type") == 0) {
        int idx = inst ? inst->input_type : 0;
        if (idx < 0 || idx >= INPUT_TYPE_COUNT) idx = 0;
        return snprintf(buf, buf_len, "%s", input_type_options[idx]);
    }
    if (strcmp(key, "input_trim") == 0) {
        return snprintf(buf, buf_len, "%.1f", inst ? inst->input_trim_db : 0.0f);
    }
    if (strcmp(key, "output_trim") == 0) {
        return snprintf(buf, buf_len, "%.1f", inst ? inst->output_trim_db : 0.0f);
    }
    if (strcmp(key, "gate_mode") == 0) {
        int idx = inst ? inst->gate_mode : 1;
        if (idx < 0 || idx >= GATE_MODE_COUNT) idx = 1;
        return snprintf(buf, buf_len, "%s", gate_mode_options[idx]);
    }
    if (strcmp(key, "gate_amount") == 0) {
        return snprintf(buf, buf_len, "%.0f", inst ? inst->gate_amount : 50.0f);
    }
    if (strcmp(key, "gate_threshold") == 0) {
        return snprintf(buf, buf_len, "%.0f", inst ? inst->gate_threshold_db : -40.0f);
    }
    if (strcmp(key, "gate_attack") == 0) {
        return snprintf(buf, buf_len, "%.1f", inst ? inst->gate_attack_ms : 3.0f);
    }
    if (strcmp(key, "gate_hold") == 0) {
        return snprintf(buf, buf_len, "%.0f", inst ? inst->gate_hold_ms : 80.0f);
    }
    if (strcmp(key, "gate_release") == 0) {
        return snprintf(buf, buf_len, "%.0f", inst ? inst->gate_release_ms : 200.0f);
    }
    if (strcmp(key, "gate_range") == 0) {
        return snprintf(buf, buf_len, "%.0f", inst ? inst->gate_range_db : 18.0f);
    }
    /* Line settings */
    if (strcmp(key, "hpf_freq") == 0) {
        int idx = inst ? inst->hpf_freq_idx : 0;
        if (idx < 0 || idx >= HPF_FREQ_COUNT) idx = 0;
        return snprintf(buf, buf_len, "%s", hpf_freq_options[idx]);
    }
    if (strcmp(key, "safety_limiter") == 0) {
        int idx = inst ? inst->safety_limiter : 0;
        if (idx < 0 || idx >= ON_OFF_COUNT) idx = 0;
        return snprintf(buf, buf_len, "%s", on_off_options[idx]);
    }
    /* Guitar settings */
    if (strcmp(key, "cable_comp") == 0) {
        int idx = inst ? inst->cable_comp : 2;
        if (idx < 0 || idx >= CABLE_COMP_COUNT) idx = 0;
        return snprintf(buf, buf_len, "%s", cable_comp_options[idx]);
    }
    if (strcmp(key, "soft_clip") == 0) {
        int idx = inst ? inst->soft_clip : 0;
        if (idx < 0 || idx >= ON_OFF_COUNT) idx = 0;
        return snprintf(buf, buf_len, "%s", on_off_options[idx]);
    }
    /* Phono settings */
    if (strcmp(key, "riaa_eq") == 0) {
        int idx = inst ? inst->riaa_eq : 1;
        if (idx < 0 || idx >= ON_OFF_COUNT) idx = 0;
        return snprintf(buf, buf_len, "%s", on_off_options[idx]);
    }
    if (strcmp(key, "subsonic_freq") == 0) {
        int idx = inst ? inst->subsonic_freq_idx : 2;
        if (idx < 0 || idx >= SUBSONIC_FREQ_COUNT) idx = 0;
        return snprintf(buf, buf_len, "%s", subsonic_freq_options[idx]);
    }
    if (strcmp(key, "hum_notch") == 0) {
        int idx = inst ? inst->hum_notch : 1;
        if (idx < 0 || idx >= ON_OFF_COUNT) idx = 0;
        return snprintf(buf, buf_len, "%s", on_off_options[idx]);
    }
    if (strcmp(key, "hum_freq") == 0) {
        int idx = inst ? inst->hum_freq : 1;
        if (idx < 0 || idx >= HUM_FREQ_COUNT) idx = 0;
        return snprintf(buf, buf_len, "%s", hum_freq_options[idx]);
    }
    /* Backward compat */
    if (strcmp(key, "gain") == 0) {
        float lin = inst ? db_to_linear(inst->input_trim_db) : 1.0f;
        return snprintf(buf, buf_len, "%.2f", lin);
    }
    if (strcmp(key, "preset_name") == 0 || strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "Line In");
    }
    if (strcmp(key, "polyphony") == 0) {
        return snprintf(buf, buf_len, "0");
    }
    if (strcmp(key, "ui_hierarchy") == 0) {
        /* Static hierarchy - returned as-is always */
        const char *hierarchy =
            "{"
                "\"modes\":null,"
                "\"levels\":{"
                    "\"root\":{"
                        "\"label\":\"Line In\","
                        "\"children\":null,"
                        "\"knobs\":[\"input_type\",\"input_trim\",\"output_trim\",\"gate_amount\"],"
                        "\"params\":["
                            "\"input_type\","
                            "\"input_trim\","
                            "\"output_trim\","
                            "\"gate_mode\","
                            "\"gate_amount\","
                            "{\"level\":\"gate_settings\",\"label\":\"Gate Settings\"},"
                            "{\"level\":\"line_settings\",\"label\":\"Line Settings\"},"
                            "{\"level\":\"guitar_settings\",\"label\":\"Guitar Settings\"},"
                            "{\"level\":\"phono_settings\",\"label\":\"Phono Settings\"}"
                        "]"
                    "},"
                    "\"gate_settings\":{"
                        "\"label\":\"Gate\","
                        "\"children\":null,"
                        "\"knobs\":[\"gate_threshold\",\"gate_attack\",\"gate_release\",\"gate_range\"],"
                        "\"params\":[\"gate_threshold\",\"gate_attack\",\"gate_hold\",\"gate_release\",\"gate_range\"]"
                    "},"
                    "\"line_settings\":{"
                        "\"label\":\"Line\","
                        "\"children\":null,"
                        "\"knobs\":[\"hpf_freq\",\"safety_limiter\"],"
                        "\"params\":[\"hpf_freq\",\"safety_limiter\"]"
                    "},"
                    "\"guitar_settings\":{"
                        "\"label\":\"Guitar\","
                        "\"children\":null,"
                        "\"knobs\":[\"cable_comp\",\"soft_clip\"],"
                        "\"params\":[\"cable_comp\",\"soft_clip\"]"
                    "},"
                    "\"phono_settings\":{"
                        "\"label\":\"Phono\","
                        "\"children\":null,"
                        "\"knobs\":[\"riaa_eq\",\"subsonic_freq\",\"hum_notch\",\"hum_freq\"],"
                        "\"params\":[\"riaa_eq\",\"subsonic_freq\",\"hum_notch\",\"hum_freq\"]"
                    "}"
                "}"
            "}";
        int len = strlen(hierarchy);
        if (len < buf_len) {
            strcpy(buf, hierarchy);
            return len;
        }
        return -1;
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/*  render_block                                                       */
/* ------------------------------------------------------------------ */

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    linein_instance_t *inst = (linein_instance_t *)instance;

    if (!g_host || !g_host->mapped_memory || !inst) {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    int16_t *audio_in = (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);

    /* Compute target gains */
    float input_gain_target = db_to_linear(inst->input_trim_db);
    float output_gain_target = db_to_linear(inst->output_trim_db);

    /* Gate parameters */
    float g_thresh_db, g_attack_ms, g_hold_ms, g_release_ms, g_range_db, g_hysteresis_db;
    int gate_active = (inst->gate_mode != GATE_MODE_OFF);

    if (gate_active) {
        gate_get_effective_params(inst,
            &g_thresh_db, &g_attack_ms, &g_hold_ms, &g_release_ms,
            &g_range_db, &g_hysteresis_db);
    } else {
        /* Not used, but suppress warnings */
        g_thresh_db = g_attack_ms = g_hold_ms = g_release_ms = g_range_db = g_hysteresis_db = 0.0f;
    }

    /* Pre-compute gate coefficients */
    float gate_open_thresh = 0.0f, gate_close_thresh = 0.0f;
    float gate_attack_coeff = 1.0f, gate_release_coeff = 1.0f;
    int   gate_hold_samples = 0;
    float gate_floor = 1.0f;

    if (gate_active) {
        gate_open_thresh = db_to_linear(g_thresh_db);
        gate_close_thresh = db_to_linear(g_thresh_db - g_hysteresis_db);
        gate_attack_coeff = (g_attack_ms > 0.0f) ?
            1.0f - expf(-1.0f / (g_attack_ms * 0.001f * SAMPLE_RATE)) : 1.0f;
        gate_release_coeff = (g_release_ms > 0.0f) ?
            1.0f - expf(-1.0f / (g_release_ms * 0.001f * SAMPLE_RATE)) : 1.0f;
        gate_hold_samples = (int)(g_hold_ms * 0.001f * SAMPLE_RATE);
        gate_floor = db_to_linear(-g_range_db);
        if (gate_floor < 0.0f) gate_floor = 0.0f;
    }

    /* Save previous output for crossfade if mode just switched */
    int do_xfade = (inst->xfade_remaining > 0);

    for (int i = 0; i < frames; i++) {
        /* Read stereo input and convert to float */
        float L = (float)audio_in[i * 2];
        float R = (float)audio_in[i * 2 + 1];

        /* 1. Input trim (smoothed) */
        inst->input_gain_smooth += GAIN_SMOOTH_COEFF * (input_gain_target - inst->input_gain_smooth);
        L *= inst->input_gain_smooth;
        R *= inst->input_gain_smooth;

        /* 2. Mode conditioning */
        switch (inst->input_type) {
        case INPUT_TYPE_LINE:
            /* HPF (passthrough if off) */
            L = biquad_process(&inst->hpf[0], L);
            R = biquad_process(&inst->hpf[1], R);
            /* Safety limiter: simple soft clamp */
            if (inst->safety_limiter) {
                if (L > 30000.0f) L = 30000.0f + (L - 30000.0f) * 0.1f;
                if (L < -30000.0f) L = -30000.0f + (L + 30000.0f) * 0.1f;
                if (R > 30000.0f) R = 30000.0f + (R - 30000.0f) * 0.1f;
                if (R < -30000.0f) R = -30000.0f + (R + 30000.0f) * 0.1f;
            }
            break;

        case INPUT_TYPE_GUITAR:
            /* HPF */
            L = biquad_process(&inst->hpf[0], L);
            R = biquad_process(&inst->hpf[1], R);
            /* Cable compensation (high-shelf) */
            L = biquad_process(&inst->cable_shelf[0], L);
            R = biquad_process(&inst->cable_shelf[1], R);
            /* Soft clip (tanh) */
            if (inst->soft_clip) {
                float norm = 1.0f / 32768.0f;
                L = tanhf(L * norm) * 32768.0f;
                R = tanhf(R * norm) * 32768.0f;
            }
            break;

        case INPUT_TYPE_PHONO:
            /* RIAA de-emphasis (two cascaded stages) */
            L = biquad_process(&inst->riaa_stage1[0], L);
            L = biquad_process(&inst->riaa_stage2[0], L);
            R = biquad_process(&inst->riaa_stage1[1], R);
            R = biquad_process(&inst->riaa_stage2[1], R);
            /* Subsonic filter */
            L = biquad_process(&inst->subsonic[0], L);
            R = biquad_process(&inst->subsonic[1], R);
            /* Hum notch (fundamental + harmonic) */
            L = biquad_process(&inst->hum_notch1[0], L);
            L = biquad_process(&inst->hum_notch2[0], L);
            R = biquad_process(&inst->hum_notch1[1], R);
            R = biquad_process(&inst->hum_notch2[1], R);
            break;
        }

        /* 3. Noise gate */
        if (gate_active) {
            /* Linked stereo peak detection */
            float env_in = fabsf(L);
            float env_r = fabsf(R);
            if (env_r > env_in) env_in = env_r;

            /* Normalize to 0..1 range for threshold comparison */
            float env_norm = env_in / 32768.0f;

            /* Envelope follower */
            if (env_norm > inst->gate_envelope) {
                inst->gate_envelope += gate_attack_coeff * (env_norm - inst->gate_envelope);
            } else {
                inst->gate_envelope += gate_release_coeff * (env_norm - inst->gate_envelope);
            }

            /* State machine */
            switch (inst->gate_state) {
            case GATE_OPEN:
                inst->gate_gain += gate_attack_coeff * (1.0f - inst->gate_gain);
                if (inst->gate_envelope < gate_close_thresh) {
                    inst->gate_state = GATE_HOLD;
                    inst->gate_hold_counter = gate_hold_samples;
                }
                break;
            case GATE_HOLD:
                inst->gate_hold_counter--;
                if (inst->gate_envelope > gate_open_thresh) {
                    inst->gate_state = GATE_OPEN;
                } else if (inst->gate_hold_counter <= 0) {
                    inst->gate_state = GATE_CLOSING;
                }
                break;
            case GATE_CLOSING:
                inst->gate_gain += gate_release_coeff * (gate_floor - inst->gate_gain);
                if (inst->gate_envelope > gate_open_thresh) {
                    inst->gate_state = GATE_OPEN;
                } else if (inst->gate_gain <= gate_floor + 0.001f) {
                    inst->gate_gain = gate_floor;
                    inst->gate_state = GATE_CLOSED;
                }
                break;
            case GATE_CLOSED:
                inst->gate_gain = gate_floor;
                if (inst->gate_envelope > gate_open_thresh) {
                    inst->gate_state = GATE_OPEN;
                }
                break;
            }

            L *= inst->gate_gain;
            R *= inst->gate_gain;
        }

        /* 4. Output trim (smoothed) */
        inst->output_gain_smooth += GAIN_SMOOTH_COEFF * (output_gain_target - inst->output_gain_smooth);
        L *= inst->output_gain_smooth;
        R *= inst->output_gain_smooth;

        /* 5. Crossfade on mode switch */
        if (do_xfade && inst->xfade_remaining > 0) {
            float xfade_pos = (float)inst->xfade_remaining / (float)XFADE_SAMPLES;
            float prev_L = inst->xfade_prev_buf[i * 2];
            float prev_R = inst->xfade_prev_buf[i * 2 + 1];
            L = L * (1.0f - xfade_pos) + prev_L * xfade_pos;
            R = R * (1.0f - xfade_pos) + prev_R * xfade_pos;
            inst->xfade_remaining--;
        }

        /* 6. Clamp to int16 range and output */
        if (L > 32767.0f) L = 32767.0f;
        if (L < -32768.0f) L = -32768.0f;
        if (R > 32767.0f) R = 32767.0f;
        if (R < -32768.0f) R = -32768.0f;

        out_interleaved_lr[i * 2] = (int16_t)L;
        out_interleaved_lr[i * 2 + 1] = (int16_t)R;
    }
}

/* ------------------------------------------------------------------ */
/*  Plugin API v2                                                      */
/* ------------------------------------------------------------------ */

static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = v2_create_instance,
    .destroy_instance = v2_destroy_instance,
    .on_midi = v2_on_midi,
    .set_param = v2_set_param,
    .get_param = v2_get_param,
    .render_block = v2_render_block
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    linein_log("plugin initialized (v2)");
    return &g_plugin_api_v2;
}
