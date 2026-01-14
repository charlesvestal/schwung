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
#include <time.h>
#include <pthread.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v1.h"

/* Recording constants */
#define RECORDINGS_DIR "/data/UserData/move-anything/recordings"
#define NUM_CHANNELS 2
#define BITS_PER_SAMPLE 16
#define CC_RECORD_BUTTON 118
#define LED_COLOR_RED 1
#define LED_COLOR_WHITE 120
#define LED_COLOR_OFF 0

/* Ring buffer for threaded recording (2 seconds of stereo audio) */
#define RING_BUFFER_SAMPLES (SAMPLE_RATE * 2)
#define RING_BUFFER_SIZE (RING_BUFFER_SAMPLES * NUM_CHANNELS * sizeof(int16_t))

/* WAV file header structure */
typedef struct {
    char riff_id[4];        /* "RIFF" */
    uint32_t file_size;     /* File size - 8 */
    char wave_id[4];        /* "WAVE" */
    char fmt_id[4];         /* "fmt " */
    uint32_t fmt_size;      /* 16 for PCM */
    uint16_t audio_format;  /* 1 for PCM */
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_id[4];        /* "data" */
    uint32_t data_size;     /* Number of bytes of audio data */
} wav_header_t;

/* Limits */
#define MAX_PATCHES 32      /* Max patches to list in browser */
#define MAX_AUDIO_FX 4      /* Max FX loaded per active chain */
#define MAX_MIDI_FX_JS 4    /* Max JS MIDI FX per patch */
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

/* MIDI input filter */
typedef enum {
    MIDI_INPUT_ANY = 0,
    MIDI_INPUT_PADS,
    MIDI_INPUT_EXTERNAL
} midi_input_t;

/* Arpeggiator constants */
#define MAX_ARP_NOTES 16
#define SAMPLE_RATE 44100
#define FRAMES_PER_BLOCK 128
#define MOVE_STEP_NOTE_MIN 16
#define MOVE_STEP_NOTE_MAX 31
#define MOVE_PAD_NOTE_MIN 68

/* Knob mapping constants */
#define MAX_KNOB_MAPPINGS 8
#define KNOB_CC_START 71
#define KNOB_CC_END 78
#define KNOB_STEP_FLOAT 0.05f  /* Step size for float params */
#define KNOB_STEP_INT 1        /* Step size for int params */

/* Knob mapping types */
typedef enum {
    KNOB_TYPE_FLOAT = 0,
    KNOB_TYPE_INT = 1
} knob_type_t;

/* Knob mapping structure */
typedef struct {
    int cc;              /* CC number (71-78) */
    char target[16];     /* "synth", "fx1", "fx2", "midi_fx" */
    char param[32];      /* Parameter key */
    knob_type_t type;    /* Parameter type (float or int) */
    float min_val;       /* Minimum value */
    float max_val;       /* Maximum value */
    float current_value; /* Current parameter value */
} knob_mapping_t;

/* Chain parameter info from module.json */
#define MAX_CHAIN_PARAMS 16
typedef struct {
    char key[32];           /* Parameter key (e.g., "preset", "decay") */
    char name[32];          /* Display name */
    knob_type_t type;       /* float or int */
    float min_val;          /* Minimum value */
    float max_val;          /* Maximum value (or -1 if dynamic via max_param) */
    float default_val;      /* Default value */
    char max_param[32];     /* Dynamic max param key (e.g., "preset_count") */
} chain_param_info_t;

#define MOVE_PAD_NOTE_MAX 99

/* Patch info */
typedef struct {
    char name[MAX_NAME_LEN];
    char path[MAX_PATH_LEN];
    char synth_module[MAX_NAME_LEN];
    int synth_preset;
    char midi_source_module[MAX_NAME_LEN];
    char audio_fx[MAX_AUDIO_FX][MAX_NAME_LEN];
    int audio_fx_count;
    char midi_fx_js[MAX_MIDI_FX_JS][MAX_NAME_LEN];
    int midi_fx_js_count;
    chord_type_t chord_type;
    arp_mode_t arp_mode;
    int arp_tempo_bpm;       /* BPM for arpeggiator */
    int arp_note_division;   /* 1=quarter, 2=8th, 4=16th */
    midi_input_t midi_input;
    knob_mapping_t knob_mappings[MAX_KNOB_MAPPINGS];
    int knob_mapping_count;
} patch_info_t;

/* Host API provided by main host */
static const host_api_v1_t *g_host = NULL;

/* Sub-plugin state */
static void *g_synth_handle = NULL;
static plugin_api_v1_t *g_synth_plugin = NULL;
static char g_current_synth_module[MAX_NAME_LEN] = "";

static void *g_source_handle = NULL;
static plugin_api_v1_t *g_source_plugin = NULL;
static char g_current_source_module[MAX_NAME_LEN] = "";

/* Audio FX state */
static void *g_fx_handles[MAX_AUDIO_FX];
static audio_fx_api_v1_t *g_fx_plugins[MAX_AUDIO_FX];
static int g_fx_count = 0;

/* Module parameter info (from chain_params in module.json) */
static chain_param_info_t g_synth_params[MAX_CHAIN_PARAMS];
static int g_synth_param_count = 0;
static chain_param_info_t g_fx_params[MAX_AUDIO_FX][MAX_CHAIN_PARAMS];
static int g_fx_param_counts[MAX_AUDIO_FX] = {0};

/* Patch state */
static patch_info_t g_patches[MAX_PATCHES];
static int g_patch_count = 0;
static int g_current_patch = 0;
static char g_module_dir[MAX_PATH_LEN] = "";

/* MIDI FX state - Chords */
static chord_type_t g_chord_type = CHORD_NONE;

/* JS MIDI FX state */
static int g_js_midi_fx_enabled = 0;

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

/* Knob mapping state */
static knob_mapping_t g_knob_mappings[MAX_KNOB_MAPPINGS];
static int g_knob_mapping_count = 0;

/* Mute countdown after patch switch (in blocks) to drain old audio */
static int g_mute_countdown = 0;
#define MUTE_BLOCKS_AFTER_SWITCH 8  /* ~23ms at 44100Hz, 128 frames/block */

/* Recording state */
static int g_recording = 0;
static FILE *g_wav_file = NULL;
static uint32_t g_samples_written = 0;
static char g_current_recording[MAX_PATH_LEN] = "";

/* Ring buffer for threaded recording */
static int16_t *g_ring_buffer = NULL;
static volatile size_t g_ring_write_pos = 0;
static volatile size_t g_ring_read_pos = 0;
static pthread_t g_writer_thread;
static pthread_mutex_t g_ring_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ring_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_writer_running = 0;
static volatile int g_writer_should_exit = 0;

/* MIDI input filter (per patch) */
static midi_input_t g_midi_input = MIDI_INPUT_ANY;

/* Raw MIDI bypass (module-level) */
static int g_raw_midi = 0;

/* Source UI state (used to suppress pad-thru when editing) */
static int g_source_ui_active = 0;

/* Component UI mode - when set, bypass knob CC macro mappings */
/* 0 = normal (macro mode), 1 = synth, 2 = fx1, 3 = fx2 */
static int g_component_ui_mode = 0;
/* Our host API for sub-plugins (forwards to main host) */
static host_api_v1_t g_subplugin_host_api;
static host_api_v1_t g_source_host_api;

static void plugin_on_midi(const uint8_t *msg, int len, int source);
static int midi_source_send(const uint8_t *msg, int len);
static int scan_patches(const char *module_dir);
static void unload_patch(void);
static int parse_chain_params(const char *module_path, chain_param_info_t *params, int *count);
static chain_param_info_t *find_param_info(chain_param_info_t *params, int count, const char *key);

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

/* === Recording Functions === */

static void write_wav_header(FILE *f, uint32_t data_size) {
    wav_header_t header;

    memcpy(header.riff_id, "RIFF", 4);
    header.file_size = 36 + data_size;
    memcpy(header.wave_id, "WAVE", 4);

    memcpy(header.fmt_id, "fmt ", 4);
    header.fmt_size = 16;
    header.audio_format = 1;  /* PCM */
    header.num_channels = NUM_CHANNELS;
    header.sample_rate = SAMPLE_RATE;
    header.byte_rate = SAMPLE_RATE * NUM_CHANNELS * (BITS_PER_SAMPLE / 8);
    header.block_align = NUM_CHANNELS * (BITS_PER_SAMPLE / 8);
    header.bits_per_sample = BITS_PER_SAMPLE;

    memcpy(header.data_id, "data", 4);
    header.data_size = data_size;

    fseek(f, 0, SEEK_SET);
    fwrite(&header, sizeof(header), 1, f);
}

/* Ring buffer helpers - lock-free for single producer (audio thread) */
static size_t ring_available_write(void) {
    size_t write_pos = g_ring_write_pos;
    size_t read_pos = g_ring_read_pos;
    size_t buffer_samples = RING_BUFFER_SAMPLES * NUM_CHANNELS;

    if (write_pos >= read_pos) {
        return buffer_samples - (write_pos - read_pos) - 1;
    } else {
        return read_pos - write_pos - 1;
    }
}

static size_t ring_available_read(void) {
    size_t write_pos = g_ring_write_pos;
    size_t read_pos = g_ring_read_pos;
    size_t buffer_samples = RING_BUFFER_SAMPLES * NUM_CHANNELS;

    if (write_pos >= read_pos) {
        return write_pos - read_pos;
    } else {
        return buffer_samples - (read_pos - write_pos);
    }
}

