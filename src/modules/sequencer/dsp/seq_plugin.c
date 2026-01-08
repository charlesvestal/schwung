/*
 * Multi-Track Step Sequencer DSP Plugin
 *
 * 8-track sequencer with per-track timing, MIDI output, and master clock.
 * Inspired by OP-Z architecture.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Include plugin API */
#include "host/plugin_api_v1.h"

/* ============ Constants ============ */

#define NUM_TRACKS 8
#define NUM_STEPS 16
#define NUM_PATTERNS 8
#define MAX_NOTES_PER_STEP 4
#define MAX_PENDING_NOTES 64

#define DEFAULT_VELOCITY 100
#define DEFAULT_GATE 50

/* MIDI real-time messages */
#define MIDI_CLOCK      0xF8
#define MIDI_START      0xFA
#define MIDI_CONTINUE   0xFB
#define MIDI_STOP       0xFC

/* ============ Data Structures ============ */

/* Step data */
typedef struct {
    uint8_t notes[MAX_NOTES_PER_STEP];  /* Up to 4 notes per step (0 = empty slot) */
    uint8_t num_notes;                   /* Number of active notes */
    uint8_t velocity;                    /* 1-127 */
    uint8_t gate;                        /* Gate length as % of step (1-100) */
    /* Future: length, probability, ratchet, condition */
} step_t;

/* Pattern data - contains steps and loop points */
typedef struct {
    step_t steps[NUM_STEPS];
    uint8_t loop_start;     /* Loop start step (0-15) */
    uint8_t loop_end;       /* Loop end step (0-15), wraps after this */
} pattern_t;

/* Track data */
typedef struct {
    pattern_t patterns[NUM_PATTERNS];  /* 8 patterns per track */
    uint8_t current_pattern;           /* Currently active pattern (0-7) */
    uint8_t midi_channel;   /* 0-15 */
    uint8_t length;         /* 1-64 steps (for now max 16) */
    uint8_t current_step;
    uint8_t muted;
    double phase;           /* Step phase accumulator */
    double gate_phase;      /* Gate timing */
    int8_t last_notes[MAX_NOTES_PER_STEP];  /* Last triggered notes (-1 = none) */
    uint8_t num_last_notes;                  /* Number of active notes */
    uint8_t note_on_active;
} track_t;

/* Pending note for overlapping long notes */
typedef struct {
    uint8_t note;
    uint8_t channel;
    double off_phase;       /* Global phase when note should end */
    uint8_t active;
} pending_note_t;

/* ============ Plugin State ============ */

static const host_api_v1_t *g_host = NULL;
static plugin_api_v1_t g_plugin_api;

/* Tracks */
static track_t g_tracks[NUM_TRACKS];

/* Pending notes for long/overlapping notes */
static pending_note_t g_pending_notes[MAX_PENDING_NOTES];

/* Global playback state */
static int g_bpm = 120;
static int g_playing = 0;
static int g_send_clock = 1;
static double g_clock_phase = 0.0;
static double g_global_phase = 0.0;  /* For pending note timing */

/* ============ Helpers ============ */

static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        g_host->log(msg);
    }
}

static void send_note_on(int note, int velocity, int channel) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = {
        0x29,                           /* Cable 2, CIN 0x9 (Note On) */
        (uint8_t)(0x90 | (channel & 0x0F)),
        (uint8_t)(note & 0x7F),
        (uint8_t)(velocity & 0x7F)
    };
    g_host->midi_send_external(msg, 4);
}

static void send_note_off(int note, int channel) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = {
        0x28,                           /* Cable 2, CIN 0x8 (Note Off) */
        (uint8_t)(0x80 | (channel & 0x0F)),
        (uint8_t)(note & 0x7F),
        0x00
    };
    g_host->midi_send_external(msg, 4);
}

static void send_midi_clock(void) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = { 0x2F, MIDI_CLOCK, 0x00, 0x00 };
    g_host->midi_send_external(msg, 4);
}

static void send_midi_start(void) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = { 0x2F, MIDI_START, 0x00, 0x00 };
    g_host->midi_send_external(msg, 4);
    plugin_log("MIDI Start");
}

