/*
 * Signal Chain Host DSP Plugin
 *
 * Orchestrates a signal chain: Input → MIDI FX → Sound Generator → Audio FX → Output
 * Phase 5: Arpeggiator support
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <dirent.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v1.h"

/* Limits */
#define MAX_PATCHES 32      /* Max patches to list in browser */
#define MAX_AUDIO_FX 4      /* Max FX loaded per active chain */
#define MAX_PATH_LEN 256
#define MAX_NAME_LEN 64

/* Chord types */
typedef enum {
    CHORD_NONE = 0,
    CHORD_MAJOR,     /* root + major 3rd + 5th */
    CHORD_MINOR,     /* root + minor 3rd + 5th */
    CHORD_POWER,     /* root + 5th */
    CHORD_OCTAVE     /* root + octave */
} chord_type_t;

/* Arpeggiator modes */
typedef enum {
    ARP_OFF = 0,
    ARP_UP,          /* Low to high */
    ARP_DOWN,        /* High to low */
    ARP_UPDOWN,      /* Up then down */
    ARP_RANDOM       /* Random order */
} arp_mode_t;

/* Arpeggiator constants */
#define MAX_ARP_NOTES 16
#define SAMPLE_RATE 44100
#define FRAMES_PER_BLOCK 128

/* Patch info */
typedef struct {
    char name[MAX_NAME_LEN];
    char path[MAX_PATH_LEN];
    char synth_module[MAX_NAME_LEN];
    int synth_preset;
    char audio_fx[MAX_AUDIO_FX][MAX_NAME_LEN];
    int audio_fx_count;
    chord_type_t chord_type;
    arp_mode_t arp_mode;
    int arp_tempo_bpm;       /* BPM for arpeggiator */
    int arp_note_division;   /* 1=quarter, 2=8th, 4=16th */
} patch_info_t;

/* Host API provided by main host */
static const host_api_v1_t *g_host = NULL;

/* Sub-plugin state */
static void *g_synth_handle = NULL;
static plugin_api_v1_t *g_synth_plugin = NULL;
static char g_current_synth_module[MAX_NAME_LEN] = "";

/* Audio FX state */
static void *g_fx_handles[MAX_AUDIO_FX];
static audio_fx_api_v1_t *g_fx_plugins[MAX_AUDIO_FX];
static int g_fx_count = 0;

/* Patch state */
static patch_info_t g_patches[MAX_PATCHES];
static int g_patch_count = 0;
static int g_current_patch = 0;
static char g_module_dir[MAX_PATH_LEN] = "";

/* MIDI FX state - Chords */
static chord_type_t g_chord_type = CHORD_NONE;

/* MIDI FX state - Arpeggiator */
static arp_mode_t g_arp_mode = ARP_OFF;
static uint8_t g_arp_held_notes[MAX_ARP_NOTES];  /* Sorted low to high */
static uint8_t g_arp_held_velocities[MAX_ARP_NOTES];
static int g_arp_held_count = 0;
static int g_arp_step = 0;           /* Current step in arp sequence */
static int g_arp_direction = 1;      /* 1=up, -1=down (for up-down mode) */
static int g_arp_sample_counter = 0;
static int g_arp_samples_per_step = 0;
static int8_t g_arp_last_note = -1;  /* Currently sounding arp note, -1 if none */
static uint8_t g_arp_velocity = 100; /* Velocity for arp notes */

/* Mute countdown after patch switch (in blocks) to drain old audio */
static int g_mute_countdown = 0;
#define MUTE_BLOCKS_AFTER_SWITCH 8  /* ~23ms at 44100Hz, 128 frames/block */

/* Our host API for sub-plugins (forwards to main host) */
static host_api_v1_t g_subplugin_host_api;

/* Plugin API we return to host */
static plugin_api_v1_t g_plugin_api;

/* Logging helper */
static void chain_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[chain] %s", msg);
        g_host->log(buf);
    } else {
        printf("[chain] %s\n", msg);
    }
}

/* === Arpeggiator Functions === */