/* Writer thread - runs in background, writes buffered audio to disk */
static void *writer_thread_func(void *arg) {
    (void)arg;
    size_t buffer_samples = RING_BUFFER_SAMPLES * NUM_CHANNELS;
    size_t write_chunk = SAMPLE_RATE * NUM_CHANNELS / 4;  /* Write ~250ms at a time */

    while (1) {
        pthread_mutex_lock(&g_ring_mutex);

        /* Wait for data or exit signal */
        while (ring_available_read() < write_chunk && !g_writer_should_exit) {
            pthread_cond_wait(&g_ring_cond, &g_ring_mutex);
        }

        int should_exit = g_writer_should_exit;
        pthread_mutex_unlock(&g_ring_mutex);

        /* Write available data to file */
        size_t available = ring_available_read();
        while (available > 0 && g_wav_file) {
            size_t read_pos = g_ring_read_pos;
            size_t to_end = buffer_samples - read_pos;
            size_t to_write = (available < to_end) ? available : to_end;

            fwrite(&g_ring_buffer[read_pos], sizeof(int16_t), to_write, g_wav_file);
            g_samples_written += to_write / NUM_CHANNELS;

            g_ring_read_pos = (read_pos + to_write) % buffer_samples;
            available = ring_available_read();
        }

        if (should_exit) {
            break;
        }
    }

    return NULL;
}

static void start_recording(void) {
    if (g_writer_running) {
        /* Already recording */
        return;
    }

    /* Create recordings directory */
    char mkdir_cmd[512];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", RECORDINGS_DIR);
    system(mkdir_cmd);

    /* Generate filename with timestamp */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    snprintf(g_current_recording, sizeof(g_current_recording),
             "%s/rec_%04d%02d%02d_%02d%02d%02d.wav",
             RECORDINGS_DIR,
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec);

    /* Allocate ring buffer */
    g_ring_buffer = malloc(RING_BUFFER_SIZE);
    if (!g_ring_buffer) {
        chain_log("Failed to allocate ring buffer for recording");
        return;
    }

    /* Open file for writing */
    g_wav_file = fopen(g_current_recording, "wb");
    if (!g_wav_file) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Failed to open recording file: %s", g_current_recording);
        chain_log(msg);
        free(g_ring_buffer);
        g_ring_buffer = NULL;
        return;
    }

    /* Initialize state */
    g_samples_written = 0;
    g_ring_write_pos = 0;
    g_ring_read_pos = 0;
    g_writer_should_exit = 0;

    /* Write placeholder header */
    write_wav_header(g_wav_file, 0);

    /* Start writer thread */
    if (pthread_create(&g_writer_thread, NULL, writer_thread_func, NULL) != 0) {
        chain_log("Failed to create writer thread");
        fclose(g_wav_file);
        g_wav_file = NULL;
        free(g_ring_buffer);
        g_ring_buffer = NULL;
        return;
    }

    g_writer_running = 1;

    char msg[512];
    snprintf(msg, sizeof(msg), "Recording started: %s", g_current_recording);
    chain_log(msg);
}

static void stop_recording(void) {
    if (!g_writer_running) {
        chain_log("stop_recording called but writer not running");
        return;
    }

    chain_log("Stopping recording - signaling writer thread");

    /* Signal writer thread to exit */
    pthread_mutex_lock(&g_ring_mutex);
    g_writer_should_exit = 1;
    pthread_cond_signal(&g_ring_cond);
    pthread_mutex_unlock(&g_ring_mutex);

    /* Wait for writer thread to finish */
    chain_log("Waiting for writer thread to finish");
    pthread_join(g_writer_thread, NULL);
    g_writer_running = 0;
    chain_log("Writer thread finished");

    /* Update WAV header with final size */
    if (g_wav_file) {
        uint32_t data_size = g_samples_written * NUM_CHANNELS * (BITS_PER_SAMPLE / 8);
        write_wav_header(g_wav_file, data_size);
        fclose(g_wav_file);
        g_wav_file = NULL;
    }

    /* Free ring buffer */
    if (g_ring_buffer) {
        free(g_ring_buffer);
        g_ring_buffer = NULL;
    }

    char msg[512];
    snprintf(msg, sizeof(msg), "Recording stopped: %s (%u samples, %.1f seconds)",
             g_current_recording, g_samples_written,
             (float)g_samples_written / SAMPLE_RATE);
    chain_log(msg);

    g_current_recording[0] = '\0';
}

static void update_record_led(void) {
    if (!g_host || !g_host->midi_send_internal) return;

    /* Determine LED color based on state:
     * - Off (black) when no patch loaded
     * - White when patch loaded but not recording
     * - Red when recording
     */
    uint8_t color;
    if (!g_synth_plugin) {
        color = LED_COLOR_OFF;
    } else if (g_recording) {
        color = LED_COLOR_RED;
    } else {
        color = LED_COLOR_WHITE;
    }

    /* Send CC to set record LED color */
    /* USB-MIDI packet: [cable|CIN, status, cc, value] */
    uint8_t msg[4] = {
        0x0B,  /* Cable 0, CIN = Control Change */
        0xB0,  /* CC on channel 0 */
        CC_RECORD_BUTTON,
        color
    };
    g_host->midi_send_internal(msg, 4);
}

static void toggle_recording(void) {
    /* Don't allow recording without a patch loaded */
    if (!g_synth_plugin) {
        chain_log("Cannot record - no patch loaded");
        return;
    }

    if (g_recording) {
        stop_recording();
        g_recording = 0;
    } else {
        g_recording = 1;
        start_recording();
    }
    update_record_led();
}

/* === End Recording Functions === */

static int json_get_bool(const char *json, const char *key, int *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    pos = strchr(pos + strlen(search), ':');
    if (!pos) return -1;

    /* Skip whitespace */
    while (*pos && (*pos == ':' || *pos == ' ' || *pos == '\t' || *pos == '\n')) pos++;

    *out = (strncmp(pos, "true", 4) == 0) ? 1 : 0;
    return 0;
}

static void load_module_settings(const char *module_dir) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/module.json", module_dir);

    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 4096) {
        fclose(f);
        return;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return;
    }

    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);

    g_raw_midi = 0;
    json_get_bool(json, "raw_midi", &g_raw_midi);

    free(json);
}

static int midi_source_allowed(int source) {
    if (source == MOVE_MIDI_SOURCE_HOST) {
        return 1;
    }

    if (g_midi_input == MIDI_INPUT_PADS) {
        return source == MOVE_MIDI_SOURCE_INTERNAL;
    }

    if (g_midi_input == MIDI_INPUT_EXTERNAL) {
        return source == MOVE_MIDI_SOURCE_EXTERNAL;
    }

    return 1;
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

    /* Parse chain_params from module.json */
    parse_chain_params(module_path, g_synth_params, &g_synth_param_count);
    snprintf(msg, sizeof(msg), "Synth loaded successfully (%d params)", g_synth_param_count);
    chain_log(msg);
    return 0;
}

static int midi_source_send(const uint8_t *msg, int len) {
    if (!msg || len < 2) return 0;

    uint8_t status = msg[1];
    if (status == 0) return len;

    int msg_len = 3;
    uint8_t status_type = status & 0xF0;
    if (status >= 0xF8) {
        msg_len = 1;
    } else if (status_type == 0xC0 || status_type == 0xD0) {
        msg_len = 2;
    }

    plugin_on_midi(&msg[1], msg_len, MOVE_MIDI_SOURCE_HOST);
    return len;
}

static int load_midi_source(const char *module_path, const char *config_json) {
    char msg[256];

    char dsp_path[512];
    snprintf(dsp_path, sizeof(dsp_path), "%s/dsp.so", module_path);

    snprintf(msg, sizeof(msg), "Loading MIDI source from: %s", dsp_path);
    chain_log(msg);

    g_source_handle = dlopen(dsp_path, RTLD_NOW | RTLD_LOCAL);
    if (!g_source_handle) {
        snprintf(msg, sizeof(msg), "dlopen failed: %s", dlerror());
        chain_log(msg);
        return -1;
    }

    move_plugin_init_v1_fn init_fn = (move_plugin_init_v1_fn)dlsym(g_source_handle, MOVE_PLUGIN_INIT_SYMBOL);
    if (!init_fn) {
        snprintf(msg, sizeof(msg), "dlsym failed: %s", dlerror());
        chain_log(msg);
        dlclose(g_source_handle);
        g_source_handle = NULL;
        return -1;
    }

    g_source_plugin = init_fn(&g_source_host_api);
    if (!g_source_plugin) {
        chain_log("MIDI source plugin init returned NULL");
        dlclose(g_source_handle);
        g_source_handle = NULL;
        return -1;
    }

    if (g_source_plugin->api_version != MOVE_PLUGIN_API_VERSION) {
        snprintf(msg, sizeof(msg), "MIDI source API version mismatch: %d vs %d",
                 g_source_plugin->api_version, MOVE_PLUGIN_API_VERSION);
        chain_log(msg);
        dlclose(g_source_handle);
        g_source_handle = NULL;
        g_source_plugin = NULL;
        return -1;
    }

    if (g_source_plugin->on_load) {
        int ret = g_source_plugin->on_load(module_path, config_json);
        if (ret != 0) {
            snprintf(msg, sizeof(msg), "MIDI source on_load failed: %d", ret);
            chain_log(msg);
            dlclose(g_source_handle);
            g_source_handle = NULL;
            g_source_plugin = NULL;
            return -1;
        }
    }

    chain_log("MIDI source loaded successfully");
    return 0;
}

