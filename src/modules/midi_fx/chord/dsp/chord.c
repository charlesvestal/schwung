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

/* JSON helpers for state parsing */
static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    if (!json || !key || !out || out_len < 1) return 0;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;
    const char *colon = strchr(pos, ':');
    if (!colon) return 0;
    while (*colon && (*colon == ':' || *colon == ' ' || *colon == '\t')) colon++;
    if (*colon != '"') return 0;
    colon++;
    const char *end = strchr(colon, '"');
    if (!end) return 0;
    int len = (int)(end - colon);
    if (len >= out_len) len = out_len - 1;
    strncpy(out, colon, len);
    out[len] = '\0';
    return len;
}

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

typedef enum {
    CHORD_NONE = 0,
    CHORD_MAJOR,
    CHORD_MINOR,
    CHORD_DIM,
    CHORD_AUG,
    CHORD_SUS2,
    CHORD_SUS4,
    CHORD_MAJ7,
    CHORD_MIN7,
    CHORD_DOM7,
    CHORD_DIM7,
    CHORD_POWER,
    CHORD_FIFTH,
    CHORD_OCTAVE,
    CHORD_ADD9
} chord_type_t;

typedef enum {
    STRUM_UP = 0,
    STRUM_DOWN
} strum_dir_t;

typedef enum {
    INVERSION_ROOT = 0,
    INVERSION_FIRST,
    INVERSION_SECOND,
    INVERSION_THIRD
} inversion_t;