/* Add a note to the held notes array (sorted insertion) */
static void arp_add_note(uint8_t note, uint8_t velocity) {
    if (g_arp_held_count >= MAX_ARP_NOTES) return;

    /* Find insertion point to keep sorted */
    int i;
    for (i = 0; i < g_arp_held_count; i++) {
        if (g_arp_held_notes[i] == note) return;  /* Already held */
        if (g_arp_held_notes[i] > note) break;
    }

    /* Shift higher notes up */
    for (int j = g_arp_held_count; j > i; j--) {
        g_arp_held_notes[j] = g_arp_held_notes[j - 1];
        g_arp_held_velocities[j] = g_arp_held_velocities[j - 1];
    }

    /* Insert new note */
    g_arp_held_notes[i] = note;
    g_arp_held_velocities[i] = velocity;
    g_arp_held_count++;

    /* Use first note's velocity for arp */
    if (g_arp_held_count == 1) {
        g_arp_velocity = velocity;
    }
}

/* Remove a note from the held notes array */
static void arp_remove_note(uint8_t note) {
    int found = -1;
    for (int i = 0; i < g_arp_held_count; i++) {
        if (g_arp_held_notes[i] == note) {
            found = i;
            break;
        }
    }

    if (found < 0) return;

    /* Shift remaining notes down */
    for (int i = found; i < g_arp_held_count - 1; i++) {
        g_arp_held_notes[i] = g_arp_held_notes[i + 1];
        g_arp_held_velocities[i] = g_arp_held_velocities[i + 1];
    }
    g_arp_held_count--;

    /* Reset step if we removed notes */
    if (g_arp_step >= g_arp_held_count && g_arp_held_count > 0) {
        g_arp_step = 0;
    }
}

/* Reset arpeggiator state */
static void arp_reset(void) {
    g_arp_held_count = 0;
    g_arp_step = 0;
    g_arp_direction = 1;
    g_arp_sample_counter = 0;
    g_arp_last_note = -1;
}

/* Calculate samples per step from BPM and division */
static void arp_set_tempo(int bpm, int division) {
    /* division: 1=quarter notes, 2=8th notes, 4=16th notes */
    if (bpm <= 0) bpm = 120;
    if (division <= 0) division = 4;

    /* beats per second = bpm / 60 */
    /* notes per second = beats_per_second * division */
    /* samples per note = sample_rate / notes_per_second */
    float notes_per_second = (float)bpm / 60.0f * (float)division;
    g_arp_samples_per_step = (int)(SAMPLE_RATE / notes_per_second);
}

/* Get the next note in the arp sequence */
static int arp_get_next_note(void) {
    if (g_arp_held_count == 0) return -1;

    int note_index = g_arp_step;

    /* Advance step based on mode */
    switch (g_arp_mode) {
        case ARP_UP:
            g_arp_step = (g_arp_step + 1) % g_arp_held_count;
            break;

        case ARP_DOWN:
            g_arp_step = g_arp_step - 1;
            if (g_arp_step < 0) g_arp_step = g_arp_held_count - 1;
            break;

        case ARP_UPDOWN:
            g_arp_step += g_arp_direction;
            if (g_arp_step >= g_arp_held_count) {
                g_arp_step = g_arp_held_count - 2;
                if (g_arp_step < 0) g_arp_step = 0;
                g_arp_direction = -1;
            } else if (g_arp_step < 0) {
                g_arp_step = 1;
                if (g_arp_step >= g_arp_held_count) g_arp_step = 0;
                g_arp_direction = 1;
            }
            break;

        case ARP_RANDOM:
            g_arp_step = rand() % g_arp_held_count;
            break;

        default:
            break;
    }

    return g_arp_held_notes[note_index];
}

/* Send note to synth (helper for arp) */
static void arp_send_note(uint8_t note, uint8_t velocity, int note_on) {
    if (!g_synth_plugin || !g_synth_plugin->on_midi) return;

    uint8_t msg[3];
    msg[0] = note_on ? 0x90 : 0x80;  /* Note on/off, channel 0 */
    msg[1] = note;
    msg[2] = velocity;
    g_synth_plugin->on_midi(msg, 3, 0);
}

