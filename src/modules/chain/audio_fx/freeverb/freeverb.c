/*
 * Freeverb Audio FX Plugin
 *
 * Classic Schroeder-Moorer reverb algorithm.
 * Based on public domain Freeverb by Jezar at Dreampoint.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "host/audio_fx_api_v1.h"

/* Freeverb constants */
#define NUM_COMBS 8
#define NUM_ALLPASSES 4
#define SAMPLE_RATE 44100

/* Comb filter delay lengths (in samples at 44100Hz) */
static const int comb_tuning_l[NUM_COMBS] = {
    1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
};
static const int comb_tuning_r[NUM_COMBS] = {
    1116 + 23, 1188 + 23, 1277 + 23, 1356 + 23,
    1422 + 23, 1491 + 23, 1557 + 23, 1617 + 23
};

/* Allpass filter delay lengths */
static const int allpass_tuning_l[NUM_ALLPASSES] = { 556, 441, 341, 225 };
static const int allpass_tuning_r[NUM_ALLPASSES] = { 556 + 23, 441 + 23, 341 + 23, 225 + 23 };

/* Maximum delay length */
#define MAX_DELAY 2048

/* Comb filter state */
typedef struct {
    float buffer[MAX_DELAY];
    int bufsize;
    int bufidx;
    float filterstore;
} comb_filter_t;

/* Allpass filter state */
typedef struct {
    float buffer[MAX_DELAY];
    int bufsize;
    int bufidx;
} allpass_filter_t;

/* Plugin state */
static const host_api_v1_t *g_host = NULL;
static audio_fx_api_v1_t g_fx_api;

/* Reverb parameters */
static float g_room_size = 0.5f;
static float g_damping = 0.5f;
static float g_wet = 0.3f;
static float g_dry = 0.7f;
static float g_width = 1.0f;

/* Derived parameters */
static float g_feedback;
static float g_damp1;
static float g_damp2;
static float g_wet1;
static float g_wet2;

/* Filter instances */
static comb_filter_t g_comb_l[NUM_COMBS];
static comb_filter_t g_comb_r[NUM_COMBS];
static allpass_filter_t g_allpass_l[NUM_ALLPASSES];
static allpass_filter_t g_allpass_r[NUM_ALLPASSES];

/* Logging helper */
static void fx_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[freeverb] %s", msg);
        g_host->log(buf);
    }
}

/* Initialize a comb filter */
static void comb_init(comb_filter_t *c, int size) {
    memset(c->buffer, 0, sizeof(c->buffer));
    c->bufsize = (size < MAX_DELAY) ? size : MAX_DELAY;
    c->bufidx = 0;
    c->filterstore = 0.0f;
}

/* Process one sample through comb filter */
static inline float comb_process(comb_filter_t *c, float input) {
    float output = c->buffer[c->bufidx];
    c->filterstore = (output * g_damp2) + (c->filterstore * g_damp1);
    c->buffer[c->bufidx] = input + (c->filterstore * g_feedback);
    if (++c->bufidx >= c->bufsize) c->bufidx = 0;
    return output;
}

/* Initialize an allpass filter */
static void allpass_init(allpass_filter_t *a, int size) {
    memset(a->buffer, 0, sizeof(a->buffer));
    a->bufsize = (size < MAX_DELAY) ? size : MAX_DELAY;
    a->bufidx = 0;
}

/* Process one sample through allpass filter */
static inline float allpass_process(allpass_filter_t *a, float input) {
    float bufout = a->buffer[a->bufidx];
    float output = -input + bufout;
    a->buffer[a->bufidx] = input + (bufout * 0.5f);
    if (++a->bufidx >= a->bufsize) a->bufidx = 0;
    return output;
}

/* Update derived parameters */
static void update_params(void) {
    g_feedback = g_room_size * 0.28f + 0.7f;
    g_damp1 = g_damping * 0.4f;
    g_damp2 = 1.0f - g_damp1;
    g_wet1 = g_wet * (g_width / 2.0f + 0.5f);
    g_wet2 = g_wet * ((1.0f - g_width) / 2.0f);
}