static void unload_midi_source(void) {
    if (g_source_plugin && g_source_plugin->on_unload) {
        g_source_plugin->on_unload();
    }
    if (g_source_handle) {
        dlclose(g_source_handle);
        g_source_handle = NULL;
    }
    g_source_plugin = NULL;
    g_current_source_module[0] = '\0';
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

    /* Build path to FX .so - try chain/audio_fx/<name>/<name>.so first (built-in) */
    char fx_path[MAX_PATH_LEN];
    char fx_dir[MAX_PATH_LEN];
    snprintf(fx_path, sizeof(fx_path), "%s/audio_fx/%s/%s.so",
             g_module_dir, fx_name, fx_name);
    snprintf(fx_dir, sizeof(fx_dir), "%s/audio_fx/%s", g_module_dir, fx_name);

    snprintf(msg, sizeof(msg), "Loading audio FX: %s", fx_path);
    chain_log(msg);

    /* Open the shared library */
    void *handle = dlopen(fx_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        /* Try external module path: modules/<name>/<name>.so (Store-installed) */
        snprintf(fx_path, sizeof(fx_path), "%s/../%s/%s.so",
                 g_module_dir, fx_name, fx_name);
        snprintf(fx_dir, sizeof(fx_dir), "%s/../%s", g_module_dir, fx_name);

        snprintf(msg, sizeof(msg), "Trying external path: %s", fx_path);
        chain_log(msg);

        handle = dlopen(fx_path, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            snprintf(msg, sizeof(msg), "dlopen failed: %s", dlerror());
            chain_log(msg);
            return -1;
        }
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

    /* Call on_load - fx_dir was already set above based on where we found the .so */
    if (fx->on_load) {
        if (fx->on_load(fx_dir, NULL) != 0) {
            chain_log("FX on_load failed");
            dlclose(handle);
            return -1;
        }
    }

    /* Store in array */
    int slot = g_fx_count;
    g_fx_handles[slot] = handle;
    g_fx_plugins[slot] = fx;

    /* Parse chain_params from module.json - fx_dir was set above */
    parse_chain_params(fx_dir, g_fx_params[slot], &g_fx_param_counts[slot]);

    g_fx_count++;

    snprintf(msg, sizeof(msg), "Audio FX loaded: %s (slot %d, %d params)",
             fx_name, slot, g_fx_param_counts[slot]);
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
        g_fx_param_counts[i] = 0;
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

static int json_get_section_bounds(const char *json, const char *section_key,
                                   const char **out_start, const char **out_end) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", section_key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    const char *start = strchr(pos, '{');
    if (!start) return -1;

    int depth = 0;
    const char *end = NULL;
    for (const char *p = start; *p; p++) {
        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) {
                end = p;
                break;
            }
        }
    }
    if (!end) return -1;

    *out_start = start;
    *out_end = end;
    return 0;
}

static int json_get_string_in_section(const char *json, const char *section_key,
                                      const char *key, char *out, int out_len) {
    const char *start = NULL;
    const char *end = NULL;
    if (json_get_section_bounds(json, section_key, &start, &end) != 0) {
        return -1;
    }

    int len = (int)(end - start + 1);
    char *section = malloc((size_t)len + 1);
    if (!section) return -1;

    memcpy(section, start, (size_t)len);
    section[len] = '\0';

    int ret = json_get_string(section, key, out, out_len);
    free(section);
    return ret;
}

static int json_get_int_in_section(const char *json, const char *section_key,
                                   const char *key, int *out) {
    const char *start = NULL;
    const char *end = NULL;
    if (json_get_section_bounds(json, section_key, &start, &end) != 0) {
        return -1;
    }

    int len = (int)(end - start + 1);
    char *section = malloc((size_t)len + 1);
    if (!section) return -1;

    memcpy(section, start, (size_t)len);
    section[len] = '\0';

    int ret = json_get_int(section, key, out);
    free(section);
    return ret;
}