/* Arp tick - called each render block */
static void arp_tick(int frames) {
    if (g_arp_mode == ARP_OFF || g_arp_held_count == 0) {
        /* If arp was playing but now stopped, send note off */
        if (g_arp_last_note >= 0) {
            arp_send_note(g_arp_last_note, 0, 0);
            g_arp_last_note = -1;
        }
        return;
    }

    g_arp_sample_counter += frames;

    if (g_arp_sample_counter >= g_arp_samples_per_step) {
        g_arp_sample_counter -= g_arp_samples_per_step;

        /* Note off for previous note */
        if (g_arp_last_note >= 0) {
            arp_send_note(g_arp_last_note, 0, 0);
        }

        /* Get next note and play it */
        int next_note = arp_get_next_note();
        if (next_note >= 0) {
            arp_send_note(next_note, g_arp_velocity, 1);
            g_arp_last_note = next_note;
        }
    }
}

/* Load a sound generator sub-plugin */
static int load_synth(const char *module_path, const char *config_json) {
    char msg[256];

    /* Build path to dsp.so */
    char dsp_path[512];
    snprintf(dsp_path, sizeof(dsp_path), "%s/dsp.so", module_path);

    snprintf(msg, sizeof(msg), "Loading synth from: %s", dsp_path);
    chain_log(msg);

    /* Open the shared library */
    g_synth_handle = dlopen(dsp_path, RTLD_NOW | RTLD_LOCAL);
    if (!g_synth_handle) {
        snprintf(msg, sizeof(msg), "dlopen failed: %s", dlerror());
        chain_log(msg);
        return -1;
    }

    /* Get init function */
    move_plugin_init_v1_fn init_fn = (move_plugin_init_v1_fn)dlsym(g_synth_handle, MOVE_PLUGIN_INIT_SYMBOL);
    if (!init_fn) {
        snprintf(msg, sizeof(msg), "dlsym failed: %s", dlerror());
        chain_log(msg);
        dlclose(g_synth_handle);
        g_synth_handle = NULL;
        return -1;
    }

    /* Initialize sub-plugin with our forwarding host API */
    g_synth_plugin = init_fn(&g_subplugin_host_api);
    if (!g_synth_plugin) {
        chain_log("Synth plugin init returned NULL");
        dlclose(g_synth_handle);
        g_synth_handle = NULL;
        return -1;
    }

    /* Verify API version */
    if (g_synth_plugin->api_version != MOVE_PLUGIN_API_VERSION) {
        snprintf(msg, sizeof(msg), "Synth API version mismatch: %d vs %d",
                 g_synth_plugin->api_version, MOVE_PLUGIN_API_VERSION);
        chain_log(msg);
        dlclose(g_synth_handle);
        g_synth_handle = NULL;
        g_synth_plugin = NULL;
        return -1;
    }

    /* Call on_load */
    if (g_synth_plugin->on_load) {
        int ret = g_synth_plugin->on_load(module_path, config_json);
        if (ret != 0) {
            snprintf(msg, sizeof(msg), "Synth on_load failed: %d", ret);
            chain_log(msg);
            dlclose(g_synth_handle);
            g_synth_handle = NULL;
            g_synth_plugin = NULL;
            return -1;
        }
    }

    chain_log("Synth loaded successfully");
    return 0;
}

/* Unload synth sub-plugin */
static void unload_synth(void) {
    if (g_synth_plugin && g_synth_plugin->on_unload) {
        g_synth_plugin->on_unload();
    }
    if (g_synth_handle) {
        dlclose(g_synth_handle);
        g_synth_handle = NULL;
    }
    g_synth_plugin = NULL;
    g_current_synth_module[0] = '\0';
}

