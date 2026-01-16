/*
 * SEQOMD DSP Plugin - Main
 *
 * 8-track sequencer with per-track timing, MIDI output, and master clock.
 * Inspired by OP-Z architecture.
 */

#include "seq_plugin.h"

/* ============ Global Variable Definitions ============ */

/* Arp step rates array */
const double ARP_STEP_RATES[] = {
    0.5,      /* 1/32 - 32nd notes (2 per step) */
    2.0/3.0,  /* 1/24 - triplet 16ths */
    1.0,      /* 1/16 - 16th notes (1 per step) */
    4.0/3.0,  /* 1/12 - triplet 8ths */
    2.0,      /* 1/8  - 8th notes */
    8.0/3.0,  /* 1/6  - triplet quarters */
    4.0,      /* 1/4  - quarter notes */
    16.0/3.0, /* 1/3  - triplet halves */
    8.0,      /* 1/2  - half notes */
    16.0      /* 1/1  - whole note */
};

/* Scale templates - ordered by preference (simpler scales first) */
const scale_template_t g_scale_templates[NUM_SCALE_TEMPLATES] = {
    { "Minor Penta",    {0, 3, 5, 7, 10, 255, 255, 255}, 5 },
    { "Major Penta",    {0, 2, 4, 7, 9, 255, 255, 255}, 5 },
    { "Blues",          {0, 3, 5, 6, 7, 10, 255, 255}, 6 },
    { "Whole Tone",     {0, 2, 4, 6, 8, 10, 255, 255}, 6 },
    { "Major",          {0, 2, 4, 5, 7, 9, 11, 255}, 7 },
    { "Natural Minor",  {0, 2, 3, 5, 7, 8, 10, 255}, 7 },
    { "Dorian",         {0, 2, 3, 5, 7, 9, 10, 255}, 7 },
    { "Mixolydian",     {0, 2, 4, 5, 7, 9, 10, 255}, 7 },
    { "Phrygian",       {0, 1, 3, 5, 7, 8, 10, 255}, 7 },
    { "Lydian",         {0, 2, 4, 6, 7, 9, 11, 255}, 7 },
    { "Locrian",        {0, 1, 3, 5, 6, 8, 10, 255}, 7 },
    { "Harmonic Minor", {0, 2, 3, 5, 7, 8, 11, 255}, 7 },
    { "Melodic Minor",  {0, 2, 3, 5, 7, 9, 11, 255}, 7 },
    { "Diminished HW",  {0, 1, 3, 4, 6, 7, 9, 10}, 8 },
    { "Diminished WH",  {0, 2, 3, 5, 6, 8, 9, 11}, 8 }
};

/* Host API pointer */
const host_api_v1_t *g_host = NULL;
static plugin_api_v1_t g_plugin_api;

/* Tracks */
track_t g_tracks[NUM_TRACKS];

/* Centralized note scheduler */
scheduled_note_t g_scheduled_notes[MAX_SCHEDULED_NOTES];

/* Global playback state */
int g_bpm = 120;
int g_playing = 0;
int g_send_clock = 1;
double g_clock_phase = 0.0;
double g_global_phase = 0.0;  /* Master clock for all timing */

/* Transpose/chord follow state */
int g_chord_follow[NUM_TRACKS] = {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1};  /* Tracks 5-8, 13-16 follow by default */
int g_current_transpose = 0;  /* Current transpose offset in semitones (legacy, kept for compatibility) */
int g_live_transpose = 0;     /* Live transpose offset (-24 to +24) applied on top of sequence */
uint32_t g_beat_count = 0;    /* Global beat counter for UI sync */

/* Transpose sequence - managed internally by DSP */
transpose_step_t g_transpose_sequence[MAX_TRANSPOSE_STEPS];
int g_transpose_step_count = 0;          /* Number of active steps */
uint32_t g_transpose_total_steps = 0;    /* Sum of all durations */
int8_t *g_transpose_lookup = NULL;       /* Pre-computed lookup table (dynamically allocated) */
uint32_t g_transpose_lookup_size = 0;    /* Size of lookup table */
int g_transpose_lookup_valid = 0;        /* Is lookup table valid? */
int g_transpose_sequence_enabled = 1;    /* Enable/disable transpose sequence automation */
uint32_t g_transpose_step_iteration[MAX_TRANSPOSE_STEPS];  /* Per-step iteration counter for conditions */
int g_transpose_virtual_step = 0;        /* Virtual playhead for jumps (0 to step_count-1) */
uint32_t g_transpose_virtual_entry_step = 0;  /* Beat position when we entered current virtual step */
int g_transpose_first_call = 1;          /* First call flag for initialization */