/* === Audio FX API Implementation === */

static int fx_on_load(const char *module_dir, const char *config_json) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Freeverb loading from: %s", module_dir);
    fx_log(msg);

    /* Initialize comb filters */
    for (int i = 0; i < NUM_COMBS; i++) {
        comb_init(&g_comb_l[i], comb_tuning_l[i]);
        comb_init(&g_comb_r[i], comb_tuning_r[i]);
    }

    /* Initialize allpass filters */
    for (int i = 0; i < NUM_ALLPASSES; i++) {
        allpass_init(&g_allpass_l[i], allpass_tuning_l[i]);
        allpass_init(&g_allpass_r[i], allpass_tuning_r[i]);
    }

    update_params();

    fx_log("Freeverb initialized");
    return 0;
}

static void fx_on_unload(void) {
    fx_log("Freeverb unloading");
}

static void fx_process_block(int16_t *audio_inout, int frames) {
    for (int i = 0; i < frames; i++) {
        /* Convert to float (-1.0 to 1.0) */
        float in_l = audio_inout[i * 2] / 32768.0f;
        float in_r = audio_inout[i * 2 + 1] / 32768.0f;

        /* Mix input to mono for reverb processing */
        float input = (in_l + in_r) * 0.5f;

        /* Accumulate comb filter outputs */
        float out_l = 0.0f;
        float out_r = 0.0f;

        for (int c = 0; c < NUM_COMBS; c++) {
            out_l += comb_process(&g_comb_l[c], input);
            out_r += comb_process(&g_comb_r[c], input);
        }

        /* Pass through allpass filters in series */
        for (int a = 0; a < NUM_ALLPASSES; a++) {
            out_l = allpass_process(&g_allpass_l[a], out_l);
            out_r = allpass_process(&g_allpass_r[a], out_r);
        }

        /* Mix wet and dry */
        float mix_l = out_l * g_wet1 + out_r * g_wet2 + in_l * g_dry;
        float mix_r = out_r * g_wet1 + out_l * g_wet2 + in_r * g_dry;

        /* Clamp and convert back to int16 */
        if (mix_l > 1.0f) mix_l = 1.0f;
        if (mix_l < -1.0f) mix_l = -1.0f;
        if (mix_r > 1.0f) mix_r = 1.0f;
        if (mix_r < -1.0f) mix_r = -1.0f;

        audio_inout[i * 2] = (int16_t)(mix_l * 32767.0f);
        audio_inout[i * 2 + 1] = (int16_t)(mix_r * 32767.0f);
    }
}

static void fx_set_param(const char *key, const char *val) {
    float v = atof(val);

    if (strcmp(key, "room_size") == 0) {
        g_room_size = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    } else if (strcmp(key, "damping") == 0) {
        g_damping = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    } else if (strcmp(key, "wet") == 0) {
        g_wet = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    } else if (strcmp(key, "dry") == 0) {
        g_dry = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    } else if (strcmp(key, "width") == 0) {
        g_width = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    }

    update_params();
}

static int fx_get_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "room_size") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_room_size);
    } else if (strcmp(key, "damping") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_damping);
    } else if (strcmp(key, "wet") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_wet);
    } else if (strcmp(key, "dry") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_dry);
    } else if (strcmp(key, "width") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_width);
    } else if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "Freeverb");
    }
    return -1;
}

/* === Entry Point === */

audio_fx_api_v1_t* move_audio_fx_init_v1(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api, 0, sizeof(g_fx_api));
    g_fx_api.api_version = AUDIO_FX_API_VERSION;
    g_fx_api.on_load = fx_on_load;
    g_fx_api.on_unload = fx_on_unload;
    g_fx_api.process_block = fx_process_block;
    g_fx_api.set_param = fx_set_param;
    g_fx_api.get_param = fx_get_param;

    fx_log("Freeverb plugin initialized");

    return &g_fx_api;
}