/* Load an audio FX plugin */
static int load_audio_fx(const char *fx_name) {
    char msg[256];

    if (g_fx_count >= MAX_AUDIO_FX) {
        chain_log("Max audio FX reached");
        return -1;
    }

    /* Build path to FX .so - look in chain/audio_fx/<name>/<name>.so */
    char fx_path[MAX_PATH_LEN];
    snprintf(fx_path, sizeof(fx_path), "%s/audio_fx/%s/%s.so",
             g_module_dir, fx_name, fx_name);

    snprintf(msg, sizeof(msg), "Loading audio FX: %s", fx_path);
    chain_log(msg);

    /* Open the shared library */
    void *handle = dlopen(fx_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        snprintf(msg, sizeof(msg), "dlopen failed: %s", dlerror());
        chain_log(msg);
        return -1;
    }

    /* Get init function */
    audio_fx_init_v1_fn init_fn = (audio_fx_init_v1_fn)dlsym(handle, AUDIO_FX_INIT_SYMBOL);
    if (!init_fn) {
        snprintf(msg, sizeof(msg), "dlsym failed: %s", dlerror());
        chain_log(msg);
        dlclose(handle);
        return -1;
    }

    /* Initialize FX plugin */
    audio_fx_api_v1_t *fx = init_fn(&g_subplugin_host_api);
    if (!fx) {
        chain_log("FX plugin init returned NULL");
        dlclose(handle);
        return -1;
    }

    /* Verify API version */
    if (fx->api_version != AUDIO_FX_API_VERSION) {
        snprintf(msg, sizeof(msg), "FX API version mismatch: %d vs %d",
                 fx->api_version, AUDIO_FX_API_VERSION);
        chain_log(msg);
        dlclose(handle);
        return -1;
    }

    /* Call on_load */
    if (fx->on_load) {
        char fx_dir[MAX_PATH_LEN];
        snprintf(fx_dir, sizeof(fx_dir), "%s/audio_fx/%s", g_module_dir, fx_name);
        if (fx->on_load(fx_dir, NULL) != 0) {
            chain_log("FX on_load failed");
            dlclose(handle);
            return -1;
        }
    }

    /* Store in array */
    g_fx_handles[g_fx_count] = handle;
    g_fx_plugins[g_fx_count] = fx;
    g_fx_count++;

    snprintf(msg, sizeof(msg), "Audio FX loaded: %s (slot %d)", fx_name, g_fx_count - 1);
    chain_log(msg);

    return 0;
}

/* Unload all audio FX */
static void unload_all_audio_fx(void) {
    for (int i = 0; i < g_fx_count; i++) {
        if (g_fx_plugins[i] && g_fx_plugins[i]->on_unload) {
            g_fx_plugins[i]->on_unload();
        }
        if (g_fx_handles[i]) {
            dlclose(g_fx_handles[i]);
        }
        g_fx_handles[i] = NULL;
        g_fx_plugins[i] = NULL;
    }
    g_fx_count = 0;
    chain_log("All audio FX unloaded");
}

/* Simple JSON string extraction - finds "key": "value" and returns value */
static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    /* Find the colon after the key */
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return -1;

    /* Skip whitespace and find opening quote */
    while (*pos && (*pos == ' ' || *pos == '\t' || *pos == ':')) pos++;
    if (*pos != '"') return -1;
    pos++;

    /* Copy until closing quote */
    int i = 0;
    while (*pos && *pos != '"' && i < out_len - 1) {
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return 0;
}

/* Simple JSON integer extraction - finds "key": number */
static int json_get_int(const char *json, const char *key, int *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    /* Find the colon after the key */
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return -1;

    /* Skip whitespace */
    while (*pos && (*pos == ' ' || *pos == '\t' || *pos == ':')) pos++;

    /* Parse integer */
    *out = atoi(pos);
    return 0;
}