/* Scale detection state */
int8_t g_detected_scale_root = -1;       /* 0-11, or -1 if none */
int8_t g_detected_scale_index = -1;      /* Index into g_scale_templates, or -1 */
int g_scale_dirty = 1;                   /* Needs recalculation */

/* Random state */
uint32_t g_random_state = 1;

/* ============ Helper Functions ============ */

/* Simple PRNG for probability (xorshift32) */
uint32_t random_next(void) {
    uint32_t x = g_random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_random_state = x;
    return x;
}

/* Returns true with probability (percent/100) */
int random_check(int percent) {
    if (percent >= 100) return 1;
    if (percent <= 0) return 0;
    return (random_next() % 100) < (uint32_t)percent;
}

void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        g_host->log(msg);
    }
}

/* ============ Plugin Callbacks ============ */

static int plugin_on_load(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "SEQOMD loading from: %s", module_dir);
    plugin_log(msg);

    /* Initialize all tracks with default MIDI channels */
    for (int t = 0; t < NUM_TRACKS; t++) {
        init_track(&g_tracks[t], t);  /* Track 0 = ch 0, etc. */
    }

    /* Clear note scheduler */
    memset(g_scheduled_notes, 0, sizeof(g_scheduled_notes));

    /* Parse BPM from defaults if provided */
    if (json_defaults) {
        const char *pos = strstr(json_defaults, "\"bpm\"");
        if (pos) {
            pos = strchr(pos, ':');
            if (pos) {
                g_bpm = atoi(pos + 1);
                if (g_bpm < 20) g_bpm = 20;
                if (g_bpm > 300) g_bpm = 300;
            }
        }
    }

    snprintf(msg, sizeof(msg), "SEQOMD ready: %d tracks, BPM: %d", NUM_TRACKS, g_bpm);
    plugin_log(msg);

    return 0;
}

static void plugin_on_unload(void) {
    plugin_log("SEQOMD unloading");
    all_notes_off();

    /* Free transpose lookup table */
    if (g_transpose_lookup) {
        free(g_transpose_lookup);
        g_transpose_lookup = NULL;
        g_transpose_lookup_size = 0;
    }
}

static void plugin_on_midi(const uint8_t *msg, int len, int source) {
    /* Currently no MIDI input handling - Move is master */
    (void)msg;
    (void)len;
    (void)source;
}