static void send_midi_stop(void) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = { 0x2F, MIDI_STOP, 0x00, 0x00 };
    g_host->midi_send_external(msg, 4);
    plugin_log("MIDI Stop");
}

/* Send note-off for all active notes */
static void all_notes_off(void) {
    /* Stop track notes */
    for (int t = 0; t < NUM_TRACKS; t++) {
        for (int i = 0; i < g_tracks[t].num_last_notes; i++) {
            if (g_tracks[t].last_notes[i] >= 0) {
                send_note_off(g_tracks[t].last_notes[i], g_tracks[t].midi_channel);
                g_tracks[t].last_notes[i] = -1;
            }
        }
        g_tracks[t].num_last_notes = 0;
        g_tracks[t].note_on_active = 0;
    }

    /* Stop pending notes */
    for (int i = 0; i < MAX_PENDING_NOTES; i++) {
        if (g_pending_notes[i].active) {
            send_note_off(g_pending_notes[i].note, g_pending_notes[i].channel);
            g_pending_notes[i].active = 0;
        }
    }
}

/* ============ Track Functions ============ */

static void init_pattern(pattern_t *pattern) {
    pattern->loop_start = 0;
    pattern->loop_end = NUM_STEPS - 1;
    for (int i = 0; i < NUM_STEPS; i++) {
        for (int n = 0; n < MAX_NOTES_PER_STEP; n++) {
            pattern->steps[i].notes[n] = 0;
        }
        pattern->steps[i].num_notes = 0;
        pattern->steps[i].velocity = DEFAULT_VELOCITY;
        pattern->steps[i].gate = DEFAULT_GATE;
    }
}

static void init_track(track_t *track, int channel) {
    memset(track, 0, sizeof(track_t));
    track->midi_channel = channel;
    track->length = NUM_STEPS;
    track->current_pattern = 0;

    for (int i = 0; i < MAX_NOTES_PER_STEP; i++) {
        track->last_notes[i] = -1;
    }

    /* Initialize all patterns */
    for (int p = 0; p < NUM_PATTERNS; p++) {
        init_pattern(&track->patterns[p]);
    }
}

/* Get current pattern for a track */
static inline pattern_t* get_current_pattern(track_t *track) {
    return &track->patterns[track->current_pattern];
}

static void trigger_track_step(track_t *track) {
    /* Send note off for previous notes if still on */
    for (int i = 0; i < track->num_last_notes; i++) {
        if (track->last_notes[i] >= 0) {
            send_note_off(track->last_notes[i], track->midi_channel);
            track->last_notes[i] = -1;
        }
    }
    track->num_last_notes = 0;
    track->note_on_active = 0;
    track->gate_phase = 0.0;

    /* Skip if muted or step is empty */
    if (track->muted) return;

    pattern_t *pattern = get_current_pattern(track);
    step_t *step = &pattern->steps[track->current_step];
    if (step->num_notes == 0) return;

    /* Trigger all notes in step */
    for (int i = 0; i < step->num_notes && i < MAX_NOTES_PER_STEP; i++) {
        if (step->notes[i] > 0) {
            send_note_on(step->notes[i], step->velocity, track->midi_channel);
            track->last_notes[track->num_last_notes] = step->notes[i];
            track->num_last_notes++;
        }
    }
    if (track->num_last_notes > 0) {
        track->note_on_active = 1;
    }
}

static void advance_track(track_t *track) {
    /* Advance step, respecting loop points from current pattern */
    pattern_t *pattern = get_current_pattern(track);
    if (track->current_step >= pattern->loop_end) {
        track->current_step = pattern->loop_start;
    } else {
        track->current_step++;
    }
    trigger_track_step(track);
}

/* ============ Plugin Callbacks ============ */