/* Parse a patch file and populate patch_info */
static int parse_patch_file(const char *path, patch_info_t *patch) {
    char msg[256];

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(msg, sizeof(msg), "Failed to open patch: %s", path);
        chain_log(msg);
        return -1;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size > 4096) {
        fclose(f);
        chain_log("Patch file too large");
        return -1;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return -1;
    }

    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);

    /* Parse fields */
    strncpy(patch->path, path, MAX_PATH_LEN - 1);

    if (json_get_string(json, "name", patch->name, MAX_NAME_LEN) != 0) {
        strcpy(patch->name, "Unnamed");
    }

    if (json_get_string(json, "module", patch->synth_module, MAX_NAME_LEN) != 0) {
        strcpy(patch->synth_module, "sf2");  /* Default to SF2 */
    }

    if (json_get_int(json, "preset", &patch->synth_preset) != 0) {
        patch->synth_preset = 0;
    }

    /* Parse audio_fx - look for "audio_fx" and extract FX names */
    patch->audio_fx_count = 0;
    const char *fx_pos = strstr(json, "\"audio_fx\"");
    if (fx_pos) {
        /* Find opening bracket */
        const char *bracket = strchr(fx_pos, '[');
        if (bracket) {
            bracket++;
            /* Parse FX entries - look for "type": "name" patterns */
            while (patch->audio_fx_count < MAX_AUDIO_FX) {
                const char *type_pos = strstr(bracket, "\"type\"");
                if (!type_pos) break;

                /* Find closing bracket to make sure we're still in array */
                const char *end_bracket = strchr(fx_pos, ']');
                if (end_bracket && type_pos > end_bracket) break;

                /* Extract the type value */
                const char *colon = strchr(type_pos, ':');
                if (!colon) break;

                /* Find opening quote */
                const char *quote1 = strchr(colon, '"');
                if (!quote1) break;
                quote1++;

                /* Find closing quote */
                const char *quote2 = strchr(quote1, '"');
                if (!quote2) break;

                int len = quote2 - quote1;
                if (len > 0 && len < MAX_NAME_LEN) {
                    strncpy(patch->audio_fx[patch->audio_fx_count], quote1, len);
                    patch->audio_fx[patch->audio_fx_count][len] = '\0';
                    patch->audio_fx_count++;
                }

                bracket = quote2 + 1;
            }
        }
    }

    /* Parse chord type from midi_fx section */
    patch->chord_type = CHORD_NONE;
    char chord_str[MAX_NAME_LEN] = "";
    if (json_get_string(json, "chord", chord_str, MAX_NAME_LEN) == 0) {
        if (strcmp(chord_str, "major") == 0) {
            patch->chord_type = CHORD_MAJOR;
        } else if (strcmp(chord_str, "minor") == 0) {
            patch->chord_type = CHORD_MINOR;
        } else if (strcmp(chord_str, "power") == 0) {
            patch->chord_type = CHORD_POWER;
        } else if (strcmp(chord_str, "octave") == 0) {
            patch->chord_type = CHORD_OCTAVE;
        }
    }

    /* Parse arpeggiator settings from midi_fx section */
    patch->arp_mode = ARP_OFF;
    patch->arp_tempo_bpm = 120;
    patch->arp_note_division = 4;  /* 16th notes default */

    char arp_str[MAX_NAME_LEN] = "";
    if (json_get_string(json, "arp", arp_str, MAX_NAME_LEN) == 0) {
        if (strcmp(arp_str, "up") == 0) {
            patch->arp_mode = ARP_UP;
        } else if (strcmp(arp_str, "down") == 0) {
            patch->arp_mode = ARP_DOWN;
        } else if (strcmp(arp_str, "up_down") == 0 || strcmp(arp_str, "updown") == 0) {
            patch->arp_mode = ARP_UPDOWN;
        } else if (strcmp(arp_str, "random") == 0) {
            patch->arp_mode = ARP_RANDOM;
        }
    }

    /* Parse arp tempo and division */
    json_get_int(json, "arp_bpm", &patch->arp_tempo_bpm);
    json_get_int(json, "arp_division", &patch->arp_note_division);

    free(json);

    snprintf(msg, sizeof(msg), "Parsed patch: %s -> %s preset %d, %d FX, chord=%d, arp=%d",
             patch->name, patch->synth_module, patch->synth_preset,
             patch->audio_fx_count, patch->chord_type, patch->arp_mode);
    chain_log(msg);

    return 0;
}

