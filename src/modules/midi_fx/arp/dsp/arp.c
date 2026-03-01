/*
 * Arpeggiator MIDI FX
 *
 * Converts held notes into arpeggiated sequences.
 * Supports: up, down, up_down, random modes.
 * Can sync to internal BPM or external MIDI clock.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

#define MAX_ARP_NOTES 16
#define DEFAULT_BPM 120
#define DEFAULT_DIVISION 4.0f  /* 1/16 notes */
#define CLOCKS_PER_QUARTER 24  /* MIDI standard: 24 PPQN */

/* Division values (notes per beat) */
#define DIV_1_4     1.0f      /* Quarter notes */
#define DIV_1_4D    0.6667f   /* Dotted quarter (1 note per 1.5 beats) */
#define DIV_1_4T    1.5f      /* Quarter triplet (3 in 2 beats) */
#define DIV_1_8     2.0f      /* Eighth notes */
#define DIV_1_8D    1.3333f   /* Dotted eighth */
#define DIV_1_8T    3.0f      /* Eighth triplet */
#define DIV_1_16    4.0f      /* Sixteenth notes */
#define DIV_1_16D   2.6667f   /* Dotted sixteenth */
#define DIV_1_16T   6.0f      /* Sixteenth triplet */
#define DIV_1_32    8.0f      /* Thirty-second notes */

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
    ARP_OFF = 0,
    ARP_UP,
    ARP_DOWN,
    ARP_UPDOWN,
    ARP_RANDOM
} arp_mode_t;

typedef enum {
    SYNC_INTERNAL = 0,
    SYNC_CLOCK
} sync_mode_t;

typedef struct {
    arp_mode_t mode;
    int bpm;
    float division;  /* Notes per beat: 1=quarter, 2=8th, 4=16th, etc. */
    sync_mode_t sync_mode;

    /* Held notes (sorted low to high) */
    uint8_t held_notes[MAX_ARP_NOTES];
    uint8_t held_velocities[MAX_ARP_NOTES];
    int held_count;

    /* Sequencer state */
    int step;
    int direction;  /* 1=up, -1=down for up_down mode */
    int sample_counter;
    int samples_per_step;
    int8_t last_note;  /* Currently sounding note, -1 if none */
    uint8_t velocity;  /* Velocity for arp notes */

    /* MIDI clock sync state */
    int clock_counter;     /* Counts 0xF8 clock messages */
    int clocks_per_step;   /* How many clocks per arp step */
    int clock_running;     /* Has start message been received */
} arp_instance_t;

static const host_api_v1_t *g_host = NULL;

static int arp_query_clock_status(const arp_instance_t *inst) {
    if (g_host && g_host->get_clock_status) {
        return g_host->get_clock_status();
    }

    /* Fallback for hosts that don't expose clock status. */
    if (inst && inst->clock_running) {
        return MOVE_CLOCK_STATUS_RUNNING;
    }
    return MOVE_CLOCK_STATUS_STOPPED;
}

static int arp_get_sync_warning(arp_instance_t *inst, char *buf, int buf_len) {
    if (!inst || !buf || buf_len < 1) return -1;

    if (inst->sync_mode != SYNC_CLOCK) {
        buf[0] = '\0';
        return 0;
    }

    int status = arp_query_clock_status(inst);
    if (status == MOVE_CLOCK_STATUS_UNAVAILABLE) {
        return snprintf(buf, buf_len, "Enable MIDI Clock Out in Move settings");
    }
    if (status == MOVE_CLOCK_STATUS_STOPPED) {
        return snprintf(buf, buf_len, "Clock out enabled, transport stopped");
    }

    buf[0] = '\0';
    return 0;
}

static void arp_calc_samples_per_step(arp_instance_t *inst, int sample_rate) {
    if (inst->bpm <= 0) inst->bpm = DEFAULT_BPM;
    if (inst->division <= 0.0f) inst->division = DEFAULT_DIVISION;

    float notes_per_second = (float)inst->bpm / 60.0f * inst->division;
    inst->samples_per_step = (int)((float)sample_rate / notes_per_second);
    if (inst->samples_per_step <= 0) inst->samples_per_step = sample_rate / 8;
}

static void arp_calc_clocks_per_step(arp_instance_t *inst) {
    /* MIDI clock: 24 PPQN (pulses per quarter note) */
    /* division=1 (quarter): 24 clocks, division=2 (8th): 12, division=4 (16th): 6 */
    if (inst->division <= 0.0f) inst->division = DEFAULT_DIVISION;
    inst->clocks_per_step = (int)(CLOCKS_PER_QUARTER / inst->division + 0.5f);
    if (inst->clocks_per_step < 1) inst->clocks_per_step = 1;
}

