/*
 * SF2 Synth DSP Plugin
 *
 * Uses TinySoundFont to render SoundFont (.sf2) files.
 * Provides polyphonic synthesis with preset selection.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Include plugin API */
#include "host/plugin_api_v1.h"

/* TinySoundFont implementation */
#define TSF_IMPLEMENTATION
#include "third_party/tsf.h"

/* Plugin state */
static const host_api_v1_t *g_host = NULL;
static tsf *g_tsf = NULL;
static int g_current_preset = 0;
static int g_preset_count = 0;
static int g_octave_transpose = 0;
static char g_soundfont_path[512] = {0};
static char g_soundfont_name[128] = "No SF2 loaded";
static char g_preset_name[128] = "";
static int g_active_voices = 0;

/* Rendering buffer (float) */
static float g_render_buffer[MOVE_FRAMES_PER_BLOCK * 2];

/* Plugin API implementation */
static plugin_api_v1_t g_plugin_api;

/* Helper: log via host */
static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        g_host->log(msg);
    } else {
        printf("[sf2] %s\n", msg);
    }
}

/* Load a SoundFont file */
static int load_soundfont(const char *path) {
    char msg[256];

    if (g_tsf) {
        tsf_close(g_tsf);
        g_tsf = NULL;
    }

    snprintf(msg, sizeof(msg), "Loading SF2: %s", path);
    plugin_log(msg);

    g_tsf = tsf_load_filename(path);
    if (!g_tsf) {
        snprintf(msg, sizeof(msg), "Failed to load SF2: %s", path);
        plugin_log(msg);
        strcpy(g_soundfont_name, "Load failed");
        g_preset_count = 0;
        return -1;
    }

    /* Set output mode: stereo interleaved, 44100 Hz */
    tsf_set_output(g_tsf, TSF_STEREO_INTERLEAVED, MOVE_SAMPLE_RATE, 0.0f);

    /* Get preset count */
    g_preset_count = tsf_get_presetcount(g_tsf);

    /* Extract filename for display */
    const char *fname = strrchr(path, '/');
    if (fname) {
        strncpy(g_soundfont_name, fname + 1, sizeof(g_soundfont_name) - 1);
    } else {
        strncpy(g_soundfont_name, path, sizeof(g_soundfont_name) - 1);
    }

    strncpy(g_soundfont_path, path, sizeof(g_soundfont_path) - 1);

    snprintf(msg, sizeof(msg), "SF2 loaded: %d presets", g_preset_count);
    plugin_log(msg);

    /* Select first preset */
    g_current_preset = 0;
    if (g_preset_count > 0) {
        const char *name = tsf_get_presetname(g_tsf, g_current_preset);
        if (name) {
            strncpy(g_preset_name, name, sizeof(g_preset_name) - 1);
        }
    }

    return 0;
}

/* Select a preset by index */
static void select_preset(int index) {
    if (!g_tsf || g_preset_count == 0) return;

    if (index < 0) index = g_preset_count - 1;
    if (index >= g_preset_count) index = 0;

    g_current_preset = index;

    /* Get preset name */
    const char *name = tsf_get_presetname(g_tsf, g_current_preset);
    if (name) {
        strncpy(g_preset_name, name, sizeof(g_preset_name) - 1);
    } else {
        snprintf(g_preset_name, sizeof(g_preset_name), "Preset %d", g_current_preset);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Preset %d: %s", g_current_preset, g_preset_name);
    plugin_log(msg);
}

/* === Plugin API callbacks === */

static int plugin_on_load(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "SF2 plugin loading from: %s", module_dir);
    plugin_log(msg);

    /* Try to parse default soundfont path from JSON */
    char default_sf[512] = {0};
    if (json_defaults) {
        /* Simple JSON parsing for soundfont_path */
        const char *pos = strstr(json_defaults, "\"soundfont_path\"");
        if (pos) {
            pos = strchr(pos, ':');
            if (pos) {
                pos = strchr(pos, '"');
                if (pos) {
                    pos++;
                    int i = 0;
                    while (*pos && *pos != '"' && i < (int)sizeof(default_sf) - 1) {
                        default_sf[i++] = *pos++;
                    }
                    default_sf[i] = '\0';
                }
            }
        }
    }

    /* Try to load default soundfont */
    if (default_sf[0]) {
        load_soundfont(default_sf);
    } else {
        /* Try module directory */
        char sf_path[512];
        snprintf(sf_path, sizeof(sf_path), "%s/instrument.sf2", module_dir);
        load_soundfont(sf_path);
    }

    return 0;
}

