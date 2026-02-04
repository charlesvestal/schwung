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

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"

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

/* V2 API - Instance-based (V1 API removed) */
static const host_api_v1_t *g_host = NULL;

/* Instance structure */
typedef struct {
    /* Reverb parameters */
    float room_size;
    float damping;
    float wet;
    float dry;
    float width;

    /* Derived parameters */
    float feedback;
    float damp1;
    float damp2;
    float wet1;
    float wet2;

    /* Filter instances */
    comb_filter_t comb_l[NUM_COMBS];
    comb_filter_t comb_r[NUM_COMBS];
    allpass_filter_t allpass_l[NUM_ALLPASSES];
    allpass_filter_t allpass_r[NUM_ALLPASSES];
} freeverb_instance_t;

/* Logging helper */
static void v2_log(const char *msg) {
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

/* Process one sample through comb filter (instance-based) */
static inline float comb_process(comb_filter_t *c, float input, float feedback, float damp1, float damp2) {
    float output = c->buffer[c->bufidx];
    c->filterstore = (output * damp2) + (c->filterstore * damp1);
    c->buffer[c->bufidx] = input + (c->filterstore * feedback);
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

/* Update derived parameters (instance-based) */
static void v2_update_params(freeverb_instance_t *inst) {
    inst->feedback = inst->room_size * 0.28f + 0.7f;
    inst->damp1 = inst->damping * 0.4f;
    inst->damp2 = 1.0f - inst->damp1;
    inst->wet1 = inst->wet * (inst->width / 2.0f + 0.5f);
    inst->wet2 = inst->wet * ((1.0f - inst->width) / 2.0f);
}

/* === V2 API Implementation === */

static void* v2_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;

    v2_log("Creating instance");

    freeverb_instance_t *inst = (freeverb_instance_t*)calloc(1, sizeof(freeverb_instance_t));
    if (!inst) {
        v2_log("Failed to allocate instance");
        return NULL;
    }

    /* Set default parameters */
    inst->room_size = 0.5f;
    inst->damping = 0.5f;
    inst->wet = 0.3f;
    inst->dry = 0.7f;
    inst->width = 1.0f;

    /* Initialize comb filters */
    for (int i = 0; i < NUM_COMBS; i++) {
        comb_init(&inst->comb_l[i], comb_tuning_l[i]);
        comb_init(&inst->comb_r[i], comb_tuning_r[i]);
    }

    /* Initialize allpass filters */
    for (int i = 0; i < NUM_ALLPASSES; i++) {
        allpass_init(&inst->allpass_l[i], allpass_tuning_l[i]);
        allpass_init(&inst->allpass_r[i], allpass_tuning_r[i]);
    }

    v2_update_params(inst);

    v2_log("Instance created");
    return inst;
}

static void v2_destroy_instance(void *instance) {
    freeverb_instance_t *inst = (freeverb_instance_t*)instance;
    if (!inst) return;

    v2_log("Destroying instance");
    free(inst);
}

static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {
    freeverb_instance_t *inst = (freeverb_instance_t*)instance;
    if (!inst) return;

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
            out_l += comb_process(&inst->comb_l[c], input, inst->feedback, inst->damp1, inst->damp2);
            out_r += comb_process(&inst->comb_r[c], input, inst->feedback, inst->damp1, inst->damp2);
        }

        /* Scale down comb output (8 filters summed) */
        out_l *= 0.125f;
        out_r *= 0.125f;

        /* Pass through allpass filters in series */
        for (int a = 0; a < NUM_ALLPASSES; a++) {
            out_l = allpass_process(&inst->allpass_l[a], out_l);
            out_r = allpass_process(&inst->allpass_r[a], out_r);
        }

        /* Mix wet and dry */
        float mix_l = out_l * inst->wet1 + out_r * inst->wet2 + in_l * inst->dry;
        float mix_r = out_r * inst->wet1 + out_l * inst->wet2 + in_r * inst->dry;

        /* Clamp and convert back to int16 */
        if (mix_l > 1.0f) mix_l = 1.0f;
        if (mix_l < -1.0f) mix_l = -1.0f;
        if (mix_r > 1.0f) mix_r = 1.0f;
        if (mix_r < -1.0f) mix_r = -1.0f;

        audio_inout[i * 2] = (int16_t)(mix_l * 32767.0f);
        audio_inout[i * 2 + 1] = (int16_t)(mix_r * 32767.0f);
    }
}

/* Simple JSON number extraction */
static int json_get_float(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    *out = atof(p);
    return 0;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    freeverb_instance_t *inst = (freeverb_instance_t*)instance;
    if (!inst) return;

    /* State restore from patch save */
    if (strcmp(key, "state") == 0) {
        float v;
        if (json_get_float(val, "room_size", &v) == 0) {
            inst->room_size = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
        }
        if (json_get_float(val, "damping", &v) == 0) {
            inst->damping = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
        }
        if (json_get_float(val, "wet", &v) == 0) {
            inst->wet = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
        }
        if (json_get_float(val, "dry", &v) == 0) {
            inst->dry = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
        }
        if (json_get_float(val, "width", &v) == 0) {
            inst->width = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
        }
        v2_update_params(inst);
        return;
    }

    float v = atof(val);

    if (strcmp(key, "room_size") == 0) {
        inst->room_size = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    } else if (strcmp(key, "damping") == 0) {
        inst->damping = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    } else if (strcmp(key, "wet") == 0) {
        inst->wet = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    } else if (strcmp(key, "dry") == 0) {
        inst->dry = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    } else if (strcmp(key, "width") == 0) {
        inst->width = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    }

    v2_update_params(inst);
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    freeverb_instance_t *inst = (freeverb_instance_t*)instance;
    if (!inst) return -1;

    if (strcmp(key, "room_size") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->room_size);
    } else if (strcmp(key, "damping") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->damping);
    } else if (strcmp(key, "wet") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->wet);
    } else if (strcmp(key, "dry") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->dry);
    } else if (strcmp(key, "width") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->width);
    } else if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "Freeverb");
    } else if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"room_size\":%.4f,\"damping\":%.4f,\"wet\":%.4f,\"dry\":%.4f,\"width\":%.4f}",
            inst->room_size, inst->damping, inst->wet, inst->dry, inst->width);
    } else if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"room_size\",\"damping\",\"wet\",\"dry\"],"
                    "\"params\":[\"room_size\",\"damping\",\"wet\",\"dry\",\"width\"]"
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

/* === V2 Entry Point === */

static audio_fx_api_v2_t g_fx_api_v2;

audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api_v2, 0, sizeof(g_fx_api_v2));
    g_fx_api_v2.api_version = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance = v2_create_instance;
    g_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_fx_api_v2.process_block = v2_process_block;
    g_fx_api_v2.set_param = v2_set_param;
    g_fx_api_v2.get_param = v2_get_param;

    v2_log("Freeverb v2 plugin initialized");

    return &g_fx_api_v2;
}
