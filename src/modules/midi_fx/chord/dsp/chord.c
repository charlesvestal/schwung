/*
 * Chord MIDI FX
 *
 * Generates chord notes from single note input.
 * Supports: major, minor, power, octave chords.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

typedef enum {
    CHORD_NONE = 0,
    CHORD_MAJOR,
    CHORD_MINOR,
    CHORD_POWER,
    CHORD_OCTAVE
} chord_type_t;

typedef struct {
    chord_type_t type;
} chord_instance_t;

static const host_api_v1_t *g_host = NULL;

static void* chord_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;

    chord_instance_t *inst = calloc(1, sizeof(chord_instance_t));
    if (!inst) return NULL;

    inst->type = CHORD_MAJOR;
    return inst;
}

static void chord_destroy_instance(void *instance) {
    if (instance) free(instance);
}

static int chord_process_midi(void *instance,
                              const uint8_t *in_msg, int in_len,
                              uint8_t out_msgs[][3], int out_lens[],
                              int max_out) {
    chord_instance_t *inst = (chord_instance_t *)instance;
    if (!inst || in_len < 1 || max_out < 1) return 0;

    uint8_t status = in_msg[0] & 0xF0;

    /* Only process note on/off */
    if ((status != 0x90 && status != 0x80) || in_len < 3) {
        /* Pass through non-note messages */
        out_msgs[0][0] = in_msg[0];
        out_msgs[0][1] = in_len > 1 ? in_msg[1] : 0;
        out_msgs[0][2] = in_len > 2 ? in_msg[2] : 0;
        out_lens[0] = in_len;
        return 1;
    }

    /* No chord type set - pass through */
    if (inst->type == CHORD_NONE) {
        out_msgs[0][0] = in_msg[0];
        out_msgs[0][1] = in_msg[1];
        out_msgs[0][2] = in_msg[2];
        out_lens[0] = 3;
        return 1;
    }

    uint8_t note = in_msg[1];
    int count = 0;

    /* Root note */
    out_msgs[count][0] = in_msg[0];
    out_msgs[count][1] = note;
    out_msgs[count][2] = in_msg[2];
    out_lens[count] = 3;
    count++;

    /* Add chord intervals */
    int intervals[3] = {0, 0, 0};
    int num_intervals = 0;

    switch (inst->type) {
        case CHORD_MAJOR:
            intervals[0] = 4;   /* Major 3rd */
            intervals[1] = 7;   /* Perfect 5th */
            num_intervals = 2;
            break;
        case CHORD_MINOR:
            intervals[0] = 3;   /* Minor 3rd */
            intervals[1] = 7;   /* Perfect 5th */
            num_intervals = 2;
            break;
        case CHORD_POWER:
            intervals[0] = 7;   /* Perfect 5th */
            num_intervals = 1;
            break;
        case CHORD_OCTAVE:
            intervals[0] = 12;  /* Octave */
            num_intervals = 1;
            break;
        default:
            break;
    }

    for (int i = 0; i < num_intervals && count < max_out; i++) {
        int transposed = (int)note + intervals[i];
        if (transposed <= 127) {
            out_msgs[count][0] = in_msg[0];
            out_msgs[count][1] = (uint8_t)transposed;
            out_msgs[count][2] = in_msg[2];
            out_lens[count] = 3;
            count++;
        }
    }

    return count;
}

static int chord_tick(void *instance,
                      int frames, int sample_rate,
                      uint8_t out_msgs[][3], int out_lens[],
                      int max_out) {
    /* Chord FX doesn't generate time-based events */
    (void)instance;
    (void)frames;
    (void)sample_rate;
    (void)out_msgs;
    (void)out_lens;
    (void)max_out;
    return 0;
}

static void chord_set_param(void *instance, const char *key, const char *val) {
    chord_instance_t *inst = (chord_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "type") == 0) {
        if (strcmp(val, "none") == 0) inst->type = CHORD_NONE;
        else if (strcmp(val, "major") == 0) inst->type = CHORD_MAJOR;
        else if (strcmp(val, "minor") == 0) inst->type = CHORD_MINOR;
        else if (strcmp(val, "power") == 0) inst->type = CHORD_POWER;
        else if (strcmp(val, "octave") == 0) inst->type = CHORD_OCTAVE;
    }
}

static int chord_get_param(void *instance, const char *key, char *buf, int buf_len) {
    chord_instance_t *inst = (chord_instance_t *)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    if (strcmp(key, "type") == 0) {
        const char *val = "none";
        switch (inst->type) {
            case CHORD_MAJOR: val = "major"; break;
            case CHORD_MINOR: val = "minor"; break;
            case CHORD_POWER: val = "power"; break;
            case CHORD_OCTAVE: val = "octave"; break;
            default: break;
        }
        int len = strlen(val);
        if (len >= buf_len) len = buf_len - 1;
        memcpy(buf, val, len);
        buf[len] = '\0';
        return len;
    }

    return -1;
}

static midi_fx_api_v1_t g_api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = chord_create_instance,
    .destroy_instance = chord_destroy_instance,
    .process_midi = chord_process_midi,
    .tick = chord_tick,
    .set_param = chord_set_param,
    .get_param = chord_get_param
};

midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