static void* arp_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;

    arp_instance_t *inst = calloc(1, sizeof(arp_instance_t));
    if (!inst) return NULL;

    inst->mode = ARP_UP;
    inst->bpm = DEFAULT_BPM;
    inst->division = DEFAULT_DIVISION;
    inst->sync_mode = SYNC_INTERNAL;
    inst->held_count = 0;
    inst->step = 0;
    inst->direction = 1;
    inst->sample_counter = 0;
    inst->samples_per_step = 0;  /* Will be calculated on first tick */
    inst->last_note = -1;
    inst->velocity = 100;
    inst->clock_counter = 0;
    inst->clocks_per_step = 6;  /* Default 1/16 notes */
    inst->clock_running = 0;

    return inst;
}

static void arp_destroy_instance(void *instance) {
    if (instance) free(instance);
}

/* Add note to held notes array (sorted insertion) */
static void arp_add_note(arp_instance_t *inst, uint8_t note, uint8_t velocity) {
    if (inst->held_count >= MAX_ARP_NOTES) return;

    /* Find insertion point to keep sorted */
    int i;
    for (i = 0; i < inst->held_count; i++) {
        if (inst->held_notes[i] == note) return;  /* Already held */
        if (inst->held_notes[i] > note) break;
    }

    /* Shift higher notes up */
    for (int j = inst->held_count; j > i; j--) {
        inst->held_notes[j] = inst->held_notes[j - 1];
        inst->held_velocities[j] = inst->held_velocities[j - 1];
    }

    /* Insert new note */
    inst->held_notes[i] = note;
    inst->held_velocities[i] = velocity;
    inst->held_count++;

    /* Use first note's velocity for arp */
    if (inst->held_count == 1) {
        inst->velocity = velocity;
    }
}

/* Remove note from held notes array */
static void arp_remove_note(arp_instance_t *inst, uint8_t note) {
    int found = -1;
    for (int i = 0; i < inst->held_count; i++) {
        if (inst->held_notes[i] == note) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        /* Shift remaining notes down */
        for (int i = found; i < inst->held_count - 1; i++) {
            inst->held_notes[i] = inst->held_notes[i + 1];
            inst->held_velocities[i] = inst->held_velocities[i + 1];
        }
        inst->held_count--;

        /* If no notes held, reset step */
        if (inst->held_count == 0) {
            inst->step = 0;
            inst->direction = 1;
        }
    }
}

/* Get next note in arp sequence and advance step */
static int arp_get_next_note(arp_instance_t *inst) {
    if (inst->held_count == 0) return -1;

    int note_idx = inst->step;
    if (note_idx < 0) note_idx = 0;
    if (note_idx >= inst->held_count) note_idx = inst->held_count - 1;

    /* Advance step based on mode */
    switch (inst->mode) {
        case ARP_UP:
            inst->step = (inst->step + 1) % inst->held_count;
            break;
        case ARP_DOWN:
            inst->step--;
            if (inst->step < 0) inst->step = inst->held_count - 1;
            break;
        case ARP_UPDOWN:
            inst->step += inst->direction;
            if (inst->step >= inst->held_count) {
                inst->step = inst->held_count - 2;
                if (inst->step < 0) inst->step = 0;
                inst->direction = -1;
            } else if (inst->step < 0) {
                inst->step = 1;
                if (inst->step >= inst->held_count) inst->step = 0;
                inst->direction = 1;
            }
            break;
        case ARP_RANDOM:
            if (inst->held_count > 1) {
                inst->step = rand() % inst->held_count;
            }
            break;
        default:
            break;
    }

    return inst->held_notes[note_idx];
}

/* Trigger an arp step - note off for previous, note on for next */
static int arp_trigger_step(arp_instance_t *inst, uint8_t out_msgs[][3], int out_lens[], int max_out) {
    int count = 0;

    /* Note off for previous note */
    if (inst->last_note >= 0 && count < max_out) {
        out_msgs[count][0] = 0x80;
        out_msgs[count][1] = (uint8_t)inst->last_note;
        out_msgs[count][2] = 0;
        out_lens[count] = 3;
        count++;
    }

    /* Get and play next note */
    int next = arp_get_next_note(inst);
    if (next >= 0 && count < max_out) {
        out_msgs[count][0] = 0x90;  /* Note on */
        out_msgs[count][1] = (uint8_t)next;
        out_msgs[count][2] = inst->velocity;
        out_lens[count] = 3;
        count++;
        inst->last_note = (int8_t)next;
    }

    return count;
}

