/*
 * Velocity Scale MIDI FX
 *
 * Shapes incoming note velocities in two additive stages:
 *   1. MPC Curve  - a tunable power curve (off by default, Curve=0 = linear).
 *   2. Min/Max    - linear scale of the curve output into a configurable range.
 * Velocity 0 (note-off) is always passed through unchanged.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

/* JSON helpers for state parsing */
static int json_get_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return 0;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;
    const char *colon = strchr(pos, ':');
    if (!colon) return 0;
    colon++;
    while (*colon && (*colon == ' ' || *colon == '\t')) colon++;
    *out = atoi(colon);
    return 1;
}

typedef struct {
    int vel_min;  /* 1-127 */
    int vel_max;  /* 1-127 */
    int curve;    /* -100..+100, 0 = linear (no-op) */
} velocity_scale_instance_t;

static const host_api_v1_t *g_host = NULL;

/* Named Curve presets (just convenience setters for the continuous Curve). */
typedef struct { const char *name; int value; } curve_preset_t;
static const curve_preset_t CURVE_PRESETS[] = {
    { "Linear",  0 },
    { "Soft",   40 },
    { "Softer", 70 },
    { "Hard",  -40 },
    { "Harder",-70 },
};
#define NUM_CURVE_PRESETS ((int)(sizeof(CURVE_PRESETS) / sizeof(CURVE_PRESETS[0])))

/*
 * MPC Curve velocity shaping
 * --------------------------
 * Applies a tunable power curve to note-on velocity. Approximates the feel of
 * an Akai MPC pad/velocity curve; the MPC's exact internal transfer function is
 * unpublished, so this is a continuous, monotonic power curve.
 *
 * Sign convention (Curve in -100..+100, 0 = linear / no change):
 *   Positive Curve = more sensitive / easier to play loud (boosts soft hits).
 *   Negative Curve = harder; requires harder hits to reach high velocity.
 *
 * For a note-on velocity v (1..127):
 *   n      = (v - 1) / 126.0                 // v=1 -> 0, v=127 -> 1
 *   gamma  = pow(2.0, -Curve / 100.0 * 2.0)  // 0 -> 1, +100 -> 0.25, -100 -> 4.0
 *   shaped = pow(n, gamma)
 *   out    = 1 + shaped * 126.0
 * Endpoints are exact for any Curve: v=1 -> 1 and v=127 -> 127.
 */
static double curve_stage(double v, int curve) {
    if (curve == 0) return v;  /* linear: exact no-op */
    double n = (v - 1.0) / 126.0;
    double gamma = pow(2.0, -curve / 100.0 * 2.0);
    double shaped = pow(n, gamma);
    return 1.0 + shaped * 126.0;
}

static void* velocity_scale_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;

    velocity_scale_instance_t *inst = calloc(1, sizeof(velocity_scale_instance_t));
    if (!inst) return NULL;

    inst->vel_min = 1;
    inst->vel_max = 127;
    inst->curve = 0;
    return inst;
}

static void velocity_scale_destroy_instance(void *instance) {
    if (instance) free(instance);
}

static int velocity_scale_process_midi(void *instance,
                                       const uint8_t *in_msg, int in_len,
                                       uint8_t out_msgs[][3], int out_lens[],
                                       int max_out) {
    velocity_scale_instance_t *inst = (velocity_scale_instance_t *)instance;
    if (!inst || in_len < 1 || max_out < 1) return 0;

    uint8_t status = in_msg[0] & 0xF0;

    /* Only scale velocity on note-on messages with velocity > 0 */
    if (status == 0x90 && in_len >= 3 && in_msg[2] > 0) {
        int vel = in_msg[2];  /* 1-127 */
        int lo = inst->vel_min;
        int hi = inst->vel_max;

        /* Ensure min <= max for the mapping */
        if (lo > hi) { int tmp = lo; lo = hi; hi = tmp; }

        /* Stage order: MPC curve first, then linear min/max scale, then round.
         * curve_stage and the scale are computed in floating point and rounded
         * exactly once (round-half-up) so Curve=0 with the default 1..127 range
         * is a true no-op (out == vel). */
        double curved = curve_stage((double)vel, inst->curve);
        double mapped = (double)lo + (curved - 1.0) * (double)(hi - lo) / 126.0;
        int scaled = (int)floor(mapped + 0.5);  /* round-half-up */
        if (scaled < 1) scaled = 1;
        if (scaled > 127) scaled = 127;

        out_msgs[0][0] = in_msg[0];
        out_msgs[0][1] = in_msg[1];
        out_msgs[0][2] = (uint8_t)scaled;
        out_lens[0] = 3;
        return 1;
    }

    /* Pass through all other messages unchanged */
    out_msgs[0][0] = in_msg[0];
    out_msgs[0][1] = in_len > 1 ? in_msg[1] : 0;
    out_msgs[0][2] = in_len > 2 ? in_msg[2] : 0;
    out_lens[0] = in_len;
    return 1;
}

