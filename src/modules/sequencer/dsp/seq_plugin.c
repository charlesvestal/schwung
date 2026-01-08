/*
 * Step Sequencer DSP Plugin
 *
 * Sample-accurate 16-step sequencer with MIDI output.
 * Timing runs in render_block() at audio rate for tight sync.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Include plugin API */
#include "host/plugin_api_v1.h"

/* Plugin state */
static const host_api_v1_t *g_host = NULL;

/* Sequence state */
#define NUM_STEPS 16
#define DEFAULT_NOTE 60  /* Middle C */
#define DEFAULT_VELOCITY 100
#define DEFAULT_GATE 50  /* 50% gate */

static int g_step_note[NUM_STEPS];      /* MIDI note (0 = off, 1-127 = note) */
static int g_step_velocity[NUM_STEPS];  /* 1-127 */
static int g_step_gate[NUM_STEPS];      /* Gate length % (1-100) */

/* Playback state */
static int g_bpm = 120;
static int g_playing = 0;
static int g_current_step = 0;
static uint64_t g_sample_counter = 0;
static int g_note_on_active = 0;
static int g_last_note = -1;
static int g_last_channel = 0;

/* MIDI channel (0-15) */
static int g_midi_channel = 0;

/* Plugin API struct */
static plugin_api_v1_t g_plugin_api;

/* Helper: log via host */
static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        g_host->log(msg);
    }
}

/* Helper: send MIDI note on via external port */
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

/* Helper: send MIDI note off via external port */
static void send_note_off(int note, int channel) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = {
        0x28,                           /* Cable 2, CIN 0x8 (Note Off) */
        (uint8_t)(0x80 | (channel & 0x0F)),
        (uint8_t)(note & 0x7F),
        0x00                            /* Velocity 0 */
    };
    g_host->midi_send_external(msg, 4);
}

/* Helper: send all notes off */
static void all_notes_off(void) {
    if (g_last_note >= 0) {
        send_note_off(g_last_note, g_last_channel);
        g_last_note = -1;
    }
    g_note_on_active = 0;
}

/* Initialize step defaults */
static void init_steps(void) {
    for (int i = 0; i < NUM_STEPS; i++) {
        g_step_note[i] = 0;  /* Off by default */
        g_step_velocity[i] = DEFAULT_VELOCITY;
        g_step_gate[i] = DEFAULT_GATE;
    }
}

/* === Plugin API callbacks === */

static int plugin_on_load(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Sequencer plugin loading from: %s", module_dir);
    plugin_log(msg);

    init_steps();

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

    snprintf(msg, sizeof(msg), "Sequencer ready, BPM: %d", g_bpm);
    plugin_log(msg);

    return 0;
}

static void plugin_on_unload(void) {
    plugin_log("Sequencer plugin unloading");
    all_notes_off();
}

static void plugin_on_midi(const uint8_t *msg, int len, int source) {
    /*
     * DSP plugin receives MIDI but we handle input in JS UI.
     * Could use this for external clock sync in the future.
     */
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
            /* Starting playback */
            g_current_step = 0;
            g_sample_counter = 0;
            g_note_on_active = 0;
        } else if (!new_playing && g_playing) {
            /* Stopping playback */
            all_notes_off();
        }
        g_playing = new_playing;
    }
    else if (strcmp(key, "midi_channel") == 0) {
        int ch = atoi(val);
        if (ch >= 0 && ch <= 15) {
            g_midi_channel = ch;
        }
    }
    else if (strncmp(key, "step_", 5) == 0) {
        /* Parse step parameters: step_N_note, step_N_vel, step_N_gate */
        int step = atoi(key + 5);
        if (step >= 0 && step < NUM_STEPS) {
            const char *param = strchr(key + 5, '_');
            if (param) {
                param++; /* Skip underscore */
                if (strcmp(param, "note") == 0) {
                    int note = atoi(val);
                    if (note >= 0 && note <= 127) {
                        g_step_note[step] = note;
                    }
                } else if (strcmp(param, "vel") == 0) {
                    int vel = atoi(val);
                    if (vel >= 1 && vel <= 127) {
                        g_step_velocity[step] = vel;
                    }
                } else if (strcmp(param, "gate") == 0) {
                    int gate = atoi(val);
                    if (gate >= 1 && gate <= 100) {
                        g_step_gate[step] = gate;
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
    else if (strcmp(key, "current_step") == 0) {
        return snprintf(buf, buf_len, "%d", g_current_step);
    }
    else if (strcmp(key, "midi_channel") == 0) {
        return snprintf(buf, buf_len, "%d", g_midi_channel);
    }
    else if (strncmp(key, "step_", 5) == 0) {
        int step = atoi(key + 5);
        if (step >= 0 && step < NUM_STEPS) {
            const char *param = strchr(key + 5, '_');
            if (param) {
                param++;
                if (strcmp(param, "note") == 0) {
                    return snprintf(buf, buf_len, "%d", g_step_note[step]);
                } else if (strcmp(param, "vel") == 0) {
                    return snprintf(buf, buf_len, "%d", g_step_velocity[step]);
                } else if (strcmp(param, "gate") == 0) {
                    return snprintf(buf, buf_len, "%d", g_step_gate[step]);
                }
            }
        }
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

    /* Calculate timing
     * At 120 BPM, one beat = 0.5 seconds = 22050 samples
     * 16th note = beat / 4 = 5512.5 samples
     * samples_per_step = (sample_rate * 60) / (bpm * 4)
     */
    int samples_per_step = (MOVE_SAMPLE_RATE * 60) / (g_bpm * 4);
    int gate_samples = (samples_per_step * g_step_gate[g_current_step]) / 100;

    for (int i = 0; i < frames; i++) {
        g_sample_counter++;

        /* Check if we need to send note off (gate ended) */
        if (g_note_on_active && g_sample_counter >= (uint64_t)gate_samples) {
            if (g_last_note >= 0) {
                send_note_off(g_last_note, g_last_channel);
                g_last_note = -1;
            }
            g_note_on_active = 0;
        }

        /* Check if we need to advance to next step */
        if (g_sample_counter >= (uint64_t)samples_per_step) {
            g_sample_counter = 0;
            g_current_step = (g_current_step + 1) % NUM_STEPS;

            /* Trigger note if step is active */
            if (g_step_note[g_current_step] > 0) {
                /* Send note off for previous note if still on */
                if (g_last_note >= 0) {
                    send_note_off(g_last_note, g_last_channel);
                }

                /* Send note on */
                int note = g_step_note[g_current_step];
                int vel = g_step_velocity[g_current_step];
                send_note_on(note, vel, g_midi_channel);

                g_last_note = note;
                g_last_channel = g_midi_channel;
                g_note_on_active = 1;
            }
        }
    }
}

/* === Plugin entry point === */

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

    plugin_log("Sequencer plugin initialized");

    return &g_plugin_api;
}
