/*
 * Signal Chain Host DSP Plugin
 *
 * Orchestrates a signal chain: Input → MIDI FX → Sound Generator → Audio FX → Output
 * Phase 5: Arpeggiator support
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v1.h"
#include "host/audio_fx_api_v2.h"
#include "host/midi_fx_api_v1.h"
#include "../../../host/unified_log.h"

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
#define MAX_MIDI_FX 2       /* Max native MIDI FX modules per chain */
#define MAX_PATH_LEN 256
#define MAX_NAME_LEN 64

/* Optional file-based debug tracing for chain parsing/preset save diagnostics. */
#define CHAIN_DEBUG_FLAG_PATH "/data/UserData/move-anything/chain_debug_on"
#define CHAIN_DEBUG_LOG_PATH "/data/UserData/move-anything/chain_debug.log"

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
#define KNOB_STEP_FLOAT 0.0015f /* Base step for floats (~600 clicks for 0-1 at min speed) */
#define KNOB_STEP_INT 1        /* Base step for int params */

/* Knob acceleration settings */
#define KNOB_ACCEL_MIN_MULT 1    /* Multiplier for slow turns */
#define KNOB_ACCEL_MAX_MULT 8    /* Multiplier for fast turns (floats) */
#define KNOB_ACCEL_MAX_MULT_INT 3 /* Multiplier for fast turns (ints - smaller to avoid jumps) */
#define KNOB_ACCEL_SLOW_MS 150   /* Slower than this = min multiplier */
#define KNOB_ACCEL_FAST_MS 25    /* Faster than this = max multiplier */

/* Knob mapping types */
typedef enum {
    KNOB_TYPE_FLOAT = 0,
    KNOB_TYPE_INT = 1,
    KNOB_TYPE_ENUM = 2
} knob_type_t;

/* Knob mapping structure */
typedef struct {
    int cc;              /* CC number (71-78 for knobs 1-8) */
    char target[16];     /* Component: "synth", "fx1", "fx2", "midi_fx" */
    char param[32];      /* Parameter key (lookup metadata in chain_params) */
    float current_value; /* Current value only */
} knob_mapping_t;

/* Chain parameter info from module.json */
#define MAX_CHAIN_PARAMS 32
#define MAX_ENUM_OPTIONS 64
typedef struct {
    char key[32];           /* Parameter key (e.g., "preset", "decay") */
    char name[64];          /* Display name */
    knob_type_t type;       /* Parameter type: FLOAT, INT, or ENUM */
    float min_val;          /* Minimum value */
    float max_val;          /* Maximum value (or -1 if dynamic via max_param) */
    float default_val;      /* Default value */
    char max_param[32];     /* Dynamic max param key (e.g., "preset_count") */
    char unit[16];          /* Unit suffix (e.g., "Hz", "dB", "%") */
    char display_format[16]; /* Display format hint (e.g., "%.2f", "%d") */
    float step;             /* Step size for UI increments */
    char options[MAX_ENUM_OPTIONS][32];  /* Enum options (if type is ENUM) */
    int option_count;       /* Number of enum options */
} chain_param_info_t;

#define MOVE_PAD_NOTE_MAX 99

/* MIDI FX parameter storage (key-value pairs for flexible configuration) */
#define MAX_MIDI_FX_PARAMS 8
typedef struct {
    char key[32];
    char val[32];
} midi_fx_param_t;

/* State storage size for FX plugins */
#define MAX_FX_STATE_LEN 2048

/*
 * Format a parameter value for display based on its metadata.
 * Returns length of formatted string, or -1 on error.
 */
static int format_param_value(chain_param_info_t *param, float value, char *buf, int buf_len) {
    if (!param || !buf || buf_len < 2) return -1;

    if (param->type == KNOB_TYPE_ENUM) {
        /* Use option label for enums */
        int idx = (int)value;
        if (idx >= 0 && idx < param->option_count) {
            int len = strlen(param->options[idx]);
            if (len >= buf_len) len = buf_len - 1;
            memcpy(buf, param->options[idx], len);
            buf[len] = '\0';
            return len;
        }
        /* Fallback for out-of-range enum */
        snprintf(buf, buf_len, "%d", idx);
        return strlen(buf);
    }

    /* Scale 0-1 values to 0-100 for percentage display */
    float display_value = value;
    if (strcmp(param->unit, "%") == 0 && param->max_val <= 1.0f) {
        display_value = value * 100.0f;
    }

    /* Format numeric value */
    char val_str[32];
    if (param->display_format[0]) {
        /* Use custom format */
        snprintf(val_str, sizeof(val_str), param->display_format, display_value);
    } else {
        /* Use defaults based on type */
        if (param->type == KNOB_TYPE_FLOAT) {
            snprintf(val_str, sizeof(val_str), "%.2f", display_value);
        } else {
            snprintf(val_str, sizeof(val_str), "%d", (int)display_value);
        }
    }

    /* Add unit suffix if present */
    if (param->unit[0]) {
        snprintf(buf, buf_len, "%s %s", val_str, param->unit);
    } else {
        snprintf(buf, buf_len, "%s", val_str);
    }

    return strlen(buf);
}

/* MIDI FX configuration (module + params + state) */
typedef struct {
    char module[MAX_NAME_LEN];
    midi_fx_param_t params[MAX_MIDI_FX_PARAMS];
    int param_count;
    char state[MAX_FX_STATE_LEN];  /* JSON state for MIDI FX plugin */
} midi_fx_config_t;

/* Audio FX configuration (module + params + state) */
typedef struct {
    char module[MAX_NAME_LEN];
    midi_fx_param_t params[MAX_MIDI_FX_PARAMS];  /* Reuse param struct */
    int param_count;
    char state[MAX_FX_STATE_LEN];  /* JSON state for audio FX plugin */
} audio_fx_config_t;

/* Synth state storage size - large synths may need 5-6KB for full state */
#define MAX_SYNTH_STATE_LEN 8192

/* Patch info */
typedef struct {
    char name[MAX_NAME_LEN];
    char path[MAX_PATH_LEN];
    char synth_module[MAX_NAME_LEN];
    int synth_preset;
    char synth_state[MAX_SYNTH_STATE_LEN];  /* JSON state for synth plugin */
    char midi_source_module[MAX_NAME_LEN];
    audio_fx_config_t audio_fx[MAX_AUDIO_FX];  /* Now includes params */
    int audio_fx_count;
    midi_fx_config_t midi_fx[MAX_MIDI_FX];      /* Native MIDI FX with params */
    int midi_fx_count;
    char midi_fx_js[MAX_MIDI_FX_JS][MAX_NAME_LEN];
    int midi_fx_js_count;
    midi_input_t midi_input;
    knob_mapping_t knob_mappings[MAX_KNOB_MAPPINGS];
    int knob_mapping_count;
    int receive_channel;   /* 0=not saved, 1-16=specific channel (from saved preset) */
    int forward_channel;   /* 0=not saved, -2=passthrough, -1=auto, 1-16=specific (from saved preset) */
} patch_info_t;

/* ============================================================================
 * Parameter Smoothing (to avoid zipper noise on knob changes)
 * ============================================================================ */

#define MAX_SMOOTH_PARAMS 16
#define SMOOTH_COEFF 0.15f  /* Smoothing coefficient per block (~5ms at 128 frames/44100Hz) */

typedef struct {
    char key[MAX_NAME_LEN];
    float target;
    float current;
    int active;
} smooth_param_t;

typedef struct {
    smooth_param_t params[MAX_SMOOTH_PARAMS];
    int count;
} param_smoother_t;

/* Find or create a smoothed parameter slot */
static smooth_param_t* smoother_get_param(param_smoother_t *smoother, const char *key) {
    /* Look for existing */
    for (int i = 0; i < smoother->count; i++) {
        if (strcmp(smoother->params[i].key, key) == 0) {
            return &smoother->params[i];
        }
    }
    /* Create new if space */
    if (smoother->count < MAX_SMOOTH_PARAMS) {
        smooth_param_t *p = &smoother->params[smoother->count++];
        strncpy(p->key, key, MAX_NAME_LEN - 1);
        p->key[MAX_NAME_LEN - 1] = '\0';
        p->target = 0.0f;
        p->current = 0.0f;
        p->active = 0;
        return p;
    }
    return NULL;
}

/* Set a parameter target value for smoothing */
static void smoother_set_target(param_smoother_t *smoother, const char *key, float value) {
    smooth_param_t *p = smoother_get_param(smoother, key);
    if (p) {
        /* Always jump current to new value.  The hierarchy editor uses a
         * read-modify-write cycle: it reads the plugin's current value,
         * applies a delta, and writes back.  If current lags behind target
         * (as it does with interpolation), render_block overwrites the
         * plugin value with the lagged current, and the next UI read sees
         * that lagged value — making the parameter appear stuck near 0. */
        p->current = value;
        p->target = value;
        p->active = 1;
    }
}

/* Update all smoothed parameters toward their targets, returns 1 if any changed */
static int smoother_update(param_smoother_t *smoother) {
    int changed = 0;
    for (int i = 0; i < smoother->count; i++) {
        smooth_param_t *p = &smoother->params[i];
        if (p->active) {
            float diff = p->target - p->current;
            if (fabsf(diff) > 0.0001f) {
                p->current += diff * SMOOTH_COEFF;
                changed = 1;
            } else {
                p->current = p->target;
            }
        }
    }
    return changed;
}

/* Reset smoother state */
static void smoother_reset(param_smoother_t *smoother) {
    smoother->count = 0;
    memset(smoother->params, 0, sizeof(smoother->params));
}

/* Check if a string looks like a float value (for smoothing eligibility) */
static int is_smoothable_float(const char *val, float *out_value) {
    if (!val || !val[0]) return 0;

    /* Skip if it's clearly not a number */
    char c = val[0];
    if (c != '-' && c != '.' && (c < '0' || c > '9')) return 0;

    char *endptr;
    float f = strtof(val, &endptr);

    /* Must have parsed something and no trailing garbage (except whitespace) */
    if (endptr == val) return 0;
    while (*endptr == ' ' || *endptr == '\t') endptr++;
    if (*endptr != '\0') return 0;

    /* Don't smooth integer-like values (presets, indices) */
    if (f == (int)f && f >= 0 && f < 1000) {
        /* Could be an index - only smooth if it's in 0-1 range or has decimal */
        if (strchr(val, '.') == NULL && (f < 0.0f || f > 1.0f)) {
            return 0;  /* Likely an integer index, don't smooth */
        }
    }

    if (out_value) *out_value = f;
    return 1;
}

/* ============================================================================
 * V2 Instance-Based API
 * ============================================================================ */

/* Chain instance state - contains all per-instance data for v2 API */
typedef struct chain_instance {
    /* Module directory */
    char module_dir[MAX_PATH_LEN];

    /* Sub-plugin state - Synth */
    void *synth_handle;
    plugin_api_v1_t *synth_plugin;
    plugin_api_v2_t *synth_plugin_v2;
    void *synth_instance;
    char current_synth_module[MAX_NAME_LEN];
    int synth_default_forward_channel;  /* -1 = no default, 0-15 = channel */

    /* Sub-plugin state - MIDI Source */
    void *source_handle;
    plugin_api_v1_t *source_plugin;
    char current_source_module[MAX_NAME_LEN];

    /* Audio FX state */
    void *fx_handles[MAX_AUDIO_FX];
    audio_fx_api_v1_t *fx_plugins[MAX_AUDIO_FX];
    audio_fx_api_v2_t *fx_plugins_v2[MAX_AUDIO_FX];
    void *fx_instances[MAX_AUDIO_FX];
    int fx_is_v2[MAX_AUDIO_FX];
    int fx_count;
    char current_fx_modules[MAX_AUDIO_FX][MAX_NAME_LEN];  /* Track loaded FX names */

    /* Optional MIDI handler for audio FX (discovered via dlsym) */
    void (*fx_on_midi[MAX_AUDIO_FX])(void *instance, const uint8_t *msg, int len, int source);

    /* Module parameter info */
    chain_param_info_t synth_params[MAX_CHAIN_PARAMS];
    int synth_param_count;
    chain_param_info_t fx_params[MAX_AUDIO_FX][MAX_CHAIN_PARAMS];
    int fx_param_counts[MAX_AUDIO_FX];

    /* Patch state */
    patch_info_t patches[MAX_PATCHES];
    int patch_count;
    int current_patch;

    /* MIDI FX module state */
    void *midi_fx_handles[MAX_MIDI_FX];
    midi_fx_api_v1_t *midi_fx_plugins[MAX_MIDI_FX];
    void *midi_fx_instances[MAX_MIDI_FX];
    int midi_fx_count;
    char current_midi_fx_modules[MAX_MIDI_FX][MAX_NAME_LEN];
    chain_param_info_t midi_fx_params[MAX_MIDI_FX][MAX_CHAIN_PARAMS];
    int midi_fx_param_counts[MAX_MIDI_FX];
    char midi_fx_ui_hierarchy[MAX_MIDI_FX][2048];  /* Cached ui_hierarchy JSON */

    /* Knob mapping state */
    knob_mapping_t knob_mappings[MAX_KNOB_MAPPINGS];
    int knob_mapping_count;
    uint64_t knob_last_time_ms[MAX_KNOB_MAPPINGS];  /* For acceleration */

    /* Mute countdown after patch switch */
    int mute_countdown;

    /* Recording state */
    int recording;
    FILE *wav_file;
    uint32_t samples_written;
    char current_recording[MAX_PATH_LEN];
    int16_t *ring_buffer;
    volatile size_t ring_write_pos;
    volatile size_t ring_read_pos;
    pthread_t writer_thread;
    pthread_mutex_t ring_mutex;
    pthread_cond_t ring_cond;
    volatile int writer_running;
    volatile int writer_should_exit;

    /* MIDI input filter */
    midi_input_t midi_input;

    /* Raw MIDI bypass */
    int raw_midi;

    /* Source UI state */
    int source_ui_active;

    /* Component UI mode */
    int component_ui_mode;

    /* Host APIs for sub-plugins */
    host_api_v1_t subplugin_host_api;
    host_api_v1_t source_host_api;

    /* Reference to host API (shared) */
    const host_api_v1_t *host;

    /* Parameter smoothing for synth and FX */
    param_smoother_t synth_smoother;
    param_smoother_t fx_smoothers[MAX_AUDIO_FX];

    /* External audio injection (e.g. Move track audio from Link Audio).
     * Set by host before render_block; mixed after synth, before FX. */
    int16_t *inject_audio;
    int inject_audio_frames;

    /* When set, render_block outputs raw synth only (no inject mix, no FX).
     * The shim calls chain_process_fx() separately for same-frame FX. */
    int external_fx_mode;
} chain_instance_t;

/* ============================================================================
 * Global State (v1 compatibility)
 * ============================================================================ */

/* Host API provided by main host */
static const host_api_v1_t *g_host = NULL;

/* Sub-plugin state */
static void *g_synth_handle = NULL;
static plugin_api_v1_t *g_synth_plugin = NULL;
static plugin_api_v2_t *g_synth_plugin_v2 = NULL;   /* v2 API */
static void *g_synth_instance = NULL;                /* v2 instance */
static int g_synth_is_v2 = 0;                        /* 1 if using v2 */
static char g_current_synth_module[MAX_NAME_LEN] = "";

static void *g_source_handle = NULL;
static plugin_api_v1_t *g_source_plugin = NULL;
static char g_current_source_module[MAX_NAME_LEN] = "";

/* Audio FX state */
static void *g_fx_handles[MAX_AUDIO_FX];
static audio_fx_api_v1_t *g_fx_plugins[MAX_AUDIO_FX];        /* v1 API */
static audio_fx_api_v2_t *g_fx_plugins_v2[MAX_AUDIO_FX];     /* v2 API */
static void *g_fx_instances[MAX_AUDIO_FX];                   /* v2 instances */
static int g_fx_is_v2[MAX_AUDIO_FX];                         /* 1 if using v2 */
static int g_fx_count = 0;

/* Synth V1/V2 helper functions */
static inline int synth_loaded(void) {
    return (g_synth_is_v2 && g_synth_plugin_v2 && g_synth_instance) ||
           (!g_synth_is_v2 && g_synth_plugin);
}

static inline void synth_on_midi(const uint8_t *msg, int len, int source) {
    if (g_synth_is_v2 && g_synth_plugin_v2 && g_synth_plugin_v2->on_midi && g_synth_instance) {
        g_synth_plugin_v2->on_midi(g_synth_instance, msg, len, source);
    } else if (g_synth_plugin && g_synth_plugin->on_midi) {
        g_synth_plugin->on_midi(msg, len, source);
    }
}

static inline void synth_set_param(const char *key, const char *val) {
    if (g_synth_is_v2 && g_synth_plugin_v2 && g_synth_plugin_v2->set_param && g_synth_instance) {
        g_synth_plugin_v2->set_param(g_synth_instance, key, val);
    } else if (g_synth_plugin && g_synth_plugin->set_param) {
        g_synth_plugin->set_param(key, val);
    }
}

static inline int synth_get_param(const char *key, char *buf, int buf_len) {
    if (g_synth_is_v2 && g_synth_plugin_v2 && g_synth_plugin_v2->get_param && g_synth_instance) {
        return g_synth_plugin_v2->get_param(g_synth_instance, key, buf, buf_len);
    } else if (g_synth_plugin && g_synth_plugin->get_param) {
        return g_synth_plugin->get_param(key, buf, buf_len);
    }
    return -1;
}

static inline void synth_render_block(int16_t *out, int frames) {
    if (g_synth_is_v2 && g_synth_plugin_v2 && g_synth_plugin_v2->render_block && g_synth_instance) {
        g_synth_plugin_v2->render_block(g_synth_instance, out, frames);
    } else if (g_synth_plugin && g_synth_plugin->render_block) {
        g_synth_plugin->render_block(out, frames);
    }
}

static inline int synth_get_error(char *buf, int buf_len) {
    if (g_synth_is_v2 && g_synth_plugin_v2 && g_synth_plugin_v2->get_error && g_synth_instance) {
        return g_synth_plugin_v2->get_error(g_synth_instance, buf, buf_len);
    }
    /* V1 plugins don't have get_error, try via get_param as fallback */
    if (g_synth_plugin && g_synth_plugin->get_param) {
        return g_synth_plugin->get_param("load_error", buf, buf_len);
    }
    return 0;  /* No error */
}

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

/* JS MIDI FX state */
static int g_js_midi_fx_enabled = 0;

/* Knob mapping state */
static knob_mapping_t g_knob_mappings[MAX_KNOB_MAPPINGS];
static int g_knob_mapping_count = 0;
static uint64_t g_knob_last_time_ms[MAX_KNOB_MAPPINGS] = {0};  /* For acceleration */

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
static chain_param_info_t *find_param_by_key(chain_instance_t *inst, const char *target, const char *key);
static float dsp_value_to_float(const char *val_str, chain_param_info_t *pinfo, float fallback);
static void v2_chain_log(chain_instance_t *inst, const char *msg);  /* Forward declaration */
static void parse_debug_log(const char *msg);  /* Forward declaration */

/* Plugin API we return to host */
static plugin_api_v1_t g_plugin_api;

/* Logging helper */
/* Validate a module/FX name contains no path traversal sequences */
static int valid_module_name(const char *name) {
    if (!name || !name[0]) return 0;
    if (strstr(name, "..") != NULL) return 0;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return 0;
    return 1;
}

static void chain_log(const char *msg) {
    /* Use unified log */
    unified_log("chain", LOG_LEVEL_DEBUG, "%s", msg);

    /* Also call host log if available */
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[chain] %s", msg);
        g_host->log(buf);
    }
}