static int arp_process_midi(void *instance,
                            const uint8_t *in_msg, int in_len,
                            uint8_t out_msgs[][3], int out_lens[],
                            int max_out) {
    arp_instance_t *inst = (arp_instance_t *)instance;
    if (!inst || in_len < 1 || max_out < 1) return 0;

    uint8_t status = in_msg[0];
    uint8_t status_type = status & 0xF0;

    /* Handle MIDI clock messages when in clock sync mode */
    if (inst->sync_mode == SYNC_CLOCK) {
        if (status == 0xF8) {  /* Timing clock */
            if (inst->mode != ARP_OFF && inst->held_count > 0 && inst->clock_running) {
                inst->clock_counter++;
                if (inst->clock_counter >= inst->clocks_per_step) {
                    inst->clock_counter = 0;
                    return arp_trigger_step(inst, out_msgs, out_lens, max_out);
                }
            }
            return 0;  /* Don't pass clock through */
        }
        else if (status == 0xFA) {  /* Start */
            inst->clock_counter = 0;
            inst->step = 0;
            inst->direction = 1;
            inst->clock_running = 1;
            return 0;
        }
        else if (status == 0xFC) {  /* Stop */
            inst->clock_running = 0;
            /* Send note off if playing */
            if (inst->last_note >= 0 && max_out > 0) {
                out_msgs[0][0] = 0x80;
                out_msgs[0][1] = (uint8_t)inst->last_note;
                out_msgs[0][2] = 0;
                out_lens[0] = 3;
                inst->last_note = -1;
                return 1;
            }
            return 0;
        }
        else if (status == 0xFB) {  /* Continue */
            inst->clock_running = 1;
            return 0;
        }
    }

    /* Only intercept note on/off when arp is active */
    if (inst->mode != ARP_OFF && (status_type == 0x90 || status_type == 0x80) && in_len >= 3) {
        uint8_t note = in_msg[1];
        uint8_t velocity = in_msg[2];

        if (status_type == 0x90 && velocity > 0) {
            arp_add_note(inst, note, velocity);
        } else {
            arp_remove_note(inst, note);
        }
        /* Don't output - arp generates notes via tick or clock */
        return 0;
    }

    /* Pass through other messages */
    out_msgs[0][0] = in_msg[0];
    out_msgs[0][1] = in_len > 1 ? in_msg[1] : 0;
    out_msgs[0][2] = in_len > 2 ? in_msg[2] : 0;
    out_lens[0] = in_len;
    return 1;
}

static int arp_tick(void *instance,
                    int frames, int sample_rate,
                    uint8_t out_msgs[][3], int out_lens[],
                    int max_out) {
    arp_instance_t *inst = (arp_instance_t *)instance;
    if (!inst) return 0;

    int count = 0;

    /* If arp is off or no notes held, send note-off for any sounding note */
    if (inst->mode == ARP_OFF || inst->held_count == 0) {
        if (inst->last_note >= 0 && count < max_out) {
            out_msgs[count][0] = 0x80;  /* Note off */
            out_msgs[count][1] = (uint8_t)inst->last_note;
            out_msgs[count][2] = 0;
            out_lens[count] = 3;
            count++;
            inst->last_note = -1;
        }
        return count;
    }

    /* If using clock sync, timing is driven by process_midi, not tick */
    if (inst->sync_mode == SYNC_CLOCK) {
        return 0;
    }

    /* Internal timing mode */
    /* Calculate samples per step if not set */
    if (inst->samples_per_step <= 0) {
        arp_calc_samples_per_step(inst, sample_rate);
    }

    inst->sample_counter += frames;

    if (inst->sample_counter >= inst->samples_per_step) {
        inst->sample_counter -= inst->samples_per_step;
        return arp_trigger_step(inst, out_msgs, out_lens, max_out);
    }

    return count;
}