static int plugin_on_load(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Multi-track sequencer loading from: %s", module_dir);
    plugin_log(msg);

    /* Initialize all tracks with default MIDI channels */
    for (int t = 0; t < NUM_TRACKS; t++) {
        init_track(&g_tracks[t], t);  /* Track 0 = ch 0, etc. */
    }

    /* Clear pending notes */
    memset(g_pending_notes, 0, sizeof(g_pending_notes));

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

    snprintf(msg, sizeof(msg), "Sequencer ready: %d tracks, BPM: %d", NUM_TRACKS, g_bpm);
    plugin_log(msg);

    return 0;
}

static void plugin_on_unload(void) {
    plugin_log("Sequencer unloading");
    all_notes_off();
}

static void plugin_on_midi(const uint8_t *msg, int len, int source) {
    /* Currently no MIDI input handling - Move is master */
    (void)msg;
    (void)len;
    (void)source;
}

static void plugin_set_param(const char *key, const char *val) {
    if (strcmp(key, "bpm") == 0) {
        int new_bpm = atoi(val);
        if (new_bpm >= 20 && new_bpm <= 300) {
            g_bpm = new_bpm;
        }
    }
    else if (strcmp(key, "playing") == 0) {
        int new_playing = atoi(val);
        if (new_playing && !g_playing) {
            /* Starting playback - reset all tracks to their loop start */
            for (int t = 0; t < NUM_TRACKS; t++) {
                g_tracks[t].current_step = get_current_pattern(&g_tracks[t])->loop_start;
                g_tracks[t].phase = 0.0;
                g_tracks[t].gate_phase = 0.0;
                g_tracks[t].note_on_active = 0;
                g_tracks[t].num_last_notes = 0;
                for (int n = 0; n < MAX_NOTES_PER_STEP; n++) {
                    g_tracks[t].last_notes[n] = -1;
                }
            }
            g_clock_phase = 0.0;
            g_global_phase = 0.0;

            if (g_send_clock) {
                send_midi_start();
                send_midi_clock();
            }

            /* Trigger first step on all tracks */
            for (int t = 0; t < NUM_TRACKS; t++) {
                trigger_track_step(&g_tracks[t]);
            }
        } else if (!new_playing && g_playing) {
            /* Stopping playback */
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
    /* Track-specific parameters: track_T_step_S_note, track_T_step_S_vel, etc. */
    else if (strncmp(key, "track_", 6) == 0) {
        int track = atoi(key + 6);
        if (track >= 0 && track < NUM_TRACKS) {
            const char *param = strchr(key + 6, '_');
            if (param) {
                param++;

                /* Track-level params: track_T_channel, track_T_mute, track_T_length */
                if (strcmp(param, "channel") == 0) {
                    int ch = atoi(val);
                    if (ch >= 0 && ch <= 15) {
                        g_tracks[track].midi_channel = ch;
                    }
                }
                else if (strcmp(param, "mute") == 0) {
                    g_tracks[track].muted = atoi(val) ? 1 : 0;
                }
                else if (strcmp(param, "length") == 0) {
                    int len = atoi(val);
                    if (len >= 1 && len <= NUM_STEPS) {
                        g_tracks[track].length = len;
                    }
                }
                else if (strcmp(param, "loop_start") == 0) {
                    int start = atoi(val);
                    if (start >= 0 && start < NUM_STEPS) {
                        get_current_pattern(&g_tracks[track])->loop_start = start;
                    }
                }
                else if (strcmp(param, "loop_end") == 0) {
                    int end = atoi(val);
                    if (end >= 0 && end < NUM_STEPS) {
                        get_current_pattern(&g_tracks[track])->loop_end = end;
                    }
                }
                else if (strcmp(param, "pattern") == 0) {
                    int pat = atoi(val);
                    if (pat >= 0 && pat < NUM_PATTERNS) {
                        g_tracks[track].current_pattern = pat;
                    }
                }
                /* Preview note - play a note immediately for auditioning */
                else if (strcmp(param, "preview_note") == 0) {
                    int note = atoi(val);
                    if (note > 0 && note <= 127) {
                        send_note_on(note, DEFAULT_VELOCITY, g_tracks[track].midi_channel);
                    }
                }
                else if (strcmp(param, "preview_note_off") == 0) {
                    int note = atoi(val);
                    if (note > 0 && note <= 127) {
                        send_note_off(note, g_tracks[track].midi_channel);
                    }
                }
                /* Step-level params: track_T_step_S_note, etc. */
                else if (strncmp(param, "step_", 5) == 0) {
                    int step = atoi(param + 5);
                    if (step >= 0 && step < NUM_STEPS) {
                        const char *step_param = strchr(param + 5, '_');
                        if (step_param) {
                            step_param++;
                            /* Set single note (backward compat - clears other notes) */
                            if (strcmp(step_param, "note") == 0) {
                                int note = atoi(val);
                                step_t *s = &get_current_pattern(&g_tracks[track])->steps[step];
                                if (note == 0) {
                                    /* Clear all notes */
                                    s->num_notes = 0;
                                    for (int n = 0; n < MAX_NOTES_PER_STEP; n++) {
                                        s->notes[n] = 0;
                                    }
                                } else if (note >= 1 && note <= 127) {
                                    /* Set as single note */
                                    s->notes[0] = note;
                                    s->num_notes = 1;
                                    for (int n = 1; n < MAX_NOTES_PER_STEP; n++) {
                                        s->notes[n] = 0;
                                    }
                                }
                            }
                            /* Add a note to the step (for chords) */
                            else if (strcmp(step_param, "add_note") == 0) {
                                int note = atoi(val);
                                if (note >= 1 && note <= 127) {
                                    step_t *s = &get_current_pattern(&g_tracks[track])->steps[step];
                                    /* Check if note already exists */
                                    int exists = 0;
                                    for (int n = 0; n < s->num_notes; n++) {
                                        if (s->notes[n] == note) {
                                            exists = 1;
                                            break;
                                        }
                                    }
                                    /* Add if not exists and room available */
                                    if (!exists && s->num_notes < MAX_NOTES_PER_STEP) {
                                        s->notes[s->num_notes] = note;
                                        s->num_notes++;
                                    }
                                }
                            }
                            /* Remove a note from the step */
                            else if (strcmp(step_param, "remove_note") == 0) {
                                int note = atoi(val);
                                if (note >= 1 && note <= 127) {
                                    step_t *s = &get_current_pattern(&g_tracks[track])->steps[step];
                                    for (int n = 0; n < s->num_notes; n++) {
                                        if (s->notes[n] == note) {
                                            /* Shift remaining notes down */
                                            for (int m = n; m < s->num_notes - 1; m++) {
                                                s->notes[m] = s->notes[m + 1];
                                            }
                                            s->notes[s->num_notes - 1] = 0;
                                            s->num_notes--;
                                            break;
                                        }
                                    }
                                }
                            }
                            /* Clear all notes from step */
                            else if (strcmp(step_param, "clear") == 0) {
                                step_t *s = &get_current_pattern(&g_tracks[track])->steps[step];
                                s->num_notes = 0;
                                for (int n = 0; n < MAX_NOTES_PER_STEP; n++) {
                                    s->notes[n] = 0;
                                }
                            }
                            else if (strcmp(step_param, "vel") == 0) {
                                int vel = atoi(val);
                                if (vel >= 1 && vel <= 127) {
                                    get_current_pattern(&g_tracks[track])->steps[step].velocity = vel;
                                }
                            }
                            else if (strcmp(step_param, "gate") == 0) {
                                int gate = atoi(val);
                                if (gate >= 1 && gate <= 100) {
                                    get_current_pattern(&g_tracks[track])->steps[step].gate = gate;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    /* Legacy single-track params for backward compatibility */
    else if (strncmp(key, "step_", 5) == 0) {
        int step = atoi(key + 5);
        if (step >= 0 && step < NUM_STEPS) {
            const char *param = strchr(key + 5, '_');
            if (param) {
                param++;
                /* Apply to track 0 for backward compat */
                if (strcmp(param, "note") == 0) {
                    int note = atoi(val);
                    if (note >= 0 && note <= 127) {
                        /* Backward compat: clear step and set single note */
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
}

static int plugin_get_param(const char *key, char *buf, int buf_len) {
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
    /* Track params */
    else if (strncmp(key, "track_", 6) == 0) {
        int track = atoi(key + 6);
        if (track >= 0 && track < NUM_TRACKS) {
            const char *param = strchr(key + 6, '_');
            if (param) {
                param++;
                if (strcmp(param, "channel") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].midi_channel);
                }
                else if (strcmp(param, "mute") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].muted);
                }
                else if (strcmp(param, "length") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].length);
                }
                else if (strcmp(param, "loop_start") == 0) {
                    return snprintf(buf, buf_len, "%d", get_current_pattern(&g_tracks[track])->loop_start);
                }
                else if (strcmp(param, "loop_end") == 0) {
                    return snprintf(buf, buf_len, "%d", get_current_pattern(&g_tracks[track])->loop_end);
                }
                else if (strcmp(param, "pattern") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].current_pattern);
                }
                else if (strcmp(param, "current_step") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].current_step);
                }
                else if (strncmp(param, "step_", 5) == 0) {
                    int step = atoi(param + 5);
                    if (step >= 0 && step < NUM_STEPS) {
                        const char *step_param = strchr(param + 5, '_');
                        if (step_param) {
                            step_param++;
                            /* Return first note (backward compat) */
                            if (strcmp(step_param, "note") == 0) {
                                step_t *s = &get_current_pattern(&g_tracks[track])->steps[step];
                                return snprintf(buf, buf_len, "%d", s->num_notes > 0 ? s->notes[0] : 0);
                            }
                            /* Return all notes as comma-separated */
                            else if (strcmp(step_param, "notes") == 0) {
                                step_t *s = &get_current_pattern(&g_tracks[track])->steps[step];
                                if (s->num_notes == 0) {
                                    return snprintf(buf, buf_len, "");
                                }
                                int pos = 0;
                                for (int n = 0; n < s->num_notes && pos < buf_len - 4; n++) {
                                    if (n > 0) {
                                        pos += snprintf(buf + pos, buf_len - pos, ",");
                                    }
                                    pos += snprintf(buf + pos, buf_len - pos, "%d", s->notes[n]);
                                }
                                return pos;
                            }
                            else if (strcmp(step_param, "num_notes") == 0) {
                                return snprintf(buf, buf_len, "%d", get_current_pattern(&g_tracks[track])->steps[step].num_notes);
                            }
                            else if (strcmp(step_param, "vel") == 0) {
                                return snprintf(buf, buf_len, "%d", get_current_pattern(&g_tracks[track])->steps[step].velocity);
                            }
                            else if (strcmp(step_param, "gate") == 0) {
                                return snprintf(buf, buf_len, "%d", get_current_pattern(&g_tracks[track])->steps[step].gate);
                            }
                        }
                    }
                }
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

    for (int i = 0; i < frames; i++) {
        g_clock_phase += clock_inc;
        g_global_phase += step_inc;

        /* Send MIDI clock at 24 PPQN */
        if (g_send_clock && g_clock_phase >= 1.0) {
            g_clock_phase -= 1.0;
            send_midi_clock();
        }

        /* Process each track */
        for (int t = 0; t < NUM_TRACKS; t++) {
            track_t *track = &g_tracks[t];

            track->phase += step_inc;

            /* Check gate off */
            if (track->note_on_active) {
                track->gate_phase += step_inc;
                double gate_length = (double)get_current_pattern(track)->steps[track->current_step].gate / 100.0;
                if (track->gate_phase >= gate_length) {
                    /* Send note off for all active notes */
                    for (int n = 0; n < track->num_last_notes; n++) {
                        if (track->last_notes[n] >= 0) {
                            send_note_off(track->last_notes[n], track->midi_channel);
                            track->last_notes[n] = -1;
                        }
                    }
                    track->num_last_notes = 0;
                    track->note_on_active = 0;
                }
            }

            /* Check step advance */
            if (track->phase >= 1.0) {
                track->phase -= 1.0;
                advance_track(track);
            }
        }
    }
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

    plugin_log("Multi-track sequencer initialized");

    return &g_plugin_api;
}
