/*
 * Chord MIDI FX
 *
 * Generates chord notes from single note input.
 * Supports: major, minor, power, octave chords.
 * Optional strum delay between successive chord notes.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

#define SAMPLE_RATE 44100
#define MAX_PENDING 16

typedef enum {
    CHORD_NONE = 0,
    CHORD_MAJOR,
    CHORD_MINOR,
    CHORD_POWER,
    CHORD_OCTAVE
} chord_type_t;

typedef enum {
    STRUM_UP = 0,
    STRUM_DOWN
} strum_dir_t;

typedef struct {
    uint8_t status;         /* Note on/off status byte */
    uint8_t note;
    uint8_t velocity;
    int delay_samples;      /* Samples remaining until trigger */
} pending_note_t;

typedef struct {
    chord_type_t type;
    int strum_ms;           /* 0-100 ms delay between notes */
    strum_dir_t strum_dir;  /* up or down */
    pending_note_t pending[MAX_PENDING];
    int pending_count;
} chord_instance_t;

static const host_api_v1_t *g_host = NULL;

static void* chord_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;

    chord_instance_t *inst = calloc(1, sizeof(chord_instance_t));
    if (!inst) return NULL;

    inst->type = CHORD_MAJOR;
    inst->strum_ms = 0;
    inst->strum_dir = STRUM_UP;
    inst->pending_count = 0;
    return inst;
}

static void chord_destroy_instance(void *instance) {
    if (instance) free(instance);
}

/* Queue a note for delayed emission */
static void queue_note(chord_instance_t *inst, uint8_t status, uint8_t note, uint8_t velocity, int delay_samples) {
    if (inst->pending_count >= MAX_PENDING) return;

    pending_note_t *p = &inst->pending[inst->pending_count++];
    p->status = status;
    p->note = note;
    p->velocity = velocity;
    p->delay_samples = delay_samples;
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
    uint8_t velocity = in_msg[2];
    int count = 0;

    /* Build chord intervals */
    int intervals[4] = {0, 0, 0, 0};  /* Root + up to 3 intervals */
    int num_notes = 1;  /* Always have root */

    switch (inst->type) {
        case CHORD_MAJOR:
            intervals[1] = 4;   /* Major 3rd */
            intervals[2] = 7;   /* Perfect 5th */
            num_notes = 3;
            break;
        case CHORD_MINOR:
            intervals[1] = 3;   /* Minor 3rd */
            intervals[2] = 7;   /* Perfect 5th */
            num_notes = 3;
            break;
        case CHORD_POWER:
            intervals[1] = 7;   /* Perfect 5th */
            num_notes = 2;
            break;
        case CHORD_OCTAVE:
            intervals[1] = 12;  /* Octave */
            num_notes = 2;
            break;
        default:
            break;
    }

    /* Calculate strum delay in samples */
    int strum_samples = (inst->strum_ms * SAMPLE_RATE) / 1000;

    /* Determine note order based on strum direction */
    int order[4];
    if (inst->strum_dir == STRUM_DOWN) {
        /* Reverse order: high to low */
        for (int i = 0; i < num_notes; i++) {
            order[i] = num_notes - 1 - i;
        }
    } else {
        /* Normal order: low to high */
        for (int i = 0; i < num_notes; i++) {
            order[i] = i;
        }
    }

    /* Process each note in the chord */
    for (int i = 0; i < num_notes && count < max_out; i++) {
        int idx = order[i];
        int transposed = (int)note + intervals[idx];
        if (transposed > 127) continue;

        int delay = i * strum_samples;

        if (delay == 0 || strum_samples == 0) {
            /* Emit immediately */
            out_msgs[count][0] = in_msg[0];  /* Preserve channel */
            out_msgs[count][1] = (uint8_t)transposed;
            out_msgs[count][2] = velocity;
            out_lens[count] = 3;
            count++;
        } else {
            /* Queue for later emission */
            queue_note(inst, in_msg[0], (uint8_t)transposed, velocity, delay);
        }
    }

    return count;
}

static int chord_tick(void *instance,
                      int frames, int sample_rate,
                      uint8_t out_msgs[][3], int out_lens[],
                      int max_out) {
    chord_instance_t *inst = (chord_instance_t *)instance;
    if (!inst || inst->pending_count == 0) return 0;

    (void)sample_rate;
    int count = 0;

    /* Process pending notes */
    int i = 0;
    while (i < inst->pending_count) {
        pending_note_t *p = &inst->pending[i];
        p->delay_samples -= frames;

        if (p->delay_samples <= 0 && count < max_out) {
            /* Emit this note */
            out_msgs[count][0] = p->status;
            out_msgs[count][1] = p->note;
            out_msgs[count][2] = p->velocity;
            out_lens[count] = 3;
            count++;

            /* Remove from queue by shifting remaining */
            for (int j = i; j < inst->pending_count - 1; j++) {
                inst->pending[j] = inst->pending[j + 1];
            }
            inst->pending_count--;
            /* Don't increment i - we shifted the next item into current position */
        } else {
            i++;
        }
    }

    return count;
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
    else if (strcmp(key, "strum") == 0) {
        int ms = atoi(val);
        if (ms < 0) ms = 0;
        if (ms > 100) ms = 100;
        inst->strum_ms = ms;
    }
    else if (strcmp(key, "strum_dir") == 0) {
        if (strcmp(val, "up") == 0) inst->strum_dir = STRUM_UP;
        else if (strcmp(val, "down") == 0) inst->strum_dir = STRUM_DOWN;
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
        return snprintf(buf, buf_len, "%s", val);
    }
    else if (strcmp(key, "strum") == 0) {
        return snprintf(buf, buf_len, "%d", inst->strum_ms);
    }
    else if (strcmp(key, "strum_dir") == 0) {
        return snprintf(buf, buf_len, "%s", inst->strum_dir == STRUM_DOWN ? "down" : "up");
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