/* Scan patches directory and populate patch list */
static int scan_patches(const char *module_dir) {
    char patches_dir[MAX_PATH_LEN];
    char msg[256];

    snprintf(patches_dir, sizeof(patches_dir), "%s/patches", module_dir);

    snprintf(msg, sizeof(msg), "Scanning patches in: %s", patches_dir);
    chain_log(msg);

    DIR *dir = opendir(patches_dir);
    if (!dir) {
        chain_log("No patches directory found");
        return 0;
    }

    g_patch_count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && g_patch_count < MAX_PATCHES) {
        /* Look for .json files */
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext, ".json") != 0) continue;

        char patch_path[MAX_PATH_LEN];
        snprintf(patch_path, sizeof(patch_path), "%s/%s", patches_dir, entry->d_name);

        if (parse_patch_file(patch_path, &g_patches[g_patch_count]) == 0) {
            g_patch_count++;
        }
    }

    closedir(dir);

    snprintf(msg, sizeof(msg), "Found %d patches", g_patch_count);
    chain_log(msg);

    return g_patch_count;
}

/* Send all-notes-off to synth to prevent stuck notes */
static void synth_panic(void) {
    if (!g_synth_plugin || !g_synth_plugin->on_midi) return;

    /* Send All Sound Off (CC 120) and All Notes Off (CC 123) on all channels */
    for (int ch = 0; ch < 16; ch++) {
        uint8_t all_sound_off[] = { (uint8_t)(0xB0 | ch), 120, 0 };
        uint8_t all_notes_off[] = { (uint8_t)(0xB0 | ch), 123, 0 };
        g_synth_plugin->on_midi(all_sound_off, 3, 0);
        g_synth_plugin->on_midi(all_notes_off, 3, 0);
    }
    chain_log("Sent panic (all notes off)");
}

/* Load a patch by index */
static int load_patch(int index) {
    char msg[256];

    if (index < 0 || index >= g_patch_count) {
        snprintf(msg, sizeof(msg), "Invalid patch index: %d", index);
        chain_log(msg);
        return -1;
    }

    patch_info_t *patch = &g_patches[index];

    snprintf(msg, sizeof(msg), "Loading patch: %s", patch->name);
    chain_log(msg);

    /* Panic before any changes to prevent stuck notes */
    synth_panic();

    /* Check if we need to switch synth modules */
    if (strcmp(g_current_synth_module, patch->synth_module) != 0) {
        /* Unload current synth */
        unload_synth();

        /* Build path to new synth module */
        char synth_path[MAX_PATH_LEN];

        /* Check if it's an internal sound generator (in chain/sound_generators/) */
        if (strcmp(patch->synth_module, "linein") == 0) {
            /* Internal sound generator - look in chain's sound_generators dir */
            snprintf(synth_path, sizeof(synth_path), "%s/sound_generators/%s",
                     g_module_dir, patch->synth_module);
        } else {
            /* External module (sf2, dx7, etc.) - look in modules dir */
            strncpy(synth_path, g_module_dir, sizeof(synth_path) - 1);
            char *last_slash = strrchr(synth_path, '/');
            if (last_slash) {
                snprintf(last_slash + 1, sizeof(synth_path) - (last_slash - synth_path) - 1,
                         "%s", patch->synth_module);
            }
        }

        /* Load new synth */
        if (load_synth(synth_path, NULL) != 0) {
            snprintf(msg, sizeof(msg), "Failed to load synth: %s", patch->synth_module);
            chain_log(msg);
            return -1;
        }

        strncpy(g_current_synth_module, patch->synth_module, MAX_NAME_LEN - 1);
    }

    /* Set preset on synth */
    if (g_synth_plugin && g_synth_plugin->set_param) {
        char preset_str[16];
        snprintf(preset_str, sizeof(preset_str), "%d", patch->synth_preset);
        g_synth_plugin->set_param("preset", preset_str);
    }

    /* Unload old audio FX and load new ones */
    unload_all_audio_fx();
    for (int i = 0; i < patch->audio_fx_count; i++) {
        if (load_audio_fx(patch->audio_fx[i]) != 0) {
            snprintf(msg, sizeof(msg), "Warning: Failed to load FX: %s", patch->audio_fx[i]);
            chain_log(msg);
        }
    }

    g_current_patch = index;

    /* Set MIDI FX chord type */
    g_chord_type = patch->chord_type;

    /* Set arpeggiator mode and tempo */
    g_arp_mode = patch->arp_mode;
    arp_reset();
    arp_set_tempo(patch->arp_tempo_bpm, patch->arp_note_division);

    /* Mute briefly to drain any old synth audio before FX process it */
    g_mute_countdown = MUTE_BLOCKS_AFTER_SWITCH;

    snprintf(msg, sizeof(msg), "Loaded patch %d: %s (%d FX, arp=%d)",
             index, patch->name, g_fx_count, g_arp_mode);
    chain_log(msg);

    return 0;
}