/* Get current time in milliseconds (for knob acceleration) */
static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Calculate knob acceleration multiplier based on time between events */
static int calc_knob_accel(int knob_index) {
    if (knob_index < 0 || knob_index >= MAX_KNOB_MAPPINGS) return 1;

    uint64_t now = get_time_ms();
    uint64_t last = g_knob_last_time_ms[knob_index];
    g_knob_last_time_ms[knob_index] = now;

    if (last == 0) return KNOB_ACCEL_MIN_MULT;  /* First event */

    uint64_t elapsed = now - last;

    if (elapsed >= KNOB_ACCEL_SLOW_MS) {
        return KNOB_ACCEL_MIN_MULT;
    } else if (elapsed <= KNOB_ACCEL_FAST_MS) {
        return KNOB_ACCEL_MAX_MULT;
    } else {
        /* Linear interpolation between min and max */
        float ratio = (float)(KNOB_ACCEL_SLOW_MS - elapsed) /
                      (float)(KNOB_ACCEL_SLOW_MS - KNOB_ACCEL_FAST_MS);
        return KNOB_ACCEL_MIN_MULT + (int)(ratio * (KNOB_ACCEL_MAX_MULT - KNOB_ACCEL_MIN_MULT));
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
    struct stat st;
    if (stat(RECORDINGS_DIR, &st) != 0) {
        mkdir(RECORDINGS_DIR, 0755);
    }

    /* Generate filename with timestamp */
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm_info = localtime_r(&now, &tm_buf);

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

    { size_t nr = fread(json, 1, size, f); json[nr] = '\0'; }
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

    int plugin_loaded = 0;

    /* V2 API required */
    move_plugin_init_v2_fn init_v2 = (move_plugin_init_v2_fn)dlsym(g_synth_handle, MOVE_PLUGIN_INIT_V2_SYMBOL);
    if (!init_v2) {
        chain_log("Synth plugin does not support V2 API (V2 required)");
        dlclose(g_synth_handle);
        g_synth_handle = NULL;
        return -1;
    }

    g_synth_plugin_v2 = init_v2(&g_subplugin_host_api);
    if (!g_synth_plugin_v2 || g_synth_plugin_v2->api_version != MOVE_PLUGIN_API_VERSION_2) {
        chain_log("Synth V2 API version mismatch");
        dlclose(g_synth_handle);
        g_synth_handle = NULL;
        return -1;
    }

    g_synth_instance = g_synth_plugin_v2->create_instance(module_path, config_json);
    if (!g_synth_instance) {
        chain_log("Synth V2 create_instance failed");
        dlclose(g_synth_handle);
        g_synth_handle = NULL;
        g_synth_plugin_v2 = NULL;
        return -1;
    }

    g_synth_is_v2 = 1;
    plugin_loaded = 1;
    chain_log("Synth loaded with V2 API");

    /* Parse chain_params from module.json */
    if (parse_chain_params(module_path, g_synth_params, &g_synth_param_count) < 0) {
        chain_log("ERROR: Failed to parse synth parameters");
        g_synth_plugin_v2->destroy_instance(g_synth_instance);
        dlclose(g_synth_handle);
        g_synth_handle = NULL;
        g_synth_plugin_v2 = NULL;
        g_synth_instance = NULL;
        g_synth_is_v2 = 0;
        return -1;
    }
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
    if (g_synth_is_v2 && g_synth_plugin_v2) {
        /* V2: destroy instance */
        if (g_synth_plugin_v2->destroy_instance && g_synth_instance) {
            g_synth_plugin_v2->destroy_instance(g_synth_instance);
        }
        g_synth_instance = NULL;
        g_synth_plugin_v2 = NULL;
    } else if (g_synth_plugin) {
        /* V1: call on_unload */
        if (g_synth_plugin->on_unload) {
            g_synth_plugin->on_unload();
        }
        g_synth_plugin = NULL;
    }
    if (g_synth_handle) {
        dlclose(g_synth_handle);
        g_synth_handle = NULL;
    }
    g_synth_is_v2 = 0;
    g_current_synth_module[0] = '\0';
}

/* Load an audio FX plugin */
static int load_audio_fx(const char *fx_name) {
    char msg[256];

    if (!valid_module_name(fx_name)) {
        chain_log("Invalid audio FX name");
        return -1;
    }

    if (g_fx_count >= MAX_AUDIO_FX) {
        chain_log("Max audio FX reached");
        return -1;
    }

    /* Build path to FX - all audio FX in modules/audio_fx/ */
    char fx_path[MAX_PATH_LEN];
    char fx_dir[MAX_PATH_LEN];
    snprintf(fx_path, sizeof(fx_path), "%s/../audio_fx/%s/%s.so",
             g_module_dir, fx_name, fx_name);
    snprintf(fx_dir, sizeof(fx_dir), "%s/../audio_fx/%s", g_module_dir, fx_name);

    snprintf(msg, sizeof(msg), "Loading audio FX: %s", fx_path);
    chain_log(msg);

    /* Open the shared library */
    void *handle = dlopen(fx_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        snprintf(msg, sizeof(msg), "dlopen failed: %s", dlerror());
        chain_log(msg);
        return -1;
    }

    int slot = g_fx_count;

    /* V2 API required */
    audio_fx_init_v2_fn init_v2_fn = (audio_fx_init_v2_fn)dlsym(handle, AUDIO_FX_INIT_V2_SYMBOL);
    if (!init_v2_fn) {
        snprintf(msg, sizeof(msg), "Audio FX %s does not support V2 API (V2 required)", fx_name);
        chain_log(msg);
        dlclose(handle);
        return -1;
    }

    audio_fx_api_v2_t *fx_v2 = init_v2_fn(&g_subplugin_host_api);
    if (!fx_v2 || fx_v2->api_version != AUDIO_FX_API_VERSION_2) {
        snprintf(msg, sizeof(msg), "Audio FX %s V2 API version mismatch", fx_name);
        chain_log(msg);
        dlclose(handle);
        return -1;
    }

    void *instance = fx_v2->create_instance(fx_dir, NULL);
    if (!instance) {
        snprintf(msg, sizeof(msg), "Audio FX %s V2 create_instance failed", fx_name);
        chain_log(msg);
        dlclose(handle);
        return -1;
    }

    g_fx_handles[slot] = handle;
    g_fx_plugins[slot] = NULL;
    g_fx_plugins_v2[slot] = fx_v2;
    g_fx_instances[slot] = instance;
    g_fx_is_v2[slot] = 1;

    if (parse_chain_params(fx_dir, g_fx_params[slot], &g_fx_param_counts[slot]) < 0) {
        chain_log("ERROR: Failed to parse audio FX parameters");
        fx_v2->destroy_instance(instance);
        dlclose(handle);
        g_fx_handles[slot] = NULL;
        g_fx_plugins_v2[slot] = NULL;
        g_fx_instances[slot] = NULL;
        g_fx_is_v2[slot] = 0;
        return -1;
    }
    g_fx_count++;

    snprintf(msg, sizeof(msg), "Audio FX v2 loaded: %s (slot %d, %d params)",
             fx_name, slot, g_fx_param_counts[slot]);
    chain_log(msg);
    return 0;
}

/* Unload all audio FX */
static void unload_all_audio_fx(void) {
    for (int i = 0; i < g_fx_count; i++) {
        if (g_fx_is_v2[i]) {
            /* v2 API - destroy instance */
            if (g_fx_plugins_v2[i] && g_fx_instances[i] && g_fx_plugins_v2[i]->destroy_instance) {
                g_fx_plugins_v2[i]->destroy_instance(g_fx_instances[i]);
            }
        } else {
            /* v1 API - call on_unload */
            if (g_fx_plugins[i] && g_fx_plugins[i]->on_unload) {
                g_fx_plugins[i]->on_unload();
            }
        }
        if (g_fx_handles[i]) {
            dlclose(g_fx_handles[i]);
        }
        g_fx_param_counts[i] = 0;
        g_fx_handles[i] = NULL;
        g_fx_plugins[i] = NULL;
        g_fx_plugins_v2[i] = NULL;
        g_fx_instances[i] = NULL;
        g_fx_is_v2[i] = 0;
    }
    g_fx_count = 0;
    chain_log("All audio FX unloaded");
}

/* Load a MIDI FX plugin into an instance slot */
static int v2_load_midi_fx(chain_instance_t *inst, const char *fx_name) {
    char msg[256];

    if (!inst || !fx_name || !fx_name[0]) return -1;

    if (inst->midi_fx_count >= MAX_MIDI_FX) {
        v2_chain_log(inst, "Max MIDI FX reached");
        return -1;
    }

    /* Build path to MIDI FX - in modules/midi_fx/ */
    char fx_path[MAX_PATH_LEN];
    char fx_dir[MAX_PATH_LEN];
    snprintf(fx_path, sizeof(fx_path), "%s/../midi_fx/%s/dsp.so",
             inst->module_dir, fx_name);
    snprintf(fx_dir, sizeof(fx_dir), "%s/../midi_fx/%s", inst->module_dir, fx_name);

    snprintf(msg, sizeof(msg), "Loading MIDI FX: %s", fx_path);
    v2_chain_log(inst, msg);

    /* Open the shared library */
    void *handle = dlopen(fx_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        snprintf(msg, sizeof(msg), "dlopen failed: %s", dlerror());
        v2_chain_log(inst, msg);
        return -1;
    }

    int slot = inst->midi_fx_count;

    /* Look for init function */
    midi_fx_init_fn init_fn = (midi_fx_init_fn)dlsym(handle, MIDI_FX_INIT_SYMBOL);
    if (!init_fn) {
        snprintf(msg, sizeof(msg), "MIDI FX %s missing init symbol", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    midi_fx_api_v1_t *api = init_fn(&g_subplugin_host_api);
    if (!api || api->api_version != MIDI_FX_API_VERSION) {
        snprintf(msg, sizeof(msg), "MIDI FX %s API version mismatch", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    void *instance = api->create_instance(fx_dir, NULL);
    if (!instance) {
        snprintf(msg, sizeof(msg), "MIDI FX %s create_instance failed", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    inst->midi_fx_handles[slot] = handle;
    inst->midi_fx_plugins[slot] = api;
    inst->midi_fx_instances[slot] = instance;
    strncpy(inst->current_midi_fx_modules[slot], fx_name, MAX_NAME_LEN - 1);
    inst->current_midi_fx_modules[slot][MAX_NAME_LEN - 1] = '\0';

    /* Parse chain_params from module.json for type info */
    if (parse_chain_params(fx_dir, inst->midi_fx_params[slot], &inst->midi_fx_param_counts[slot]) < 0) {
        v2_chain_log(inst, "ERROR: Failed to parse MIDI FX parameters");
        api->destroy_instance(instance);
        dlclose(handle);
        inst->midi_fx_handles[slot] = NULL;
        inst->midi_fx_plugins[slot] = NULL;
        inst->midi_fx_instances[slot] = NULL;
        inst->current_midi_fx_modules[slot][0] = '\0';
        return -1;
    }

    /* Parse ui_hierarchy from module.json */
    {
        char json_path[MAX_PATH_LEN];
        snprintf(json_path, sizeof(json_path), "%s/module.json", fx_dir);
        FILE *f = fopen(json_path, "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (size > 0 && size < 8192) {
                char *json = malloc(size + 1);
                if (json) {
                    { size_t nr = fread(json, 1, size, f); json[nr] = '\0'; }
                    /* Find ui_hierarchy object */
                    const char *hier_start = strstr(json, "\"ui_hierarchy\"");
                    if (hier_start) {
                        const char *obj_start = strchr(hier_start, '{');
                        if (obj_start) {
                            /* Find matching } by counting braces */
                            int depth = 1;
                            const char *obj_end = obj_start + 1;
                            while (*obj_end && depth > 0) {
                                if (*obj_end == '{') depth++;
                                else if (*obj_end == '}') depth--;
                                obj_end++;
                            }
                            int len = (int)(obj_end - obj_start);
                            if (len > 0 && len < 2047) {
                                strncpy(inst->midi_fx_ui_hierarchy[slot], obj_start, len);
                                inst->midi_fx_ui_hierarchy[slot][len] = '\0';
                            }
                        }
                    }
                    free(json);
                }
            }
            fclose(f);
        }
    }

    inst->midi_fx_count++;

    snprintf(msg, sizeof(msg), "MIDI FX loaded: %s (slot %d)", fx_name, slot);
    v2_chain_log(inst, msg);
    return 0;
}

/* Unload all MIDI FX from an instance */
static void v2_unload_all_midi_fx(chain_instance_t *inst) {
    if (!inst) return;

    for (int i = 0; i < inst->midi_fx_count; i++) {
        if (inst->midi_fx_plugins[i] && inst->midi_fx_instances[i] &&
            inst->midi_fx_plugins[i]->destroy_instance) {
            inst->midi_fx_plugins[i]->destroy_instance(inst->midi_fx_instances[i]);
        }
        if (inst->midi_fx_handles[i]) {
            dlclose(inst->midi_fx_handles[i]);
        }
        inst->midi_fx_handles[i] = NULL;
        inst->midi_fx_plugins[i] = NULL;
        inst->midi_fx_instances[i] = NULL;
        inst->current_midi_fx_modules[i][0] = '\0';
        inst->midi_fx_param_counts[i] = 0;
        inst->midi_fx_ui_hierarchy[i][0] = '\0';
    }
    inst->midi_fx_count = 0;
}

/* Process MIDI through all loaded MIDI FX modules */
static int v2_process_midi_fx(chain_instance_t *inst,
                              const uint8_t *in_msg, int in_len,
                              uint8_t out_msgs[][3], int out_lens[],
                              int max_out) {
    if (!inst || inst->midi_fx_count == 0) {
        /* No MIDI FX - copy input to output */
        if (max_out > 0) {
            out_msgs[0][0] = in_msg[0];
            out_msgs[0][1] = in_len > 1 ? in_msg[1] : 0;
            out_msgs[0][2] = in_len > 2 ? in_msg[2] : 0;
            out_lens[0] = in_len;
            return 1;
        }
        return 0;
    }

    /* Process through chain of MIDI FX */
    uint8_t current[MIDI_FX_MAX_OUT_MSGS][3];
    int current_lens[MIDI_FX_MAX_OUT_MSGS];
    int current_count = 1;

    current[0][0] = in_msg[0];
    current[0][1] = in_len > 1 ? in_msg[1] : 0;
    current[0][2] = in_len > 2 ? in_msg[2] : 0;
    current_lens[0] = in_len;

    for (int fx = 0; fx < inst->midi_fx_count; fx++) {
        midi_fx_api_v1_t *api = inst->midi_fx_plugins[fx];
        void *fx_inst = inst->midi_fx_instances[fx];
        if (!api || !fx_inst || !api->process_midi) continue;

        uint8_t next[MIDI_FX_MAX_OUT_MSGS][3];
        int next_lens[MIDI_FX_MAX_OUT_MSGS];
        int next_count = 0;

        /* Process each message from previous stage */
        for (int m = 0; m < current_count && next_count < MIDI_FX_MAX_OUT_MSGS; m++) {
            int out_count = api->process_midi(fx_inst,
                                              current[m], current_lens[m],
                                              &next[next_count], &next_lens[next_count],
                                              MIDI_FX_MAX_OUT_MSGS - next_count);
            next_count += out_count;
        }

        /* Copy to current for next iteration */
        current_count = next_count;
        for (int i = 0; i < next_count; i++) {
            current[i][0] = next[i][0];
            current[i][1] = next[i][1];
            current[i][2] = next[i][2];
            current_lens[i] = next_lens[i];
        }
    }

    /* Copy final output */
    int out_count = 0;
    for (int i = 0; i < current_count && out_count < max_out; i++) {
        out_msgs[out_count][0] = current[i][0];
        out_msgs[out_count][1] = current[i][1];
        out_msgs[out_count][2] = current[i][2];
        out_lens[out_count] = current_lens[i];
        out_count++;
    }
    return out_count;
}

/* Call tick on all MIDI FX modules and send generated messages to synth */
static void v2_tick_midi_fx(chain_instance_t *inst, int frames) {
    if (!inst) return;

    for (int fx = 0; fx < inst->midi_fx_count; fx++) {
        midi_fx_api_v1_t *api = inst->midi_fx_plugins[fx];
        void *fx_inst = inst->midi_fx_instances[fx];
        if (!api || !fx_inst || !api->tick) continue;

        uint8_t out_msgs[MIDI_FX_MAX_OUT_MSGS][3];
        int out_lens[MIDI_FX_MAX_OUT_MSGS];
        int count = api->tick(fx_inst, frames, SAMPLE_RATE,
                              out_msgs, out_lens, MIDI_FX_MAX_OUT_MSGS);

        /* Send generated messages to synth */
        for (int i = 0; i < count; i++) {
            if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->on_midi) {
                inst->synth_plugin_v2->on_midi(inst->synth_instance, out_msgs[i], out_lens[i], 0);
            } else if (inst->synth_plugin && inst->synth_plugin->on_midi) {
                inst->synth_plugin->on_midi(out_msgs[i], out_lens[i], 0);
            }
        }
    }
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

/*
 * Check if a JSON value is an object (starts with '{') vs string/primitive
 */
static int json_value_is_object(const char *val) {
    while (*val == ' ' || *val == '\t' || *val == '\n') val++;
    return *val == '{';
}

/*
 * Check if JSON object has a specific key
 */
static int json_object_has_key(const char *obj, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    return strstr(obj, search) != NULL;
}

/*
 * Parse a single parameter definition object into chain_param_info_t.
 * Returns 0 on success, -1 on error.
 */
/* Helper: bounded strstr - search for needle within [start, end) */
static const char *bounded_strstr(const char *start, const char *end, const char *needle) {
    const char *result = strstr(start, needle);
    return (result && result < end) ? result : NULL;
}

static int parse_param_object(const char *param_json, chain_param_info_t *param) {
    memset(param, 0, sizeof(chain_param_info_t));

    /* Find end of this JSON object (brace-depth tracking) */
    int brace_depth = 0;
    const char *param_obj_end = param_json;
    do {
        if (*param_obj_end == '{') brace_depth++;
        if (*param_obj_end == '}') brace_depth--;
        param_obj_end++;
    } while (brace_depth > 0 && *param_obj_end);

    /* Extract key (required) */
    const char *key_start = bounded_strstr(param_json, param_obj_end, "\"key\"");
    if (!key_start) return -1;
    key_start = strchr(key_start, ':');
    if (!key_start) return -1;
    key_start = strchr(key_start, '"');
    if (!key_start) return -1;
    key_start++;
    const char *key_end = strchr(key_start, '"');
    if (!key_end) return -1;
    int key_len = key_end - key_start;
    if (key_len >= sizeof(param->key)) key_len = sizeof(param->key) - 1;
    memcpy(param->key, key_start, key_len);
    param->key[key_len] = '\0';

    /* Extract label/name (required) */
    const char *label_start = bounded_strstr(param_json, param_obj_end, "\"label\"");
    if (!label_start) label_start = bounded_strstr(param_json, param_obj_end, "\"name\"");
    if (label_start) {
        label_start = strchr(label_start, ':');
        if (label_start) {
            label_start = strchr(label_start, '"');
            if (label_start) {
                label_start++;
                const char *label_end = strchr(label_start, '"');
                if (label_end) {
                    int len = label_end - label_start;
                    if (len >= sizeof(param->name)) len = sizeof(param->name) - 1;
                    memcpy(param->name, label_start, len);
                    param->name[len] = '\0';
                }
            }
        }
    }

    /* Extract type (required) */
    const char *type_start = bounded_strstr(param_json, param_obj_end, "\"type\"");
    if (!type_start) return -1;
    type_start = strchr(type_start, ':');
    if (!type_start) return -1;
    type_start = strchr(type_start, '"');
    if (!type_start) return -1;
    type_start++;

    if (strncmp(type_start, "float", 5) == 0) {
        param->type = KNOB_TYPE_FLOAT;
    } else if (strncmp(type_start, "int", 3) == 0) {
        param->type = KNOB_TYPE_INT;
    } else if (strncmp(type_start, "enum", 4) == 0) {
        param->type = KNOB_TYPE_ENUM;
    } else {
        return -1;
    }

    /* Extract min (optional for enum) */
    const char *min_start = bounded_strstr(param_json, param_obj_end, "\"min\"");
    if (min_start) {
        min_start = strchr(min_start, ':');
        if (min_start) {
            param->min_val = atof(min_start + 1);
        }
    }

    /* Extract max (optional for enum) */
    const char *max_start = bounded_strstr(param_json, param_obj_end, "\"max\"");
    if (max_start) {
        max_start = strchr(max_start, ':');
        if (max_start) {
            param->max_val = atof(max_start + 1);
        }
    }

    /* Extract default (optional) */
    const char *default_start = bounded_strstr(param_json, param_obj_end, "\"default\"");
    if (default_start) {
        default_start = strchr(default_start, ':');
        if (default_start) {
            param->default_val = atof(default_start + 1);
        }
    } else {
        /* Default to min for numeric, 0 for enum */
        param->default_val = (param->type == KNOB_TYPE_ENUM) ? 0 : param->min_val;
    }

    /* Extract step (optional) */
    const char *step_start = bounded_strstr(param_json, param_obj_end, "\"step\"");
    if (step_start) {
        step_start = strchr(step_start, ':');
        if (step_start) {
            param->step = atof(step_start + 1);
        }
    } else {
        /* Default step values */
        if (param->type == KNOB_TYPE_FLOAT) {
            param->step = 0.0015f;
        } else {
            param->step = 1.0f;
        }
    }

    /* Extract unit (optional) */
    const char *unit_start = bounded_strstr(param_json, param_obj_end, "\"unit\"");
    if (unit_start) {
        unit_start = strchr(unit_start, ':');
        if (unit_start) {
            unit_start = strchr(unit_start, '"');
            if (unit_start) {
                unit_start++;
                const char *unit_end = strchr(unit_start, '"');
                if (unit_end) {
                    int len = unit_end - unit_start;
                    if (len >= sizeof(param->unit)) len = sizeof(param->unit) - 1;
                    memcpy(param->unit, unit_start, len);
                    param->unit[len] = '\0';
                }
            }
        }
    }

    /* Extract display_format (optional) */
    const char *format_start = bounded_strstr(param_json, param_obj_end, "\"display_format\"");
    if (format_start) {
        format_start = strchr(format_start, ':');
        if (format_start) {
            format_start = strchr(format_start, '"');
            if (format_start) {
                format_start++;
                const char *format_end = strchr(format_start, '"');
                if (format_end) {
                    int len = format_end - format_start;
                    if (len >= sizeof(param->display_format)) len = sizeof(param->display_format) - 1;
                    memcpy(param->display_format, format_start, len);
                    param->display_format[len] = '\0';
                }
            }
        }
    }

    /* Extract options array (for enums) */
    if (param->type == KNOB_TYPE_ENUM) {
        const char *options_start = bounded_strstr(param_json, param_obj_end, "\"options\"");
        if (options_start) {
            options_start = strchr(options_start, '[');
            if (options_start) {
                options_start++;
                param->option_count = 0;

                /* Parse each option string */
                const char *opt = options_start;
                while (param->option_count < MAX_ENUM_OPTIONS) {
                    opt = strchr(opt, '"');
                    if (!opt || opt > strstr(options_start, "]")) break;
                    opt++;
                    const char *opt_end = strchr(opt, '"');
                    if (!opt_end) break;

                    int len = opt_end - opt;
                    if (len >= 32) len = 31;
                    memcpy(param->options[param->option_count], opt, len);
                    param->options[param->option_count][len] = '\0';
                    param->option_count++;

                    opt = opt_end + 1;
                }
            }
        }

        /* Set max_val for enums to option_count - 1 */
        if (param->option_count > 0) {
            param->max_val = (float)(param->option_count - 1);
        }
    }

    /* Extract max_param (dynamic max reference) */
    const char *max_param_start = bounded_strstr(param_json, param_obj_end, "\"max_param\"");
    if (max_param_start) {
        max_param_start = strchr(max_param_start, ':');
        if (max_param_start) {
            max_param_start = strchr(max_param_start, '"');
            if (max_param_start) {
                max_param_start++;
                const char *max_param_end = strchr(max_param_start, '"');
                if (max_param_end) {
                    int len = max_param_end - max_param_start;
                    if (len >= sizeof(param->max_param)) len = sizeof(param->max_param) - 1;
                    memcpy(param->max_param, max_param_start, len);
                    param->max_param[len] = '\0';
                    param->max_val = -1; /* Marker for dynamic max */
                }
            }
        }
    }

    return 0;
}

/*
 * Parse params array from a single level.
 * Recursively processes nested levels if needed.
 */
static int parse_level_params(const char *level_json, chain_param_info_t *out_params, int *param_count, int max_params) {
    /* Find params array in this level */
    const char *params = strstr(level_json, "\"params\"");
    if (!params) return 0;

    params = strchr(params, '[');
    if (!params) return 0;
    params++;

    /* Iterate through params array */
    const char *param_start = params;
    while (*param_count < max_params) {
        /* Skip whitespace */
        while (*param_start == ' ' || *param_start == '\t' || *param_start == '\n') param_start++;

        /* Check for end of array */
        if (*param_start == ']') break;

        /* Check if this is an object */
        if (*param_start == '{') {
            /* Find end of this object */
            int brace_depth = 0;
            const char *param_end = param_start;
            do {
                if (*param_end == '{') brace_depth++;
                if (*param_end == '}') brace_depth--;
                param_end++;
            } while (brace_depth > 0 && *param_end);

            /* Check if this is a navigation item (has "level" key) or param definition (has "type" key) */
            if (json_object_has_key(param_start, "type")) {
                /* This is a param definition - parse it */
                if (parse_param_object(param_start, &out_params[*param_count]) == 0) {
                    (*param_count)++;
                }
            }
            /* Skip navigation items (they don't define params) */

            param_start = param_end;
        } else if (*param_start == '"') {
            /* String reference - skip (already defined elsewhere) */
            param_start = strchr(param_start + 1, '"');
            if (param_start) param_start++;
        }

        /* Skip comma */
        param_start = strchr(param_start, ',');
        if (!param_start) break;
        param_start++;
    }

    return 0;
}

/*
 * Parse parameters from ui_hierarchy structure.
 * Extracts param definitions from shared_params and all levels.
 */
static int parse_hierarchy_params(const char *json, chain_param_info_t *out_params, int max_params) {
    int param_count = 0;

    /* Find ui_hierarchy section */
    const char *hierarchy = strstr(json, "\"ui_hierarchy\"");
    if (!hierarchy) return 0;

    /* Parse shared_params if present */
    const char *shared = strstr(hierarchy, "\"shared_params\"");
    if (shared) {
        shared = strchr(shared, '[');
        if (shared) {
            shared++;

            /* Iterate through shared_params array */
            const char *param_start = shared;
            while (param_count < max_params) {
                /* Skip whitespace */
                while (*param_start == ' ' || *param_start == '\t' || *param_start == '\n') param_start++;

                /* Check for end of array */
                if (*param_start == ']') break;

                /* Check if this is an object (not a string reference) */
                if (*param_start == '{') {
                    /* Find end of this object */
                    int brace_depth = 0;
                    const char *param_end = param_start;
                    do {
                        if (*param_end == '{') brace_depth++;
                        if (*param_end == '}') brace_depth--;
                        param_end++;
                    } while (brace_depth > 0 && *param_end);

                    /* Parse this param object */
                    if (parse_param_object(param_start, &out_params[param_count]) == 0) {
                        param_count++;
                    }

                    param_start = param_end;
                } else if (*param_start == '"') {
                    /* String reference - skip for now (just advance past it) */
                    param_start = strchr(param_start + 1, '"');
                    if (param_start) param_start++;
                }

                /* Skip comma */
                param_start = strchr(param_start, ',');
                if (!param_start) break;
                param_start++;
            }
        }
    }

    /* Parse params from all levels */
    const char *levels = strstr(hierarchy, "\"levels\"");
    if (levels) {
        const char *levels_open = strchr(levels, '{');
        if (levels_open) {
            /* Find end of levels object using brace-depth tracking */
            int levels_depth = 0;
            const char *levels_end = levels_open;
            do {
                if (*levels_end == '{') levels_depth++;
                if (*levels_end == '}') levels_depth--;
                levels_end++;
            } while (levels_depth > 0 && *levels_end);

            /* Iterate through each level object, bounded by levels_end */
            const char *level_start = levels_open + 1;
            while (param_count < max_params && level_start < levels_end) {
                /* Skip to next level definition */
                level_start = strchr(level_start, '{');
                if (!level_start || level_start >= levels_end) break;

                /* Find end of this level */
                int brace_depth = 0;
                const char *level_end = level_start;
                do {
                    if (*level_end == '{') brace_depth++;
                    if (*level_end == '}') brace_depth--;
                    level_end++;
                } while (brace_depth > 0 && *level_end);

                /* Parse params from this level */
                parse_level_params(level_start, out_params, &param_count, max_params);

                level_start = level_end;
            }
        }
    }

    /* Validate no duplicate keys */
    for (int i = 0; i < param_count; i++) {
        for (int j = i + 1; j < param_count; j++) {
            if (strcmp(out_params[i].key, out_params[j].key) == 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "ERROR: Duplicate parameter key '%s' in ui_hierarchy", out_params[i].key);
                chain_log(msg);
                return -1; /* Signal error */
            }
        }
    }

    return param_count;
}

/*
 * Parse parameter definitions from module.json.
 * First tries ui_hierarchy (new format), falls back to chain_params (legacy).
 */
static int parse_chain_params(const char *module_path, chain_param_info_t *params, int *count) {
    char json_path[MAX_PATH_LEN];
    snprintf(json_path, sizeof(json_path), "%s/module.json", module_path);

    FILE *f = fopen(json_path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 16384) {
        fclose(f);
        return -1;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return -1;
    }

    { size_t nr = fread(json, 1, size, f); json[nr] = '\0'; }
    fclose(f);

    /* Try ui_hierarchy first */
    const char *hierarchy = strstr(json, "\"ui_hierarchy\"");
    if (hierarchy) {
        *count = parse_hierarchy_params(json, params, MAX_CHAIN_PARAMS);
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg), "Parsed ui_hierarchy params: count=%d", *count);
        chain_log(log_msg);
        for (int i = 0; i < *count && i < 10; i++) {
            snprintf(log_msg, sizeof(log_msg), "  Param[%d]: key=%s, name=%s, type=%d",
                     i, params[i].key, params[i].name, params[i].type);
            chain_log(log_msg);
        }
        if (*count > 0) {
            free(json);
            return 0;
        }
        /* count == 0: hierarchy had no inline params (string refs only).
         * Fall through to chain_params for metadata. */
        chain_log("No inline params in ui_hierarchy, falling through to chain_params");
    }

    /* Fall back to legacy chain_params */
    *count = 0;

    /* Find chain_params array */
    const char *chain_params_str = strstr(json, "\"chain_params\"");
    if (!chain_params_str) {
        free(json);
        return 0;  /* No params is OK */
    }

    const char *arr_start = strchr(chain_params_str, '[');
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

    /* Parse each parameter object (legacy chain_params format) */
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

        /* Parse key - look for "key": to find field, not value */
        const char *key_pos = strstr(obj_start, "\"key\":");
        if (key_pos && key_pos < obj_end) {
            const char *q1 = strchr(key_pos + 6, '"');
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

        /* Parse name - look for "name": to find field */
        const char *name_pos = strstr(obj_start, "\"name\":");
        if (name_pos && name_pos < obj_end) {
            const char *q1 = strchr(name_pos + 7, '"');
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

        /* Parse type - look for "type": to find field, not value */
        const char *type_pos = strstr(obj_start, "\"type\":");
        if (type_pos && type_pos < obj_end) {
            const char *q1 = strchr(type_pos + 7, '"');
            if (q1 && q1 < obj_end) {
                q1++;
                if (strncmp(q1, "int", 3) == 0) {
                    p->type = KNOB_TYPE_INT;
                    p->max_val = 9999.0f;  /* Default for int */
                } else if (strncmp(q1, "enum", 4) == 0) {
                    p->type = KNOB_TYPE_ENUM;
                }
            }
        }

        /* Parse options (for enum type) */
        const char *options_pos = strstr(obj_start, "\"options\":");
        if (options_pos && options_pos < obj_end) {
            const char *arr_start2 = strchr(options_pos, '[');
            if (arr_start2 && arr_start2 < obj_end) {
                const char *arr_end2 = strchr(arr_start2, ']');
                if (arr_end2 && arr_end2 < obj_end) {
                    const char *opt_pos = arr_start2 + 1;
                    while (opt_pos < arr_end2 && p->option_count < MAX_ENUM_OPTIONS) {
                        const char *oq1 = strchr(opt_pos, '"');
                        if (!oq1 || oq1 >= arr_end2) break;
                        oq1++;
                        const char *oq2 = strchr(oq1, '"');
                        if (!oq2 || oq2 >= arr_end2) break;
                        int olen = (int)(oq2 - oq1);
                        if (olen > 31) olen = 31;
                        strncpy(p->options[p->option_count], oq1, olen);
                        p->options[p->option_count][olen] = '\0';
                        p->option_count++;
                        opt_pos = oq2 + 1;
                    }
                }
            }
        }

        /* Parse min */
        const char *min_pos = strstr(obj_start, "\"min\":");
        if (min_pos && min_pos < obj_end) {
            const char *colon = strchr(min_pos, ':');
            if (colon && colon < obj_end) {
                p->min_val = (float)atof(colon + 1);
            }
        }

        /* Parse max - look for "max": but not "max_param": */
        const char *max_pos = strstr(obj_start, "\"max\":");
        if (max_pos && max_pos < obj_end) {
            const char *colon = strchr(max_pos, ':');
            if (colon && colon < obj_end) {
                p->max_val = (float)atof(colon + 1);
            }
        }

        /* Parse max_param (dynamic max) */
        const char *max_param_pos = strstr(obj_start, "\"max_param\":");
        if (max_param_pos && max_param_pos < obj_end) {
            const char *q1 = strchr(max_param_pos + 12, '"');
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
        const char *def_pos = strstr(obj_start, "\"default\":");
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

    if (size <= 0 || size > 4096) {
        fclose(f);
        chain_log("Patch file too large or empty");
        return -1;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return -1;
    }

    { size_t nr = fread(json, 1, size, f); json[nr] = '\0'; }
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
    /* Try current format first: "midi_source_module": "module_id" */
    if (json_get_string(json, "midi_source_module", patch->midi_source_module, MAX_NAME_LEN) != 0) {
        /* Fall back to legacy formats for backward compat */
        if (json_get_string_in_section(json, "midi_source", "module",
                                       patch->midi_source_module, MAX_NAME_LEN) != 0) {
            json_get_string(json, "midi_source", patch->midi_source_module, MAX_NAME_LEN);
        }
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
                    strncpy(patch->audio_fx[patch->audio_fx_count].module, quote1, len);
                    patch->audio_fx[patch->audio_fx_count].module[len] = '\0';
                    patch->audio_fx[patch->audio_fx_count].param_count = 0;
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

                /* Type/min/max now come from chain_params - not parsed from JSON */

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
                    patch->knob_mappings[patch->knob_mapping_count].current_value = saved_value;
                    patch->knob_mapping_count++;
                }

                bracket = obj_end + 1;
            }
        }
    }

    free(json);

    snprintf(msg, sizeof(msg),
             "Parsed patch: %s -> %s preset %d, source=%s, %d FX",
             patch->name, patch->synth_module, patch->synth_preset,
             patch->midi_source_module[0] ? patch->midi_source_module : "none",
             patch->audio_fx_count);
    chain_log(msg);

    return 0;
}

/* Generate a patch name from components */
static void generate_patch_name(char *out, int out_len,
                                const char *synth, int preset,
                                const char *fx1, const char *fx2) {
    char preset_name[MAX_NAME_LEN] = "";

    /* Try to get preset name from synth */
    if (synth_loaded()) {
        synth_get_param("preset_name", preset_name, sizeof(preset_name));
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
    snprintf(patches_dir, sizeof(patches_dir), "%s/../../patches", g_module_dir);

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

    /* Escape name for JSON embedding (handle quotes and backslashes) */
    char escaped_name[MAX_NAME_LEN * 2];
    {
        int si = 0, di = 0;
        while (name[si] && di < (int)sizeof(escaped_name) - 2) {
            if (name[si] == '"' || name[si] == '\\') {
                escaped_name[di++] = '\\';
            }
            escaped_name[di++] = name[si++];
        }
        escaped_name[di] = '\0';
    }

    /* Build final JSON with generated name */
    char final_json[8192];
    snprintf(final_json, sizeof(final_json),
        "{\n"
        "    \"name\": \"%s\",\n"
        "    \"version\": 1,\n"
        "    \"chain\": %s\n"
        "}\n",
        escaped_name, json_data);

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

    /* Update current patch to the newly saved one */
    for (int i = 0; i < g_patch_count; i++) {
        if (strcmp(g_patches[i].name, name) == 0) {
            g_current_patch = i;
            break;
        }
    }

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
    char final_json[8192];
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

    /* Update current patch index (may have shifted due to alphabetical sort) */
    for (int i = 0; i < g_patch_count; i++) {
        if (strcmp(g_patches[i].name, name) == 0) {
            g_current_patch = i;
            break;
        }
    }

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
/* Compare function for sorting patches alphabetically by name */
static int compare_patches(const void *a, const void *b) {
    const patch_info_t *pa = (const patch_info_t *)a;
    const patch_info_t *pb = (const patch_info_t *)b;
    return strcasecmp(pa->name, pb->name);
}

static int scan_patches(const char *module_dir) {
    char patches_dir[MAX_PATH_LEN];
    char msg[256];

    snprintf(patches_dir, sizeof(patches_dir), "%s/../../patches", module_dir);

    snprintf(msg, sizeof(msg), "Scanning patches in: %s", patches_dir);
    chain_log(msg);

    DIR *dir = opendir(patches_dir);
    if (!dir) {
        chain_log("No patches directory found");
        return 0;
    }

    g_patch_count = 0;
    /* Clear old data to prevent stale/garbage entries */
    memset(g_patches, 0, sizeof(g_patches));
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

    /* Sort patches alphabetically by name */
    if (g_patch_count > 1) {
        qsort(g_patches, g_patch_count, sizeof(patch_info_t), compare_patches);
    }

    snprintf(msg, sizeof(msg), "Found %d patches", g_patch_count);
    chain_log(msg);

    return g_patch_count;
}

/* Send all-notes-off to synth to prevent stuck notes */
static void synth_panic(void) {
    if (!synth_loaded()) return;

    /* Send All Sound Off (CC 120) and All Notes Off (CC 123) on all channels */
    for (int ch = 0; ch < 16; ch++) {
        uint8_t all_sound_off[] = { (uint8_t)(0xB0 | ch), 120, 0 };
        uint8_t all_notes_off[] = { (uint8_t)(0xB0 | ch), 123, 0 };
        synth_on_midi(all_sound_off, 3, 0);
        synth_on_midi(all_notes_off, 3, 0);
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
    g_js_midi_fx_enabled = 0;
    g_midi_input = MIDI_INPUT_ANY;
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

        /* Build path to new synth module - all sound generators in modules/sound_generators/ */
        char synth_path[MAX_PATH_LEN];
        snprintf(synth_path, sizeof(synth_path), "%s/../sound_generators/%s",
                 g_module_dir, patch->synth_module);

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
    if (synth_loaded()) {
        char preset_str[16];
        snprintf(preset_str, sizeof(preset_str), "%d", patch->synth_preset);
        synth_set_param("preset", preset_str);
    }

    /* Unload old audio FX and load new ones */
    unload_all_audio_fx();
    for (int i = 0; i < patch->audio_fx_count; i++) {
        audio_fx_config_t *cfg = &patch->audio_fx[i];
        if (load_audio_fx(cfg->module) != 0) {
            snprintf(msg, sizeof(msg), "Warning: Failed to load FX: %s", cfg->module);
            chain_log(msg);
            continue;
        }

        /* Apply audio FX params (e.g., plugin_id for CLAP) */
        int fx_idx = g_fx_count - 1;  /* Just loaded */
        if (fx_idx >= 0 && fx_idx < MAX_AUDIO_FX && cfg->param_count > 0) {
            snprintf(msg, sizeof(msg), "Applying %d params to FX%d", cfg->param_count, fx_idx + 1);
            chain_log(msg);
            for (int p = 0; p < cfg->param_count; p++) {
                snprintf(msg, sizeof(msg), "  FX%d param: %s = %s",
                         fx_idx + 1, cfg->params[p].key, cfg->params[p].val);
                chain_log(msg);
                if (g_fx_is_v2[fx_idx] && g_fx_plugins_v2[fx_idx] && g_fx_instances[fx_idx] &&
                    g_fx_plugins_v2[fx_idx]->set_param) {
                    g_fx_plugins_v2[fx_idx]->set_param(g_fx_instances[fx_idx],
                                                       cfg->params[p].key, cfg->params[p].val);
                } else if (g_fx_plugins[fx_idx] && g_fx_plugins[fx_idx]->set_param) {
                    g_fx_plugins[fx_idx]->set_param(cfg->params[p].key, cfg->params[p].val);
                }
            }
        }
    }

    g_current_patch = index;

    /* Enable JS MIDI FX if patch declares any */
    g_js_midi_fx_enabled = (patch->midi_fx_js_count > 0);

    /* Set MIDI input filter */
    g_midi_input = patch->midi_input;
    g_source_ui_active = 0;

    /* Copy knob mappings and initialize current values */
    g_knob_mapping_count = patch->knob_mapping_count;
    for (int i = 0; i < patch->knob_mapping_count && i < MAX_KNOB_MAPPINGS; i++) {
        g_knob_mappings[i] = patch->knob_mappings[i];

        /* Look up param info to initialize current value */
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

        /* Use saved value if present, otherwise initialize to middle of range */
        float saved = patch->knob_mappings[i].current_value;
        if (saved > -999998.0f && pinfo) {
            /* Has saved value - use it (clamp to valid range) */
            if (saved < pinfo->min_val) saved = pinfo->min_val;
            if (saved > pinfo->max_val) saved = pinfo->max_val;
            if (pinfo->type == KNOB_TYPE_INT) {
                g_knob_mappings[i].current_value = (float)((int)saved);
            } else {
                g_knob_mappings[i].current_value = saved;
            }
        } else if (pinfo) {
            /* No saved value - initialize to middle of min/max range */
            float mid = (pinfo->min_val + pinfo->max_val) / 2.0f;
            if (pinfo->type == KNOB_TYPE_INT) {
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

        /* Look up param info for type */
        chain_param_info_t *pinfo = NULL;
        if (strcmp(target, "synth") == 0) {
            pinfo = find_param_info(g_synth_params, g_synth_param_count, param);
        } else if (strcmp(target, "fx1") == 0 && g_fx_count > 0) {
            pinfo = find_param_info(g_fx_params[0], g_fx_param_counts[0], param);
        } else if (strcmp(target, "fx2") == 0 && g_fx_count > 1) {
            pinfo = find_param_info(g_fx_params[1], g_fx_param_counts[1], param);
        }

        /* Format value string */
        char val_str[32];
        if (pinfo && pinfo->type == KNOB_TYPE_INT) {
            snprintf(val_str, sizeof(val_str), "%d", (int)value);
        } else {
            snprintf(val_str, sizeof(val_str), "%.3f", value);
        }

        /* Send to appropriate target */
        if (strcmp(target, "synth") == 0) {
            if (synth_loaded()) {
                synth_set_param(param, val_str);
            }
        } else if (strcmp(target, "fx1") == 0) {
            if (g_fx_count > 0) {
                if (g_fx_is_v2[0] && g_fx_plugins_v2[0] && g_fx_instances[0] && g_fx_plugins_v2[0]->set_param) {
                    g_fx_plugins_v2[0]->set_param(g_fx_instances[0], param, val_str);
                } else if (g_fx_plugins[0] && g_fx_plugins[0]->set_param) {
                    g_fx_plugins[0]->set_param(param, val_str);
                }
            }
        } else if (strcmp(target, "fx2") == 0) {
            if (g_fx_count > 1) {
                if (g_fx_is_v2[1] && g_fx_plugins_v2[1] && g_fx_instances[1] && g_fx_plugins_v2[1]->set_param) {
                    g_fx_plugins_v2[1]->set_param(g_fx_instances[1], param, val_str);
                } else if (g_fx_plugins[1] && g_fx_plugins[1]->set_param) {
                    g_fx_plugins[1]->set_param(param, val_str);
                }
            }
        }
    }

    /* Mute briefly to drain any old synth audio before FX process it */
    g_mute_countdown = MUTE_BLOCKS_AFTER_SWITCH;

    snprintf(msg, sizeof(msg), "Loaded patch %d: %s (%d FX)",
             index, patch->name, g_fx_count);
    chain_log(msg);

    /* Update record button LED (white when patch loaded) */
    update_record_led();

    /* Reset mod wheel (CC 1) to 0 on all channels after patch load.
     * This prevents stale mod wheel values from Move's track state
     * causing unwanted vibrato on startup. */
    if (synth_loaded()) {
        for (int ch = 0; ch < 16; ch++) {
            uint8_t mod_reset[3] = {(uint8_t)(0xB0 | ch), 1, 0};  /* CC 1 = mod wheel */
            synth_on_midi(mod_reset, 3, MOVE_MIDI_SOURCE_HOST);
        }
    }

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
    if (!synth_loaded()) return;

    if (interval == 0) {
        /* No transposition, send as-is */
        synth_on_midi(msg, len, source);
    } else {
        /* Transpose the note */
        int transposed_note = (int)msg[1] + interval;
        if (transposed_note < 0 || transposed_note > 127) return;

        uint8_t transposed[3];
        transposed[0] = msg[0];
        transposed[1] = (uint8_t)transposed_note;
        transposed[2] = msg[2];  /* Velocity */
        synth_on_midi(transposed, len, source);
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

    if (!synth_loaded()) return;

    /* Handle knob CC mappings (CC 71-78) - relative encoders */
    /* Skip if in component UI mode (UI handles knobs directly) */
    if (len >= 3 && (msg[0] & 0xF0) == 0xB0 && g_component_ui_mode == 0) {
        uint8_t cc = msg[1];
        if (cc >= KNOB_CC_START && cc <= KNOB_CC_END) {
            for (int i = 0; i < g_knob_mapping_count; i++) {
                if (g_knob_mappings[i].cc == cc) {
                    /* Look up parameter metadata dynamically */
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

                    if (!pinfo) continue;  /* Skip if param not found */

                    /* Calculate acceleration multiplier based on turn speed */
                    int accel = calc_knob_accel(i);
                    int is_int = (pinfo->type == KNOB_TYPE_INT || pinfo->type == KNOB_TYPE_ENUM);

                    /* Cap acceleration for ints/enums to avoid jumping too far */
                    if (is_int && accel > KNOB_ACCEL_MAX_MULT_INT) {
                        accel = KNOB_ACCEL_MAX_MULT_INT;
                    }

                    /* Relative encoder: 1 = increment, 127 = decrement */
                    /* Use step from param metadata (defaults: 0.0015 float, 1 int) */
                    float base_step = (pinfo->step > 0) ? pinfo->step
                        : (is_int ? (float)KNOB_STEP_INT : KNOB_STEP_FLOAT);
                    float delta = 0.0f;
                    if (msg[2] == 1) {
                        delta = base_step * accel;
                    } else if (msg[2] == 127) {
                        delta = -base_step * accel;
                    } else {
                        return;  /* Ignore other values */
                    }

                    /* Update current value with min/max clamping */
                    float new_val = g_knob_mappings[i].current_value + delta;
                    if (new_val < pinfo->min_val) new_val = pinfo->min_val;
                    if (new_val > pinfo->max_val) new_val = pinfo->max_val;
                    if (is_int) new_val = (float)((int)new_val);  /* Round to int */
                    g_knob_mappings[i].current_value = new_val;

                    /* Convert to string for set_param - int or float format */
                    char val_str[16];
                    if (is_int) {
                        snprintf(val_str, sizeof(val_str), "%d", (int)new_val);
                    } else {
                        snprintf(val_str, sizeof(val_str), "%.3f", new_val);
                    }

                    if (strcmp(target, "synth") == 0) {
                        /* Route to synth */
                        if (synth_loaded()) {
                            synth_set_param(param, val_str);
                        }
                    } else if (strcmp(target, "fx1") == 0) {
                        /* Route to first audio FX */
                        if (g_fx_count > 0) {
                            if (g_fx_is_v2[0] && g_fx_plugins_v2[0] && g_fx_instances[0] && g_fx_plugins_v2[0]->set_param) {
                                g_fx_plugins_v2[0]->set_param(g_fx_instances[0], param, val_str);
                            } else if (g_fx_plugins[0] && g_fx_plugins[0]->set_param) {
                                g_fx_plugins[0]->set_param(param, val_str);
                            }
                        }
                    } else if (strcmp(target, "fx2") == 0) {
                        /* Route to second audio FX */
                        if (g_fx_count > 1) {
                            if (g_fx_is_v2[1] && g_fx_plugins_v2[1] && g_fx_instances[1] && g_fx_plugins_v2[1]->set_param) {
                                g_fx_plugins_v2[1]->set_param(g_fx_instances[1], param, val_str);
                            } else if (g_fx_plugins[1] && g_fx_plugins[1]->set_param) {
                                g_fx_plugins[1]->set_param(param, val_str);
                            }
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

    /* Forward to synth (V1 API - no MIDI FX processing, use V2 for that) */
    synth_on_midi(msg, len, source);
}

static void plugin_set_param(const char *key, const char *val) {
    {
        char dbg[256];
        snprintf(dbg, sizeof(dbg), "[v1_set_param] key='%s' val='%s'", key, val ? val : "null");
        parse_debug_log(dbg);
    }

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
        if (synth_loaded() && subkey[0] != '\0') {
            synth_set_param(subkey, val);
        }
        return;
    }

    /* Route to FX1 with fx1: prefix */
    if (strncmp(key, "fx1:", 4) == 0) {
        const char *subkey = key + 4;
        if (g_fx_count > 0 && subkey[0] != '\0') {
            if (g_fx_is_v2[0] && g_fx_plugins_v2[0] && g_fx_instances[0] && g_fx_plugins_v2[0]->set_param) {
                g_fx_plugins_v2[0]->set_param(g_fx_instances[0], subkey, val);
            } else if (g_fx_plugins[0] && g_fx_plugins[0]->set_param) {
                g_fx_plugins[0]->set_param(subkey, val);
            }
        }
        return;
    }

    /* Route to FX2 with fx2: prefix */
    if (strncmp(key, "fx2:", 4) == 0) {
        const char *subkey = key + 4;
        if (g_fx_count > 1 && subkey[0] != '\0') {
            if (g_fx_is_v2[1] && g_fx_plugins_v2[1] && g_fx_instances[1] && g_fx_plugins_v2[1]->set_param) {
                g_fx_plugins_v2[1]->set_param(g_fx_instances[1], subkey, val);
            } else if (g_fx_plugins[1] && g_fx_plugins[1]->set_param) {
                g_fx_plugins[1]->set_param(subkey, val);
            }
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
        if (g_patch_count > 0) {
            int next = (g_current_patch + 1) % g_patch_count;
            load_patch(next);
        }
        return;
    }
    if (strcmp(key, "prev_patch") == 0) {
        if (g_patch_count > 0) {
            int prev = (g_current_patch - 1 + g_patch_count) % g_patch_count;
            load_patch(prev);
        }
        return;
    }

    /* Forward to synth (includes octave_transpose) */
    if (synth_loaded()) {
        synth_set_param(key, val);
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
        if (synth_loaded() && subkey[0] != '\0') {
            return synth_get_param(subkey, buf, buf_len);
        }
        return -1;
    }

    /* Route to FX1 with fx1: prefix */
    if (strncmp(key, "fx1:", 4) == 0) {
        const char *subkey = key + 4;
        if (g_fx_count > 0 && subkey[0] != '\0') {
            if (g_fx_is_v2[0] && g_fx_plugins_v2[0] && g_fx_instances[0] && g_fx_plugins_v2[0]->get_param) {
                return g_fx_plugins_v2[0]->get_param(g_fx_instances[0], subkey, buf, buf_len);
            } else if (g_fx_plugins[0] && g_fx_plugins[0]->get_param) {
                return g_fx_plugins[0]->get_param(subkey, buf, buf_len);
            }
        }
        return -1;
    }

    /* Route to FX2 with fx2: prefix */
    if (strncmp(key, "fx2:", 4) == 0) {
        const char *subkey = key + 4;
        if (g_fx_count > 1 && subkey[0] != '\0') {
            if (g_fx_is_v2[1] && g_fx_plugins_v2[1] && g_fx_instances[1] && g_fx_plugins_v2[1]->get_param) {
                return g_fx_plugins_v2[1]->get_param(g_fx_instances[1], subkey, buf, buf_len);
            } else if (g_fx_plugins[1] && g_fx_plugins[1]->get_param) {
                return g_fx_plugins[1]->get_param(subkey, buf, buf_len);
            }
        }
        return -1;
    }

    /* Component UI mode */
    if (strcmp(key, "component_ui_mode") == 0) {
        const char *modes[] = {"none", "synth", "fx1", "fx2"};
        int mode_idx = (g_component_ui_mode >= 0 && g_component_ui_mode < 4) ? g_component_ui_mode : 0;
        snprintf(buf, buf_len, "%s", modes[mode_idx]);
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

            /* Build audio_fx JSON array */
            char fx_json[512] = "[";
            for (int i = 0; i < p->audio_fx_count && i < MAX_AUDIO_FX; i++) {
                if (i > 0) strcat(fx_json, ",");
                char fx_item[64];
                snprintf(fx_item, sizeof(fx_item), "\"%s\"", p->audio_fx[i].module);
                strcat(fx_json, fx_item);
            }
            strcat(fx_json, "]");

            /* Build knob_mappings JSON array - look up type/min/max from param info */
            char knob_json[2048] = "[";
            int knob_off = 1;
            for (int i = 0; i < p->knob_mapping_count && i < MAX_KNOB_MAPPINGS; i++) {
                /* Look up param metadata from V1 globals */
                const char *km_target = p->knob_mappings[i].target;
                const char *km_param = p->knob_mappings[i].param;
                chain_param_info_t *ki = NULL;
                if (strcmp(km_target, "synth") == 0) {
                    ki = find_param_info(g_synth_params, g_synth_param_count, km_param);
                } else if (strcmp(km_target, "fx1") == 0) {
                    ki = find_param_info(g_fx_params[0], g_fx_param_counts[0], km_param);
                } else if (strcmp(km_target, "fx2") == 0) {
                    ki = find_param_info(g_fx_params[1], g_fx_param_counts[1], km_param);
                } else if (strcmp(km_target, "fx3") == 0) {
                    ki = find_param_info(g_fx_params[2], g_fx_param_counts[2], km_param);
                }
                const char *type_str = (ki && (ki->type == KNOB_TYPE_INT || ki->type == KNOB_TYPE_ENUM)) ? "int" : "float";
                float min_v = ki ? ki->min_val : 0.0f;
                float max_v = ki ? ki->max_val : 1.0f;
                int written = snprintf(knob_json + knob_off, sizeof(knob_json) - knob_off,
                    "%s{\"cc\":%d,\"target\":\"%s\",\"param\":\"%s\",\"type\":\"%s\",\"min\":%.3f,\"max\":%.3f}",
                    (i > 0) ? "," : "",
                    p->knob_mappings[i].cc,
                    p->knob_mappings[i].target,
                    p->knob_mappings[i].param,
                    type_str,
                    min_v, max_v);
                if (written > 0 && knob_off + written < (int)sizeof(knob_json) - 1) {
                    knob_off += written;
                } else {
                    break;
                }
            }
            knob_json[knob_off++] = ']';
            knob_json[knob_off] = '\0';

            snprintf(buf, buf_len,
                "{\"synth\":\"%s\",\"preset\":%d,\"source\":\"%s\","
                "\"input\":\"%s\",\"audio_fx\":%s,"
                "\"knob_mappings\":%s}",
                p->synth_module, p->synth_preset,
                p->midi_source_module[0] ? p->midi_source_module : "",
                input_str, fx_json, knob_json);
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
    if (strcmp(key, "synth_error") == 0) {
        return synth_get_error(buf, buf_len);
    }
    if (strcmp(key, "midi_source_module") == 0) {
        snprintf(buf, buf_len, "%s", g_current_source_module);
        return 0;
    }
    if (strcmp(key, "fx1_module") == 0) {
        if (g_current_patch >= 0 && g_current_patch < g_patch_count &&
            g_patches[g_current_patch].audio_fx_count > 0) {
            snprintf(buf, buf_len, "%s", g_patches[g_current_patch].audio_fx[0].module);
        } else {
            buf[0] = '\0';
        }
        return 0;
    }
    if (strcmp(key, "fx2_module") == 0) {
        if (g_current_patch >= 0 && g_current_patch < g_patch_count &&
            g_patches[g_current_patch].audio_fx_count > 1) {
            snprintf(buf, buf_len, "%s", g_patches[g_current_patch].audio_fx[1].module);
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
        if (synth_loaded()) {
            char preset_buf[32];
            if (synth_get_param("preset", preset_buf, sizeof(preset_buf)) >= 0) {
                current_preset = atoi(preset_buf);
            }
        }

        /* Build input string */
        const char *input_str = "both";
        if (g_midi_input == MIDI_INPUT_PADS) input_str = "pads";
        else if (g_midi_input == MIDI_INPUT_EXTERNAL) input_str = "external";

        /* Build audio_fx JSON array */
        char fx_json[512] = "[";
        for (int i = 0; i < p->audio_fx_count && i < MAX_AUDIO_FX; i++) {
            if (i > 0) strcat(fx_json, ",");
            char fx_item[64];
            snprintf(fx_item, sizeof(fx_item), "{\"type\":\"%s\"}", p->audio_fx[i].module);
            strcat(fx_json, fx_item);
        }
        strcat(fx_json, "]");

        /* Build knob_mappings JSON array with CURRENT values (type/min/max from chain_params) */
        char knob_json[2048] = "[";
        for (int i = 0; i < g_knob_mapping_count && i < MAX_KNOB_MAPPINGS; i++) {
            if (i > 0) strcat(knob_json, ",");
            char knob_item[256];
            snprintf(knob_item, sizeof(knob_item),
                "{\"cc\":%d,\"target\":\"%s\",\"param\":\"%s\",\"value\":%.3f}",
                g_knob_mappings[i].cc,
                g_knob_mappings[i].target,
                g_knob_mappings[i].param,
                g_knob_mappings[i].current_value);
            strcat(knob_json, knob_item);
        }
        strcat(knob_json, "]");

        /* Build final JSON */
        snprintf(buf, buf_len,
            "{\"synth\":{\"module\":\"%s\",\"preset\":%d},"
            "\"source\":\"%s\",\"input\":\"%s\",\"audio_fx\":%s,"
            "\"knob_mappings\":%s}",
            g_current_synth_module, current_preset,
            g_current_source_module,
            input_str, fx_json, knob_json);
        return 0;
    }

    /* Forward to synth */
    if (synth_loaded()) {
        return synth_get_param(key, buf, buf_len);
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

    if (synth_loaded()) {
        /* Get audio from synth */
        synth_render_block(out_interleaved_lr, frames);

        /* Process through audio FX chain */
        for (int i = 0; i < g_fx_count; i++) {
            if (g_fx_is_v2[i]) {
                if (g_fx_plugins_v2[i] && g_fx_instances[i] && g_fx_plugins_v2[i]->process_block) {
                    g_fx_plugins_v2[i]->process_block(g_fx_instances[i], out_interleaved_lr, frames);
                }
            } else {
                if (g_fx_plugins[i] && g_fx_plugins[i]->process_block) {
                    g_fx_plugins[i]->process_block(out_interleaved_lr, frames);
                }
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

/* ============================================================================
 * V2 Instance-Based API Implementation
 * ============================================================================ */

/* Global instance used by v1 API (for backwards compatibility) */
static chain_instance_t *g_v1_instance = NULL;

static void v2_chain_log(chain_instance_t *inst, const char *msg) {
    if (inst && inst->host && inst->host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[chain-v2] %s", msg);
        inst->host->log(buf);
    }
}

/* Forward declarations for v2 helper functions */
static void v2_synth_panic(chain_instance_t *inst);
static void v2_unload_synth(chain_instance_t *inst);
static void v2_unload_all_audio_fx(chain_instance_t *inst);
static void v2_unload_midi_source(chain_instance_t *inst);
static int v2_load_synth(chain_instance_t *inst, const char *module_name);
static int v2_load_audio_fx(chain_instance_t *inst, const char *fx_name);
static int v2_parse_patch_file(chain_instance_t *inst, const char *path, patch_info_t *patch);
static int v2_scan_patches(chain_instance_t *inst);
static int v2_load_patch(chain_instance_t *inst, int patch_idx);

/* Create a new chain instance */
static void* v2_create_instance(const char *module_dir, const char *config_json) {
    (void)config_json;

    chain_instance_t *inst = calloc(1, sizeof(chain_instance_t));
    if (!inst) return NULL;

    strncpy(inst->module_dir, module_dir, MAX_PATH_LEN - 1);

    /* Initialize mutex and condition for recording thread */
    pthread_mutex_init(&inst->ring_mutex, NULL);
    pthread_cond_init(&inst->ring_cond, NULL);

    /* Set up host API for sub-plugins */
    if (g_host) {
        inst->host = g_host;
        memcpy(&inst->subplugin_host_api, g_host, sizeof(host_api_v1_t));
        memcpy(&inst->source_host_api, g_host, sizeof(host_api_v1_t));
        /* Note: MIDI source routing would need instance-specific handling for full v2 */
    }

    /* Scan patches */
    v2_scan_patches(inst);

    char msg[256];
    snprintf(msg, sizeof(msg), "Instance created, found %d patches", inst->patch_count);
    v2_chain_log(inst, msg);

    return inst;
}

/* Destroy a chain instance */
static void v2_destroy_instance(void *instance) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst) return;

    v2_chain_log(inst, "Destroying instance");

    /* Stop recording if active */
    if (inst->writer_running) {
        pthread_mutex_lock(&inst->ring_mutex);
        inst->writer_should_exit = 1;
        pthread_cond_signal(&inst->ring_cond);
        pthread_mutex_unlock(&inst->ring_mutex);
        pthread_join(inst->writer_thread, NULL);
        inst->writer_running = 0;
    }

    if (inst->wav_file) {
        fclose(inst->wav_file);
        inst->wav_file = NULL;
    }

    if (inst->ring_buffer) {
        free(inst->ring_buffer);
        inst->ring_buffer = NULL;
    }

    /* Unload all plugins */
    v2_synth_panic(inst);
    v2_unload_all_audio_fx(inst);
    v2_unload_synth(inst);
    v2_unload_midi_source(inst);

    /* Cleanup mutex/cond */
    pthread_mutex_destroy(&inst->ring_mutex);
    pthread_cond_destroy(&inst->ring_cond);

    free(inst);
}

/* V2 synth panic - send all notes off */
static void v2_synth_panic(chain_instance_t *inst) {
    if (!inst) return;

    for (int ch = 0; ch < 16; ch++) {
        uint8_t msg[3] = {(uint8_t)(0xB0 | ch), 123, 0};  /* All notes off */

        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->on_midi) {
            inst->synth_plugin_v2->on_midi(inst->synth_instance, msg, 3, MOVE_MIDI_SOURCE_HOST);
        } else if (inst->synth_plugin && inst->synth_plugin->on_midi) {
            inst->synth_plugin->on_midi(msg, 3, MOVE_MIDI_SOURCE_HOST);
        }
    }
}

/* V2 get synth error */
static int v2_synth_get_error(chain_instance_t *inst, char *buf, int buf_len) {
    if (!inst) return 0;

    if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->get_error) {
        return inst->synth_plugin_v2->get_error(inst->synth_instance, buf, buf_len);
    }
    /* V1 plugins don't have get_error, try via get_param as fallback */
    if (inst->synth_plugin && inst->synth_plugin->get_param) {
        return inst->synth_plugin->get_param("load_error", buf, buf_len);
    }
    return 0;  /* No error */
}

/* V2 unload synth */
static void v2_unload_synth(chain_instance_t *inst) {
    if (!inst) return;

    if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->destroy_instance) {
        inst->synth_plugin_v2->destroy_instance(inst->synth_instance);
    } else if (inst->synth_plugin && inst->synth_plugin->on_unload) {
        inst->synth_plugin->on_unload();
    }

    if (inst->synth_handle) {
        dlclose(inst->synth_handle);
    }

    inst->synth_handle = NULL;
    inst->synth_plugin = NULL;
    inst->synth_plugin_v2 = NULL;
    inst->synth_instance = NULL;
    inst->current_synth_module[0] = '\0';
    inst->synth_param_count = 0;
    inst->synth_default_forward_channel = -1;
}

/* V2 unload all audio FX */
static void v2_unload_all_audio_fx(chain_instance_t *inst) {
    if (!inst) return;

    for (int i = 0; i < inst->fx_count; i++) {
        if (inst->fx_is_v2[i]) {
            if (inst->fx_plugins_v2[i] && inst->fx_instances[i] && inst->fx_plugins_v2[i]->destroy_instance) {
                inst->fx_plugins_v2[i]->destroy_instance(inst->fx_instances[i]);
            }
        } else {
            if (inst->fx_plugins[i] && inst->fx_plugins[i]->on_unload) {
                inst->fx_plugins[i]->on_unload();
            }
        }

        if (inst->fx_handles[i]) {
            dlclose(inst->fx_handles[i]);
        }

        inst->fx_handles[i] = NULL;
        inst->fx_plugins[i] = NULL;
        inst->fx_plugins_v2[i] = NULL;
        inst->fx_instances[i] = NULL;
        inst->fx_is_v2[i] = 0;
        inst->fx_on_midi[i] = NULL;
        inst->fx_param_counts[i] = 0;
        inst->current_fx_modules[i][0] = '\0';
    }
    inst->fx_count = 0;
}

/* V2 unload a single audio FX slot */
static void v2_unload_audio_fx_slot(chain_instance_t *inst, int slot) {
    if (!inst || slot < 0 || slot >= MAX_AUDIO_FX) return;

    if (inst->fx_is_v2[slot]) {
        if (inst->fx_plugins_v2[slot] && inst->fx_instances[slot] && inst->fx_plugins_v2[slot]->destroy_instance) {
            inst->fx_plugins_v2[slot]->destroy_instance(inst->fx_instances[slot]);
        }
    } else {
        if (inst->fx_plugins[slot] && inst->fx_plugins[slot]->on_unload) {
            inst->fx_plugins[slot]->on_unload();
        }
    }

    if (inst->fx_handles[slot]) {
        dlclose(inst->fx_handles[slot]);
    }

    inst->fx_handles[slot] = NULL;
    inst->fx_plugins[slot] = NULL;
    inst->fx_plugins_v2[slot] = NULL;
    inst->fx_instances[slot] = NULL;
    inst->fx_is_v2[slot] = 0;
    inst->fx_on_midi[slot] = NULL;
    inst->fx_param_counts[slot] = 0;
    inst->current_fx_modules[slot][0] = '\0';
}

/* V2 load audio FX into a specific slot */
static int v2_load_audio_fx_slot(chain_instance_t *inst, int slot, const char *fx_name) {
    char msg[256];
    char fx_path[MAX_PATH_LEN];
    char fx_dir[MAX_PATH_LEN];

    if (!inst || slot < 0 || slot >= MAX_AUDIO_FX) return -1;
    if (fx_name && fx_name[0] && strcmp(fx_name, "none") != 0 && !valid_module_name(fx_name)) {
        v2_chain_log(inst, "Invalid audio FX name");
        return -1;
    }

    /* Unload existing FX in this slot first */
    v2_unload_audio_fx_slot(inst, slot);

    /* Empty/none means just unload */
    if (!fx_name || fx_name[0] == '\0' || strcmp(fx_name, "none") == 0) {
        snprintf(msg, sizeof(msg), "Audio FX slot %d cleared", slot);
        v2_chain_log(inst, msg);
        /* Update fx_count if this was the last slot */
        while (inst->fx_count > 0 && inst->fx_handles[inst->fx_count - 1] == NULL) {
            inst->fx_count--;
        }
        return 0;
    }

    /* Build path to FX - all audio FX in modules/audio_fx/ */
    snprintf(fx_path, sizeof(fx_path), "%s/../audio_fx/%s/%s.so",
             inst->module_dir, fx_name, fx_name);
    snprintf(fx_dir, sizeof(fx_dir), "%s/../audio_fx/%s", inst->module_dir, fx_name);

    void *handle = dlopen(fx_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        snprintf(msg, sizeof(msg), "dlopen failed for FX %s: %s", fx_name, dlerror());
        v2_chain_log(inst, msg);
        return -1;
    }

    /* V2 API required */
    audio_fx_init_v2_fn init_v2 = (audio_fx_init_v2_fn)dlsym(handle, AUDIO_FX_INIT_V2_SYMBOL);
    if (!init_v2) {
        snprintf(msg, sizeof(msg), "Audio FX %s does not support V2 API (V2 required)", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    audio_fx_api_v2_t *api = init_v2(&inst->subplugin_host_api);
    if (!api || api->api_version != AUDIO_FX_API_VERSION_2) {
        snprintf(msg, sizeof(msg), "Audio FX %s V2 API version mismatch", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    void *fx_inst = api->create_instance(fx_dir, NULL);
    if (!fx_inst) {
        snprintf(msg, sizeof(msg), "Audio FX %s V2 create_instance failed", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    inst->fx_handles[slot] = handle;
    inst->fx_plugins[slot] = NULL;
    inst->fx_plugins_v2[slot] = api;
    inst->fx_instances[slot] = fx_inst;
    inst->fx_is_v2[slot] = 1;

    /* Check for optional MIDI handler (e.g. ducker) */
    {
        typedef void (*fx_on_midi_fn)(void *, const uint8_t *, int, int);
        inst->fx_on_midi[slot] = (fx_on_midi_fn)dlsym(handle, "move_audio_fx_on_midi");
    }

    /* Track the loaded module name */
    strncpy(inst->current_fx_modules[slot], fx_name, MAX_NAME_LEN - 1);
    inst->current_fx_modules[slot][MAX_NAME_LEN - 1] = '\0';

    /* Parse chain_params from module.json for type info */
    if (parse_chain_params(fx_dir, inst->fx_params[slot], &inst->fx_param_counts[slot]) < 0) {
        v2_chain_log(inst, "ERROR: Failed to parse audio FX parameters");
        api->destroy_instance(fx_inst);
        dlclose(handle);
        inst->fx_handles[slot] = NULL;
        inst->fx_plugins_v2[slot] = NULL;
        inst->fx_instances[slot] = NULL;
        inst->fx_is_v2[slot] = 0;
        inst->fx_on_midi[slot] = NULL;
        inst->current_fx_modules[slot][0] = '\0';
        return -1;
    }

    /* Update fx_count to include this slot */
    if (slot >= inst->fx_count) {
        inst->fx_count = slot + 1;
    }

    snprintf(msg, sizeof(msg), "Audio FX v2 loaded: %s (slot %d, %d params)", fx_name, slot, inst->fx_param_counts[slot]);
    v2_chain_log(inst, msg);
    return 0;
}

/* V2 unload MIDI source */
static void v2_unload_midi_source(chain_instance_t *inst) {
    if (!inst) return;

    if (inst->source_plugin && inst->source_plugin->on_unload) {
        inst->source_plugin->on_unload();
    }

    if (inst->source_handle) {
        dlclose(inst->source_handle);
    }

    inst->source_handle = NULL;
    inst->source_plugin = NULL;
    inst->current_source_module[0] = '\0';
}

/* V2 load synth - loads a sound generator module */
static int v2_load_synth(chain_instance_t *inst, const char *module_name) {
    char msg[256];
    char synth_path[MAX_PATH_LEN];
    char module_name_copy[MAX_NAME_LEN];  /* Local copy to avoid pointer invalidation */

    if (!inst) return -1;
    if (!module_name || !module_name[0]) return -1;
    if (!valid_module_name(module_name)) {
        v2_chain_log(inst, "Invalid synth module name");
        return -1;
    }

    /* Make a local copy of module_name immediately - the original pointer may
     * become invalid during file operations (e.g., shared param buffer reuse) */
    strncpy(module_name_copy, module_name, MAX_NAME_LEN - 1);
    module_name_copy[MAX_NAME_LEN - 1] = '\0';
    module_name = module_name_copy;  /* Use local copy from now on */

    /* Build path to synth module - all sound generators in modules/sound_generators/ */
    snprintf(synth_path, sizeof(synth_path), "%s/../sound_generators/%s",
             inst->module_dir, module_name);

    char dsp_path[MAX_PATH_LEN];
    snprintf(dsp_path, sizeof(dsp_path), "%s/dsp.so", synth_path);

    snprintf(msg, sizeof(msg), "Loading synth: %s", dsp_path);
    v2_chain_log(inst, msg);

    /* Open shared library */
    void *handle = dlopen(dsp_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        snprintf(msg, sizeof(msg), "dlopen failed: %s", dlerror());
        v2_chain_log(inst, msg);
        return -1;
    }

    /* V2 API required */
    move_plugin_init_v2_fn init_v2 = (move_plugin_init_v2_fn)dlsym(handle, MOVE_PLUGIN_INIT_V2_SYMBOL);
    if (!init_v2) {
        snprintf(msg, sizeof(msg), "Synth %s does not support V2 API (V2 required)", module_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    plugin_api_v2_t *api = init_v2(&inst->subplugin_host_api);
    if (!api || api->api_version != MOVE_PLUGIN_API_VERSION_2) {
        snprintf(msg, sizeof(msg), "Synth %s V2 API version mismatch", module_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    void *synth_inst = api->create_instance(synth_path, NULL);
    if (!synth_inst) {
        snprintf(msg, sizeof(msg), "Synth %s V2 create_instance failed", module_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    inst->synth_handle = handle;
    inst->synth_plugin = NULL;
    inst->synth_plugin_v2 = api;
    inst->synth_instance = synth_inst;
    strncpy(inst->current_synth_module, module_name, MAX_NAME_LEN - 1);

    /* Parse chain_params from module.json for type info */
    if (parse_chain_params(synth_path, inst->synth_params, &inst->synth_param_count) < 0) {
        v2_chain_log(inst, "ERROR: Failed to parse synth parameters");
        api->destroy_instance(synth_inst);
        dlclose(handle);
        inst->synth_handle = NULL;
        inst->synth_plugin_v2 = NULL;
        inst->synth_instance = NULL;
        inst->current_synth_module[0] = '\0';
        return -1;
    }

    /* Parse default_forward_channel from capabilities in module.json */
    inst->synth_default_forward_channel = -1;  /* Default: no forwarding preference */
    {
        char json_path[MAX_PATH_LEN];
        snprintf(json_path, sizeof(json_path), "%s/module.json", synth_path);
        FILE *f = fopen(json_path, "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (size > 0 && size < 65536) {
                char *json = malloc(size + 1);
                if (json) {
                    { size_t nr = fread(json, 1, size, f); json[nr] = '\0'; }
                    int fwd_ch = -1;
                    if (json_get_int_in_section(json, "capabilities", "default_forward_channel", &fwd_ch) == 0) {
                        if (fwd_ch >= 1 && fwd_ch <= 16) {
                            inst->synth_default_forward_channel = fwd_ch - 1;  /* Store as 0-15 */
                            snprintf(msg, sizeof(msg), "Synth default_forward_channel: %d", fwd_ch);
                            v2_chain_log(inst, msg);
                        }
                    }
                    free(json);
                }
            }
            fclose(f);
        }
    }

    snprintf(msg, sizeof(msg), "Synth v2 loaded: %s (%d params)", module_name, inst->synth_param_count);
    v2_chain_log(inst, msg);
    return 0;
}

/* V2 load audio FX */
static int v2_load_audio_fx(chain_instance_t *inst, const char *fx_name) {
    char msg[256];
    char fx_path[MAX_PATH_LEN];
    char fx_dir[MAX_PATH_LEN];

    if (!inst || inst->fx_count >= MAX_AUDIO_FX) return -1;

    /* Build path to FX - all audio FX in modules/audio_fx/ */
    snprintf(fx_path, sizeof(fx_path), "%s/../audio_fx/%s/%s.so",
             inst->module_dir, fx_name, fx_name);
    snprintf(fx_dir, sizeof(fx_dir), "%s/../audio_fx/%s", inst->module_dir, fx_name);

    void *handle = dlopen(fx_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        snprintf(msg, sizeof(msg), "dlopen failed for FX %s: %s", fx_name, dlerror());
        v2_chain_log(inst, msg);
        return -1;
    }

    int slot = inst->fx_count;

    /* V2 API required */
    audio_fx_init_v2_fn init_v2 = (audio_fx_init_v2_fn)dlsym(handle, AUDIO_FX_INIT_V2_SYMBOL);
    if (!init_v2) {
        snprintf(msg, sizeof(msg), "Audio FX %s does not support V2 API (V2 required)", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    audio_fx_api_v2_t *api = init_v2(&inst->subplugin_host_api);
    if (!api || api->api_version != AUDIO_FX_API_VERSION_2) {
        snprintf(msg, sizeof(msg), "Audio FX %s V2 API version mismatch", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    void *fx_inst = api->create_instance(fx_dir, NULL);
    if (!fx_inst) {
        snprintf(msg, sizeof(msg), "Audio FX %s V2 create_instance failed", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    inst->fx_handles[slot] = handle;
    inst->fx_plugins[slot] = NULL;
    inst->fx_plugins_v2[slot] = api;
    inst->fx_instances[slot] = fx_inst;
    inst->fx_is_v2[slot] = 1;

    /* Check for optional MIDI handler (e.g. ducker) */
    {
        typedef void (*fx_on_midi_fn)(void *, const uint8_t *, int, int);
        inst->fx_on_midi[slot] = (fx_on_midi_fn)dlsym(handle, "move_audio_fx_on_midi");
    }

    /* Track the loaded module name */
    strncpy(inst->current_fx_modules[slot], fx_name, MAX_NAME_LEN - 1);
    inst->current_fx_modules[slot][MAX_NAME_LEN - 1] = '\0';

    /* Parse chain_params from module.json for type info */
    if (parse_chain_params(fx_dir, inst->fx_params[slot], &inst->fx_param_counts[slot]) < 0) {
        v2_chain_log(inst, "ERROR: Failed to parse audio FX parameters");
        api->destroy_instance(fx_inst);
        dlclose(handle);
        inst->fx_handles[slot] = NULL;
        inst->fx_plugins_v2[slot] = NULL;
        inst->fx_instances[slot] = NULL;
        inst->fx_is_v2[slot] = 0;
        inst->fx_on_midi[slot] = NULL;
        inst->current_fx_modules[slot][0] = '\0';
        return -1;
    }

    inst->fx_count++;

    snprintf(msg, sizeof(msg), "Audio FX v2 loaded: %s (slot %d, %d params)", fx_name, slot, inst->fx_param_counts[slot]);
    v2_chain_log(inst, msg);
    return 0;
}

/* V2 scan patches - simple version */
static int v2_scan_patches(chain_instance_t *inst) {
    char patches_dir[MAX_PATH_LEN];
    char msg[256];

    if (!inst) return -1;

    snprintf(patches_dir, sizeof(patches_dir), "%s/../../patches", inst->module_dir);
    inst->patch_count = 0;
    /* Clear old data to prevent stale/garbage entries */
    memset(inst->patches, 0, sizeof(inst->patches));

    DIR *dir = opendir(patches_dir);
    if (!dir) {
        snprintf(msg, sizeof(msg), "Cannot open patches dir: %s", patches_dir);
        v2_chain_log(inst, msg);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && inst->patch_count < MAX_PATCHES) {
        if (entry->d_name[0] == '.') continue;

        size_t len = strlen(entry->d_name);
        if (len < 5 || strcmp(entry->d_name + len - 5, ".json") != 0) continue;

        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", patches_dir, entry->d_name);

        patch_info_t *patch = &inst->patches[inst->patch_count];
        if (v2_parse_patch_file(inst, path, patch) == 0) {
            strncpy(patch->path, path, MAX_PATH_LEN - 1);
            inst->patch_count++;
        }
    }

    closedir(dir);

    /* Sort patches alphabetically by name */
    if (inst->patch_count > 1) {
        qsort(inst->patches, inst->patch_count, sizeof(patch_info_t), compare_patches);
    }

    return inst->patch_count;
}

/* V2 save patch - uses instance data instead of globals */
static int v2_save_patch(chain_instance_t *inst, const char *json_data) {
    char msg[256];
    char patches_dir[MAX_PATH_LEN];
    snprintf(patches_dir, sizeof(patches_dir), "%s/../../patches", inst->module_dir);

    /* Parse incoming JSON to get components for name generation */
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
        generate_patch_name(name, sizeof(name), synth, preset, fx1, fx2);
    }

    /* Sanitize to filename */
    char base_filename[MAX_NAME_LEN];
    sanitize_filename(base_filename, sizeof(base_filename), name);

    /* Find available filename */
    char filepath[MAX_PATH_LEN];
    if (check_filename_exists(patches_dir, base_filename, filepath, sizeof(filepath))) {
        for (int i = 2; i < 100; i++) {
            char suffixed[MAX_NAME_LEN];
            snprintf(suffixed, sizeof(suffixed), "%s_%02d", base_filename, i);
            if (!check_filename_exists(patches_dir, suffixed, filepath, sizeof(filepath))) {
                int namelen = strlen(name);
                snprintf(name + namelen, sizeof(name) - namelen, " %02d", i);
                break;
            }
        }
    }

    /* Escape name for JSON embedding (handle quotes and backslashes) */
    char escaped_name[MAX_NAME_LEN * 2];
    {
        int si = 0, di = 0;
        while (name[si] && di < (int)sizeof(escaped_name) - 2) {
            if (name[si] == '"' || name[si] == '\\') {
                escaped_name[di++] = '\\';
            }
            escaped_name[di++] = name[si++];
        }
        escaped_name[di] = '\0';
    }

    /* Build final JSON with generated name */
    char final_json[8192];
    snprintf(final_json, sizeof(final_json),
        "{\n"
        "    \"name\": \"%s\",\n"
        "    \"version\": 1,\n"
        "    \"chain\": %s\n"
        "}\n",
        escaped_name, json_data);

    /* Write file */
    FILE *f = fopen(filepath, "w");
    if (!f) {
        snprintf(msg, sizeof(msg), "[v2] Failed to create patch file: %s", filepath);
        v2_chain_log(inst, msg);
        return -1;
    }

    fwrite(final_json, 1, strlen(final_json), f);
    fclose(f);

    snprintf(msg, sizeof(msg), "[v2] Saved patch: %s", filepath);
    v2_chain_log(inst, msg);

    return 0;
}

/* V2 update patch - uses instance data instead of globals */
static int v2_update_patch(chain_instance_t *inst, int index, const char *json_data) {
    char msg[256];

    if (index < 0 || index >= inst->patch_count) {
        snprintf(msg, sizeof(msg), "[v2] Invalid patch index for update: %d (count=%d)", index, inst->patch_count);
        v2_chain_log(inst, msg);
        return -1;
    }

    const char *filepath = inst->patches[index].path;

    /* Check for custom name, otherwise keep existing name */
    char name[MAX_NAME_LEN];
    if (json_get_string(json_data, "custom_name", name, sizeof(name)) != 0) {
        strncpy(name, inst->patches[index].name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }

    /* Build final JSON with name */
    char final_json[8192];
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
        snprintf(msg, sizeof(msg), "[v2] Failed to update patch file: %s", filepath);
        v2_chain_log(inst, msg);
        return -1;
    }

    fwrite(final_json, 1, strlen(final_json), f);
    fclose(f);

    snprintf(msg, sizeof(msg), "[v2] Updated patch: %s", filepath);
    v2_chain_log(inst, msg);

    return 0;
}

/* V2 delete patch - uses instance data instead of globals */
static int v2_delete_patch(chain_instance_t *inst, int index) {
    char msg[256];

    if (index < 0 || index >= inst->patch_count) {
        snprintf(msg, sizeof(msg), "[v2] Invalid patch index for delete: %d (count=%d)", index, inst->patch_count);
        v2_chain_log(inst, msg);
        return -1;
    }

    const char *path = inst->patches[index].path;

    if (remove(path) != 0) {
        snprintf(msg, sizeof(msg), "[v2] Failed to delete patch: %s", path);
        v2_chain_log(inst, msg);
        return -1;
    }

    snprintf(msg, sizeof(msg), "[v2] Deleted patch: %s", path);
    v2_chain_log(inst, msg);

    /* Adjust current_patch index if needed */
    if (index == inst->current_patch) {
        inst->current_patch = -1;  /* Mark as no patch loaded */
    } else if (index < inst->current_patch) {
        inst->current_patch--;
    }

    return 0;
}

/* ========== Master Preset Functions ========== */

#define PRESETS_MASTER_DIR "/data/UserData/move-anything/presets_master"
#define MAX_MASTER_PRESETS 64

/* Master preset info storage (simpler than chain patches) */
static char master_preset_names[MAX_MASTER_PRESETS][MAX_NAME_LEN];
static char master_preset_paths[MAX_MASTER_PRESETS][MAX_PATH_LEN];
static int master_preset_count = 0;

static void ensure_presets_master_dir(void) {
    struct stat st = {0};
    if (stat(PRESETS_MASTER_DIR, &st) == -1) {
        mkdir(PRESETS_MASTER_DIR, 0755);
    }
}

static void scan_master_presets(void) {
    master_preset_count = 0;
    /* Clear old data to prevent stale/garbage entries */
    memset(master_preset_names, 0, sizeof(master_preset_names));
    memset(master_preset_paths, 0, sizeof(master_preset_paths));
    ensure_presets_master_dir();

    DIR *dir = opendir(PRESETS_MASTER_DIR);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && master_preset_count < MAX_MASTER_PRESETS) {
        if (entry->d_type != DT_REG) continue;

        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len < 6 || strcmp(name + len - 5, ".json") != 0) continue;

        /* Extract name without .json extension */
        size_t name_len = len - 5;
        if (name_len >= MAX_NAME_LEN) name_len = MAX_NAME_LEN - 1;

        /* Try to read the "name" field from the JSON file */
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", PRESETS_MASTER_DIR, name);

        FILE *f = fopen(path, "r");
        if (f) {
            char json_buf[2048];
            size_t read_len = fread(json_buf, 1, sizeof(json_buf) - 1, f);
            json_buf[read_len] = '\0';
            fclose(f);

            /* Try to get name from JSON */
            char parsed_name[MAX_NAME_LEN] = {0};
            if (json_get_string(json_buf, "name", parsed_name, sizeof(parsed_name)) == 0) {
                strncpy(master_preset_names[master_preset_count], parsed_name, MAX_NAME_LEN - 1);
                master_preset_names[master_preset_count][MAX_NAME_LEN - 1] = '\0';
            } else {
                /* Fall back to filename */
                memcpy(master_preset_names[master_preset_count], name, name_len);
                master_preset_names[master_preset_count][name_len] = '\0';
            }
        } else {
            memcpy(master_preset_names[master_preset_count], name, name_len);
            master_preset_names[master_preset_count][name_len] = '\0';
        }

        strncpy(master_preset_paths[master_preset_count], path, MAX_PATH_LEN - 1);
        master_preset_paths[master_preset_count][MAX_PATH_LEN - 1] = '\0';
        master_preset_count++;
    }
    closedir(dir);
}

/* Helper to extract JSON object section as string */
static int extract_fx_section(const char *json, const char *key, char *out, int out_len) {
    const char *start = NULL;
    const char *end = NULL;
    if (json_get_section_bounds(json, key, &start, &end) != 0) {
        strncpy(out, "null", out_len - 1);
        out[out_len - 1] = '\0';
        return -1;
    }
    int len = (int)(end - start + 1);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

static int save_master_preset(const char *json_str) {
    ensure_presets_master_dir();

    /* Get custom_name from JSON */
    char name[MAX_NAME_LEN] = "Master FX";
    json_get_string(json_str, "custom_name", name, sizeof(name));

    /* Debug: log incoming JSON and extracted name */
    {
        struct stat st;
        if (stat(CHAIN_DEBUG_FLAG_PATH, &st) == 0) {
            FILE *dbg = fopen(CHAIN_DEBUG_LOG_PATH, "a");
            if (dbg) {
                fprintf(dbg, "save_master_preset json='%.200s'\n", json_str);
                fprintf(dbg, "save_master_preset name='%s' len=%zu\n", name, strlen(name));
                fclose(dbg);
            }
        }
    }

    /* Sanitize name for filename */
    char filename[MAX_NAME_LEN];
    sanitize_filename(filename, sizeof(filename), name);

    /* Build path */
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s.json", PRESETS_MASTER_DIR, filename);

    /* Extract each FX section */
    char fx1[512], fx2[512], fx3[512], fx4[512];
    extract_fx_section(json_str, "fx1", fx1, sizeof(fx1));
    extract_fx_section(json_str, "fx2", fx2, sizeof(fx2));
    extract_fx_section(json_str, "fx3", fx3, sizeof(fx3));
    extract_fx_section(json_str, "fx4", fx4, sizeof(fx4));

    /* Build wrapped JSON */
    char final_json[8192];
    snprintf(final_json, sizeof(final_json),
        "{\n"
        "    \"name\": \"%s\",\n"
        "    \"version\": 1,\n"
        "    \"master_fx\": {\n"
        "        \"fx1\": %s,\n"
        "        \"fx2\": %s,\n"
        "        \"fx3\": %s,\n"
        "        \"fx4\": %s\n"
        "    }\n"
        "}\n",
        name, fx1, fx2, fx3, fx4);

    /* Write file */
    FILE *f = fopen(path, "w");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to save master preset: %s", path);
        chain_log(msg);
        return -1;
    }

    fputs(final_json, f);
    fclose(f);
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Saved master preset: %s", name);
        chain_log(msg);
    }

    scan_master_presets();
    return 0;
}

static int update_master_preset(int index, const char *json_str) {
    if (index < 0 || index >= master_preset_count) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Invalid master preset index: %d", index);
        chain_log(msg);
        return -1;
    }

    /* Get name from JSON or use existing */
    char name[MAX_NAME_LEN];
    if (json_get_string(json_str, "custom_name", name, sizeof(name)) != 0) {
        strncpy(name, master_preset_names[index], sizeof(name) - 1);
    }

    /* Extract each FX section */
    char fx1[512], fx2[512], fx3[512], fx4[512];
    extract_fx_section(json_str, "fx1", fx1, sizeof(fx1));
    extract_fx_section(json_str, "fx2", fx2, sizeof(fx2));
    extract_fx_section(json_str, "fx3", fx3, sizeof(fx3));
    extract_fx_section(json_str, "fx4", fx4, sizeof(fx4));

    /* Build wrapped JSON */
    char final_json[8192];
    snprintf(final_json, sizeof(final_json),
        "{\n"
        "    \"name\": \"%s\",\n"
        "    \"version\": 1,\n"
        "    \"master_fx\": {\n"
        "        \"fx1\": %s,\n"
        "        \"fx2\": %s,\n"
        "        \"fx3\": %s,\n"
        "        \"fx4\": %s\n"
        "    }\n"
        "}\n",
        name, fx1, fx2, fx3, fx4);

    /* Write to existing path */
    FILE *f = fopen(master_preset_paths[index], "w");
    if (!f) {
        return -1;
    }

    fputs(final_json, f);
    fclose(f);

    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Updated master preset: %s", name);
        chain_log(msg);
    }
    scan_master_presets();
    return 0;
}

static int delete_master_preset(int index) {
    if (index < 0 || index >= master_preset_count) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Invalid master preset index: %d", index);
        chain_log(msg);
        return -1;
    }

    if (remove(master_preset_paths[index]) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to delete master preset: %s", master_preset_paths[index]);
        chain_log(msg);
        return -1;
    }

    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Deleted master preset: %s", master_preset_names[index]);
        chain_log(msg);
    }
    scan_master_presets();
    return 0;
}

static int load_master_preset_json(int index, char *buf, int buf_len) {
    if (index < 0 || index >= master_preset_count) {
        buf[0] = '\0';
        return 0;
    }

    FILE *f = fopen(master_preset_paths[index], "r");
    if (!f) {
        buf[0] = '\0';
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0) {
        fclose(f);
        buf[0] = '\0';
        return 0;
    }
    if (len >= buf_len) len = buf_len - 1;
    size_t nr = fread(buf, 1, len, f);
    buf[nr] = '\0';
    len = (long)nr;
    fclose(f);

    return (int)len;
}

/* ========== End Master Preset Functions ========== */

/* Debug logging helper for parsing */
static void parse_debug_log(const char *msg) {
    struct stat st;
    if (stat(CHAIN_DEBUG_FLAG_PATH, &st) != 0) return;
    FILE *dbg = fopen(CHAIN_DEBUG_LOG_PATH, "a");
    if (dbg) {
        fprintf(dbg, "%s\n", msg);
        fclose(dbg);
    }
}

/* V2 parse patch file - simplified version */
static int v2_parse_patch_file(chain_instance_t *inst, const char *path, patch_info_t *patch) {
    (void)inst;

    char dbgmsg[512];
    snprintf(dbgmsg, sizeof(dbgmsg), "=== Parsing: %s ===", path);
    parse_debug_log(dbgmsg);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 16384) {
        fclose(f);
        return -1;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return -1;
    }

    { size_t nr = fread(json, 1, size, f); json[nr] = '\0'; }
    fclose(f);

    memset(patch, 0, sizeof(*patch));

    /* Parse name */
    json_get_string(json, "name", patch->name, MAX_NAME_LEN);
    if (!patch->name[0]) {
        /* Use filename as name */
        const char *slash = strrchr(path, '/');
        const char *base = slash ? slash + 1 : path;
        strncpy(patch->name, base, MAX_NAME_LEN - 1);
        char *dot = strrchr(patch->name, '.');
        if (dot) *dot = '\0';
    }

    /* Parse synth */
    json_get_string_in_section(json, "synth", "module", patch->synth_module, MAX_NAME_LEN);
    json_get_int_in_section(json, "synth", "preset", &patch->synth_preset);

    /* Parse synth config.state if present */
    patch->synth_state[0] = '\0';
    const char *synth_pos = strstr(json, "\"synth\"");
    if (synth_pos) {
        const char *config_pos = strstr(synth_pos, "\"config\"");
        if (config_pos) {
            const char *state_pos = strstr(config_pos, "\"state\"");
            if (state_pos) {
                const char *state_obj = strchr(state_pos, '{');
                if (state_obj) {
                    /* Find matching closing brace */
                    const char *end = state_obj + 1;
                    int depth = 1;
                    while (*end && depth > 0) {
                        if (*end == '{') depth++;
                        else if (*end == '}') depth--;
                        if (depth > 0) end++;
                    }
                    if (*end && depth == 0) {
                        int len = end - state_obj + 1;
                        if (len > 0 && len < MAX_SYNTH_STATE_LEN) {
                            strncpy(patch->synth_state, state_obj, len);
                            patch->synth_state[len] = '\0';
                        }
                    }
                }
            }
        }
    }

    /* Parse audio_fx array */
    const char *fx_pos = strstr(json, "\"audio_fx\"");
    if (fx_pos) {
        const char *bracket = strchr(fx_pos, '[');
        if (bracket) {
            bracket++;
            while (patch->audio_fx_count < MAX_AUDIO_FX) {
                /* Find the start of next FX object */
                const char *obj_start = strchr(bracket, '{');
                if (!obj_start) break;

                /* Find matching closing brace (handle nested objects) */
                const char *obj_end = obj_start + 1;
                int depth = 1;
                while (*obj_end && depth > 0) {
                    if (*obj_end == '{') depth++;
                    else if (*obj_end == '}') depth--;
                    if (depth > 0) obj_end++;
                }
                if (!*obj_end || depth != 0) break;

                audio_fx_config_t *cfg = &patch->audio_fx[patch->audio_fx_count];
                memset(cfg, 0, sizeof(*cfg));

                /* Parse "type" field */
                const char *type_pos = strstr(obj_start, "\"type\"");
                if (!type_pos || type_pos > obj_end) {
                    bracket = obj_end + 1;
                    continue;
                }

                const char *colon = strchr(type_pos, ':');
                if (!colon || colon > obj_end) {
                    bracket = obj_end + 1;
                    continue;
                }

                const char *quote1 = strchr(colon, '"');
                if (!quote1 || quote1 > obj_end) {
                    bracket = obj_end + 1;
                    continue;
                }
                quote1++;

                const char *quote2 = strchr(quote1, '"');
                if (!quote2 || quote2 > obj_end) {
                    bracket = obj_end + 1;
                    continue;
                }

                int len = quote2 - quote1;
                if (len > 0 && len < MAX_NAME_LEN) {
                    strncpy(cfg->module, quote1, len);
                    cfg->module[len] = '\0';
                }

                /* Parse "params" object if present */
                const char *params_pos = strstr(obj_start, "\"params\"");
                {
                    char dbg[256];
                    snprintf(dbg, sizeof(dbg), "[parse] audio_fx type='%s' params_pos=%s",
                             cfg->module, params_pos ? "found" : "null");
                    parse_debug_log(dbg);
                }
                if (params_pos && params_pos < obj_end) {
                    const char *params_obj = strchr(params_pos, '{');
                    /* Find matching closing brace for params (handle nested objects) */
                    const char *params_end = NULL;
                    if (params_obj && params_obj < obj_end) {
                        const char *p = params_obj + 1;
                        int pdepth = 1;
                        while (*p && pdepth > 0 && p < obj_end) {
                            if (*p == '{') pdepth++;
                            else if (*p == '}') pdepth--;
                            if (pdepth > 0) p++;
                        }
                        if (pdepth == 0) params_end = p;
                    }
                    {
                        char dbg[256];
                        snprintf(dbg, sizeof(dbg), "[parse] params_obj=%p params_end=%p obj_end=%p check=%s",
                                 (void*)params_obj, (void*)params_end, (void*)obj_end,
                                 (params_obj && params_end && params_end <= obj_end) ? "pass" : "fail");
                        parse_debug_log(dbg);
                    }
                    if (params_obj && params_end && params_end <= obj_end) {
                        /* Check for nested "state" object in params */
                        cfg->state[0] = '\0';
                        const char *state_key = strstr(params_obj, "\"state\"");
                        if (state_key && state_key < params_end) {
                            const char *state_colon = strchr(state_key, ':');
                            if (state_colon && state_colon < params_end) {
                                /* Skip whitespace */
                                const char *sv = state_colon + 1;
                                while (sv < params_end && (*sv == ' ' || *sv == '\t' || *sv == '\n')) sv++;
                                if (*sv == '{') {
                                    /* Extract entire state object */
                                    const char *state_start = sv;
                                    const char *se = sv + 1;
                                    int sdepth = 1;
                                    while (*se && sdepth > 0 && se < params_end) {
                                        if (*se == '{') sdepth++;
                                        else if (*se == '}') sdepth--;
                                        if (sdepth > 0) se++;
                                    }
                                    if (sdepth == 0) {
                                        int slen = se - state_start + 1;
                                        if (slen > 0 && slen < MAX_FX_STATE_LEN) {
                                            strncpy(cfg->state, state_start, slen);
                                            cfg->state[slen] = '\0';
                                            parse_debug_log("[parse] Extracted audio_fx state object");
                                        }
                                    }
                                }
                            }
                        }
                        /* Parse key-value pairs in params object */
                        const char *scan = params_obj;
                        while (cfg->param_count < MAX_MIDI_FX_PARAMS && scan < params_end) {
                            const char *key_start = strchr(scan, '"');
                            if (!key_start || key_start >= params_end) break;

                            const char *key_end = strchr(key_start + 1, '"');
                            if (!key_end || key_end >= params_end) break;

                            int key_len = key_end - key_start - 1;
                            if (key_len <= 0 || key_len >= 32) {
                                scan = key_end + 1;
                                continue;
                            }

                            /* Skip "state" key - already extracted separately */
                            if (key_len == 5 && strncmp(key_start + 1, "state", 5) == 0) {
                                /* Skip to end of state object */
                                const char *sc = strchr(key_end, ':');
                                if (sc && sc < params_end) {
                                    const char *sv = sc + 1;
                                    while (sv < params_end && (*sv == ' ' || *sv == '\t' || *sv == '\n')) sv++;
                                    if (*sv == '{') {
                                        /* Skip object */
                                        int od = 1;
                                        sv++;
                                        while (*sv && od > 0 && sv < params_end) {
                                            if (*sv == '{') od++;
                                            else if (*sv == '}') od--;
                                            sv++;
                                        }
                                        scan = sv;
                                    } else {
                                        scan = sv;
                                    }
                                } else {
                                    scan = key_end + 1;
                                }
                                continue;
                            }

                            /* Find value after colon */
                            const char *val_colon = strchr(key_end, ':');
                            if (!val_colon || val_colon >= params_end) {
                                scan = key_end + 1;
                                continue;
                            }

                            /* Skip whitespace */
                            const char *val_start = val_colon + 1;
                            while (val_start < params_end && (*val_start == ' ' || *val_start == '\t')) {
                                val_start++;
                            }

                            /* Skip object values (other nested objects besides state) */
                            if (*val_start == '{') {
                                int od = 1;
                                const char *ov = val_start + 1;
                                while (*ov && od > 0 && ov < params_end) {
                                    if (*ov == '{') od++;
                                    else if (*ov == '}') od--;
                                    ov++;
                                }
                                scan = ov;
                                continue;
                            }

                            char val_buf[32] = "";
                            if (*val_start == '"') {
                                /* String value */
                                val_start++;
                                const char *val_end = strchr(val_start, '"');
                                if (val_end && val_end < params_end) {
                                    int val_len = val_end - val_start;
                                    if (val_len > 31) val_len = 31;
                                    strncpy(val_buf, val_start, val_len);
                                    val_buf[val_len] = '\0';
                                    scan = val_end + 1;
                                } else {
                                    scan = val_start;
                                    continue;
                                }
                            } else {
                                /* Numeric value */
                                const char *val_end = val_start;
                                while (val_end < params_end && *val_end != ',' && *val_end != '}' &&
                                       *val_end != ' ' && *val_end != '\n' && *val_end != '\r') {
                                    val_end++;
                                }
                                int val_len = val_end - val_start;
                                if (val_len > 31) val_len = 31;
                                strncpy(val_buf, val_start, val_len);
                                val_buf[val_len] = '\0';
                                scan = val_end;
                            }

                            /* Store the param */
                            strncpy(cfg->params[cfg->param_count].key, key_start + 1, key_len);
                            cfg->params[cfg->param_count].key[key_len] = '\0';
                            strncpy(cfg->params[cfg->param_count].val, val_buf, 31);
                            cfg->params[cfg->param_count].val[31] = '\0';
                            {
                                char dbg[256];
                                snprintf(dbg, sizeof(dbg), "[parse] stored param[%d]: key='%s' val='%s'",
                                         cfg->param_count, cfg->params[cfg->param_count].key, val_buf);
                                parse_debug_log(dbg);
                            }
                            cfg->param_count++;
                        }
                    }
                }

                {
                    char dbg[128];
                    snprintf(dbg, sizeof(dbg), "[parse] audio_fx[%d] param_count: %d",
                             patch->audio_fx_count, cfg->param_count);
                    parse_debug_log(dbg);
                }
                patch->audio_fx_count++;
                bracket = obj_end + 1;
            }
        }
    }

    /* Parse midi_fx array (native MIDI FX modules with params) */
    const char *midi_fx_pos = strstr(json, "\"midi_fx\"");
    if (midi_fx_pos) {
        const char *arr_start = strchr(midi_fx_pos, '[');
        const char *arr_end = arr_start ? strchr(arr_start, ']') : NULL;

        if (arr_start && arr_end) {
            const char *obj_start = arr_start;
            while (patch->midi_fx_count < MAX_MIDI_FX) {
                obj_start = strchr(obj_start + 1, '{');
                if (!obj_start || obj_start > arr_end) break;

                /* Find matching closing brace for midi_fx object (handle nested objects) */
                const char *obj_end = obj_start + 1;
                int mfx_depth = 1;
                while (*obj_end && mfx_depth > 0 && obj_end < arr_end) {
                    if (*obj_end == '{') mfx_depth++;
                    else if (*obj_end == '}') mfx_depth--;
                    if (mfx_depth > 0) obj_end++;
                }
                if (!*obj_end || mfx_depth != 0) break;

                midi_fx_config_t *cfg = &patch->midi_fx[patch->midi_fx_count];
                memset(cfg, 0, sizeof(*cfg));

                /* Parse "type" (module name) - consistent with audio_fx format */
                const char *type_pos = strstr(obj_start, "\"type\"");
                if (type_pos && type_pos < obj_end) {
                    const char *q1 = strchr(strchr(type_pos, ':'), '"');
                    if (q1 && q1 < obj_end) {
                        const char *q2 = strchr(q1 + 1, '"');
                        if (q2 && q2 < obj_end) {
                            int len = q2 - q1 - 1;
                            if (len > MAX_NAME_LEN - 1) len = MAX_NAME_LEN - 1;
                            strncpy(cfg->module, q1 + 1, len);
                            cfg->module[len] = '\0';
                        }
                    }
                }

                /* Check for "params" object and extract state if present */
                cfg->state[0] = '\0';
                const char *params_pos = strstr(obj_start, "\"params\"");
                if (params_pos && params_pos < obj_end) {
                    const char *params_obj = strchr(params_pos, '{');
                    if (params_obj && params_obj < obj_end) {
                        /* Find matching closing brace for params */
                        const char *params_end = params_obj + 1;
                        int pdepth = 1;
                        while (*params_end && pdepth > 0 && params_end < obj_end) {
                            if (*params_end == '{') pdepth++;
                            else if (*params_end == '}') pdepth--;
                            if (pdepth > 0) params_end++;
                        }
                        if (pdepth == 0) {
                            /* Look for state object inside params */
                            const char *state_key = strstr(params_obj, "\"state\"");
                            if (state_key && state_key < params_end) {
                                const char *state_colon = strchr(state_key, ':');
                                if (state_colon && state_colon < params_end) {
                                    const char *sv = state_colon + 1;
                                    while (sv < params_end && (*sv == ' ' || *sv == '\t' || *sv == '\n')) sv++;
                                    if (*sv == '{') {
                                        const char *state_start = sv;
                                        const char *se = sv + 1;
                                        int sdepth = 1;
                                        while (*se && sdepth > 0 && se < params_end) {
                                            if (*se == '{') sdepth++;
                                            else if (*se == '}') sdepth--;
                                            if (sdepth > 0) se++;
                                        }
                                        if (sdepth == 0) {
                                            int slen = se - state_start + 1;
                                            if (slen > 0 && slen < MAX_FX_STATE_LEN) {
                                                strncpy(cfg->state, state_start, slen);
                                                cfg->state[slen] = '\0';
                                                parse_debug_log("[parse] Extracted midi_fx state object");
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                /* Parse other fields as params (skip "type" and "params") */
                const char *scan = obj_start;
                while (cfg->param_count < MAX_MIDI_FX_PARAMS && scan < obj_end) {
                    const char *key_start = strchr(scan, '"');
                    if (!key_start || key_start >= obj_end) break;

                    const char *key_end = strchr(key_start + 1, '"');
                    if (!key_end || key_end >= obj_end) break;

                    int key_len = key_end - key_start - 1;
                    if (key_len <= 0 || key_len >= 32) {
                        scan = key_end + 1;
                        continue;
                    }

                    /* Skip "type" - already parsed as module name */
                    if (key_len == 4 && strncmp(key_start + 1, "type", 4) == 0) {
                        const char *colon = strchr(key_end, ':');
                        if (colon && colon < obj_end) {
                            const char *vq1 = strchr(colon, '"');
                            if (vq1 && vq1 < obj_end) {
                                const char *vq2 = strchr(vq1 + 1, '"');
                                scan = vq2 ? vq2 + 1 : obj_end;
                            } else {
                                scan = obj_end;
                            }
                        } else {
                            scan = key_end + 1;
                        }
                        continue;
                    }

                    /* Skip "params" - contains nested state object */
                    if (key_len == 6 && strncmp(key_start + 1, "params", 6) == 0) {
                        const char *colon = strchr(key_end, ':');
                        if (colon && colon < obj_end) {
                            const char *pv = colon + 1;
                            while (pv < obj_end && (*pv == ' ' || *pv == '\t' || *pv == '\n')) pv++;
                            if (*pv == '{') {
                                int pd = 1;
                                pv++;
                                while (*pv && pd > 0 && pv < obj_end) {
                                    if (*pv == '{') pd++;
                                    else if (*pv == '}') pd--;
                                    pv++;
                                }
                                scan = pv;
                            } else {
                                scan = pv;
                            }
                        } else {
                            scan = key_end + 1;
                        }
                        continue;
                    }

                    /* Found a param key */
                    midi_fx_param_t *p = &cfg->params[cfg->param_count];
                    strncpy(p->key, key_start + 1, key_len);
                    p->key[key_len] = '\0';

                    /* Get value */
                    const char *colon = strchr(key_end, ':');
                    if (!colon || colon >= obj_end) {
                        scan = key_end + 1;
                        continue;
                    }

                    /* Skip whitespace */
                    const char *val_start = colon + 1;
                    while (val_start < obj_end && (*val_start == ' ' || *val_start == '\t')) val_start++;

                    if (val_start >= obj_end) {
                        scan = obj_end;
                        continue;
                    }

                    if (*val_start == '"') {
                        /* String value */
                        const char *vq1 = val_start;
                        const char *vq2 = strchr(vq1 + 1, '"');
                        if (vq2 && vq2 < obj_end) {
                            int val_len = vq2 - vq1 - 1;
                            if (val_len > 31) val_len = 31;
                            strncpy(p->val, vq1 + 1, val_len);
                            p->val[val_len] = '\0';
                            cfg->param_count++;
                            scan = vq2 + 1;
                        } else {
                            scan = obj_end;
                        }
                    } else {
                        /* Numeric value */
                        const char *val_end = val_start;
                        while (val_end < obj_end && *val_end != ',' && *val_end != '}') val_end++;
                        int val_len = val_end - val_start;
                        if (val_len > 31) val_len = 31;
                        strncpy(p->val, val_start, val_len);
                        p->val[val_len] = '\0';
                        /* Trim trailing whitespace */
                        while (val_len > 0 && (p->val[val_len-1] == ' ' || p->val[val_len-1] == '\t' || p->val[val_len-1] == '\n')) {
                            p->val[--val_len] = '\0';
                        }
                        cfg->param_count++;
                        scan = val_end;
                    }
                }

                if (cfg->module[0]) {
                    patch->midi_fx_count++;
                }

                obj_start = obj_end;
            }
        }
    }

    /* Parse knob_mappings - simplified */
    const char *mappings_pos = strstr(json, "\"knob_mappings\"");
    if (mappings_pos) {
        const char *arr_start = strchr(mappings_pos, '[');
        const char *arr_end = arr_start ? strchr(arr_start, ']') : NULL;

        if (arr_start && arr_end) {
            const char *obj_start = arr_start;
            while (patch->knob_mapping_count < MAX_KNOB_MAPPINGS) {
                obj_start = strchr(obj_start + 1, '{');
                if (!obj_start || obj_start > arr_end) break;

                const char *obj_end = strchr(obj_start, '}');
                if (!obj_end || obj_end > arr_end) break;

                knob_mapping_t *m = &patch->knob_mappings[patch->knob_mapping_count];

                /* Parse cc */
                const char *cc_pos = strstr(obj_start, "\"cc\"");
                if (cc_pos && cc_pos < obj_end) {
                    const char *colon = strchr(cc_pos, ':');
                    if (colon) m->cc = atoi(colon + 1);
                }

                /* Parse target */
                const char *target_pos = strstr(obj_start, "\"target\"");
                if (target_pos && target_pos < obj_end) {
                    const char *q1 = strchr(strchr(target_pos, ':'), '"');
                    if (q1 && q1 < obj_end) {
                        const char *q2 = strchr(q1 + 1, '"');
                        if (q2 && q2 < obj_end) {
                            int len = q2 - q1 - 1;
                            if (len > 15) len = 15;
                            strncpy(m->target, q1 + 1, len);
                        }
                    }
                }

                /* Parse param */
                const char *param_pos = strstr(obj_start, "\"param\"");
                if (param_pos && param_pos < obj_end) {
                    const char *q1 = strchr(strchr(param_pos, ':'), '"');
                    if (q1 && q1 < obj_end) {
                        const char *q2 = strchr(q1 + 1, '"');
                        if (q2 && q2 < obj_end) {
                            int len = q2 - q1 - 1;
                            if (len > 31) len = 31;
                            strncpy(m->param, q1 + 1, len);
                        }
                    }
                }

                /* Parse saved value if present */
                m->current_value = -999999.0f;  /* Sentinel: no saved value */
                const char *val_pos = strstr(obj_start, "\"value\"");
                if (val_pos && val_pos < obj_end) {
                    const char *colon = strchr(val_pos, ':');
                    if (colon) m->current_value = strtof(colon + 1, NULL);
                }

                if (m->cc >= KNOB_CC_START && m->cc <= KNOB_CC_END && m->param[0]) {
                    patch->knob_mapping_count++;
                }

                obj_start = obj_end;
            }
        }
    }

    /* Parse receive_channel and forward_channel (top-level in chain content) */
    json_get_int(json, "receive_channel", &patch->receive_channel);
    json_get_int(json, "forward_channel", &patch->forward_channel);

    free(json);
    return 0;
}

/* V2 load patch */
static int v2_load_patch(chain_instance_t *inst, int patch_idx) {
    char msg[256];

    if (!inst || patch_idx < 0 || patch_idx >= inst->patch_count) {
        return -1;
    }

    patch_info_t *patch = &inst->patches[patch_idx];

    snprintf(msg, sizeof(msg), "Loading patch: %s (synth=%s, %d FX)",
             patch->name, patch->synth_module, patch->audio_fx_count);
    v2_chain_log(inst, msg);

    /* Unload existing */
    v2_synth_panic(inst);
    v2_unload_all_midi_fx(inst);
    v2_unload_all_audio_fx(inst);
    v2_unload_synth(inst);

    /* Load synth */
    if (patch->synth_module[0]) {
        if (v2_load_synth(inst, patch->synth_module) != 0) {
            snprintf(msg, sizeof(msg), "Failed to load synth: %s", patch->synth_module);
            v2_chain_log(inst, msg);
            return -1;
        }

        /* Set preset first (state may override params set by preset) */
        char preset_str[16];
        snprintf(preset_str, sizeof(preset_str), "%d", patch->synth_preset);
        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->set_param) {
            inst->synth_plugin_v2->set_param(inst->synth_instance, "preset", preset_str);
        } else if (inst->synth_plugin && inst->synth_plugin->set_param) {
            inst->synth_plugin->set_param("preset", preset_str);
        }

        /* Reset mod wheel (CC 1) to 0 BEFORE state restore.
         * This clears stale mod wheel values from Move's track state,
         * but must happen before state restore so saved param values
         * (e.g. Braids FM which is mapped to CC 1) are not clobbered. */
        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->on_midi) {
            for (int ch = 0; ch < 16; ch++) {
                uint8_t mod_reset[3] = {(uint8_t)(0xB0 | ch), 1, 0};
                inst->synth_plugin_v2->on_midi(inst->synth_instance, mod_reset, 3, MOVE_MIDI_SOURCE_HOST);
            }
        } else if (inst->synth_plugin && inst->synth_plugin->on_midi) {
            for (int ch = 0; ch < 16; ch++) {
                uint8_t mod_reset[3] = {(uint8_t)(0xB0 | ch), 1, 0};
                inst->synth_plugin->on_midi(mod_reset, 3, MOVE_MIDI_SOURCE_HOST);
            }
        }

        /* Apply saved state if present */
        if (patch->synth_state[0] && inst->synth_plugin_v2 && inst->synth_instance) {
            snprintf(msg, sizeof(msg), "Applying synth state: %.50s...", patch->synth_state);
            v2_chain_log(inst, msg);
            inst->synth_plugin_v2->set_param(inst->synth_instance, "state", patch->synth_state);
        }
    }

    /* Load audio FX */
    for (int i = 0; i < patch->audio_fx_count; i++) {
        audio_fx_config_t *cfg = &patch->audio_fx[i];
        {
            char dbg[256];
            snprintf(dbg, sizeof(dbg), "[load] Loading audio_fx[%d]: module='%s' param_count=%d",
                     i, cfg->module, cfg->param_count);
            parse_debug_log(dbg);
        }
        if (v2_load_audio_fx(inst, cfg->module) != 0) {
            snprintf(msg, sizeof(msg), "Failed to load FX: %s", cfg->module);
            v2_chain_log(inst, msg);
            parse_debug_log("[load] FX load failed!");
            /* Continue loading other FX */
            continue;
        }

        /* Apply audio FX parameters (e.g., plugin_id for CLAP) */
        int fx_idx = inst->fx_count - 1;  /* Just loaded */
        {
            char dbg[256];
            snprintf(dbg, sizeof(dbg), "[load] FX loaded, fx_idx=%d is_v2=%d plugins_v2=%p instances=%p",
                     fx_idx, inst->fx_is_v2[fx_idx], (void*)inst->fx_plugins_v2[fx_idx],
                     (void*)inst->fx_instances[fx_idx]);
            parse_debug_log(dbg);
        }
        if (fx_idx >= 0 && fx_idx < MAX_AUDIO_FX && cfg->param_count > 0) {
            if (inst->fx_is_v2[fx_idx] && inst->fx_plugins_v2[fx_idx] && inst->fx_instances[fx_idx]) {
                for (int p = 0; p < cfg->param_count; p++) {
                    snprintf(msg, sizeof(msg), "Setting FX%d param: %s = %s",
                             fx_idx + 1, cfg->params[p].key, cfg->params[p].val);
                    v2_chain_log(inst, msg);
                    parse_debug_log(msg);
                    inst->fx_plugins_v2[fx_idx]->set_param(inst->fx_instances[fx_idx],
                                                           cfg->params[p].key, cfg->params[p].val);
                }
            } else if (inst->fx_plugins[fx_idx] && inst->fx_plugins[fx_idx]->set_param) {
                parse_debug_log("[load] Using v1 API for params");
                for (int p = 0; p < cfg->param_count; p++) {
                    inst->fx_plugins[fx_idx]->set_param(cfg->params[p].key, cfg->params[p].val);
                }
            } else {
                parse_debug_log("[load] No API available for setting params!");
            }
        } else {
            char dbg[128];
            snprintf(dbg, sizeof(dbg), "[load] Skipping params: fx_idx=%d param_count=%d", fx_idx, cfg->param_count);
            parse_debug_log(dbg);
        }

        /* Apply saved audio FX state if present */
        if (cfg->state[0] && fx_idx >= 0 && fx_idx < MAX_AUDIO_FX) {
            if (inst->fx_is_v2[fx_idx] && inst->fx_plugins_v2[fx_idx] && inst->fx_instances[fx_idx]) {
                snprintf(msg, sizeof(msg), "Applying FX%d state: %.50s...", fx_idx + 1, cfg->state);
                v2_chain_log(inst, msg);
                inst->fx_plugins_v2[fx_idx]->set_param(inst->fx_instances[fx_idx], "state", cfg->state);
            } else if (inst->fx_plugins[fx_idx] && inst->fx_plugins[fx_idx]->set_param) {
                inst->fx_plugins[fx_idx]->set_param("state", cfg->state);
            }
        }
    }

    /* Load MIDI FX */
    for (int i = 0; i < patch->midi_fx_count; i++) {
        midi_fx_config_t *cfg = &patch->midi_fx[i];
        if (v2_load_midi_fx(inst, cfg->module) != 0) {
            snprintf(msg, sizeof(msg), "Failed to load MIDI FX: %s", cfg->module);
            v2_chain_log(inst, msg);
            continue;  /* Skip params if load failed */
        }

        /* Apply MIDI FX parameters */
        int fx_idx = inst->midi_fx_count - 1;  /* Just loaded */
        if (fx_idx >= 0 && fx_idx < MAX_MIDI_FX && inst->midi_fx_plugins[fx_idx]) {
            midi_fx_api_v1_t *api = inst->midi_fx_plugins[fx_idx];
            void *instance = inst->midi_fx_instances[fx_idx];
            if (api->set_param && instance) {
                for (int p = 0; p < cfg->param_count; p++) {
                    snprintf(msg, sizeof(msg), "Setting MIDI FX param: %s = %s",
                             cfg->params[p].key, cfg->params[p].val);
                    v2_chain_log(inst, msg);
                    api->set_param(instance, cfg->params[p].key, cfg->params[p].val);
                }

                /* Apply saved MIDI FX state if present */
                if (cfg->state[0]) {
                    snprintf(msg, sizeof(msg), "Applying MIDI FX state: %.50s...", cfg->state);
                    v2_chain_log(inst, msg);
                    api->set_param(instance, "state", cfg->state);
                }
            }
        }
    }

    /* Copy knob mappings to instance and initialize current values */
    inst->knob_mapping_count = patch->knob_mapping_count;
    memcpy(inst->knob_mappings, patch->knob_mappings,
           sizeof(knob_mapping_t) * patch->knob_mapping_count);

    /* Sync knob current_value from actual DSP state.
     * The saved value may be stale if params were changed via module UI,
     * so always read the real value from the plugin after state restore. */
    for (int i = 0; i < inst->knob_mapping_count; i++) {
        const char *target = inst->knob_mappings[i].target;
        const char *param = inst->knob_mappings[i].param;

        char val_buf[64];
        int got = -1;
        if (strcmp(target, "synth") == 0 && inst->synth_plugin_v2 && inst->synth_instance) {
            got = inst->synth_plugin_v2->get_param(inst->synth_instance, param, val_buf, sizeof(val_buf));
        } else if (strcmp(target, "fx1") == 0 && inst->fx_count > 0 &&
                   inst->fx_is_v2[0] && inst->fx_plugins_v2[0] && inst->fx_instances[0]) {
            got = inst->fx_plugins_v2[0]->get_param(inst->fx_instances[0], param, val_buf, sizeof(val_buf));
        } else if (strcmp(target, "fx2") == 0 && inst->fx_count > 1 &&
                   inst->fx_is_v2[1] && inst->fx_plugins_v2[1] && inst->fx_instances[1]) {
            got = inst->fx_plugins_v2[1]->get_param(inst->fx_instances[1], param, val_buf, sizeof(val_buf));
        } else if (strcmp(target, "midi_fx1") == 0 && inst->midi_fx_count > 0 &&
                   inst->midi_fx_plugins[0] && inst->midi_fx_instances[0]) {
            got = inst->midi_fx_plugins[0]->get_param(inst->midi_fx_instances[0], param, val_buf, sizeof(val_buf));
        } else if (strcmp(target, "midi_fx2") == 0 && inst->midi_fx_count > 1 &&
                   inst->midi_fx_plugins[1] && inst->midi_fx_instances[1]) {
            got = inst->midi_fx_plugins[1]->get_param(inst->midi_fx_instances[1], param, val_buf, sizeof(val_buf));
        }

        chain_param_info_t *pinfo = find_param_by_key(inst, target, param);
        if (got > 0) {
            inst->knob_mappings[i].current_value = dsp_value_to_float(
                val_buf, pinfo, pinfo ? (pinfo->min_val + pinfo->max_val) / 2.0f : 0.5f);
        } else if (pinfo) {
            /* No DSP read — use saved value or midpoint */
            float saved = patch->knob_mappings[i].current_value;
            if (saved > -999998.0f) {
                if (saved < pinfo->min_val) saved = pinfo->min_val;
                if (saved > pinfo->max_val) saved = pinfo->max_val;
                inst->knob_mappings[i].current_value = saved;
            } else {
                inst->knob_mappings[i].current_value = (pinfo->min_val + pinfo->max_val) / 2.0f;
            }
        } else {
            inst->knob_mappings[i].current_value = 0.5f;
        }
    }

    /* Copy other settings */
    inst->midi_input = patch->midi_input;

    inst->current_patch = patch_idx;
    inst->mute_countdown = MUTE_BLOCKS_AFTER_SWITCH;

    snprintf(msg, sizeof(msg), "Patch loaded: %s", patch->name);
    v2_chain_log(inst, msg);

    return 0;
}

/* ==========================================================================
 * Per-instance MIDI FX helpers (for V2 API)
 * ========================================================================== */

/* Send a note to synth with optional transposition (for chords) */
static void inst_send_note_to_synth(chain_instance_t *inst, const uint8_t *msg, int len, int source, int interval) {
    if (!inst || len < 3) return;

    uint8_t out_msg[3] = { msg[0], msg[1], msg[2] };

    if (interval != 0) {
        int transposed = (int)msg[1] + interval;
        if (transposed < 0 || transposed > 127) return;  /* Out of range */
        out_msg[1] = (uint8_t)transposed;
    }

    if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->on_midi) {
        inst->synth_plugin_v2->on_midi(inst->synth_instance, out_msg, len, source);
    } else if (inst->synth_plugin && inst->synth_plugin->on_midi) {
        inst->synth_plugin->on_midi(out_msg, len, source);
    }
}

/* V2 on_midi handler */
static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst || len < 1) return;

    /* FX broadcast: forward only to audio FX with on_midi (e.g. ducker).
     * Skip synth, MIDI FX, and knob handling - this MIDI is from a
     * different channel than the slot's target. */
    if (source == MOVE_MIDI_SOURCE_FX_BROADCAST) {
        for (int f = 0; f < inst->fx_count; f++) {
            if (inst->fx_on_midi[f] && inst->fx_instances[f]) {
                inst->fx_on_midi[f](inst->fx_instances[f], msg, len, source);
            }
        }
        return;
    }

    /* Handle knob CC mappings */
    if (len >= 3 && (msg[0] & 0xF0) == 0xB0) {
        uint8_t cc = msg[1];
        if (cc >= KNOB_CC_START && cc <= KNOB_CC_END) {
            for (int i = 0; i < inst->knob_mapping_count; i++) {
                if (inst->knob_mappings[i].cc == cc) {
                    /* Look up parameter metadata dynamically */
                    const char *target = inst->knob_mappings[i].target;
                    const char *param = inst->knob_mappings[i].param;
                    chain_param_info_t *pinfo = NULL;

                    if (strcmp(target, "synth") == 0) {
                        pinfo = find_param_info(inst->synth_params, inst->synth_param_count, param);
                    } else if (strcmp(target, "fx1") == 0 && inst->fx_count > 0) {
                        pinfo = find_param_info(inst->fx_params[0], inst->fx_param_counts[0], param);
                    } else if (strcmp(target, "fx2") == 0 && inst->fx_count > 1) {
                        pinfo = find_param_info(inst->fx_params[1], inst->fx_param_counts[1], param);
                    } else if (strcmp(target, "fx3") == 0 && inst->fx_count > 2) {
                        pinfo = find_param_info(inst->fx_params[2], inst->fx_param_counts[2], param);
                    } else if (strcmp(target, "midi_fx1") == 0 && inst->midi_fx_count > 0) {
                        pinfo = find_param_info(inst->midi_fx_params[0], inst->midi_fx_param_counts[0], param);
                    } else if (strcmp(target, "midi_fx2") == 0 && inst->midi_fx_count > 1) {
                        pinfo = find_param_info(inst->midi_fx_params[1], inst->midi_fx_param_counts[1], param);
                    }

                    if (!pinfo) continue;  /* Skip if param not found */

                    /* Calculate acceleration based on time between events */
                    uint64_t now = get_time_ms();
                    uint64_t last = inst->knob_last_time_ms[i];
                    inst->knob_last_time_ms[i] = now;
                    int accel = KNOB_ACCEL_MIN_MULT;
                    if (last > 0) {
                        uint64_t elapsed = now - last;
                        if (elapsed < KNOB_ACCEL_SLOW_MS) {
                            if (elapsed <= KNOB_ACCEL_FAST_MS) {
                                accel = KNOB_ACCEL_MAX_MULT;
                            } else {
                                float ratio = (float)(KNOB_ACCEL_SLOW_MS - elapsed) /
                                              (float)(KNOB_ACCEL_SLOW_MS - KNOB_ACCEL_FAST_MS);
                                accel = KNOB_ACCEL_MIN_MULT + (int)(ratio * (KNOB_ACCEL_MAX_MULT - KNOB_ACCEL_MIN_MULT));
                            }
                        }
                    }

                    /* Relative encoder: apply acceleration to base step */
                    int is_int = (pinfo->type == KNOB_TYPE_INT || pinfo->type == KNOB_TYPE_ENUM);
                    float base_step = (pinfo->step > 0) ? pinfo->step
                        : (is_int ? (float)KNOB_STEP_INT : KNOB_STEP_FLOAT);
                    float delta = 0.0f;
                    if (msg[2] == 1) {
                        delta = base_step * accel;
                    } else if (msg[2] == 127) {
                        delta = -base_step * accel;
                    } else {
                        return;  /* Ignore other values */
                    }

                    float new_val = inst->knob_mappings[i].current_value + delta;
                    if (new_val < pinfo->min_val) new_val = pinfo->min_val;
                    if (new_val > pinfo->max_val) new_val = pinfo->max_val;
                    if (is_int) new_val = (float)((int)new_val);  /* Round to int */
                    inst->knob_mappings[i].current_value = new_val;

                    /* Format as int or float */
                    char val_str[16];
                    if (is_int) {
                        snprintf(val_str, sizeof(val_str), "%d", (int)new_val);
                    } else {
                        snprintf(val_str, sizeof(val_str), "%.3f", new_val);
                    }

                    if (strcmp(target, "synth") == 0) {
                        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->set_param) {
                            inst->synth_plugin_v2->set_param(inst->synth_instance, param, val_str);
                        } else if (inst->synth_plugin && inst->synth_plugin->set_param) {
                            inst->synth_plugin->set_param(param, val_str);
                        }
                    } else if (strcmp(target, "fx1") == 0 && inst->fx_count > 0) {
                        if (inst->fx_is_v2[0] && inst->fx_plugins_v2[0] && inst->fx_instances[0]) {
                            inst->fx_plugins_v2[0]->set_param(inst->fx_instances[0], param, val_str);
                        } else if (inst->fx_plugins[0] && inst->fx_plugins[0]->set_param) {
                            inst->fx_plugins[0]->set_param(param, val_str);
                        }
                    } else if (strcmp(target, "fx2") == 0 && inst->fx_count > 1) {
                        if (inst->fx_is_v2[1] && inst->fx_plugins_v2[1] && inst->fx_instances[1]) {
                            inst->fx_plugins_v2[1]->set_param(inst->fx_instances[1], param, val_str);
                        } else if (inst->fx_plugins[1] && inst->fx_plugins[1]->set_param) {
                            inst->fx_plugins[1]->set_param(param, val_str);
                        }
                    } else if (strcmp(target, "fx3") == 0 && inst->fx_count > 2) {
                        if (inst->fx_is_v2[2] && inst->fx_plugins_v2[2] && inst->fx_instances[2]) {
                            inst->fx_plugins_v2[2]->set_param(inst->fx_instances[2], param, val_str);
                        } else if (inst->fx_plugins[2] && inst->fx_plugins[2]->set_param) {
                            inst->fx_plugins[2]->set_param(param, val_str);
                        }
                    }
                    return;
                }
            }
        }
    }

    /* Process through MIDI FX modules (if any loaded) */
    uint8_t out_msgs[MIDI_FX_MAX_OUT_MSGS][3];
    int out_lens[MIDI_FX_MAX_OUT_MSGS];
    int out_count = v2_process_midi_fx(inst, msg, len, out_msgs, out_lens, MIDI_FX_MAX_OUT_MSGS);

    /* Send processed messages to synth */
    for (int i = 0; i < out_count; i++) {
        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->on_midi) {
            inst->synth_plugin_v2->on_midi(inst->synth_instance, out_msgs[i], out_lens[i], source);
        } else if (inst->synth_plugin && inst->synth_plugin->on_midi) {
            inst->synth_plugin->on_midi(out_msgs[i], out_lens[i], source);
        }
    }

    /* Forward MIDI to audio FX that have on_midi (e.g. ducker) */
    for (int f = 0; f < inst->fx_count; f++) {
        if (inst->fx_on_midi[f] && inst->fx_instances[f]) {
            for (int j = 0; j < out_count; j++) {
                inst->fx_on_midi[f](inst->fx_instances[f], out_msgs[j], out_lens[j], source);
            }
        }
    }
}

/* V2 set_param handler */
static void v2_set_param(void *instance, const char *key, const char *val) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst) return;

    {
        char dbg[256];
        snprintf(dbg, sizeof(dbg), "[v2_set_param] key='%s' val='%s'", key, val ? val : "null");
        parse_debug_log(dbg);
    }

    if (strcmp(key, "load_patch") == 0 || strcmp(key, "patch") == 0) {
        int idx = atoi(val);
        if (idx < 0) {
            /* Unload patch */
            v2_synth_panic(inst);
            v2_unload_all_midi_fx(inst);
            v2_unload_all_audio_fx(inst);
            v2_unload_synth(inst);
            inst->current_patch = -1;
        } else {
            v2_load_patch(inst, idx);
        }
    }
    else if (strcmp(key, "save_patch") == 0) {
        /* Save new patch to disk, then rescan this instance's patch list */
        v2_save_patch(inst, val);
        v2_scan_patches(inst);
    }
    else if (strcmp(key, "delete_patch") == 0) {
        int index = atoi(val);
        v2_delete_patch(inst, index);
        v2_scan_patches(inst);
    }
    else if (strcmp(key, "update_patch") == 0) {
        /* Format: "index:json_data" */
        const char *colon = strchr(val, ':');
        if (colon) {
            int index = atoi(val);
            v2_update_patch(inst, index, colon + 1);
            v2_scan_patches(inst);
        }
    }
    /* Master preset commands */
    else if (strcmp(key, "save_master_preset") == 0) {
        save_master_preset(val);
    }
    else if (strcmp(key, "delete_master_preset") == 0) {
        int index = atoi(val);
        delete_master_preset(index);
    }
    else if (strcmp(key, "update_master_preset") == 0) {
        /* Format: "index:json_data" */
        const char *colon = strchr(val, ':');
        if (colon) {
            int index = atoi(val);
            update_master_preset(index, colon + 1);
        }
    }
    else if (strncmp(key, "synth:", 6) == 0) {
        const char *subkey = key + 6;
        /* Intercept module change to swap synth dynamically */
        if (strcmp(subkey, "module") == 0) {
            inst->mute_countdown = MUTE_BLOCKS_AFTER_SWITCH;
            v2_synth_panic(inst);
            v2_unload_synth(inst);
            smoother_reset(&inst->synth_smoother);  /* Reset smoother on module change */
            if (val && val[0] != '\0' && strcmp(val, "none") != 0) {
                v2_load_synth(inst, val);
            } else {
                /* Clearing synth - also clear knob mappings */
                inst->knob_mapping_count = 0;
            }
        } else {
            /* Check if this is a smoothable float param */
            float fval;
            if (is_smoothable_float(val, &fval)) {
                /* Store target for smoothing - will be applied in render_block */
                smoother_set_target(&inst->synth_smoother, subkey, fval);
            }
            /* Always forward immediately (smoother will override with interpolated values) */
            if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->set_param) {
                inst->synth_plugin_v2->set_param(inst->synth_instance, subkey, val);
            } else if (inst->synth_plugin && inst->synth_plugin->set_param) {
                inst->synth_plugin->set_param(subkey, val);
            }
        }
    }
    else if (strncmp(key, "fx1:", 4) == 0) {
        const char *subkey = key + 4;
        /* Intercept module change to swap FX1 dynamically */
        if (strcmp(subkey, "module") == 0) {
            inst->mute_countdown = MUTE_BLOCKS_AFTER_SWITCH;
            v2_load_audio_fx_slot(inst, 0, val);
            smoother_reset(&inst->fx_smoothers[0]);  /* Reset smoother on module change */
        } else if (inst->fx_count > 0) {
            float fval;
            if (is_smoothable_float(val, &fval)) {
                smoother_set_target(&inst->fx_smoothers[0], subkey, fval);
            }
            if (inst->fx_is_v2[0] && inst->fx_plugins_v2[0] && inst->fx_instances[0]) {
                inst->fx_plugins_v2[0]->set_param(inst->fx_instances[0], subkey, val);
            } else if (inst->fx_plugins[0] && inst->fx_plugins[0]->set_param) {
                inst->fx_plugins[0]->set_param(subkey, val);
            }
        }
    }
    else if (strncmp(key, "fx2:", 4) == 0) {
        const char *subkey = key + 4;
        /* Intercept module change to swap FX2 dynamically */
        if (strcmp(subkey, "module") == 0) {
            inst->mute_countdown = MUTE_BLOCKS_AFTER_SWITCH;
            v2_load_audio_fx_slot(inst, 1, val);
            smoother_reset(&inst->fx_smoothers[1]);  /* Reset smoother on module change */
        } else if (inst->fx_count > 1) {
            float fval;
            if (is_smoothable_float(val, &fval)) {
                smoother_set_target(&inst->fx_smoothers[1], subkey, fval);
            }
            if (inst->fx_is_v2[1] && inst->fx_plugins_v2[1] && inst->fx_instances[1]) {
                inst->fx_plugins_v2[1]->set_param(inst->fx_instances[1], subkey, val);
            } else if (inst->fx_plugins[1] && inst->fx_plugins[1]->set_param) {
                inst->fx_plugins[1]->set_param(subkey, val);
            }
        }
    }
    else if (strncmp(key, "midi_fx1:", 9) == 0) {
        const char *subkey = key + 9;
        /* Intercept module change to swap MIDI FX1 dynamically */
        if (strcmp(subkey, "module") == 0) {
            /* Unload existing MIDI FX if any */
            if (inst->midi_fx_count > 0) {
                v2_unload_all_midi_fx(inst);
            }
            if (val && val[0] != '\0' && strcmp(val, "none") != 0) {
                v2_load_midi_fx(inst, val);
            }
        } else if (inst->midi_fx_count > 0 && inst->midi_fx_plugins[0] && inst->midi_fx_instances[0]) {
            inst->midi_fx_plugins[0]->set_param(inst->midi_fx_instances[0], subkey, val);
        }
    }
    else if (strncmp(key, "midi_fx2:", 9) == 0) {
        const char *subkey = key + 9;
        /* Intercept module change to swap MIDI FX2 dynamically */
        if (strcmp(subkey, "module") == 0) {
            /* For slot 2, we'd need to unload just slot 2 - simplified for now */
            if (val && val[0] != '\0' && strcmp(val, "none") != 0) {
                v2_load_midi_fx(inst, val);
            }
        } else if (inst->midi_fx_count > 1 && inst->midi_fx_plugins[1] && inst->midi_fx_instances[1]) {
            inst->midi_fx_plugins[1]->set_param(inst->midi_fx_instances[1], subkey, val);
        }
    }
    /* Knob mapping set: knob_N_set with value "target:param" */
    else if (strncmp(key, "knob_", 5) == 0) {
        int knob_num;
        char action[32];
        if (sscanf(key + 5, "%d_%31s", &knob_num, action) == 2 && knob_num >= 1 && knob_num <= 8) {
            int cc = 70 + knob_num;  /* CC 71-78 for knobs 1-8 */

            if (strcmp(action, "set") == 0 && val) {
                /* Parse "target:param" format */
                char target[32] = "";
                char param[64] = "";
                const char *colon = strchr(val, ':');
                if (colon) {
                    int tlen = colon - val;
                    if (tlen > 0 && tlen < 32) {
                        strncpy(target, val, tlen);
                        target[tlen] = '\0';
                    }
                    strncpy(param, colon + 1, 63);
                    param[63] = '\0';
                }

                /* Find or add mapping for this CC */
                int found = -1;
                for (int i = 0; i < inst->knob_mapping_count; i++) {
                    if (inst->knob_mappings[i].cc == cc) {
                        found = i;
                        break;
                    }
                }

                if (target[0] && param[0]) {
                    /* Look up param info from the target's chain_params */
                    chain_param_info_t *pinfo = NULL;
                    if (strcmp(target, "synth") == 0) {
                        pinfo = find_param_info(inst->synth_params, inst->synth_param_count, param);
                    } else if (strcmp(target, "fx1") == 0 && inst->fx_count > 0) {
                        pinfo = find_param_info(inst->fx_params[0], inst->fx_param_counts[0], param);
                    } else if (strcmp(target, "fx2") == 0 && inst->fx_count > 1) {
                        pinfo = find_param_info(inst->fx_params[1], inst->fx_param_counts[1], param);
                    } else if (strcmp(target, "fx3") == 0 && inst->fx_count > 2) {
                        pinfo = find_param_info(inst->fx_params[2], inst->fx_param_counts[2], param);
                    } else if (strcmp(target, "midi_fx1") == 0 && inst->midi_fx_count > 0) {
                        pinfo = find_param_info(inst->midi_fx_params[0], inst->midi_fx_param_counts[0], param);
                    } else if (strcmp(target, "midi_fx2") == 0 && inst->midi_fx_count > 1) {
                        pinfo = find_param_info(inst->midi_fx_params[1], inst->midi_fx_param_counts[1], param);
                    }

                    /* Set mapping */
                    if (found >= 0) {
                        /* Update existing (type/min/max looked up dynamically from pinfo) */
                        strncpy(inst->knob_mappings[found].target, target, sizeof(inst->knob_mappings[found].target) - 1);
                        inst->knob_mappings[found].target[sizeof(inst->knob_mappings[found].target) - 1] = '\0';
                        strncpy(inst->knob_mappings[found].param, param, sizeof(inst->knob_mappings[found].param) - 1);
                        inst->knob_mappings[found].param[sizeof(inst->knob_mappings[found].param) - 1] = '\0';
                    } else if (inst->knob_mapping_count < MAX_KNOB_MAPPINGS) {
                        /* Add new */
                        int i = inst->knob_mapping_count++;
                        inst->knob_mappings[i].cc = cc;
                        strncpy(inst->knob_mappings[i].target, target, sizeof(inst->knob_mappings[i].target) - 1);
                        inst->knob_mappings[i].target[sizeof(inst->knob_mappings[i].target) - 1] = '\0';
                        strncpy(inst->knob_mappings[i].param, param, sizeof(inst->knob_mappings[i].param) - 1);
                        inst->knob_mappings[i].param[sizeof(inst->knob_mappings[i].param) - 1] = '\0';
                        if (pinfo) {
                            inst->knob_mappings[i].current_value = pinfo->default_val;
                        } else {
                            inst->knob_mappings[i].current_value = 0.5f;
                        }
                    }
                }
            }
            else if (strcmp(action, "clear") == 0) {
                /* Remove mapping for this CC */
                for (int i = 0; i < inst->knob_mapping_count; i++) {
                    if (inst->knob_mappings[i].cc == cc) {
                        /* Shift remaining mappings down */
                        for (int j = i; j < inst->knob_mapping_count - 1; j++) {
                            inst->knob_mappings[j] = inst->knob_mappings[j + 1];
                        }
                        inst->knob_mapping_count--;
                        break;
                    }
                }
            }
            else if (strcmp(action, "adjust") == 0 && val) {
                /* Adjust knob value by delta - used by Shift+Knob in Move mode */
                int delta_int = atoi(val);  /* +N or -N */
                if (delta_int == 0) return;  /* No change */

                /* Find mapping for this CC */
                for (int i = 0; i < inst->knob_mapping_count; i++) {
                    if (inst->knob_mappings[i].cc == cc) {
                        /* Look up parameter metadata */
                        const char *target = inst->knob_mappings[i].target;
                        const char *param = inst->knob_mappings[i].param;
                        chain_param_info_t *pinfo = NULL;

                        if (strcmp(target, "synth") == 0) {
                            pinfo = find_param_info(inst->synth_params, inst->synth_param_count, param);
                        } else if (strcmp(target, "fx1") == 0 && inst->fx_count > 0) {
                            pinfo = find_param_info(inst->fx_params[0], inst->fx_param_counts[0], param);
                        } else if (strcmp(target, "fx2") == 0 && inst->fx_count > 1) {
                            pinfo = find_param_info(inst->fx_params[1], inst->fx_param_counts[1], param);
                        } else if (strcmp(target, "fx3") == 0 && inst->fx_count > 2) {
                            pinfo = find_param_info(inst->fx_params[2], inst->fx_param_counts[2], param);
                        } else if (strcmp(target, "midi_fx1") == 0 && inst->midi_fx_count > 0) {
                            pinfo = find_param_info(inst->midi_fx_params[0], inst->midi_fx_param_counts[0], param);
                        } else if (strcmp(target, "midi_fx2") == 0 && inst->midi_fx_count > 1) {
                            pinfo = find_param_info(inst->midi_fx_params[1], inst->midi_fx_param_counts[1], param);
                        }

                        if (!pinfo) continue;  /* Skip if param not found */

                        /* Calculate acceleration based on time between events */
                        uint64_t now = get_time_ms();
                        uint64_t last = inst->knob_last_time_ms[i];
                        inst->knob_last_time_ms[i] = now;
                        int accel = KNOB_ACCEL_MIN_MULT;
                        if (last > 0) {
                            uint64_t elapsed = now - last;
                            if (elapsed < 50) accel = KNOB_ACCEL_MAX_MULT;
                            else if (elapsed < 100) accel = 4;
                            else if (elapsed < 200) accel = 2;
                        }

                        /* Calculate step based on type, with acceleration */
                        int is_int = (pinfo->type == KNOB_TYPE_INT || pinfo->type == KNOB_TYPE_ENUM);
                        /* Cap acceleration for ints to avoid jumping too far */
                        if (is_int && accel > KNOB_ACCEL_MAX_MULT_INT) {
                            accel = KNOB_ACCEL_MAX_MULT_INT;
                        }
                        /* Use step from param metadata, or 0.01 default for shadow adjust */
                        float base_step = (pinfo->step > 0) ? pinfo->step
                            : (is_int ? (float)KNOB_STEP_INT : 0.01f);
                        float delta = base_step * accel * (delta_int > 0 ? 1 : -1);

                        /* Apply delta with clamping */
                        float new_val = inst->knob_mappings[i].current_value + delta;
                        if (new_val < pinfo->min_val) new_val = pinfo->min_val;
                        if (new_val > pinfo->max_val) new_val = pinfo->max_val;
                        if (is_int) new_val = (float)((int)new_val);  /* Round to int */
                        inst->knob_mappings[i].current_value = new_val;

                        /* Format value as string */
                        char val_str[16];
                        if (is_int) {
                            snprintf(val_str, sizeof(val_str), "%d", (int)new_val);
                        } else {
                            snprintf(val_str, sizeof(val_str), "%.3f", new_val);
                        }

                        if (strcmp(target, "synth") == 0) {
                            if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->set_param) {
                                inst->synth_plugin_v2->set_param(inst->synth_instance, param, val_str);
                            } else if (inst->synth_plugin && inst->synth_plugin->set_param) {
                                inst->synth_plugin->set_param(param, val_str);
                            }
                        } else if (strcmp(target, "fx1") == 0 && inst->fx_count > 0) {
                            if (inst->fx_is_v2[0] && inst->fx_plugins_v2[0] && inst->fx_instances[0]) {
                                inst->fx_plugins_v2[0]->set_param(inst->fx_instances[0], param, val_str);
                            } else if (inst->fx_plugins[0] && inst->fx_plugins[0]->set_param) {
                                inst->fx_plugins[0]->set_param(param, val_str);
                            }
                        } else if (strcmp(target, "fx2") == 0 && inst->fx_count > 1) {
                            if (inst->fx_is_v2[1] && inst->fx_plugins_v2[1] && inst->fx_instances[1]) {
                                inst->fx_plugins_v2[1]->set_param(inst->fx_instances[1], param, val_str);
                            } else if (inst->fx_plugins[1] && inst->fx_plugins[1]->set_param) {
                                inst->fx_plugins[1]->set_param(param, val_str);
                            }
                        }
                        break;
                    }
                }
            }
            return;
        }
    }
    /* Forward to synth by default */
    else if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->set_param) {
        inst->synth_plugin_v2->set_param(inst->synth_instance, key, val);
    } else if (inst->synth_plugin && inst->synth_plugin->set_param) {
        inst->synth_plugin->set_param(key, val);
    }
}

/*
 * Convert a DSP get_param return string to a float value.
 * Handles numeric strings directly. For non-numeric strings (enum labels),
 * looks up the index in the param's options list.
 * Returns the float value, or fallback if conversion fails.
 */
static float dsp_value_to_float(const char *val_str, chain_param_info_t *pinfo, float fallback) {
    char *endptr;
    float v = strtof(val_str, &endptr);
    if (endptr != val_str) {
        return v;  /* Parsed a number */
    }
    /* Non-numeric — try enum option lookup */
    if (pinfo && pinfo->type == KNOB_TYPE_ENUM) {
        for (int j = 0; j < pinfo->option_count; j++) {
            if (strcmp(val_str, pinfo->options[j]) == 0) {
                return (float)j;
            }
        }
    }
    return fallback;
}

/*
 * Find parameter metadata by target and key.
 */
static chain_param_info_t* find_param_by_key(chain_instance_t *inst, const char *target, const char *key) {
    if (strcmp(target, "synth") == 0) {
        for (int i = 0; i < inst->synth_param_count; i++) {
            if (strcmp(inst->synth_params[i].key, key) == 0) {
                return &inst->synth_params[i];
            }
        }
    } else if (strncmp(target, "fx", 2) == 0) {
        int fx_slot = atoi(target + 2) - 1;
        if (fx_slot >= 0 && fx_slot < MAX_AUDIO_FX) {
            for (int i = 0; i < inst->fx_param_counts[fx_slot]; i++) {
                if (strcmp(inst->fx_params[fx_slot][i].key, key) == 0) {
                    return &inst->fx_params[fx_slot][i];
                }
            }
        }
    } else if (strncmp(target, "midi_fx", 7) == 0) {
        int midi_fx_slot = 0;  /* Default to slot 0 */
        if (target[7] != '\0') {
            /* Parse midi_fx1, midi_fx2, etc. */
            midi_fx_slot = atoi(target + 7) - 1;
        }
        if (midi_fx_slot >= 0 && midi_fx_slot < MAX_MIDI_FX) {
            for (int i = 0; i < inst->midi_fx_param_counts[midi_fx_slot]; i++) {
                if (strcmp(inst->midi_fx_params[midi_fx_slot][i].key, key) == 0) {
                    return &inst->midi_fx_params[midi_fx_slot][i];
                }
            }
        }
    }
    return NULL;
}

/* V2 get_param handler */
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst) return -1;

    if (strcmp(key, "patch_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->patch_count);
    }
    if (strcmp(key, "current_patch") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_patch);
    }
    if (strcmp(key, "patch:receive_channel") == 0) {
        if (inst->current_patch >= 0 && inst->current_patch < inst->patch_count) {
            return snprintf(buf, buf_len, "%d", inst->patches[inst->current_patch].receive_channel);
        }
        return snprintf(buf, buf_len, "0");
    }
    if (strcmp(key, "patch:forward_channel") == 0) {
        if (inst->current_patch >= 0 && inst->current_patch < inst->patch_count) {
            return snprintf(buf, buf_len, "%d", inst->patches[inst->current_patch].forward_channel);
        }
        return snprintf(buf, buf_len, "0");
    }
    if (strncmp(key, "patch_name_", 11) == 0) {
        int idx = atoi(key + 11);
        if (idx >= 0 && idx < inst->patch_count) {
            return snprintf(buf, buf_len, "%s", inst->patches[idx].name);
        }
        return -1;
    }
    if (strncmp(key, "patch_path_", 11) == 0) {
        int idx = atoi(key + 11);
        if (idx >= 0 && idx < inst->patch_count) {
            return snprintf(buf, buf_len, "%s", inst->patches[idx].path);
        }
        return -1;
    }
    if (strcmp(key, "synth_module") == 0) {
        return snprintf(buf, buf_len, "%s", inst->current_synth_module);
    }
    if (strcmp(key, "synth_error") == 0) {
        return v2_synth_get_error(inst, buf, buf_len);
    }
    if (strcmp(key, "fx1_module") == 0) {
        return snprintf(buf, buf_len, "%s", inst->current_fx_modules[0]);
    }
    if (strcmp(key, "fx2_module") == 0) {
        return snprintf(buf, buf_len, "%s", inst->current_fx_modules[1]);
    }
    if (strcmp(key, "midi_fx_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->midi_fx_count);
    }
    if (strcmp(key, "midi_fx1_module") == 0) {
        return snprintf(buf, buf_len, "%s", inst->current_midi_fx_modules[0]);
    }
    if (strcmp(key, "midi_fx2_module") == 0) {
        return snprintf(buf, buf_len, "%s", inst->current_midi_fx_modules[1]);
    }
    /* Master preset queries */
    if (strcmp(key, "master_preset_count") == 0) {
        scan_master_presets();
        return snprintf(buf, buf_len, "%d", master_preset_count);
    }
    if (strncmp(key, "master_preset_name_", 19) == 0) {
        int idx = atoi(key + 19);
        if (idx >= 0 && idx < master_preset_count) {
            return snprintf(buf, buf_len, "%s", master_preset_names[idx]);
        }
        return -1;
    }
    if (strncmp(key, "master_preset_json_", 19) == 0) {
        int idx = atoi(key + 19);
        return load_master_preset_json(idx, buf, buf_len);
    }
    if (strcmp(key, "fx_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->fx_count);
    }

    /* Knob mapping info */
    if (strcmp(key, "knob_mappings") == 0) {
        /* Return full knob mappings array as JSON for patch saving.
         * Read ACTUAL current values from DSP plugins, not the knob
         * tracking value which may be stale if params were changed
         * via module UI or state restore. */
        int off = 0;
        off += snprintf(buf + off, buf_len - off, "[");
        for (int i = 0; i < inst->knob_mapping_count && i < MAX_KNOB_MAPPINGS; i++) {
            const char *target = inst->knob_mappings[i].target;
            const char *param = inst->knob_mappings[i].param;
            float value = inst->knob_mappings[i].current_value;

            /* Try to read actual value from DSP plugin */
            char val_buf[64];
            int got = -1;
            if (strcmp(target, "synth") == 0 && inst->synth_plugin_v2 && inst->synth_instance) {
                got = inst->synth_plugin_v2->get_param(inst->synth_instance, param, val_buf, sizeof(val_buf));
            } else if (strcmp(target, "fx1") == 0 && inst->fx_count > 0 &&
                       inst->fx_is_v2[0] && inst->fx_plugins_v2[0] && inst->fx_instances[0]) {
                got = inst->fx_plugins_v2[0]->get_param(inst->fx_instances[0], param, val_buf, sizeof(val_buf));
            } else if (strcmp(target, "fx2") == 0 && inst->fx_count > 1 &&
                       inst->fx_is_v2[1] && inst->fx_plugins_v2[1] && inst->fx_instances[1]) {
                got = inst->fx_plugins_v2[1]->get_param(inst->fx_instances[1], param, val_buf, sizeof(val_buf));
            } else if (strcmp(target, "midi_fx1") == 0 && inst->midi_fx_count > 0 &&
                       inst->midi_fx_plugins[0] && inst->midi_fx_instances[0]) {
                got = inst->midi_fx_plugins[0]->get_param(inst->midi_fx_instances[0], param, val_buf, sizeof(val_buf));
            }
            if (got > 0) {
                chain_param_info_t *pinfo = find_param_by_key(inst, target, param);
                value = dsp_value_to_float(val_buf, pinfo, value);
            }

            off += snprintf(buf + off, buf_len - off,
                "%s{\"cc\":%d,\"target\":\"%s\",\"param\":\"%s\",\"value\":%.3f}",
                (i > 0) ? "," : "",
                inst->knob_mappings[i].cc, target, param, value);
            if (off >= buf_len - 1) break;
        }
        off += snprintf(buf + off, buf_len - off, "]");
        return off;
    }
    if (strcmp(key, "knob_mapping_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->knob_mapping_count);
    }
    if (strncmp(key, "knob_", 5) == 0) {
        /* knob_N_param format (N is 1-8 for knobs, mapping to CC 71-78) */
        int knob_num;
        char query_param[32];
        if (sscanf(key + 5, "%d_%31s", &knob_num, query_param) == 2) {
            /* Find mapping for this knob (CC = 70 + knob_num) */
            int cc = 70 + knob_num;
            for (int i = 0; i < inst->knob_mapping_count; i++) {
                if (inst->knob_mappings[i].cc == cc) {
                    /* Look up param info for all queries */
                    const char *target = inst->knob_mappings[i].target;
                    const char *param = inst->knob_mappings[i].param;
                    chain_param_info_t *pinfo = NULL;

                    if (strcmp(target, "synth") == 0) {
                        pinfo = find_param_info(inst->synth_params, inst->synth_param_count, param);
                    } else if (strcmp(target, "fx1") == 0 && inst->fx_count > 0) {
                        pinfo = find_param_info(inst->fx_params[0], inst->fx_param_counts[0], param);
                    } else if (strcmp(target, "fx2") == 0 && inst->fx_count > 1) {
                        pinfo = find_param_info(inst->fx_params[1], inst->fx_param_counts[1], param);
                    } else if (strcmp(target, "fx3") == 0 && inst->fx_count > 2) {
                        pinfo = find_param_info(inst->fx_params[2], inst->fx_param_counts[2], param);
                    } else if (strcmp(target, "midi_fx1") == 0 && inst->midi_fx_count > 0) {
                        pinfo = find_param_info(inst->midi_fx_params[0], inst->midi_fx_param_counts[0], param);
                    } else if (strcmp(target, "midi_fx2") == 0 && inst->midi_fx_count > 1) {
                        pinfo = find_param_info(inst->midi_fx_params[1], inst->midi_fx_param_counts[1], param);
                    }

                    if (strcmp(query_param, "name") == 0) {
                        /* Construct display name from target and param */
                        return snprintf(buf, buf_len, "%s: %s", target, param);
                    }
                    else if (strcmp(query_param, "target") == 0) {
                        return snprintf(buf, buf_len, "%s", target);
                    }
                    else if (strcmp(query_param, "param") == 0) {
                        return snprintf(buf, buf_len, "%s", param);
                    }
                    else if (strcmp(query_param, "value") == 0) {
                        /* Look up param metadata */
                        chain_param_info_t *param_info = find_param_by_key(inst, target, param);
                        if (param_info) {
                            /* Use centralized formatting */
                            return format_param_value(param_info, inst->knob_mappings[i].current_value, buf, buf_len);
                        }
                        /* Fallback for params without metadata */
                        if (pinfo && pinfo->type == KNOB_TYPE_INT) {
                            return snprintf(buf, buf_len, "%d", (int)inst->knob_mappings[i].current_value);
                        } else {
                            return snprintf(buf, buf_len, "%.2f", inst->knob_mappings[i].current_value);
                        }
                    }
                    else if (strcmp(query_param, "min") == 0) {
                        return snprintf(buf, buf_len, "%.2f", pinfo ? pinfo->min_val : 0.0f);
                    }
                    else if (strcmp(query_param, "max") == 0) {
                        return snprintf(buf, buf_len, "%.2f", pinfo ? pinfo->max_val : 1.0f);
                    }
                    else if (strcmp(query_param, "type") == 0) {
                        if (pinfo) {
                            const char *type_str = (pinfo->type == KNOB_TYPE_INT) ? "int" :
                                                   (pinfo->type == KNOB_TYPE_ENUM) ? "enum" : "float";
                            return snprintf(buf, buf_len, "%s", type_str);
                        }
                        return snprintf(buf, buf_len, "float");  /* Fallback */
                    }
                    break;
                }
            }
        }
        return -1;  /* Knob not mapped */
    }

    /* Route synth: prefixed params to synth (strip prefix) */
    if (strncmp(key, "synth:", 6) == 0) {
        const char *subkey = key + 6;

        /* Return synth's default forward channel from module.json capabilities */
        if (strcmp(subkey, "default_forward_channel") == 0) {
            return snprintf(buf, buf_len, "%d", inst->synth_default_forward_channel);
        }

        /* For chain_params: try plugin first, fall back to parsed module.json data */
        if (strcmp(subkey, "chain_params") == 0) {
            /* Try plugin's own chain_params handler first */
            if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->get_param) {
                int result = inst->synth_plugin_v2->get_param(inst->synth_instance, subkey, buf, buf_len);
                if (result > 0) return result;  /* Plugin provided chain_params */
            } else if (inst->synth_plugin && inst->synth_plugin->get_param) {
                int result = inst->synth_plugin->get_param(subkey, buf, buf_len);
                if (result > 0) return result;
            }
            /* Fall back to parsed module.json data */
            if (inst->synth_param_count > 0) {
                int offset = 0;
                offset += snprintf(buf + offset, buf_len - offset, "[");
                for (int i = 0; i < inst->synth_param_count && offset < buf_len - 100; i++) {
                    chain_param_info_t *p = &inst->synth_params[i];
                    if (i > 0) offset += snprintf(buf + offset, buf_len - offset, ",");
                    const char *type_str = (p->type == KNOB_TYPE_INT) ? "int" :
                                          (p->type == KNOB_TYPE_ENUM) ? "enum" : "float";
                    offset += snprintf(buf + offset, buf_len - offset,
                        "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g",
                        p->key, p->name[0] ? p->name : p->key,
                        type_str,
                        p->min_val, p->max_val);
                    /* Add options array for enum types */
                    if (p->type == KNOB_TYPE_ENUM && p->option_count > 0) {
                        offset += snprintf(buf + offset, buf_len - offset, ",\"options\":[");
                        for (int j = 0; j < p->option_count && j < MAX_ENUM_OPTIONS; j++) {
                            if (j > 0) offset += snprintf(buf + offset, buf_len - offset, ",");
                            offset += snprintf(buf + offset, buf_len - offset, "\"%s\"", p->options[j]);
                        }
                        offset += snprintf(buf + offset, buf_len - offset, "]");
                    }
                    /* Add unit and display_format if present */
                    if (p->unit[0]) {
                        offset += snprintf(buf + offset, buf_len - offset, ",\"unit\":\"%s\"", p->unit);
                    }
                    if (p->display_format[0]) {
                        offset += snprintf(buf + offset, buf_len - offset, ",\"display_format\":\"%s\"", p->display_format);
                    }
                    offset += snprintf(buf + offset, buf_len - offset, "}");
                }
                offset += snprintf(buf + offset, buf_len - offset, "]");
                return offset;
            }
            return -1;  /* No chain_params available */
        }

        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->get_param) {
            return inst->synth_plugin_v2->get_param(inst->synth_instance, subkey, buf, buf_len);
        } else if (inst->synth_plugin && inst->synth_plugin->get_param) {
            return inst->synth_plugin->get_param(subkey, buf, buf_len);
        }
        return -1;
    }

    /* Route fx1: prefixed params to FX1 (strip prefix) */
    if (strncmp(key, "fx1:", 4) == 0) {
        const char *subkey = key + 4;

        /* For chain_params: try plugin first, fall back to parsed module.json data */
        if (strcmp(subkey, "chain_params") == 0 && inst->fx_count > 0) {
            /* Try plugin's own chain_params handler first */
            if (inst->fx_is_v2[0] && inst->fx_plugins_v2[0] && inst->fx_instances[0] && inst->fx_plugins_v2[0]->get_param) {
                int result = inst->fx_plugins_v2[0]->get_param(inst->fx_instances[0], subkey, buf, buf_len);
                if (result > 0) return result;
            } else if (inst->fx_plugins[0] && inst->fx_plugins[0]->get_param) {
                int result = inst->fx_plugins[0]->get_param(subkey, buf, buf_len);
                if (result > 0) return result;
            }
            /* Fall back to parsed module.json data */
            if (inst->fx_param_counts[0] > 0) {
                int offset = 0;
                offset += snprintf(buf + offset, buf_len - offset, "[");
                for (int i = 0; i < inst->fx_param_counts[0] && offset < buf_len - 100; i++) {
                    chain_param_info_t *p = &inst->fx_params[0][i];
                    if (i > 0) offset += snprintf(buf + offset, buf_len - offset, ",");
                    const char *type_str = (p->type == KNOB_TYPE_INT) ? "int" :
                                          (p->type == KNOB_TYPE_ENUM) ? "enum" : "float";
                    offset += snprintf(buf + offset, buf_len - offset,
                        "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g",
                        p->key, p->name[0] ? p->name : p->key,
                        type_str,
                        p->min_val, p->max_val);
                    /* Add options array for enum types */
                    if (p->type == KNOB_TYPE_ENUM && p->option_count > 0) {
                        offset += snprintf(buf + offset, buf_len - offset, ",\"options\":[");
                        for (int j = 0; j < p->option_count && j < MAX_ENUM_OPTIONS; j++) {
                            if (j > 0) offset += snprintf(buf + offset, buf_len - offset, ",");
                            offset += snprintf(buf + offset, buf_len - offset, "\"%s\"", p->options[j]);
                        }
                        offset += snprintf(buf + offset, buf_len - offset, "]");
                    }
                    /* Add unit and display_format if present */
                    if (p->unit[0]) {
                        offset += snprintf(buf + offset, buf_len - offset, ",\"unit\":\"%s\"", p->unit);
                    }
                    if (p->display_format[0]) {
                        offset += snprintf(buf + offset, buf_len - offset, ",\"display_format\":\"%s\"", p->display_format);
                    }
                    offset += snprintf(buf + offset, buf_len - offset, "}");
                }
                offset += snprintf(buf + offset, buf_len - offset, "]");
                return offset;
            }
            return -1;
        }

        if (inst->fx_count > 0) {
            if (inst->fx_is_v2[0] && inst->fx_plugins_v2[0] && inst->fx_instances[0] && inst->fx_plugins_v2[0]->get_param) {
                return inst->fx_plugins_v2[0]->get_param(inst->fx_instances[0], subkey, buf, buf_len);
            } else if (inst->fx_plugins[0] && inst->fx_plugins[0]->get_param) {
                return inst->fx_plugins[0]->get_param(subkey, buf, buf_len);
            }
        }
        return -1;
    }

    /* Route fx2: prefixed params to FX2 (strip prefix) */
    if (strncmp(key, "fx2:", 4) == 0) {
        const char *subkey = key + 4;

        /* For chain_params: try plugin first, fall back to parsed module.json data */
        if (strcmp(subkey, "chain_params") == 0 && inst->fx_count > 1) {
            /* Try plugin's own chain_params handler first */
            if (inst->fx_is_v2[1] && inst->fx_plugins_v2[1] && inst->fx_instances[1] && inst->fx_plugins_v2[1]->get_param) {
                int result = inst->fx_plugins_v2[1]->get_param(inst->fx_instances[1], subkey, buf, buf_len);
                if (result > 0) return result;
            } else if (inst->fx_plugins[1] && inst->fx_plugins[1]->get_param) {
                int result = inst->fx_plugins[1]->get_param(subkey, buf, buf_len);
                if (result > 0) return result;
            }
            /* Fall back to parsed module.json data */
            if (inst->fx_param_counts[1] > 0) {
                int offset = 0;
                offset += snprintf(buf + offset, buf_len - offset, "[");
                for (int i = 0; i < inst->fx_param_counts[1] && offset < buf_len - 100; i++) {
                    chain_param_info_t *p = &inst->fx_params[1][i];
                    if (i > 0) offset += snprintf(buf + offset, buf_len - offset, ",");
                    const char *type_str = (p->type == KNOB_TYPE_INT) ? "int" :
                                          (p->type == KNOB_TYPE_ENUM) ? "enum" : "float";
                    offset += snprintf(buf + offset, buf_len - offset,
                        "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g",
                        p->key, p->name[0] ? p->name : p->key,
                        type_str,
                        p->min_val, p->max_val);
                    /* Add options array for enum types */
                    if (p->type == KNOB_TYPE_ENUM && p->option_count > 0) {
                        offset += snprintf(buf + offset, buf_len - offset, ",\"options\":[");
                        for (int j = 0; j < p->option_count && j < MAX_ENUM_OPTIONS; j++) {
                            if (j > 0) offset += snprintf(buf + offset, buf_len - offset, ",");
                            offset += snprintf(buf + offset, buf_len - offset, "\"%s\"", p->options[j]);
                        }
                        offset += snprintf(buf + offset, buf_len - offset, "]");
                    }
                    /* Add unit and display_format if present */
                    if (p->unit[0]) {
                        offset += snprintf(buf + offset, buf_len - offset, ",\"unit\":\"%s\"", p->unit);
                    }
                    if (p->display_format[0]) {
                        offset += snprintf(buf + offset, buf_len - offset, ",\"display_format\":\"%s\"", p->display_format);
                    }
                    offset += snprintf(buf + offset, buf_len - offset, "}");
                }
                offset += snprintf(buf + offset, buf_len - offset, "]");
                return offset;
            }
            return -1;
        }

        if (inst->fx_count > 1) {
            if (inst->fx_is_v2[1] && inst->fx_plugins_v2[1] && inst->fx_instances[1] && inst->fx_plugins_v2[1]->get_param) {
                return inst->fx_plugins_v2[1]->get_param(inst->fx_instances[1], subkey, buf, buf_len);
            } else if (inst->fx_plugins[1] && inst->fx_plugins[1]->get_param) {
                return inst->fx_plugins[1]->get_param(subkey, buf, buf_len);
            }
        }
        return -1;
    }

    /* Route midi_fx1: prefixed params to MIDI FX1 (strip prefix) */
    if (strncmp(key, "midi_fx1:", 9) == 0) {
        const char *subkey = key + 9;
        /* For ui_hierarchy: return cached JSON from module.json */
        if (strcmp(subkey, "ui_hierarchy") == 0 && inst->midi_fx_count > 0) {
            if (inst->midi_fx_ui_hierarchy[0][0]) {
                int len = strlen(inst->midi_fx_ui_hierarchy[0]);
                if (len < buf_len) {
                    strcpy(buf, inst->midi_fx_ui_hierarchy[0]);
                    return len;
                }
            }
            return -1;
        }
        /* For chain_params: return parsed module.json data */
        if (strcmp(subkey, "chain_params") == 0 && inst->midi_fx_count > 0) {
            if (inst->midi_fx_param_counts[0] > 0) {
                int written = snprintf(buf, buf_len, "[");
                for (int i = 0; i < inst->midi_fx_param_counts[0] && written < buf_len - 10; i++) {
                    chain_param_info_t *p = &inst->midi_fx_params[0][i];
                    if (i > 0) written += snprintf(buf + written, buf_len - written, ",");
                    const char *type_str = (p->type == KNOB_TYPE_INT) ? "int" :
                                          (p->type == KNOB_TYPE_ENUM) ? "enum" : "float";
                    written += snprintf(buf + written, buf_len - written,
                        "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\"",
                        p->key, p->name, type_str);
                    if (p->type == KNOB_TYPE_FLOAT || p->type == KNOB_TYPE_INT) {
                        written += snprintf(buf + written, buf_len - written,
                            ",\"min\":%.2f,\"max\":%.2f,\"default\":%.2f",
                            p->min_val, p->max_val, p->default_val);
                    } else if (p->type == KNOB_TYPE_ENUM && p->option_count > 0) {
                        written += snprintf(buf + written, buf_len - written, ",\"options\":[");
                        for (int j = 0; j < p->option_count; j++) {
                            if (j > 0) written += snprintf(buf + written, buf_len - written, ",");
                            written += snprintf(buf + written, buf_len - written, "\"%s\"", p->options[j]);
                        }
                        written += snprintf(buf + written, buf_len - written, "]");
                    }
                    /* Add unit and display_format if present */
                    if (p->unit[0]) {
                        written += snprintf(buf + written, buf_len - written, ",\"unit\":\"%s\"", p->unit);
                    }
                    if (p->display_format[0]) {
                        written += snprintf(buf + written, buf_len - written, ",\"display_format\":\"%s\"", p->display_format);
                    }
                    written += snprintf(buf + written, buf_len - written, "}");
                }
                written += snprintf(buf + written, buf_len - written, "]");
                return written;
            }
            return -1;  /* No chain_params available */
        }
        if (inst->midi_fx_count > 0 && inst->midi_fx_plugins[0] && inst->midi_fx_instances[0] && inst->midi_fx_plugins[0]->get_param) {
            return inst->midi_fx_plugins[0]->get_param(inst->midi_fx_instances[0], subkey, buf, buf_len);
        }
        return -1;
    }

    /* Route midi_fx2: prefixed params to MIDI FX2 (strip prefix) */
    if (strncmp(key, "midi_fx2:", 9) == 0) {
        const char *subkey = key + 9;
        /* For ui_hierarchy: return cached JSON from module.json */
        if (strcmp(subkey, "ui_hierarchy") == 0 && inst->midi_fx_count > 1) {
            if (inst->midi_fx_ui_hierarchy[1][0]) {
                int len = strlen(inst->midi_fx_ui_hierarchy[1]);
                if (len < buf_len) {
                    strcpy(buf, inst->midi_fx_ui_hierarchy[1]);
                    return len;
                }
            }
            return -1;
        }
        /* For chain_params: return parsed module.json data */
        if (strcmp(subkey, "chain_params") == 0 && inst->midi_fx_count > 1) {
            if (inst->midi_fx_param_counts[1] > 0) {
                int written = snprintf(buf, buf_len, "[");
                for (int i = 0; i < inst->midi_fx_param_counts[1] && written < buf_len - 10; i++) {
                    chain_param_info_t *p = &inst->midi_fx_params[1][i];
                    if (i > 0) written += snprintf(buf + written, buf_len - written, ",");
                    const char *type_str = (p->type == KNOB_TYPE_INT) ? "int" :
                                          (p->type == KNOB_TYPE_ENUM) ? "enum" : "float";
                    written += snprintf(buf + written, buf_len - written,
                        "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\"",
                        p->key, p->name, type_str);
                    if (p->type == KNOB_TYPE_FLOAT || p->type == KNOB_TYPE_INT) {
                        written += snprintf(buf + written, buf_len - written,
                            ",\"min\":%.2f,\"max\":%.2f,\"default\":%.2f",
                            p->min_val, p->max_val, p->default_val);
                    } else if (p->type == KNOB_TYPE_ENUM && p->option_count > 0) {
                        written += snprintf(buf + written, buf_len - written, ",\"options\":[");
                        for (int j = 0; j < p->option_count; j++) {
                            if (j > 0) written += snprintf(buf + written, buf_len - written, ",");
                            written += snprintf(buf + written, buf_len - written, "\"%s\"", p->options[j]);
                        }
                        written += snprintf(buf + written, buf_len - written, "]");
                    }
                    /* Add unit and display_format if present */
                    if (p->unit[0]) {
                        written += snprintf(buf + written, buf_len - written, ",\"unit\":\"%s\"", p->unit);
                    }
                    if (p->display_format[0]) {
                        written += snprintf(buf + written, buf_len - written, ",\"display_format\":\"%s\"", p->display_format);
                    }
                    written += snprintf(buf + written, buf_len - written, "}");
                }
                written += snprintf(buf + written, buf_len - written, "]");
                return written;
            }
            return -1;
        }
        if (inst->midi_fx_count > 1 && inst->midi_fx_plugins[1] && inst->midi_fx_instances[1] && inst->midi_fx_plugins[1]->get_param) {
            return inst->midi_fx_plugins[1]->get_param(inst->midi_fx_instances[1], subkey, buf, buf_len);
        }
        return -1;
    }

    /* Forward unprefixed to synth as fallback */
    if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->get_param) {
        return inst->synth_plugin_v2->get_param(inst->synth_instance, key, buf, buf_len);
    } else if (inst->synth_plugin && inst->synth_plugin->get_param) {
        return inst->synth_plugin->get_param(key, buf, buf_len);
    }

    return -1;
}

/* V2 render_block handler */
static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst) {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Mute briefly after patch switch */
    if (inst->mute_countdown > 0) {
        inst->mute_countdown--;
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Update smoothed parameters and send interpolated values to sub-plugins */
    {
        /* Synth smoother */
        if (smoother_update(&inst->synth_smoother)) {
            for (int i = 0; i < inst->synth_smoother.count; i++) {
                smooth_param_t *p = &inst->synth_smoother.params[i];
                if (p->active) {
                    char val_str[32];
                    snprintf(val_str, sizeof(val_str), "%.6f", p->current);
                    if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->set_param) {
                        inst->synth_plugin_v2->set_param(inst->synth_instance, p->key, val_str);
                    } else if (inst->synth_plugin && inst->synth_plugin->set_param) {
                        inst->synth_plugin->set_param(p->key, val_str);
                    }
                }
            }
        }

        /* FX smoothers */
        for (int fx = 0; fx < inst->fx_count && fx < MAX_AUDIO_FX; fx++) {
            if (smoother_update(&inst->fx_smoothers[fx])) {
                for (int i = 0; i < inst->fx_smoothers[fx].count; i++) {
                    smooth_param_t *p = &inst->fx_smoothers[fx].params[i];
                    if (p->active) {
                        char val_str[32];
                        snprintf(val_str, sizeof(val_str), "%.6f", p->current);
                        if (inst->fx_is_v2[fx] && inst->fx_plugins_v2[fx] && inst->fx_instances[fx]) {
                            inst->fx_plugins_v2[fx]->set_param(inst->fx_instances[fx], p->key, val_str);
                        } else if (inst->fx_plugins[fx] && inst->fx_plugins[fx]->set_param) {
                            inst->fx_plugins[fx]->set_param(p->key, val_str);
                        }
                    }
                }
            }
        }
    }

    /* Process MIDI FX tick (for arpeggiator timing) */
    v2_tick_midi_fx(inst, frames);

    /* Render synth */
    if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->render_block) {
        inst->synth_plugin_v2->render_block(inst->synth_instance, out_interleaved_lr, frames);
    } else if (inst->synth_plugin && inst->synth_plugin->render_block) {
        inst->synth_plugin->render_block(out_interleaved_lr, frames);
    } else {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
    }

    /* In external_fx_mode, output raw synth only — skip inject and FX.
     * The shim reads Link Audio in the same frame as the mailbox,
     * combines with this raw synth, and calls chain_process_fx(). */
    if (inst->external_fx_mode) return;

    /* Mix in external audio (e.g. Move track from Link Audio) before FX.
     * This lets the FX chain process both synth and Move audio together. */
    if (inst->inject_audio && inst->inject_audio_frames > 0) {
        int samples = (inst->inject_audio_frames < frames ? inst->inject_audio_frames : frames) * 2;
        for (int i = 0; i < samples; i++) {
            int32_t mixed = (int32_t)out_interleaved_lr[i] + (int32_t)inst->inject_audio[i];
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            out_interleaved_lr[i] = (int16_t)mixed;
        }
        inst->inject_audio = NULL;
        inst->inject_audio_frames = 0;
    }

    /* Process through audio FX chain */
    for (int i = 0; i < inst->fx_count; i++) {
        if (inst->fx_is_v2[i]) {
            if (inst->fx_plugins_v2[i] && inst->fx_instances[i] && inst->fx_plugins_v2[i]->process_block) {
                inst->fx_plugins_v2[i]->process_block(inst->fx_instances[i], out_interleaved_lr, frames);
            }
        } else {
            if (inst->fx_plugins[i] && inst->fx_plugins[i]->process_block) {
                inst->fx_plugins[i]->process_block(out_interleaved_lr, frames);
            }
        }
    }
}

/* V2 Plugin API structure */
static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = v2_create_instance,
    .destroy_instance = v2_destroy_instance,
    .on_midi = v2_on_midi,
    .set_param = v2_set_param,
    .get_param = v2_get_param,
    .render_block = v2_render_block
};

/* V2 Entry Point */
plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;

    if (host->api_version != MOVE_PLUGIN_API_VERSION) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[chain-v2] API version mismatch: host=%d, plugin=%d",
                 host->api_version, MOVE_PLUGIN_API_VERSION);
        if (host->log) host->log(msg);
        return NULL;
    }

    if (host->log) host->log("[chain-v2] Plugin v2 API initialized");

    return &g_plugin_api_v2;
}

/* Exported: set external audio buffer to mix before FX processing.
 * Called by shim to inject Move track audio from Link Audio ring buffers.
 * The buffer is consumed (mixed + cleared) during the next render_block call. */
void chain_set_inject_audio(void *instance, int16_t *buf, int frames) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst) return;
    inst->inject_audio = buf;
    inst->inject_audio_frames = frames;
}

/* Exported: enable/disable external FX mode.
 * When enabled, render_block outputs raw synth only (no inject, no FX).
 * The caller is responsible for running chain_process_fx() separately. */
void chain_set_external_fx_mode(void *instance, int mode) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst) return;
    inst->external_fx_mode = mode;
}

/* Exported: run only the audio FX chain on the provided buffer.
 * Used by the shim for same-frame FX processing when external_fx_mode is set. */
void chain_process_fx(void *instance, int16_t *buf, int frames) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst) return;
    for (int i = 0; i < inst->fx_count; i++) {
        if (inst->fx_is_v2[i]) {
            if (inst->fx_plugins_v2[i] && inst->fx_instances[i] && inst->fx_plugins_v2[i]->process_block) {
                inst->fx_plugins_v2[i]->process_block(inst->fx_instances[i], buf, frames);
            }
        } else {
            if (inst->fx_plugins[i] && inst->fx_plugins[i]->process_block) {
                inst->fx_plugins[i]->process_block(buf, frames);
            }
        }
    }
}


