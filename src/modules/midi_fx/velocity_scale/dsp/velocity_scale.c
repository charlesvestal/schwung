/*
 * Velocity Scale MIDI FX
 *
 * Scales incoming note velocities to fit within a configurable min/max range.
 * Input velocity 1-127 is linearly mapped to the min-max range.
 * Velocity 0 (note-off) is always passed through unchanged.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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
} velocity_scale_instance_t;

static const host_api_v1_t *g_host = NULL;

static void* velocity_scale_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;

    velocity_scale_instance_t *inst = calloc(1, sizeof(velocity_scale_instance_t));
    if (!inst) return NULL;

    inst->vel_min = 1;
    inst->vel_max = 127;
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

        /* Linear map: input 1-127 -> lo-hi */
        int scaled = lo + ((vel - 1) * (hi - lo) + 63) / 126;
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
    else if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len, "{\"min\":%d,\"max\":%d}",
                        inst->vel_min, inst->vel_max);
    }
    else if (strcmp(key, "chain_params") == 0) {
        const char *params = "["
            "{\"key\":\"min\",\"name\":\"Min Velocity\",\"type\":\"int\",\"min\":1,\"max\":127,\"step\":1},"
            "{\"key\":\"max\",\"name\":\"Max Velocity\",\"type\":\"int\",\"min\":1,\"max\":127,\"step\":1}"
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