/* === Plugin API Implementation === */

static int plugin_on_load(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Chain host loading from: %s", module_dir);
    chain_log(msg);

    /* Store module directory for later use */
    strncpy(g_module_dir, module_dir, MAX_PATH_LEN - 1);

    /* Scan for patches */
    scan_patches(module_dir);

    if (g_patch_count > 0) {
        /* Load first patch */
        if (load_patch(0) != 0) {
            chain_log("Failed to load first patch, falling back to SF2");
            goto fallback;
        }
    } else {
        chain_log("No patches found, using default SF2");
        goto fallback;
    }

    chain_log("Chain host initialized with patches");
    return 0;

fallback:
    /* Fallback: Load SF2 directly */
    {
        char synth_path[MAX_PATH_LEN];
        strncpy(synth_path, module_dir, sizeof(synth_path) - 1);
        char *last_slash = strrchr(synth_path, '/');
        if (last_slash) {
            strcpy(last_slash + 1, "sf2");
        } else {
            strcpy(synth_path, "modules/sf2");
        }

        if (load_synth(synth_path, NULL) != 0) {
            chain_log("Failed to load SF2 synth");
            return -1;
        }
        strncpy(g_current_synth_module, "sf2", MAX_NAME_LEN - 1);
    }

    chain_log("Chain host initialized (fallback)");
    return 0;
}

static void plugin_on_unload(void) {
    chain_log("Chain host unloading");
    unload_all_audio_fx();
    unload_synth();
}

/* Send a note message to synth with optional interval offset */
static void send_note_to_synth(const uint8_t *msg, int len, int source, int interval) {
    if (!g_synth_plugin || !g_synth_plugin->on_midi) return;

    if (interval == 0) {
        /* No transposition, send as-is */
        g_synth_plugin->on_midi(msg, len, source);
    } else {
        /* Transpose the note */
        uint8_t transposed[3];
        transposed[0] = msg[0];
        transposed[1] = (uint8_t)(msg[1] + interval);  /* Note number + interval */
        transposed[2] = msg[2];  /* Velocity */

        /* Only send if note is in valid MIDI range */
        if (transposed[1] <= 127) {
            g_synth_plugin->on_midi(transposed, len, source);
        }
    }
}

static void plugin_on_midi(const uint8_t *msg, int len, int source) {
    if (!g_synth_plugin || !g_synth_plugin->on_midi) return;
    if (len < 1) return;

    uint8_t status = msg[0] & 0xF0;

    /* Handle note on/off for arpeggiator */
    if (g_arp_mode != ARP_OFF && len >= 3 && (status == 0x90 || status == 0x80)) {
        uint8_t note = msg[1];
        uint8_t velocity = msg[2];

        /* Note on with velocity > 0 */
        if (status == 0x90 && velocity > 0) {
            arp_add_note(note, velocity);
        } else {
            /* Note off (or note on with velocity 0) */
            arp_remove_note(note);
        }
        return;  /* Arp handles note output in tick */
    }

    /* Handle chord generation for note on/off */
    if ((status == 0x90 || status == 0x80) && len >= 3 && g_chord_type != CHORD_NONE) {
        /* Send root note */
        send_note_to_synth(msg, len, source, 0);

        /* Add chord notes based on type */
        switch (g_chord_type) {
            case CHORD_MAJOR:
                send_note_to_synth(msg, len, source, 4);   /* Major 3rd */
                send_note_to_synth(msg, len, source, 7);   /* Perfect 5th */
                break;
            case CHORD_MINOR:
                send_note_to_synth(msg, len, source, 3);   /* Minor 3rd */
                send_note_to_synth(msg, len, source, 7);   /* Perfect 5th */
                break;
            case CHORD_POWER:
                send_note_to_synth(msg, len, source, 7);   /* Perfect 5th */
                break;
            case CHORD_OCTAVE:
                send_note_to_synth(msg, len, source, 12);  /* Octave */
                break;
            default:
                break;
        }
    } else {
        /* Non-note message or no MIDI FX, forward directly */
        g_synth_plugin->on_midi(msg, len, source);
    }
}