static int velocity_scale_tick(void *instance,
                               int frames, int sample_rate,
                               uint8_t out_msgs[][3], int out_lens[],
                               int max_out) {
    (void)instance;
    (void)frames;
    (void)sample_rate;
    (void)out_msgs;
    (void)out_lens;
    (void)max_out;
    return 0;  /* No time-based output */
}

static void velocity_scale_set_param(void *instance, const char *key, const char *val) {
    velocity_scale_instance_t *inst = (velocity_scale_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "min") == 0) {
        int v = atoi(val);
        if (v < 1) v = 1;
        if (v > 127) v = 127;
        inst->vel_min = v;
    }
    else if (strcmp(key, "max") == 0) {
        int v = atoi(val);
        if (v < 1) v = 1;
        if (v > 127) v = 127;
        inst->vel_max = v;
    }
    else if (strcmp(key, "curve") == 0) {
        int v = atoi(val);
        if (v < -100) v = -100;
        if (v > 100) v = 100;
        inst->curve = v;
    }
    else if (strcmp(key, "curve_preset") == 0) {
        /* Named preset just sets the continuous Curve value. */
        for (int i = 0; i < NUM_CURVE_PRESETS; i++) {
            if (strcmp(val, CURVE_PRESETS[i].name) == 0) {
                inst->curve = CURVE_PRESETS[i].value;
                break;
            }
        }
    }
    else if (strcmp(key, "state") == 0) {
        int v;
        if (json_get_int(val, "min", &v)) {
            if (v < 1) v = 1;
            if (v > 127) v = 127;
            inst->vel_min = v;
        }
        if (json_get_int(val, "max", &v)) {
            if (v < 1) v = 1;
            if (v > 127) v = 127;
            inst->vel_max = v;
        }
        if (json_get_int(val, "curve", &v)) {
            if (v < -100) v = -100;
            if (v > 100) v = 100;
            inst->curve = v;
        }
    }
}

static int velocity_scale_get_param(void *instance, const char *key, char *buf, int buf_len) {
    velocity_scale_instance_t *inst = (velocity_scale_instance_t *)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    if (strcmp(key, "min") == 0) {
        return snprintf(buf, buf_len, "%d", inst->vel_min);
    }
    else if (strcmp(key, "max") == 0) {
        return snprintf(buf, buf_len, "%d", inst->vel_max);
    }
    else if (strcmp(key, "curve") == 0) {
        return snprintf(buf, buf_len, "%d", inst->curve);
    }
    else if (strcmp(key, "curve_preset") == 0) {
        /* Reflect the named preset matching the current Curve, else Linear. */
        const char *name = CURVE_PRESETS[0].name;
        for (int i = 0; i < NUM_CURVE_PRESETS; i++) {
            if (CURVE_PRESETS[i].value == inst->curve) {
                name = CURVE_PRESETS[i].name;
                break;
            }
        }
        return snprintf(buf, buf_len, "%s", name);
    }
    else if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len, "{\"min\":%d,\"max\":%d,\"curve\":%d}",
                        inst->vel_min, inst->vel_max, inst->curve);
    }
    else if (strcmp(key, "chain_params") == 0) {
        const char *params = "["
            "{\"key\":\"min\",\"name\":\"Min Velocity\",\"type\":\"int\",\"min\":1,\"max\":127,\"step\":1},"
            "{\"key\":\"max\",\"name\":\"Max Velocity\",\"type\":\"int\",\"min\":1,\"max\":127,\"step\":1},"
            "{\"key\":\"curve\",\"name\":\"Curve\",\"type\":\"int\",\"min\":-100,\"max\":100,\"step\":1,\"default\":0},"
            "{\"key\":\"curve_preset\",\"name\":\"Curve Preset\",\"type\":\"enum\",\"options\":[\"Linear\",\"Soft\",\"Softer\",\"Hard\",\"Harder\"]}"
        "]";
        return snprintf(buf, buf_len, "%s", params);
    }

    return -1;
}

static midi_fx_api_v1_t g_api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = velocity_scale_create_instance,
    .destroy_instance = velocity_scale_destroy_instance,
    .process_midi = velocity_scale_process_midi,
    .tick = velocity_scale_tick,
    .set_param = velocity_scale_set_param,
    .get_param = velocity_scale_get_param
};

midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