static void plugin_set_param(const char *key, const char *val) {
    /* Global params */
    if (strcmp(key, "bpm") == 0) {
        int new_bpm = atoi(val);
        if (new_bpm >= 20 && new_bpm <= 300) {
            g_bpm = new_bpm;
        }
    }
    else if (strcmp(key, "playing") == 0) {
        int new_playing = atoi(val);
        if (new_playing && !g_playing) {
            /* Starting playback - clear scheduler and reset all tracks */
            clear_scheduled_notes();
            for (int t = 0; t < NUM_TRACKS; t++) {
                g_tracks[t].current_step = get_current_pattern(&g_tracks[t])->loop_start;
                g_tracks[t].phase = 0.0;
                g_tracks[t].loop_count = 0;
                g_tracks[t].next_step_at = 1.0;
            }
            g_clock_phase = 0.0;
            g_global_phase = 0.0;
            g_beat_count = 0;
            g_random_state = 12345;
            /* Reset transpose virtual playhead and per-step iteration counters */
            g_transpose_virtual_step = 0;
            g_transpose_virtual_entry_step = 0;
            memset(g_transpose_step_iteration, 0, sizeof(g_transpose_step_iteration));
            g_transpose_first_call = 1;
            if (g_send_clock) {
                send_midi_start();
                send_midi_clock();
            }
            for (int t = 0; t < NUM_TRACKS; t++) {
                trigger_track_step(&g_tracks[t], t, 0.0);
            }
        } else if (!new_playing && g_playing) {
            all_notes_off();
            if (g_send_clock) {
                send_midi_stop();
            }
        }
        g_playing = new_playing;
    }
    else if (strcmp(key, "send_clock") == 0) {
        g_send_clock = atoi(val);
    }
    else if (strcmp(key, "current_transpose") == 0) {
        g_current_transpose = atoi(val);
    }
    else if (strcmp(key, "live_transpose") == 0) {
        int val_int = atoi(val);
        /* Clamp to -24..+24 range */
        if (val_int < -24) val_int = -24;
        if (val_int > 24) val_int = 24;
        g_live_transpose = val_int;
    }
    /* Transpose sequence params */
    else if (strncmp(key, "transpose_", 10) == 0) {
        set_transpose_param(key, val);
    }
    /* Send CC externally: send_cc_CHANNEL_CC = VALUE */
    else if (strncmp(key, "send_cc_", 8) == 0) {
        int channel = atoi(key + 8);
        const char *cc_part = strchr(key + 8, '_');
        if (cc_part) {
            int cc = atoi(cc_part + 1);
            int value = atoi(val);
            if (channel >= 0 && channel <= 15 && cc >= 0 && cc <= 127) {
                send_cc(cc, value, channel);
            }
        }
    }
    /* Track params */
    else if (strncmp(key, "track_", 6) == 0) {
        int track = atoi(key + 6);
        if (track >= 0 && track < NUM_TRACKS) {
            const char *param = strchr(key + 6, '_');
            if (param) {
                set_track_param(track, param + 1, val);
            }
        }
    }
    /* Legacy single-track params for backward compatibility */
    else if (strncmp(key, "step_", 5) == 0) {
        int step = atoi(key + 5);
        if (step >= 0 && step < NUM_STEPS) {
            const char *param = strchr(key + 5, '_');
            if (param && strcmp(param + 1, "note") == 0) {
                int note = atoi(val);
                if (note >= 0 && note <= 127) {
                    pattern_t *pat = get_current_pattern(&g_tracks[0]);
                    pat->steps[step].num_notes = 0;
                    if (note > 0) {
                        pat->steps[step].notes[0] = (uint8_t)note;
                        pat->steps[step].num_notes = 1;
                    }
                }
            }
        }
    }
}

static int plugin_get_param(const char *key, char *buf, int buf_len) {
    /* Global params */
    if (strcmp(key, "bpm") == 0) {
        return snprintf(buf, buf_len, "%d", g_bpm);
    }
    else if (strcmp(key, "playing") == 0) {
        return snprintf(buf, buf_len, "%d", g_playing);
    }
    else if (strcmp(key, "send_clock") == 0) {
        return snprintf(buf, buf_len, "%d", g_send_clock);
    }
    else if (strcmp(key, "num_tracks") == 0) {
        return snprintf(buf, buf_len, "%d", NUM_TRACKS);
    }
    else if (strcmp(key, "beat_count") == 0) {
        return snprintf(buf, buf_len, "%u", g_beat_count);
    }
    /* Transpose params */
    else if (strcmp(key, "current_transpose") == 0 ||
             strcmp(key, "current_transpose_step") == 0 ||
             strcmp(key, "transpose_sequence_enabled") == 0 ||
             strcmp(key, "transpose_step_count") == 0 ||
             strcmp(key, "transpose_total_steps") == 0) {
        return get_transpose_param(key, buf, buf_len);
    }
    else if (strcmp(key, "live_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", g_live_transpose);
    }
    /* Scale detection params */
    else if (strcmp(key, "detected_scale_root") == 0) {
        if (g_scale_dirty) detect_scale();
        return snprintf(buf, buf_len, "%d", g_detected_scale_root);
    }
    else if (strcmp(key, "detected_scale_name") == 0) {
        if (g_scale_dirty) detect_scale();
        if (g_detected_scale_index >= 0 && g_detected_scale_index < NUM_SCALE_TEMPLATES) {
            return snprintf(buf, buf_len, "%s", g_scale_templates[g_detected_scale_index].name);
        }
        return snprintf(buf, buf_len, "None");
    }
    /* Track params */
    else if (strncmp(key, "track_", 6) == 0) {
        int track = atoi(key + 6);
        if (track >= 0 && track < NUM_TRACKS) {
            const char *param = strchr(key + 6, '_');
            if (param) {
                return get_track_param(track, param + 1, buf, buf_len);
            }
        }
    }
    /* Legacy: current_step returns track 0 */
    else if (strcmp(key, "current_step") == 0) {
        return snprintf(buf, buf_len, "%d", g_tracks[0].current_step);
    }

    return -1;
}