static void plugin_set_param(const char *key, const char *val) {
    /* Handle chain-level params */
    if (strcmp(key, "patch") == 0) {
        int index = atoi(val);
        load_patch(index);
        return;
    }
    if (strcmp(key, "next_patch") == 0) {
        int next = (g_current_patch + 1) % g_patch_count;
        if (g_patch_count > 0) load_patch(next);
        return;
    }
    if (strcmp(key, "prev_patch") == 0) {
        int prev = (g_current_patch - 1 + g_patch_count) % g_patch_count;
        if (g_patch_count > 0) load_patch(prev);
        return;
    }

    /* Handle octave changes - need to reset arp to avoid stuck notes */
    if (strcmp(key, "octave_transpose") == 0) {
        /* Send note-off for current arp note before octave changes */
        if (g_arp_last_note >= 0) {
            arp_send_note(g_arp_last_note, 0, 0);
            g_arp_last_note = -1;
        }
    }

    /* Forward to synth (includes octave_transpose) */
    if (g_synth_plugin && g_synth_plugin->set_param) {
        g_synth_plugin->set_param(key, val);
    }
}

static int plugin_get_param(const char *key, char *buf, int buf_len) {
    /* Handle chain-level params */
    if (strcmp(key, "patch_count") == 0) {
        snprintf(buf, buf_len, "%d", g_patch_count);
        return 0;
    }
    if (strcmp(key, "current_patch") == 0) {
        snprintf(buf, buf_len, "%d", g_current_patch);
        return 0;
    }
    if (strcmp(key, "patch_name") == 0) {
        if (g_current_patch >= 0 && g_current_patch < g_patch_count) {
            snprintf(buf, buf_len, "%s", g_patches[g_current_patch].name);
            return 0;
        }
        snprintf(buf, buf_len, "No Patch");
        return 0;
    }
    if (strcmp(key, "synth_module") == 0) {
        snprintf(buf, buf_len, "%s", g_current_synth_module);
        return 0;
    }

    /* Forward to synth */
    if (g_synth_plugin && g_synth_plugin->get_param) {
        return g_synth_plugin->get_param(key, buf, buf_len);
    }
    return -1;
}

static void plugin_render_block(int16_t *out_interleaved_lr, int frames) {
    /* Mute output briefly after patch switch to drain old synth audio */
    if (g_mute_countdown > 0) {
        g_mute_countdown--;
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Process arpeggiator timing (sends notes to synth) */
    arp_tick(frames);

    if (g_synth_plugin && g_synth_plugin->render_block) {
        /* Get audio from synth */
        g_synth_plugin->render_block(out_interleaved_lr, frames);

        /* Process through audio FX chain */
        for (int i = 0; i < g_fx_count; i++) {
            if (g_fx_plugins[i] && g_fx_plugins[i]->process_block) {
                g_fx_plugins[i]->process_block(out_interleaved_lr, frames);
            }
        }
    } else {
        /* No synth loaded - output silence */
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
    }
}

/* === Plugin Entry Point === */

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

    /* Set up host API for sub-plugins (forward everything to main host) */
    memcpy(&g_subplugin_host_api, host, sizeof(host_api_v1_t));

    /* Initialize our plugin API struct */
    memset(&g_plugin_api, 0, sizeof(g_plugin_api));
    g_plugin_api.api_version = MOVE_PLUGIN_API_VERSION;
    g_plugin_api.on_load = plugin_on_load;
    g_plugin_api.on_unload = plugin_on_unload;
    g_plugin_api.on_midi = plugin_on_midi;
    g_plugin_api.set_param = plugin_set_param;
    g_plugin_api.get_param = plugin_get_param;
    g_plugin_api.render_block = plugin_render_block;

    chain_log("Chain host plugin initialized");

    return &g_plugin_api;
}