static void arp_set_param(void *instance, const char *key, const char *val) {
    arp_instance_t *inst = (arp_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "mode") == 0) {
        if (strcmp(val, "off") == 0) inst->mode = ARP_OFF;
        else if (strcmp(val, "up") == 0) inst->mode = ARP_UP;
        else if (strcmp(val, "down") == 0) inst->mode = ARP_DOWN;
        else if (strcmp(val, "up_down") == 0) inst->mode = ARP_UPDOWN;
        else if (strcmp(val, "random") == 0) inst->mode = ARP_RANDOM;
    }
    else if (strcmp(key, "bpm") == 0) {
        /* Ignore BPM changes when synced to external clock */
        if (inst->sync_mode == SYNC_CLOCK) return;
        inst->bpm = atoi(val);
        if (inst->bpm < 40) inst->bpm = 40;
        if (inst->bpm > 240) inst->bpm = 240;
        inst->samples_per_step = 0;  /* Recalculate on next tick */
    }
    else if (strcmp(key, "division") == 0) {
        if (strcmp(val, "1/4") == 0) inst->division = DIV_1_4;
        else if (strcmp(val, "1/4.") == 0) inst->division = DIV_1_4D;
        else if (strcmp(val, "1/4T") == 0) inst->division = DIV_1_4T;
        else if (strcmp(val, "1/8") == 0) inst->division = DIV_1_8;
        else if (strcmp(val, "1/8.") == 0) inst->division = DIV_1_8D;
        else if (strcmp(val, "1/8T") == 0) inst->division = DIV_1_8T;
        else if (strcmp(val, "1/16") == 0) inst->division = DIV_1_16;
        else if (strcmp(val, "1/16.") == 0) inst->division = DIV_1_16D;
        else if (strcmp(val, "1/16T") == 0) inst->division = DIV_1_16T;
        else if (strcmp(val, "1/32") == 0) inst->division = DIV_1_32;
        else inst->division = (float)atof(val);
        inst->samples_per_step = 0;  /* Recalculate on next tick */
        arp_calc_clocks_per_step(inst);  /* Also update clock timing */
    }
    else if (strcmp(key, "sync") == 0) {
        if (strcmp(val, "internal") == 0) {
            inst->sync_mode = SYNC_INTERNAL;
        }
        else if (strcmp(val, "clock") == 0) {
            inst->sync_mode = SYNC_CLOCK;
            inst->clock_counter = 0;
            inst->clock_running = 1;  /* Assume clock is running */
            arp_calc_clocks_per_step(inst);
        }
    }
    else if (strcmp(key, "state") == 0) {
        /* Restore from JSON state */
        char mode_str[16];
        int bpm_val;
        char division_str[8];
        char sync_str[16];

        if (json_get_string(val, "mode", mode_str, sizeof(mode_str))) {
            if (strcmp(mode_str, "off") == 0) inst->mode = ARP_OFF;
            else if (strcmp(mode_str, "up") == 0) inst->mode = ARP_UP;
            else if (strcmp(mode_str, "down") == 0) inst->mode = ARP_DOWN;
            else if (strcmp(mode_str, "up_down") == 0) inst->mode = ARP_UPDOWN;
            else if (strcmp(mode_str, "random") == 0) inst->mode = ARP_RANDOM;
        }
        if (json_get_int(val, "bpm", &bpm_val)) {
            if (bpm_val < 40) bpm_val = 40;
            if (bpm_val > 240) bpm_val = 240;
            inst->bpm = bpm_val;
            inst->samples_per_step = 0;
        }
        if (json_get_string(val, "division", division_str, sizeof(division_str))) {
            if (strcmp(division_str, "1/4") == 0) inst->division = DIV_1_4;
            else if (strcmp(division_str, "1/4.") == 0) inst->division = DIV_1_4D;
            else if (strcmp(division_str, "1/4T") == 0) inst->division = DIV_1_4T;
            else if (strcmp(division_str, "1/8") == 0) inst->division = DIV_1_8;
            else if (strcmp(division_str, "1/8.") == 0) inst->division = DIV_1_8D;
            else if (strcmp(division_str, "1/8T") == 0) inst->division = DIV_1_8T;
            else if (strcmp(division_str, "1/16") == 0) inst->division = DIV_1_16;
            else if (strcmp(division_str, "1/16.") == 0) inst->division = DIV_1_16D;
            else if (strcmp(division_str, "1/16T") == 0) inst->division = DIV_1_16T;
            else if (strcmp(division_str, "1/32") == 0) inst->division = DIV_1_32;
            inst->samples_per_step = 0;
            arp_calc_clocks_per_step(inst);
        }
        if (json_get_string(val, "sync", sync_str, sizeof(sync_str))) {
            if (strcmp(sync_str, "internal") == 0) inst->sync_mode = SYNC_INTERNAL;
            else if (strcmp(sync_str, "clock") == 0) {
                inst->sync_mode = SYNC_CLOCK;
                inst->clock_counter = 0;
                inst->clock_running = 1;
                arp_calc_clocks_per_step(inst);
            }
        }
    }
}