typedef enum {
    VOICING_CLOSE = 0,
    VOICING_OPEN,
    VOICING_DROP2,
    VOICING_DROP3
} voicing_t;

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
    inversion_t inversion;  /* root, 1st, 2nd, 3rd */
    voicing_t voicing;      /* close, open, drop2, drop3 */
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
    inst->inversion = INVERSION_ROOT;
    inst->voicing = VOICING_CLOSE;
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
        case CHORD_DIM:
            intervals[1] = 3;   /* Minor 3rd */
            intervals[2] = 6;   /* Diminished 5th */
            num_notes = 3;
            break;
        case CHORD_AUG:
            intervals[1] = 4;   /* Major 3rd */
            intervals[2] = 8;   /* Augmented 5th */
            num_notes = 3;
            break;
        case CHORD_SUS2:
            intervals[1] = 2;   /* Major 2nd */
            intervals[2] = 7;   /* Perfect 5th */
            num_notes = 3;
            break;
        case CHORD_SUS4:
            intervals[1] = 5;   /* Perfect 4th */
            intervals[2] = 7;   /* Perfect 5th */
            num_notes = 3;
            break;
        case CHORD_MAJ7:
            intervals[1] = 4;   /* Major 3rd */
            intervals[2] = 7;   /* Perfect 5th */
            intervals[3] = 11;  /* Major 7th */
            num_notes = 4;
            break;
        case CHORD_MIN7:
            intervals[1] = 3;   /* Minor 3rd */
            intervals[2] = 7;   /* Perfect 5th */
            intervals[3] = 10;  /* Minor 7th */
            num_notes = 4;
            break;
        case CHORD_DOM7:
            intervals[1] = 4;   /* Major 3rd */
            intervals[2] = 7;   /* Perfect 5th */
            intervals[3] = 10;  /* Minor 7th */
            num_notes = 4;
            break;
        case CHORD_DIM7:
            intervals[1] = 3;   /* Minor 3rd */
            intervals[2] = 6;   /* Diminished 5th */
            intervals[3] = 9;   /* Diminished 7th */
            num_notes = 4;
            break;
        case CHORD_POWER:
            intervals[1] = 7;   /* Perfect 5th */
            num_notes = 2;
            break;
        case CHORD_FIFTH:
            intervals[1] = 7;   /* Perfect 5th */
            intervals[2] = 12;  /* Octave */
            num_notes = 3;
            break;
        case CHORD_OCTAVE:
            intervals[1] = 12;  /* Octave */
            num_notes = 2;
            break;
        case CHORD_ADD9:
            intervals[1] = 4;   /* Major 3rd */
            intervals[2] = 7;   /* Perfect 5th */
            intervals[3] = 14;  /* Major 9th */
            num_notes = 4;
            break;
        default:
            break;
    }

    /* Apply inversion - move lowest notes up an octave */
    if (inst->inversion >= INVERSION_FIRST && num_notes >= 2) {
        intervals[0] += 12;  /* Move root up */
        /* Re-sort by shifting - bubble the raised note to correct position */
        for (int i = 0; i < num_notes - 1; i++) {
            if (intervals[i] > intervals[i + 1]) {
                int tmp = intervals[i];
                intervals[i] = intervals[i + 1];
                intervals[i + 1] = tmp;
            }
        }
    }
    if (inst->inversion >= INVERSION_SECOND && num_notes >= 3) {
        /* Move the new lowest note up */
        intervals[0] += 12;
        for (int i = 0; i < num_notes - 1; i++) {
            if (intervals[i] > intervals[i + 1]) {
                int tmp = intervals[i];
                intervals[i] = intervals[i + 1];
                intervals[i + 1] = tmp;
            }
        }
    }
    if (inst->inversion >= INVERSION_THIRD && num_notes >= 4) {
        /* Move the new lowest note up (for 7th chords) */
        intervals[0] += 12;
        for (int i = 0; i < num_notes - 1; i++) {
            if (intervals[i] > intervals[i + 1]) {
                int tmp = intervals[i];
                intervals[i] = intervals[i + 1];
                intervals[i + 1] = tmp;
            }
        }
    }

    /* Apply voicing */
    if (inst->voicing == VOICING_OPEN && num_notes >= 3) {
        /* Open voicing: drop every other note down an octave starting from 2nd */
        for (int i = 1; i < num_notes; i += 2) {
            intervals[i] -= 12;
        }
        /* Re-sort after voicing changes */
        for (int pass = 0; pass < num_notes - 1; pass++) {
            for (int i = 0; i < num_notes - 1; i++) {
                if (intervals[i] > intervals[i + 1]) {
                    int tmp = intervals[i];
                    intervals[i] = intervals[i + 1];
                    intervals[i + 1] = tmp;
                }
            }
        }
    }
    else if (inst->voicing == VOICING_DROP2 && num_notes >= 3) {
        /* Drop 2: drop second highest note down an octave */
        int second_highest_idx = num_notes - 2;
        intervals[second_highest_idx] -= 12;
        /* Re-sort */
        for (int pass = 0; pass < num_notes - 1; pass++) {
            for (int i = 0; i < num_notes - 1; i++) {
                if (intervals[i] > intervals[i + 1]) {
                    int tmp = intervals[i];
                    intervals[i] = intervals[i + 1];
                    intervals[i + 1] = tmp;
                }
            }
        }
    }
    else if (inst->voicing == VOICING_DROP3 && num_notes >= 4) {
        /* Drop 3: drop third highest note down an octave */
        int third_highest_idx = num_notes - 3;
        intervals[third_highest_idx] -= 12;
        /* Re-sort */
        for (int pass = 0; pass < num_notes - 1; pass++) {
            for (int i = 0; i < num_notes - 1; i++) {
                if (intervals[i] > intervals[i + 1]) {
                    int tmp = intervals[i];
                    intervals[i] = intervals[i + 1];
                    intervals[i + 1] = tmp;
                }
            }
        }
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
        else if (strcmp(val, "dim") == 0) inst->type = CHORD_DIM;
        else if (strcmp(val, "aug") == 0) inst->type = CHORD_AUG;
        else if (strcmp(val, "sus2") == 0) inst->type = CHORD_SUS2;
        else if (strcmp(val, "sus4") == 0) inst->type = CHORD_SUS4;
        else if (strcmp(val, "maj7") == 0) inst->type = CHORD_MAJ7;
        else if (strcmp(val, "min7") == 0) inst->type = CHORD_MIN7;
        else if (strcmp(val, "dom7") == 0) inst->type = CHORD_DOM7;
        else if (strcmp(val, "dim7") == 0) inst->type = CHORD_DIM7;
        else if (strcmp(val, "power") == 0) inst->type = CHORD_POWER;
        else if (strcmp(val, "5th") == 0) inst->type = CHORD_FIFTH;
        else if (strcmp(val, "octave") == 0) inst->type = CHORD_OCTAVE;
        else if (strcmp(val, "add9") == 0) inst->type = CHORD_ADD9;
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
    else if (strcmp(key, "inversion") == 0) {
        if (strcmp(val, "root") == 0) inst->inversion = INVERSION_ROOT;
        else if (strcmp(val, "1st") == 0) inst->inversion = INVERSION_FIRST;
        else if (strcmp(val, "2nd") == 0) inst->inversion = INVERSION_SECOND;
        else if (strcmp(val, "3rd") == 0) inst->inversion = INVERSION_THIRD;
    }
    else if (strcmp(key, "voicing") == 0) {
        if (strcmp(val, "close") == 0) inst->voicing = VOICING_CLOSE;
        else if (strcmp(val, "open") == 0) inst->voicing = VOICING_OPEN;
        else if (strcmp(val, "drop2") == 0) inst->voicing = VOICING_DROP2;
        else if (strcmp(val, "drop3") == 0) inst->voicing = VOICING_DROP3;
    }
    else if (strcmp(key, "state") == 0) {
        /* Restore from JSON state */
        char type_str[16];
        int strum_val;
        char strum_dir_str[8];

        if (json_get_string(val, "type", type_str, sizeof(type_str))) {
            if (strcmp(type_str, "none") == 0) inst->type = CHORD_NONE;
            else if (strcmp(type_str, "major") == 0) inst->type = CHORD_MAJOR;
            else if (strcmp(type_str, "minor") == 0) inst->type = CHORD_MINOR;
            else if (strcmp(type_str, "dim") == 0) inst->type = CHORD_DIM;
            else if (strcmp(type_str, "aug") == 0) inst->type = CHORD_AUG;
            else if (strcmp(type_str, "sus2") == 0) inst->type = CHORD_SUS2;
            else if (strcmp(type_str, "sus4") == 0) inst->type = CHORD_SUS4;
            else if (strcmp(type_str, "maj7") == 0) inst->type = CHORD_MAJ7;
            else if (strcmp(type_str, "min7") == 0) inst->type = CHORD_MIN7;
            else if (strcmp(type_str, "dom7") == 0) inst->type = CHORD_DOM7;
            else if (strcmp(type_str, "dim7") == 0) inst->type = CHORD_DIM7;
            else if (strcmp(type_str, "power") == 0) inst->type = CHORD_POWER;
            else if (strcmp(type_str, "5th") == 0) inst->type = CHORD_FIFTH;
            else if (strcmp(type_str, "octave") == 0) inst->type = CHORD_OCTAVE;
            else if (strcmp(type_str, "add9") == 0) inst->type = CHORD_ADD9;
        }
        if (json_get_int(val, "strum", &strum_val)) {
            if (strum_val < 0) strum_val = 0;
            if (strum_val > 100) strum_val = 100;
            inst->strum_ms = strum_val;
        }
        if (json_get_string(val, "strum_dir", strum_dir_str, sizeof(strum_dir_str))) {
            if (strcmp(strum_dir_str, "up") == 0) inst->strum_dir = STRUM_UP;
            else if (strcmp(strum_dir_str, "down") == 0) inst->strum_dir = STRUM_DOWN;
        }
        char inv_str[8];
        if (json_get_string(val, "inversion", inv_str, sizeof(inv_str))) {
            if (strcmp(inv_str, "root") == 0) inst->inversion = INVERSION_ROOT;
            else if (strcmp(inv_str, "1st") == 0) inst->inversion = INVERSION_FIRST;
            else if (strcmp(inv_str, "2nd") == 0) inst->inversion = INVERSION_SECOND;
            else if (strcmp(inv_str, "3rd") == 0) inst->inversion = INVERSION_THIRD;
        }
        char voicing_str[8];
        if (json_get_string(val, "voicing", voicing_str, sizeof(voicing_str))) {
            if (strcmp(voicing_str, "close") == 0) inst->voicing = VOICING_CLOSE;
            else if (strcmp(voicing_str, "open") == 0) inst->voicing = VOICING_OPEN;
            else if (strcmp(voicing_str, "drop2") == 0) inst->voicing = VOICING_DROP2;
            else if (strcmp(voicing_str, "drop3") == 0) inst->voicing = VOICING_DROP3;
        }
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
            case CHORD_DIM: val = "dim"; break;
            case CHORD_AUG: val = "aug"; break;
            case CHORD_SUS2: val = "sus2"; break;
            case CHORD_SUS4: val = "sus4"; break;
            case CHORD_MAJ7: val = "maj7"; break;
            case CHORD_MIN7: val = "min7"; break;
            case CHORD_DOM7: val = "dom7"; break;
            case CHORD_DIM7: val = "dim7"; break;
            case CHORD_POWER: val = "power"; break;
            case CHORD_FIFTH: val = "5th"; break;
            case CHORD_OCTAVE: val = "octave"; break;
            case CHORD_ADD9: val = "add9"; break;
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
    else if (strcmp(key, "inversion") == 0) {
        const char *val = "root";
        switch (inst->inversion) {
            case INVERSION_FIRST: val = "1st"; break;
            case INVERSION_SECOND: val = "2nd"; break;
            case INVERSION_THIRD: val = "3rd"; break;
            default: break;
        }
        return snprintf(buf, buf_len, "%s", val);
    }
    else if (strcmp(key, "voicing") == 0) {
        const char *val = "close";
        switch (inst->voicing) {
            case VOICING_OPEN: val = "open"; break;
            case VOICING_DROP2: val = "drop2"; break;
            case VOICING_DROP3: val = "drop3"; break;
            default: break;
        }
        return snprintf(buf, buf_len, "%s", val);
    }
    else if (strcmp(key, "state") == 0) {
        const char *type_str = "none";
        switch (inst->type) {
            case CHORD_MAJOR: type_str = "major"; break;
            case CHORD_MINOR: type_str = "minor"; break;
            case CHORD_DIM: type_str = "dim"; break;
            case CHORD_AUG: type_str = "aug"; break;
            case CHORD_SUS2: type_str = "sus2"; break;
            case CHORD_SUS4: type_str = "sus4"; break;
            case CHORD_MAJ7: type_str = "maj7"; break;
            case CHORD_MIN7: type_str = "min7"; break;
            case CHORD_DOM7: type_str = "dom7"; break;
            case CHORD_DIM7: type_str = "dim7"; break;
            case CHORD_POWER: type_str = "power"; break;
            case CHORD_FIFTH: type_str = "5th"; break;
            case CHORD_OCTAVE: type_str = "octave"; break;
            case CHORD_ADD9: type_str = "add9"; break;
            default: break;
        }
        const char *inv_str = "root";
        switch (inst->inversion) {
            case INVERSION_FIRST: inv_str = "1st"; break;
            case INVERSION_SECOND: inv_str = "2nd"; break;
            case INVERSION_THIRD: inv_str = "3rd"; break;
            default: break;
        }
        const char *voicing_str = "close";
        switch (inst->voicing) {
            case VOICING_OPEN: voicing_str = "open"; break;
            case VOICING_DROP2: voicing_str = "drop2"; break;
            case VOICING_DROP3: voicing_str = "drop3"; break;
            default: break;
        }
        return snprintf(buf, buf_len, "{\"type\":\"%s\",\"strum\":%d,\"strum_dir\":\"%s\",\"inversion\":\"%s\",\"voicing\":\"%s\"}",
                        type_str, inst->strum_ms,
                        inst->strum_dir == STRUM_DOWN ? "down" : "up",
                        inv_str, voicing_str);
    }
    else if (strcmp(key, "chain_params") == 0) {
        const char *params = "["
            "{\"key\":\"type\",\"name\":\"Chord Type\",\"type\":\"enum\",\"options\":[\"none\",\"major\",\"minor\",\"dim\",\"aug\",\"sus2\",\"sus4\",\"maj7\",\"min7\",\"dom7\",\"dim7\",\"power\",\"5th\",\"octave\",\"add9\"]},"
            "{\"key\":\"inversion\",\"name\":\"Inversion\",\"type\":\"enum\",\"options\":[\"root\",\"1st\",\"2nd\",\"3rd\"]},"
            "{\"key\":\"voicing\",\"name\":\"Voicing\",\"type\":\"enum\",\"options\":[\"close\",\"open\",\"drop2\",\"drop3\"]},"
            "{\"key\":\"strum\",\"name\":\"Strum\",\"type\":\"int\",\"min\":0,\"max\":100,\"step\":1},"
            "{\"key\":\"strum_dir\",\"name\":\"Strum Dir\",\"type\":\"enum\",\"options\":[\"up\",\"down\"]}"
        "]";
        return snprintf(buf, buf_len, "%s", params);
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