static void plugin_on_unload(void) {
    plugin_log("SF2 plugin unloading");

    if (g_tsf) {
        tsf_close(g_tsf);
        g_tsf = NULL;
    }
}

static void plugin_on_midi(const uint8_t *msg, int len, int source) {
    if (!g_tsf || len < 2) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t channel = msg[0] & 0x0F;
    uint8_t data1 = msg[1];
    uint8_t data2 = (len > 2) ? msg[2] : 0;

    /* Filter capacitive touch events from Move knobs (notes 0-9 from internal source) */
    int is_note = (status == 0x90 || status == 0x80);
    if (is_note && data1 < 10 && source == MOVE_MIDI_SOURCE_INTERNAL) {
        return; /* Ignore capacitive touch */
    }

    /* Apply octave transpose to notes */
    int note = data1;
    if (is_note) {
        note += g_octave_transpose * 12;
        if (note < 0) note = 0;
        if (note > 127) note = 127;
    }

    switch (status) {
        case 0x90: /* Note On */
            if (data2 > 0) {
                /* Note on with velocity */
                tsf_note_on(g_tsf, g_current_preset, note, data2 / 127.0f);
            } else {
                /* Velocity 0 = note off */
                tsf_note_off(g_tsf, g_current_preset, note);
            }
            break;

        case 0x80: /* Note Off */
            tsf_note_off(g_tsf, g_current_preset, note);
            break;

        case 0xB0: /* Control Change */
            /* CC1 = Modulation wheel */
            if (data1 == 1) {
                /* Could apply vibrato or modulation here */
            }
            /* CC64 = Sustain pedal */
            if (data1 == 64) {
                /* TinySoundFont doesn't have built-in sustain,
                 * but we could implement it manually */
            }
            /* CC123 = All notes off */
            if (data1 == 123) {
                tsf_note_off_all(g_tsf);
            }
            break;

        case 0xA0: /* Polyphonic Aftertouch */
            /* Could map to vibrato depth */
            break;

        case 0xD0: /* Channel Pressure (Aftertouch) */
            /* Could map to vibrato depth */
            break;

        case 0xE0: /* Pitch Bend */
            {
                int bend = ((int)data2 << 7) | data1;
                /* Use channel 0 for all pitch bend (Move uses channel 0) */
                tsf_channel_set_pitchwheel(g_tsf, 0, bend);
            }
            break;
    }
}

static void plugin_set_param(const char *key, const char *val) {
    if (strcmp(key, "soundfont_path") == 0) {
        load_soundfont(val);
    } else if (strcmp(key, "preset") == 0) {
        int preset = atoi(val);
        select_preset(preset);
    } else if (strcmp(key, "octave_transpose") == 0) {
        g_octave_transpose = atoi(val);
        if (g_octave_transpose < -4) g_octave_transpose = -4;
        if (g_octave_transpose > 4) g_octave_transpose = 4;
    }
}

static int plugin_get_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "soundfont_name") == 0) {
        strncpy(buf, g_soundfont_name, buf_len - 1);
        return strlen(buf);
    } else if (strcmp(key, "soundfont_path") == 0) {
        strncpy(buf, g_soundfont_path, buf_len - 1);
        return strlen(buf);
    } else if (strcmp(key, "preset") == 0) {
        return snprintf(buf, buf_len, "%d", g_current_preset);
    } else if (strcmp(key, "preset_name") == 0) {
        strncpy(buf, g_preset_name, buf_len - 1);
        return strlen(buf);
    } else if (strcmp(key, "preset_count") == 0) {
        return snprintf(buf, buf_len, "%d", g_preset_count);
    } else if (strcmp(key, "polyphony") == 0) {
        return snprintf(buf, buf_len, "%d", g_active_voices);
    } else if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", g_octave_transpose);
    }

    return -1;
}

static void plugin_render_block(int16_t *out_interleaved_lr, int frames) {
    if (!g_tsf) {
        /* No soundfont loaded - output silence */
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        g_active_voices = 0;
        return;
    }

    /* Render to float buffer */
    tsf_render_float(g_tsf, g_render_buffer, frames, 0);

    /* Count active voices (approximate) */
    g_active_voices = tsf_active_voice_count(g_tsf);

    /* Convert float to int16 with clipping */
    for (int i = 0; i < frames * 2; i++) {
        float sample = g_render_buffer[i];

        /* Soft clip to prevent harsh distortion */
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;

        /* Convert to int16 */
        out_interleaved_lr[i] = (int16_t)(sample * 32767.0f);
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

    plugin_log("SF2 plugin initialized");

    return &g_plugin_api;
}