static int arp_get_param(void *instance, const char *key, char *buf, int buf_len) {
    arp_instance_t *inst = (arp_instance_t *)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    if (strcmp(key, "mode") == 0) {
        const char *val = "off";
        switch (inst->mode) {
            case ARP_UP: val = "up"; break;
            case ARP_DOWN: val = "down"; break;
            case ARP_UPDOWN: val = "up_down"; break;
            case ARP_RANDOM: val = "random"; break;
            default: break;
        }
        return snprintf(buf, buf_len, "%s", val);
    }
    else if (strcmp(key, "bpm") == 0) {
        /* When synced to external clock, BPM is not used - show SYNC */
        if (inst->sync_mode == SYNC_CLOCK) {
            return snprintf(buf, buf_len, "SYNC");
        }
        return snprintf(buf, buf_len, "%d", inst->bpm);
    }
    else if (strcmp(key, "division") == 0) {
        const char *val = "1/16";
        if (inst->division <= DIV_1_4D + 0.01f) val = "1/4.";
        else if (inst->division <= DIV_1_4 + 0.01f) val = "1/4";
        else if (inst->division <= DIV_1_8D + 0.01f) val = "1/8.";
        else if (inst->division <= DIV_1_4T + 0.01f) val = "1/4T";
        else if (inst->division <= DIV_1_8 + 0.01f) val = "1/8";
        else if (inst->division <= DIV_1_16D + 0.01f) val = "1/16.";
        else if (inst->division <= DIV_1_8T + 0.01f) val = "1/8T";
        else if (inst->division <= DIV_1_16 + 0.01f) val = "1/16";
        else if (inst->division <= DIV_1_16T + 0.01f) val = "1/16T";
        else if (inst->division <= DIV_1_32 + 0.01f) val = "1/32";
        return snprintf(buf, buf_len, "%s", val);
    }
    else if (strcmp(key, "sync") == 0) {
        return snprintf(buf, buf_len, "%s", inst->sync_mode == SYNC_CLOCK ? "clock" : "internal");
    }
    else if (strcmp(key, "error") == 0) {
        return arp_get_sync_warning(inst, buf, buf_len);
    }
    else if (strcmp(key, "state") == 0) {
        const char *mode_str = "off";
        switch (inst->mode) {
            case ARP_UP: mode_str = "up"; break;
            case ARP_DOWN: mode_str = "down"; break;
            case ARP_UPDOWN: mode_str = "up_down"; break;
            case ARP_RANDOM: mode_str = "random"; break;
            default: break;
        }
        const char *div_str = "1/16";
        if (inst->division <= DIV_1_4D + 0.01f) div_str = "1/4.";
        else if (inst->division <= DIV_1_4 + 0.01f) div_str = "1/4";
        else if (inst->division <= DIV_1_8D + 0.01f) div_str = "1/8.";
        else if (inst->division <= DIV_1_4T + 0.01f) div_str = "1/4T";
        else if (inst->division <= DIV_1_8 + 0.01f) div_str = "1/8";
        else if (inst->division <= DIV_1_16D + 0.01f) div_str = "1/16.";
        else if (inst->division <= DIV_1_8T + 0.01f) div_str = "1/8T";
        else if (inst->division <= DIV_1_16 + 0.01f) div_str = "1/16";
        else if (inst->division <= DIV_1_16T + 0.01f) div_str = "1/16T";
        else if (inst->division <= DIV_1_32 + 0.01f) div_str = "1/32";
        return snprintf(buf, buf_len, "{\"mode\":\"%s\",\"bpm\":%d,\"division\":\"%s\",\"sync\":\"%s\"}",
                        mode_str, inst->bpm, div_str,
                        inst->sync_mode == SYNC_CLOCK ? "clock" : "internal");
    }
    else if (strcmp(key, "chain_params") == 0) {
        const char *params = "["
            "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"off\",\"up\",\"down\",\"up_down\",\"random\"]},"
            "{\"key\":\"bpm\",\"name\":\"BPM\",\"type\":\"int\",\"min\":40,\"max\":240,\"step\":1},"
            "{\"key\":\"division\",\"name\":\"Division\",\"type\":\"enum\",\"options\":[\"1/4.\",\"1/4\",\"1/4T\",\"1/8.\",\"1/8\",\"1/8T\",\"1/16.\",\"1/16\",\"1/16T\",\"1/32\"]},"
            "{\"key\":\"sync\",\"name\":\"Sync\",\"type\":\"enum\",\"options\":[\"internal\",\"clock\"]}"
        "]";
        return snprintf(buf, buf_len, "%s", params);
    }

    return -1;
}

static midi_fx_api_v1_t g_api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = arp_create_instance,
    .destroy_instance = arp_destroy_instance,
    .process_midi = arp_process_midi,
    .tick = arp_tick,
    .set_param = arp_set_param,
    .get_param = arp_get_param
};

midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