static void plugin_render_block(int16_t *out_interleaved_lr, int frames) {
    /* Safety check */
    if (!out_interleaved_lr || frames <= 0) {
        return;
    }

    /* Output silence - sequencer doesn't generate audio */
    memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));

    if (!g_playing || !g_host) {
        return;
    }

    /* Phase increments (drift-free timing) */
    double step_inc = (double)(g_bpm * 4) / (double)(MOVE_SAMPLE_RATE * 60);
    double clock_inc = (double)(g_bpm * 24) / (double)(MOVE_SAMPLE_RATE * 60);

    /* Track previous global phase for beat detection */
    double prev_global_phase = g_global_phase;

    for (int i = 0; i < frames; i++) {
        g_clock_phase += clock_inc;
        g_global_phase += step_inc;

        /* Track beat count (1 beat = 4 steps) for transpose sequence sync */
        /* Increment when we cross a 4-step boundary */
        uint32_t prev_beat = (uint32_t)(prev_global_phase / 4.0);
        uint32_t curr_beat = (uint32_t)(g_global_phase / 4.0);
        if (curr_beat > prev_beat) {
            g_beat_count = curr_beat;
        }

        /* Update transpose virtual playhead when we cross a step boundary */
        uint32_t prev_step = (uint32_t)prev_global_phase;
        uint32_t curr_step = (uint32_t)g_global_phase;
        if (curr_step > prev_step) {
            update_transpose_virtual_playhead(curr_step);
        }

        prev_global_phase = g_global_phase;

        /* Send MIDI clock at 24 PPQN */
        if (g_send_clock && g_clock_phase >= 1.0) {
            g_clock_phase -= 1.0;
            send_midi_clock();
        }

        /* Process each track - advance steps and schedule notes (including Cut) */
        for (int t = 0; t < NUM_TRACKS; t++) {
            track_t *track = &g_tracks[t];

            /* Per-track phase increment */
            double track_step_inc = step_inc * track->speed;
            track->phase += track_step_inc;

            /* Check step advance (fixed 1.0 step duration - swing is in note delay) */
            if (track->phase >= track->next_step_at) {
                track->phase -= track->next_step_at;
                advance_track(track, t);
            }
        }
    }

    /* Process scheduled notes ONCE per block (not per sample)
     * This reduces iterations from 128*512=65536 to just 512 per block.
     * Timing resolution is ~2.9ms at 128 samples/block, which is better
     * than Elektron's 96 PPQN (~5.2ms at 120 BPM). */
    process_scheduled_notes();
}

/* ============ Plugin Entry Point ============ */

plugin_api_v1_t* move_plugin_init_v1(const host_api_v1_t *host) {
    g_host = host;

    /* Verify API version */
    if (host->api_version != MOVE_PLUGIN_API_VERSION) {
        char msg[128];
        snprintf(msg, sizeof(msg), "API version mismatch: host=%d, plugin=%d",
                 host->api_version, MOVE_PLUGIN_API_VERSION);
        if (host->log) host->log(msg);
        return NULL;
    }

    /* Initialize plugin API struct */
    memset(&g_plugin_api, 0, sizeof(g_plugin_api));
    g_plugin_api.api_version = MOVE_PLUGIN_API_VERSION;
    g_plugin_api.on_load = plugin_on_load;
    g_plugin_api.on_unload = plugin_on_unload;
    g_plugin_api.on_midi = plugin_on_midi;
    g_plugin_api.set_param = plugin_set_param;
    g_plugin_api.get_param = plugin_get_param;
    g_plugin_api.render_block = plugin_render_block;

    plugin_log("SEQOMD initialized");

    return &g_plugin_api;
}