/* Parse chain_params from module.json */
static int parse_chain_params(const char *module_path, chain_param_info_t *params, int *count) {
    char json_path[MAX_PATH_LEN];
    snprintf(json_path, sizeof(json_path), "%s/module.json", module_path);

    FILE *f = fopen(json_path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size > 8192) {
        fclose(f);
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

    *count = 0;

    /* Find chain_params array */
    const char *chain_params = strstr(json, "\"chain_params\"");
    if (!chain_params) {
        free(json);
        return 0;  /* No params is OK */
    }

    const char *arr_start = strchr(chain_params, '[');
    if (!arr_start) {
        free(json);
        return 0;
    }

    /* Find matching ] */
    int depth = 1;
    const char *arr_end = arr_start + 1;
    while (*arr_end && depth > 0) {
        if (*arr_end == '[') depth++;
        else if (*arr_end == ']') depth--;
        arr_end++;
    }

    /* Parse each parameter object */
    const char *pos = arr_start + 1;
    while (pos < arr_end && *count < MAX_CHAIN_PARAMS) {
        const char *obj_start = strchr(pos, '{');
        if (!obj_start || obj_start >= arr_end) break;

        const char *obj_end = strchr(obj_start, '}');
        if (!obj_end || obj_end >= arr_end) break;

        chain_param_info_t *p = &params[*count];
        memset(p, 0, sizeof(*p));
        p->type = KNOB_TYPE_FLOAT;  /* Default */
        p->min_val = 0.0f;
        p->max_val = 1.0f;

        /* Parse key */
        const char *key_pos = strstr(obj_start, "\"key\"");
        if (key_pos && key_pos < obj_end) {
            const char *q1 = strchr(key_pos + 5, '"');
            if (q1 && q1 < obj_end) {
                q1++;
                const char *q2 = strchr(q1, '"');
                if (q2 && q2 < obj_end) {
                    int len = (int)(q2 - q1);
                    if (len > 31) len = 31;
                    strncpy(p->key, q1, len);
                }
            }
        }

        /* Parse name */
        const char *name_pos = strstr(obj_start, "\"name\"");
        if (name_pos && name_pos < obj_end) {
            const char *q1 = strchr(name_pos + 6, '"');
            if (q1 && q1 < obj_end) {
                q1++;
                const char *q2 = strchr(q1, '"');
                if (q2 && q2 < obj_end) {
                    int len = (int)(q2 - q1);
                    if (len > 31) len = 31;
                    strncpy(p->name, q1, len);
                }
            }
        }

        /* Parse type */
        const char *type_pos = strstr(obj_start, "\"type\"");
        if (type_pos && type_pos < obj_end) {
            const char *q1 = strchr(type_pos + 6, '"');
            if (q1 && q1 < obj_end) {
                q1++;
                if (strncmp(q1, "int", 3) == 0) {
                    p->type = KNOB_TYPE_INT;
                    p->max_val = 9999.0f;  /* Default for int */
                }
            }
        }

        /* Parse min */
        const char *min_pos = strstr(obj_start, "\"min\"");
        if (min_pos && min_pos < obj_end) {
            const char *colon = strchr(min_pos, ':');
            if (colon && colon < obj_end) {
                p->min_val = (float)atof(colon + 1);
            }
        }

        /* Parse max */
        const char *max_pos = strstr(obj_start, "\"max\"");
        if (max_pos && max_pos < obj_end) {
            /* Make sure it's not max_param */
            if (strncmp(max_pos, "\"max_param\"", 11) != 0) {
                const char *colon = strchr(max_pos, ':');
                if (colon && colon < obj_end) {
                    p->max_val = (float)atof(colon + 1);
                }
            }
        }

        /* Parse max_param (dynamic max) */
        const char *max_param_pos = strstr(obj_start, "\"max_param\"");
        if (max_param_pos && max_param_pos < obj_end) {
            const char *q1 = strchr(max_param_pos + 11, '"');
            if (q1 && q1 < obj_end) {
                q1++;
                const char *q2 = strchr(q1, '"');
                if (q2 && q2 < obj_end) {
                    int len = (int)(q2 - q1);
                    if (len > 31) len = 31;
                    strncpy(p->max_param, q1, len);
                    p->max_val = -1.0f;  /* Marker for dynamic max */
                }
            }
        }

        /* Parse default */
        const char *def_pos = strstr(obj_start, "\"default\"");
        if (def_pos && def_pos < obj_end) {
            const char *colon = strchr(def_pos, ':');
            if (colon && colon < obj_end) {
                p->default_val = (float)atof(colon + 1);
            }
        }

        if (p->key[0]) {
            (*count)++;
        }

        pos = obj_end + 1;
    }

    free(json);
    return 0;
}

/* Look up parameter info by key in a param list */
static chain_param_info_t *find_param_info(chain_param_info_t *params, int count, const char *key) {
    for (int i = 0; i < count; i++) {
        if (strcmp(params[i].key, key) == 0) {
            return &params[i];
        }
    }
    return NULL;
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

    if (json_get_string_in_section(json, "synth", "module",
                                   patch->synth_module, MAX_NAME_LEN) != 0) {
        strcpy(patch->synth_module, "sf2");  /* Default to SF2 */
    }

    if (json_get_int_in_section(json, "synth", "preset", &patch->synth_preset) != 0) {
        patch->synth_preset = 0;
    }

    patch->midi_source_module[0] = '\0';
    if (json_get_string_in_section(json, "midi_source", "module",
                                   patch->midi_source_module, MAX_NAME_LEN) != 0) {
        json_get_string(json, "midi_source", patch->midi_source_module, MAX_NAME_LEN);
    }

    /* Parse MIDI input filter */
    patch->midi_input = MIDI_INPUT_ANY;
    char input_str[MAX_NAME_LEN] = "";
    if (json_get_string(json, "input", input_str, MAX_NAME_LEN) == 0) {
        if (strcmp(input_str, "pads") == 0) {
            patch->midi_input = MIDI_INPUT_PADS;
        } else if (strcmp(input_str, "external") == 0) {
            patch->midi_input = MIDI_INPUT_EXTERNAL;
        } else if (strcmp(input_str, "both") == 0 || strcmp(input_str, "all") == 0) {
            patch->midi_input = MIDI_INPUT_ANY;
        }
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

    /* Parse JS MIDI FX list */
    patch->midi_fx_js_count = 0;
    const char *js_fx_pos = strstr(json, "\"midi_fx_js\"");
    if (js_fx_pos) {
        const char *bracket = strchr(js_fx_pos, '[');
        const char *end_bracket = strchr(js_fx_pos, ']');
        if (bracket && end_bracket && bracket < end_bracket) {
            bracket++;
            while (patch->midi_fx_js_count < MAX_MIDI_FX_JS) {
                const char *quote1 = strchr(bracket, '"');
                if (!quote1 || quote1 > end_bracket) break;
                quote1++;

                const char *quote2 = strchr(quote1, '"');
                if (!quote2 || quote2 > end_bracket) break;

                int len = quote2 - quote1;
                if (len > 0 && len < MAX_NAME_LEN) {
                    strncpy(patch->midi_fx_js[patch->midi_fx_js_count], quote1, len);
                    patch->midi_fx_js[patch->midi_fx_js_count][len] = '\0';
                    patch->midi_fx_js_count++;
                }

                bracket = quote2 + 1;
            }
        }
    }

    /* Parse MIDI FX from midi_fx array (new format) */
    patch->chord_type = CHORD_NONE;
    patch->arp_mode = ARP_OFF;
    patch->arp_tempo_bpm = 120;
    patch->arp_note_division = 4;  /* 16th notes default */

    const char *midi_fx_pos = strstr(json, "\"midi_fx\"");
    if (midi_fx_pos) {
        const char *bracket = strchr(midi_fx_pos, '[');
        if (bracket) {
            const char *end_bracket = strchr(bracket, ']');
            if (end_bracket) {
                const char *obj_pos = bracket;
                /* Iterate through objects in the array */
                while (obj_pos && obj_pos < end_bracket) {
                    const char *obj_start = strchr(obj_pos, '{');
                    if (!obj_start || obj_start > end_bracket) break;

                    const char *obj_end = strchr(obj_start, '}');
                    if (!obj_end || obj_end > end_bracket) break;

                    /* Extract type field */
                    char fx_type[MAX_NAME_LEN] = "";
                    const char *type_pos = strstr(obj_start, "\"type\"");
                    if (type_pos && type_pos < obj_end) {
                        const char *colon = strchr(type_pos, ':');
                        if (colon && colon < obj_end) {
                            const char *q1 = strchr(colon, '"');
                            if (q1 && q1 < obj_end) {
                                q1++;
                                const char *q2 = strchr(q1, '"');
                                if (q2 && q2 < obj_end) {
                                    int len = q2 - q1;
                                    if (len > 0 && len < MAX_NAME_LEN) {
                                        strncpy(fx_type, q1, len);
                                        fx_type[len] = '\0';
                                    }
                                }
                            }
                        }
                    }

                    /* Parse based on type */
                    if (strcmp(fx_type, "chord") == 0) {
                        /* Extract chord value */
                        const char *chord_pos = strstr(obj_start, "\"chord\"");
                        if (chord_pos && chord_pos < obj_end) {
                            const char *colon = strchr(chord_pos, ':');
                            if (colon && colon < obj_end) {
                                const char *q1 = strchr(colon, '"');
                                if (q1 && q1 < obj_end) {
                                    q1++;
                                    const char *q2 = strchr(q1, '"');
                                    if (q2 && q2 < obj_end) {
                                        int len = q2 - q1;
                                        char chord_str[MAX_NAME_LEN] = "";
                                        if (len > 0 && len < MAX_NAME_LEN) {
                                            strncpy(chord_str, q1, len);
                                            chord_str[len] = '\0';
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
                                    }
                                }
                            }
                        }
                    } else if (strcmp(fx_type, "arp") == 0) {
                        /* Extract arp mode */
                        const char *mode_pos = strstr(obj_start, "\"mode\"");
                        if (mode_pos && mode_pos < obj_end) {
                            const char *colon = strchr(mode_pos, ':');
                            if (colon && colon < obj_end) {
                                const char *q1 = strchr(colon, '"');
                                if (q1 && q1 < obj_end) {
                                    q1++;
                                    const char *q2 = strchr(q1, '"');
                                    if (q2 && q2 < obj_end) {
                                        int len = q2 - q1;
                                        char mode_str[MAX_NAME_LEN] = "";
                                        if (len > 0 && len < MAX_NAME_LEN) {
                                            strncpy(mode_str, q1, len);
                                            mode_str[len] = '\0';
                                            if (strcmp(mode_str, "up") == 0) {
                                                patch->arp_mode = ARP_UP;
                                            } else if (strcmp(mode_str, "down") == 0) {
                                                patch->arp_mode = ARP_DOWN;
                                            } else if (strcmp(mode_str, "up_down") == 0 || strcmp(mode_str, "updown") == 0) {
                                                patch->arp_mode = ARP_UPDOWN;
                                            } else if (strcmp(mode_str, "random") == 0) {
                                                patch->arp_mode = ARP_RANDOM;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        /* Extract bpm */
                        const char *bpm_pos = strstr(obj_start, "\"bpm\"");
                        if (bpm_pos && bpm_pos < obj_end) {
                            const char *colon = strchr(bpm_pos, ':');
                            if (colon && colon < obj_end) {
                                patch->arp_tempo_bpm = atoi(colon + 1);
                            }
                        }
                        /* Extract division */
                        const char *div_pos = strstr(obj_start, "\"division\"");
                        if (div_pos && div_pos < obj_end) {
                            const char *colon = strchr(div_pos, ':');
                            if (colon && colon < obj_end) {
                                patch->arp_note_division = atoi(colon + 1);
                            }
                        }
                    }

                    obj_pos = obj_end + 1;
                }
            }
        }
    }

    /* Backward compatibility: try old flat format if midi_fx array not found */
    if (patch->chord_type == CHORD_NONE && patch->arp_mode == ARP_OFF) {
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
        json_get_int(json, "arp_bpm", &patch->arp_tempo_bpm);
        json_get_int(json, "arp_division", &patch->arp_note_division);
    }

    /* Parse knob_mappings array */
    patch->knob_mapping_count = 0;
    const char *knob_pos = strstr(json, "\"knob_mappings\"");
    if (knob_pos) {
        const char *bracket = strchr(knob_pos, '[');
        const char *end_bracket = strchr(knob_pos, ']');
        if (bracket && end_bracket && bracket < end_bracket) {
            bracket++;
            while (patch->knob_mapping_count < MAX_KNOB_MAPPINGS) {
                /* Find next object */
                const char *obj_start = strchr(bracket, '{');
                if (!obj_start || obj_start > end_bracket) break;

                const char *obj_end = strchr(obj_start, '}');
                if (!obj_end || obj_end > end_bracket) break;

                /* Parse cc */
                int cc = 0;
                const char *cc_pos = strstr(obj_start, "\"cc\"");
                if (cc_pos && cc_pos < obj_end) {
                    const char *colon = strchr(cc_pos, ':');
                    if (colon && colon < obj_end) {
                        cc = atoi(colon + 1);
                    }
                }

                /* Parse target */
                char target[16] = "";
                const char *target_pos = strstr(obj_start, "\"target\"");
                if (target_pos && target_pos < obj_end) {
                    const char *q1 = strchr(target_pos + 8, '"');
                    if (q1 && q1 < obj_end) {
                        q1++;
                        const char *q2 = strchr(q1, '"');
                        if (q2 && q2 < obj_end) {
                            int len = q2 - q1;
                            if (len > 0 && len < 16) {
                                strncpy(target, q1, len);
                                target[len] = '\0';
                            }
                        }
                    }
                }

                /* Parse param */
                char param[32] = "";
                const char *param_pos = strstr(obj_start, "\"param\"");
                if (param_pos && param_pos < obj_end) {
                    const char *q1 = strchr(param_pos + 7, '"');
                    if (q1 && q1 < obj_end) {
                        q1++;
                        const char *q2 = strchr(q1, '"');
                        if (q2 && q2 < obj_end) {
                            int len = q2 - q1;
                            if (len > 0 && len < 32) {
                                strncpy(param, q1, len);
                                param[len] = '\0';
                            }
                        }
                    }
                }

                /* Parse type (optional, default "float") */
                knob_type_t type = KNOB_TYPE_FLOAT;
                const char *type_pos = strstr(obj_start, "\"type\"");
                if (type_pos && type_pos < obj_end) {
                    const char *q1 = strchr(type_pos + 6, '"');
                    if (q1 && q1 < obj_end) {
                        q1++;
                        if (strncmp(q1, "int", 3) == 0) {
                            type = KNOB_TYPE_INT;
                        }
                    }
                }

                /* Parse min (optional) */
                float min_val = (type == KNOB_TYPE_INT) ? 0.0f : 0.0f;
                const char *min_pos = strstr(obj_start, "\"min\"");
                if (min_pos && min_pos < obj_end) {
                    const char *colon = strchr(min_pos, ':');
                    if (colon && colon < obj_end) {
                        min_val = (float)atof(colon + 1);
                    }
                }

                /* Parse max (optional) */
                float max_val = (type == KNOB_TYPE_INT) ? 9999.0f : 1.0f;
                const char *max_pos = strstr(obj_start, "\"max\"");
                if (max_pos && max_pos < obj_end) {
                    const char *colon = strchr(max_pos, ':');
                    if (colon && colon < obj_end) {
                        max_val = (float)atof(colon + 1);
                    }
                }

                /* Parse saved value (optional) - used for "save current state" */
                float saved_value = -999999.0f;  /* Sentinel for "not set" */
                const char *value_pos = strstr(obj_start, "\"value\"");
                if (value_pos && value_pos < obj_end) {
                    const char *colon = strchr(value_pos, ':');
                    if (colon && colon < obj_end) {
                        saved_value = (float)atof(colon + 1);
                    }
                }

                /* Store mapping if valid */
                if (cc >= KNOB_CC_START && cc <= KNOB_CC_END && target[0] && param[0]) {
                    patch->knob_mappings[patch->knob_mapping_count].cc = cc;
                    strncpy(patch->knob_mappings[patch->knob_mapping_count].target, target, 15);
                    strncpy(patch->knob_mappings[patch->knob_mapping_count].param, param, 31);
                    patch->knob_mappings[patch->knob_mapping_count].type = type;
                    patch->knob_mappings[patch->knob_mapping_count].min_val = min_val;
                    patch->knob_mappings[patch->knob_mapping_count].max_val = max_val;
                    patch->knob_mappings[patch->knob_mapping_count].current_value = saved_value;
                    patch->knob_mapping_count++;
                }

                bracket = obj_end + 1;
            }
        }
    }

    free(json);

    snprintf(msg, sizeof(msg),
             "Parsed patch: %s -> %s preset %d, source=%s, %d FX, chord=%d, arp=%d",
             patch->name, patch->synth_module, patch->synth_preset,
             patch->midi_source_module[0] ? patch->midi_source_module : "none",
             patch->audio_fx_count, patch->chord_type, patch->arp_mode);
    chain_log(msg);

    return 0;
}

/* Generate a patch name from components */
static void generate_patch_name(char *out, int out_len,
                                const char *synth, int preset,
                                const char *fx1, const char *fx2) {
    char preset_name[MAX_NAME_LEN] = "";

    /* Try to get preset name from synth */
    if (g_synth_plugin && g_synth_plugin->get_param) {
        g_synth_plugin->get_param("preset_name", preset_name, sizeof(preset_name));
    }

    if (preset_name[0] != '\0') {
        snprintf(out, out_len, "%s %02d %s", synth, preset, preset_name);
    } else {
        snprintf(out, out_len, "%s %02d", synth, preset);
    }

    /* Append FX names */
    if (fx1 && fx1[0] != '\0') {
        int len = strlen(out);
        snprintf(out + len, out_len - len, " + %s", fx1);
    }
    if (fx2 && fx2[0] != '\0') {
        int len = strlen(out);
        snprintf(out + len, out_len - len, " + %s", fx2);
    }
}

static void sanitize_filename(char *out, int out_len, const char *name) {
    int j = 0;
    for (int i = 0; name[i] && j < out_len - 1; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            out[j++] = c + 32; /* lowercase */
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
        } else if (c == ' ' || c == '-') {
            out[j++] = '_';
        }
        /* Skip other characters */
    }
    out[j] = '\0';
}

static int check_filename_exists(const char *dir, const char *base, char *out_path, int out_len) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s.json", dir, base);

    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return 1; /* Exists */
    }

    strncpy(out_path, path, out_len - 1);
    out_path[out_len - 1] = '\0';
    return 0;
}

static int save_patch(const char *json_data) {
    char msg[256];
    char patches_dir[MAX_PATH_LEN];
    snprintf(patches_dir, sizeof(patches_dir), "%s/patches", g_module_dir);

    /* Parse incoming JSON to get components */
    char synth[MAX_NAME_LEN] = "sf2";
    int preset = 0;
    char fx1[MAX_NAME_LEN] = "";
    char fx2[MAX_NAME_LEN] = "";

    json_get_string_in_section(json_data, "synth", "module", synth, sizeof(synth));
    json_get_int_in_section(json_data, "config", "preset", &preset);

    /* Parse audio_fx to get fx1 and fx2 */
    const char *fx_pos = strstr(json_data, "\"audio_fx\"");
    if (fx_pos) {
        const char *bracket = strchr(fx_pos, '[');
        if (bracket) {
            /* Look for first "type" */
            const char *type1 = strstr(bracket, "\"type\"");
            if (type1) {
                const char *colon = strchr(type1, ':');
                if (colon) {
                    const char *q1 = strchr(colon, '"');
                    if (q1) {
                        q1++;
                        const char *q2 = strchr(q1, '"');
                        if (q2) {
                            int len = q2 - q1;
                            if (len < MAX_NAME_LEN) {
                                strncpy(fx1, q1, len);
                                fx1[len] = '\0';
                            }
                            /* Look for second "type" */
                            const char *type2 = strstr(q2, "\"type\"");
                            if (type2) {
                                colon = strchr(type2, ':');
                                if (colon) {
                                    q1 = strchr(colon, '"');
                                    if (q1) {
                                        q1++;
                                        q2 = strchr(q1, '"');
                                        if (q2) {
                                            len = q2 - q1;
                                            if (len < MAX_NAME_LEN) {
                                                strncpy(fx2, q1, len);
                                                fx2[len] = '\0';
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* Check for custom name first, otherwise generate from components */
    char name[MAX_NAME_LEN];
    if (json_get_string(json_data, "custom_name", name, sizeof(name)) != 0) {
        /* No custom name - generate from components */
        generate_patch_name(name, sizeof(name), synth, preset, fx1, fx2);
    }

    /* Sanitize to filename */
    char base_filename[MAX_NAME_LEN];
    sanitize_filename(base_filename, sizeof(base_filename), name);

    /* Find available filename */
    char filepath[MAX_PATH_LEN];
    if (check_filename_exists(patches_dir, base_filename, filepath, sizeof(filepath))) {
        /* Need to add suffix */
        for (int i = 2; i < 100; i++) {
            char suffixed[MAX_NAME_LEN];
            snprintf(suffixed, sizeof(suffixed), "%s_%02d", base_filename, i);
            if (!check_filename_exists(patches_dir, suffixed, filepath, sizeof(filepath))) {
                /* Update name with suffix */
                int namelen = strlen(name);
                snprintf(name + namelen, sizeof(name) - namelen, " %02d", i);
                break;
            }
        }
    }

    /* Build final JSON with generated name */
    char final_json[4096];
    snprintf(final_json, sizeof(final_json),
        "{\n"
        "    \"name\": \"%s\",\n"
        "    \"version\": 1,\n"
        "    \"chain\": %s\n"
        "}\n",
        name, json_data);

    /* Write file */
    FILE *f = fopen(filepath, "w");
    if (!f) {
        snprintf(msg, sizeof(msg), "Failed to create patch file: %s", filepath);
        chain_log(msg);
        return -1;
    }

    fwrite(final_json, 1, strlen(final_json), f);
    fclose(f);

    snprintf(msg, sizeof(msg), "Saved patch: %s", filepath);
    chain_log(msg);

    /* Rescan patches */
    scan_patches(g_module_dir);

    return 0;
}

/* Update an existing patch at the given index */
static int update_patch(int index, const char *json_data) {
    char msg[256];

    if (index < 0 || index >= g_patch_count) {
        snprintf(msg, sizeof(msg), "Invalid patch index for update: %d", index);
        chain_log(msg);
        return -1;
    }

    const char *filepath = g_patches[index].path;

    /* Check for custom name, otherwise keep existing name */
    char name[MAX_NAME_LEN];
    if (json_get_string(json_data, "custom_name", name, sizeof(name)) != 0) {
        /* No custom name - use existing patch name */
        strncpy(name, g_patches[index].name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }

    /* Build final JSON with name */
    char final_json[4096];
    snprintf(final_json, sizeof(final_json),
        "{\n"
        "    \"name\": \"%s\",\n"
        "    \"version\": 1,\n"
        "    \"chain\": %s\n"
        "}\n",
        name, json_data);

    /* Write file */
    FILE *f = fopen(filepath, "w");
    if (!f) {
        snprintf(msg, sizeof(msg), "Failed to update patch file: %s", filepath);
        chain_log(msg);
        return -1;
    }

    fwrite(final_json, 1, strlen(final_json), f);
    fclose(f);

    snprintf(msg, sizeof(msg), "Updated patch: %s", filepath);
    chain_log(msg);

    /* Rescan patches to reload the updated data */
    scan_patches(g_module_dir);

    return 0;
}

static int delete_patch(int index) {
    char msg[256];

    if (index < 0 || index >= g_patch_count) {
        snprintf(msg, sizeof(msg), "Invalid patch index for delete: %d", index);
        chain_log(msg);
        return -1;
    }

    const char *path = g_patches[index].path;

    if (remove(path) != 0) {
        snprintf(msg, sizeof(msg), "Failed to delete patch: %s", path);
        chain_log(msg);
        return -1;
    }

    snprintf(msg, sizeof(msg), "Deleted patch: %s", path);
    chain_log(msg);

    /* Rescan patches */
    scan_patches(g_module_dir);

    /* If we deleted the current patch, unload it */
    if (index == g_current_patch) {
        unload_patch();
    } else if (index < g_current_patch) {
        g_current_patch--;
    }

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

static void unload_patch(void) {
    synth_panic();
    unload_all_audio_fx();
    unload_synth();
    unload_midi_source();
    g_current_patch = -1;
    g_current_synth_module[0] = '\0';
    g_current_source_module[0] = '\0';
    g_chord_type = CHORD_NONE;
    g_js_midi_fx_enabled = 0;
    g_midi_input = MIDI_INPUT_ANY;
    g_arp_mode = ARP_OFF;
    arp_reset();
    g_arp_last_note = -1;
    g_knob_mapping_count = 0;
    g_source_ui_active = 0;
    g_mute_countdown = 0;
    chain_log("Unloaded current patch");
    /* Update record button LED (off when no patch) */
    update_record_led();
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

    /* Check if we need to switch MIDI source modules */
    if (strcmp(g_current_source_module, patch->midi_source_module) != 0) {
        unload_midi_source();

        if (patch->midi_source_module[0] != '\0') {
            char source_path[MAX_PATH_LEN];

            strncpy(source_path, g_module_dir, sizeof(source_path) - 1);
            char *last_slash = strrchr(source_path, '/');
            if (last_slash) {
                snprintf(last_slash + 1, sizeof(source_path) - (last_slash - source_path) - 1,
                         "%s", patch->midi_source_module);
            }

            if (load_midi_source(source_path, NULL) != 0) {
                snprintf(msg, sizeof(msg), "Failed to load MIDI source: %s",
                         patch->midi_source_module);
                chain_log(msg);
                return -1;
            }

            strncpy(g_current_source_module, patch->midi_source_module, MAX_NAME_LEN - 1);
        }
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

    /* Enable JS MIDI FX if patch declares any */
    g_js_midi_fx_enabled = (patch->midi_fx_js_count > 0);

    /* Set MIDI input filter */
    g_midi_input = patch->midi_input;
    g_source_ui_active = 0;

    /* Set arpeggiator mode and tempo */
    g_arp_mode = patch->arp_mode;
    arp_reset();
    arp_set_tempo(patch->arp_tempo_bpm, patch->arp_note_division);

    /* Copy knob mappings and initialize current values */
    g_knob_mapping_count = patch->knob_mapping_count;
    for (int i = 0; i < patch->knob_mapping_count && i < MAX_KNOB_MAPPINGS; i++) {
        g_knob_mappings[i] = patch->knob_mappings[i];

        /* Look up param info from module to fill in missing type/min/max */
        const char *target = g_knob_mappings[i].target;
        const char *param = g_knob_mappings[i].param;
        chain_param_info_t *pinfo = NULL;

        if (strcmp(target, "synth") == 0) {
            pinfo = find_param_info(g_synth_params, g_synth_param_count, param);
        } else if (strcmp(target, "fx1") == 0 && g_fx_count > 0) {
            pinfo = find_param_info(g_fx_params[0], g_fx_param_counts[0], param);
        } else if (strcmp(target, "fx2") == 0 && g_fx_count > 1) {
            pinfo = find_param_info(g_fx_params[1], g_fx_param_counts[1], param);
        }

        if (pinfo) {
            /* Use module's param info (unless patch explicitly overrides) */
            /* Check if patch had defaults - if so, use module's values */
            if (patch->knob_mappings[i].type == KNOB_TYPE_FLOAT &&
                patch->knob_mappings[i].min_val == 0.0f &&
                patch->knob_mappings[i].max_val == 1.0f) {
                g_knob_mappings[i].type = pinfo->type;
                g_knob_mappings[i].min_val = pinfo->min_val;
                /* Handle dynamic max (max_param like "preset_count") */
                if (pinfo->max_val < 0 && pinfo->max_param[0]) {
                    /* Query the module for the actual max value */
                    char max_buf[32];
                    int got_max = 0;
                    if (strcmp(target, "synth") == 0 && g_synth_plugin && g_synth_plugin->get_param) {
                        if (g_synth_plugin->get_param(pinfo->max_param, max_buf, sizeof(max_buf)) >= 0) {
                            g_knob_mappings[i].max_val = (float)(atoi(max_buf) - 1);
                            got_max = 1;
                        }
                    }
                    if (!got_max) {
                        g_knob_mappings[i].max_val = 9999.0f;  /* Fallback */
                    }
                } else {
                    g_knob_mappings[i].max_val = pinfo->max_val;
                }
            }
        }

        /* Use saved value if present, otherwise initialize to middle of range */
        float saved = patch->knob_mappings[i].current_value;
        if (saved > -999998.0f) {
            /* Has saved value - use it (clamp to valid range) */
            if (saved < g_knob_mappings[i].min_val) saved = g_knob_mappings[i].min_val;
            if (saved > g_knob_mappings[i].max_val) saved = g_knob_mappings[i].max_val;
            if (g_knob_mappings[i].type == KNOB_TYPE_INT) {
                g_knob_mappings[i].current_value = (float)((int)saved);
            } else {
                g_knob_mappings[i].current_value = saved;
            }
        } else {
            /* No saved value - initialize to middle of min/max range */
            float mid = (g_knob_mappings[i].min_val + g_knob_mappings[i].max_val) / 2.0f;
            if (g_knob_mappings[i].type == KNOB_TYPE_INT) {
                g_knob_mappings[i].current_value = (float)((int)mid);  /* Round to int */
            } else {
                g_knob_mappings[i].current_value = mid;
            }
        }
    }

    /* Apply saved knob values to their targets */
    for (int i = 0; i < g_knob_mapping_count; i++) {
        const char *target = g_knob_mappings[i].target;
        const char *param = g_knob_mappings[i].param;
        float value = g_knob_mappings[i].current_value;

        /* Format value string */
        char val_str[32];
        if (g_knob_mappings[i].type == KNOB_TYPE_INT) {
            snprintf(val_str, sizeof(val_str), "%d", (int)value);
        } else {
            snprintf(val_str, sizeof(val_str), "%.3f", value);
        }

        /* Send to appropriate target */
        if (strcmp(target, "synth") == 0) {
            if (g_synth_plugin && g_synth_plugin->set_param) {
                g_synth_plugin->set_param(param, val_str);
            }
        } else if (strcmp(target, "fx1") == 0) {
            if (g_fx_count > 0 && g_fx_plugins[0] && g_fx_plugins[0]->set_param) {
                g_fx_plugins[0]->set_param(param, val_str);
            }
        } else if (strcmp(target, "fx2") == 0) {
            if (g_fx_count > 1 && g_fx_plugins[1] && g_fx_plugins[1]->set_param) {
                g_fx_plugins[1]->set_param(param, val_str);
            }
        }
    }

    /* Mute briefly to drain any old synth audio before FX process it */
    g_mute_countdown = MUTE_BLOCKS_AFTER_SWITCH;

    snprintf(msg, sizeof(msg), "Loaded patch %d: %s (%d FX, arp=%d)",
             index, patch->name, g_fx_count, g_arp_mode);
    chain_log(msg);

    /* Update record button LED (white when patch loaded) */
    update_record_led();

    return 0;
}

/* === Plugin API Implementation === */

static int plugin_on_load(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Chain host loading from: %s", module_dir);
    chain_log(msg);

    /* Store module directory for later use */
    strncpy(g_module_dir, module_dir, MAX_PATH_LEN - 1);

    /* Load module-level settings (raw_midi) */
    load_module_settings(module_dir);
    g_source_ui_active = 0;

    /* Scan for patches but don't load any - user will select from list */
    scan_patches(module_dir);

    snprintf(msg, sizeof(msg), "Chain host initialized, %d patches available", g_patch_count);
    chain_log(msg);
    return 0;
}

static void plugin_on_unload(void) {
    chain_log("Chain host unloading");

    /* Stop any active recording - always try, stop_recording handles the check */
    if (g_recording || g_writer_running) {
        chain_log("Stopping recording on unload");
        stop_recording();
        g_recording = 0;
    }

    unload_all_audio_fx();
    unload_synth();
    unload_midi_source();

    chain_log("Chain host unloaded");
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
    if (len < 1) return;

    /* Handle record button (CC 118) - toggle recording on press */
    /* This must be before the synth check so recording works even without a patch loaded */
    if (len >= 3 && (msg[0] & 0xF0) == 0xB0 && msg[1] == CC_RECORD_BUTTON && msg[2] > 0) {
        chain_log("Record button pressed - toggling recording");
        toggle_recording();
        return;  /* Don't pass record button to synth */
    }

    if (!g_synth_plugin || !g_synth_plugin->on_midi) return;

    /* Handle knob CC mappings (CC 71-78) - relative encoders */
    /* Skip if in component UI mode (UI handles knobs directly) */
    if (len >= 3 && (msg[0] & 0xF0) == 0xB0 && g_component_ui_mode == 0) {
        uint8_t cc = msg[1];
        if (cc >= KNOB_CC_START && cc <= KNOB_CC_END) {
            for (int i = 0; i < g_knob_mapping_count; i++) {
                if (g_knob_mappings[i].cc == cc) {
                    /* Relative encoder: 1 = increment, 127 = decrement */
                    /* Use type-appropriate step size */
                    int is_int = (g_knob_mappings[i].type == KNOB_TYPE_INT);
                    float delta = 0.0f;
                    if (msg[2] == 1) {
                        delta = is_int ? (float)KNOB_STEP_INT : KNOB_STEP_FLOAT;
                    } else if (msg[2] == 127) {
                        delta = is_int ? (float)(-KNOB_STEP_INT) : -KNOB_STEP_FLOAT;
                    } else {
                        return;  /* Ignore other values */
                    }

                    /* Update current value with min/max clamping */
                    float new_val = g_knob_mappings[i].current_value + delta;
                    if (new_val < g_knob_mappings[i].min_val) new_val = g_knob_mappings[i].min_val;
                    if (new_val > g_knob_mappings[i].max_val) new_val = g_knob_mappings[i].max_val;
                    if (is_int) new_val = (float)((int)new_val);  /* Round to int */
                    g_knob_mappings[i].current_value = new_val;

                    /* Convert to string for set_param - int or float format */
                    char val_str[16];
                    if (is_int) {
                        snprintf(val_str, sizeof(val_str), "%d", (int)new_val);
                    } else {
                        snprintf(val_str, sizeof(val_str), "%.3f", new_val);
                    }

                    const char *target = g_knob_mappings[i].target;
                    const char *param = g_knob_mappings[i].param;

                    if (strcmp(target, "synth") == 0) {
                        /* Route to synth */
                        if (g_synth_plugin && g_synth_plugin->set_param) {
                            g_synth_plugin->set_param(param, val_str);
                        }
                    } else if (strcmp(target, "fx1") == 0) {
                        /* Route to first audio FX */
                        if (g_fx_count > 0 && g_fx_plugins[0] && g_fx_plugins[0]->set_param) {
                            g_fx_plugins[0]->set_param(param, val_str);
                        }
                    } else if (strcmp(target, "fx2") == 0) {
                        /* Route to second audio FX */
                        if (g_fx_count > 1 && g_fx_plugins[1] && g_fx_plugins[1]->set_param) {
                            g_fx_plugins[1]->set_param(param, val_str);
                        }
                    } else if (strcmp(target, "midi_fx") == 0) {
                        /* MIDI FX params handled separately */
                    }
                    return;  /* CC handled */
                }
            }
        }
    }

    if (g_source_plugin && g_source_plugin->on_midi && source != MOVE_MIDI_SOURCE_HOST) {
        g_source_plugin->on_midi(msg, len, source);
    }

    if (!midi_source_allowed(source)) return;

    if (g_js_midi_fx_enabled && source != MOVE_MIDI_SOURCE_HOST) return;

    uint8_t status = msg[0] & 0xF0;
    if (source == MOVE_MIDI_SOURCE_INTERNAL && len >= 2 &&
        (status == 0x90 || status == 0x80)) {
        uint8_t note = msg[1];
        if (note >= MOVE_STEP_NOTE_MIN && note <= MOVE_STEP_NOTE_MAX) {
            return;
        }
        if (g_source_ui_active && note >= MOVE_PAD_NOTE_MIN && note <= MOVE_PAD_NOTE_MAX) {
            return;
        }
    }

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
    if (strcmp(key, "source_ui_active") == 0) {
        g_source_ui_active = atoi(val) ? 1 : 0;
        return;
    }

    /* Component UI mode - bypasses knob macro mappings */
    /* 0 = normal, 1 = synth, 2 = fx1, 3 = fx2 */
    if (strcmp(key, "component_ui_mode") == 0) {
        if (strcmp(val, "synth") == 0) {
            g_component_ui_mode = 1;
        } else if (strcmp(val, "fx1") == 0) {
            g_component_ui_mode = 2;
        } else if (strcmp(val, "fx2") == 0) {
            g_component_ui_mode = 3;
        } else {
            g_component_ui_mode = 0;  /* "none" or any other value */
        }
        return;
    }

    /* Recording control */
    if (strcmp(key, "recording") == 0) {
        int new_state = atoi(val);
        if (new_state && !g_recording) {
            g_recording = 1;
            start_recording();
        } else if (!new_state && g_recording) {
            stop_recording();
            g_recording = 0;
        }
        return;
    }

    if (strcmp(key, "save_patch") == 0) {
        save_patch(val);
        return;
    }

    if (strcmp(key, "delete_patch") == 0) {
        int index = atoi(val);
        delete_patch(index);
        return;
    }

    if (strcmp(key, "update_patch") == 0) {
        /* Format: "index:json_data" */
        const char *colon = strchr(val, ':');
        if (colon) {
            int index = atoi(val);
            update_patch(index, colon + 1);
        }
        return;
    }

    if (strncmp(key, "source:", 7) == 0) {
        const char *subkey = key + 7;
        if (g_source_plugin && g_source_plugin->set_param && subkey[0] != '\0') {
            g_source_plugin->set_param(subkey, val);
        }
        return;
    }

    /* Route to synth with synth: prefix */
    if (strncmp(key, "synth:", 6) == 0) {
        const char *subkey = key + 6;
        if (g_synth_plugin && g_synth_plugin->set_param && subkey[0] != '\0') {
            g_synth_plugin->set_param(subkey, val);
        }
        return;
    }

    /* Route to FX1 with fx1: prefix */
    if (strncmp(key, "fx1:", 4) == 0) {
        const char *subkey = key + 4;
        if (g_fx_count > 0 && g_fx_plugins[0] && g_fx_plugins[0]->set_param && subkey[0] != '\0') {
            g_fx_plugins[0]->set_param(subkey, val);
        }
        return;
    }

    /* Route to FX2 with fx2: prefix */
    if (strncmp(key, "fx2:", 4) == 0) {
        const char *subkey = key + 4;
        if (g_fx_count > 1 && g_fx_plugins[1] && g_fx_plugins[1]->set_param && subkey[0] != '\0') {
            g_fx_plugins[1]->set_param(subkey, val);
        }
        return;
    }

    /* Handle chain-level params */
    if (strcmp(key, "patch") == 0) {
        int index = atoi(val);
        if (index < 0) {
            unload_patch();
            return;
        }
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
    if (strncmp(key, "source:", 7) == 0) {
        const char *subkey = key + 7;
        if (g_source_plugin && g_source_plugin->get_param && subkey[0] != '\0') {
            return g_source_plugin->get_param(subkey, buf, buf_len);
        }
        return -1;
    }

    /* Route to synth with synth: prefix */
    if (strncmp(key, "synth:", 6) == 0) {
        const char *subkey = key + 6;
        if (g_synth_plugin && g_synth_plugin->get_param && subkey[0] != '\0') {
            return g_synth_plugin->get_param(subkey, buf, buf_len);
        }
        return -1;
    }

    /* Route to FX1 with fx1: prefix */
    if (strncmp(key, "fx1:", 4) == 0) {
        const char *subkey = key + 4;
        if (g_fx_count > 0 && g_fx_plugins[0] && g_fx_plugins[0]->get_param && subkey[0] != '\0') {
            return g_fx_plugins[0]->get_param(subkey, buf, buf_len);
        }
        return -1;
    }

    /* Route to FX2 with fx2: prefix */
    if (strncmp(key, "fx2:", 4) == 0) {
        const char *subkey = key + 4;
        if (g_fx_count > 1 && g_fx_plugins[1] && g_fx_plugins[1]->get_param && subkey[0] != '\0') {
            return g_fx_plugins[1]->get_param(subkey, buf, buf_len);
        }
        return -1;
    }

    /* Component UI mode */
    if (strcmp(key, "component_ui_mode") == 0) {
        const char *modes[] = {"none", "synth", "fx1", "fx2"};
        snprintf(buf, buf_len, "%s", modes[g_component_ui_mode]);
        return 0;
    }

    /* Recording state */
    if (strcmp(key, "recording") == 0) {
        snprintf(buf, buf_len, "%d", g_recording);
        return 0;
    }
    if (strcmp(key, "recording_file") == 0) {
        snprintf(buf, buf_len, "%s", g_current_recording);
        return 0;
    }

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
    if (strncmp(key, "patch_name_", 11) == 0) {
        int index = atoi(key + 11);
        if (index >= 0 && index < g_patch_count) {
            snprintf(buf, buf_len, "%s", g_patches[index].name);
            return 0;
        }
        return -1;
    }
    /* Return patch configuration as JSON for editing */
    if (strncmp(key, "patch_config_", 13) == 0) {
        int index = atoi(key + 13);
        if (index >= 0 && index < g_patch_count) {
            patch_info_t *p = &g_patches[index];
            const char *input_str = "both";
            if (p->midi_input == MIDI_INPUT_PADS) input_str = "pads";
            else if (p->midi_input == MIDI_INPUT_EXTERNAL) input_str = "external";

            const char *chord_str = "none";
            if (p->chord_type == CHORD_MAJOR) chord_str = "major";
            else if (p->chord_type == CHORD_MINOR) chord_str = "minor";
            else if (p->chord_type == CHORD_POWER) chord_str = "power";
            else if (p->chord_type == CHORD_OCTAVE) chord_str = "octave";

            const char *arp_str = "off";
            if (p->arp_mode == ARP_UP) arp_str = "up";
            else if (p->arp_mode == ARP_DOWN) arp_str = "down";
            else if (p->arp_mode == ARP_UPDOWN) arp_str = "up_down";
            else if (p->arp_mode == ARP_RANDOM) arp_str = "random";

            /* Build audio_fx JSON array */
            char fx_json[512] = "[";
            for (int i = 0; i < p->audio_fx_count && i < MAX_AUDIO_FX; i++) {
                if (i > 0) strcat(fx_json, ",");
                char fx_item[64];
                snprintf(fx_item, sizeof(fx_item), "\"%s\"", p->audio_fx[i]);
                strcat(fx_json, fx_item);
            }
            strcat(fx_json, "]");

            /* Build knob_mappings JSON array with type/min/max */
            char knob_json[1024] = "[";
            for (int i = 0; i < p->knob_mapping_count && i < MAX_KNOB_MAPPINGS; i++) {
                if (i > 0) strcat(knob_json, ",");
                char knob_item[192];
                const char *type_str = (p->knob_mappings[i].type == KNOB_TYPE_INT) ? "int" : "float";
                snprintf(knob_item, sizeof(knob_item),
                    "{\"cc\":%d,\"target\":\"%s\",\"param\":\"%s\",\"type\":\"%s\",\"min\":%.3f,\"max\":%.3f}",
                    p->knob_mappings[i].cc,
                    p->knob_mappings[i].target,
                    p->knob_mappings[i].param,
                    type_str,
                    p->knob_mappings[i].min_val,
                    p->knob_mappings[i].max_val);
                strcat(knob_json, knob_item);
            }
            strcat(knob_json, "]");

            snprintf(buf, buf_len,
                "{\"synth\":\"%s\",\"preset\":%d,\"source\":\"%s\","
                "\"input\":\"%s\",\"chord\":\"%s\",\"arp\":\"%s\","
                "\"arp_bpm\":%d,\"arp_div\":%d,\"audio_fx\":%s,"
                "\"knob_mappings\":%s}",
                p->synth_module, p->synth_preset,
                p->midi_source_module[0] ? p->midi_source_module : "",
                input_str, chord_str, arp_str,
                p->arp_tempo_bpm, p->arp_note_division, fx_json, knob_json);
            return 0;
        }
        return -1;
    }
    if (strcmp(key, "midi_fx_js") == 0) {
        if (g_current_patch >= 0 && g_current_patch < g_patch_count) {
            buf[0] = '\0';
            for (int i = 0; i < g_patches[g_current_patch].midi_fx_js_count; i++) {
                if (i > 0) {
                    strncat(buf, ",", buf_len - strlen(buf) - 1);
                }
                strncat(buf, g_patches[g_current_patch].midi_fx_js[i],
                        buf_len - strlen(buf) - 1);
            }
            return 0;
        }
        buf[0] = '\0';
        return 0;
    }
    if (strcmp(key, "synth_module") == 0) {
        snprintf(buf, buf_len, "%s", g_current_synth_module);
        return 0;
    }
    if (strcmp(key, "midi_source_module") == 0) {
        snprintf(buf, buf_len, "%s", g_current_source_module);
        return 0;
    }
    if (strcmp(key, "fx1_module") == 0) {
        if (g_current_patch >= 0 && g_current_patch < g_patch_count &&
            g_patches[g_current_patch].audio_fx_count > 0) {
            snprintf(buf, buf_len, "%s", g_patches[g_current_patch].audio_fx[0]);
        } else {
            buf[0] = '\0';
        }
        return 0;
    }
    if (strcmp(key, "fx2_module") == 0) {
        if (g_current_patch >= 0 && g_current_patch < g_patch_count &&
            g_patches[g_current_patch].audio_fx_count > 1) {
            snprintf(buf, buf_len, "%s", g_patches[g_current_patch].audio_fx[1]);
        } else {
            buf[0] = '\0';
        }
        return 0;
    }
    if (strcmp(key, "raw_midi") == 0) {
        snprintf(buf, buf_len, "%d", g_raw_midi);
        return 0;
    }
    if (strcmp(key, "midi_input") == 0) {
        const char *input = "both";
        if (g_midi_input == MIDI_INPUT_PADS) {
            input = "pads";
        } else if (g_midi_input == MIDI_INPUT_EXTERNAL) {
            input = "external";
        }
        snprintf(buf, buf_len, "%s", input);
        return 0;
    }

    /* Get live configuration (current state) for saving */
    if (strcmp(key, "get_live_config") == 0) {
        if (g_current_patch < 0 || g_current_patch >= g_patch_count) {
            buf[0] = '\0';
            return -1;
        }
        patch_info_t *p = &g_patches[g_current_patch];

        /* Get current synth preset */
        int current_preset = p->synth_preset;
        if (g_synth_plugin && g_synth_plugin->get_param) {
            char preset_buf[32];
            if (g_synth_plugin->get_param("preset", preset_buf, sizeof(preset_buf)) >= 0) {
                current_preset = atoi(preset_buf);
            }
        }

        /* Build input string */
        const char *input_str = "both";
        if (g_midi_input == MIDI_INPUT_PADS) input_str = "pads";
        else if (g_midi_input == MIDI_INPUT_EXTERNAL) input_str = "external";

        /* Build chord string */
        const char *chord_str = "none";
        if (g_chord_type == CHORD_MAJOR) chord_str = "major";
        else if (g_chord_type == CHORD_MINOR) chord_str = "minor";
        else if (g_chord_type == CHORD_POWER) chord_str = "power";
        else if (g_chord_type == CHORD_OCTAVE) chord_str = "octave";

        /* Build arp string */
        const char *arp_str = "off";
        if (g_arp_mode == ARP_UP) arp_str = "up";
        else if (g_arp_mode == ARP_DOWN) arp_str = "down";
        else if (g_arp_mode == ARP_UPDOWN) arp_str = "up_down";
        else if (g_arp_mode == ARP_RANDOM) arp_str = "random";

        /* Build audio_fx JSON array */
        char fx_json[512] = "[";
        for (int i = 0; i < p->audio_fx_count && i < MAX_AUDIO_FX; i++) {
            if (i > 0) strcat(fx_json, ",");
            char fx_item[64];
            snprintf(fx_item, sizeof(fx_item), "{\"type\":\"%s\"}", p->audio_fx[i]);
            strcat(fx_json, fx_item);
        }
        strcat(fx_json, "]");

        /* Build knob_mappings JSON array with CURRENT values */
        char knob_json[2048] = "[";
        for (int i = 0; i < g_knob_mapping_count && i < MAX_KNOB_MAPPINGS; i++) {
            if (i > 0) strcat(knob_json, ",");
            char knob_item[256];
            const char *type_str = (g_knob_mappings[i].type == KNOB_TYPE_INT) ? "int" : "float";
            snprintf(knob_item, sizeof(knob_item),
                "{\"cc\":%d,\"target\":\"%s\",\"param\":\"%s\",\"type\":\"%s\",\"min\":%.3f,\"max\":%.3f,\"value\":%.3f}",
                g_knob_mappings[i].cc,
                g_knob_mappings[i].target,
                g_knob_mappings[i].param,
                type_str,
                g_knob_mappings[i].min_val,
                g_knob_mappings[i].max_val,
                g_knob_mappings[i].current_value);
            strcat(knob_json, knob_item);
        }
        strcat(knob_json, "]");

        /* Build final JSON */
        snprintf(buf, buf_len,
            "{\"synth\":{\"module\":\"%s\",\"preset\":%d},"
            "\"source\":\"%s\",\"input\":\"%s\",\"chord\":\"%s\",\"arp\":\"%s\","
            "\"arp_bpm\":%d,\"arp_div\":%d,\"audio_fx\":%s,"
            "\"knob_mappings\":%s}",
            g_current_synth_module, current_preset,
            g_current_source_module,
            input_str, chord_str, arp_str,
            p->arp_tempo_bpm, p->arp_note_division, fx_json, knob_json);
        return 0;
    }

    /* Forward to synth */
    if (g_synth_plugin && g_synth_plugin->get_param) {
        return g_synth_plugin->get_param(key, buf, buf_len);
    }
    return -1;
}

static void plugin_render_block(int16_t *out_interleaved_lr, int frames) {
    int16_t scratch[FRAMES_PER_BLOCK * 2];

    if (g_source_plugin && g_source_plugin->render_block) {
        g_source_plugin->render_block(scratch, frames);
    }

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

    /* Write to ring buffer if recording */
    if (g_recording && g_ring_buffer) {
        size_t samples_to_write = frames * NUM_CHANNELS;
        size_t buffer_samples = RING_BUFFER_SAMPLES * NUM_CHANNELS;

        /* Check if we have space (drop samples if buffer is full to avoid blocking) */
        if (ring_available_write() >= samples_to_write) {
            size_t write_pos = g_ring_write_pos;

            for (size_t i = 0; i < samples_to_write; i++) {
                g_ring_buffer[write_pos] = out_interleaved_lr[i];
                write_pos = (write_pos + 1) % buffer_samples;
            }

            g_ring_write_pos = write_pos;

            /* Signal writer thread that data is available */
            pthread_mutex_lock(&g_ring_mutex);
            pthread_cond_signal(&g_ring_cond);
            pthread_mutex_unlock(&g_ring_mutex);
        }
        /* If buffer is full, we drop samples rather than block the audio thread */
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
    memcpy(&g_source_host_api, host, sizeof(host_api_v1_t));
    g_source_host_api.midi_send_internal = midi_source_send;
    g_source_host_api.midi_send_external = midi_source_send;

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
