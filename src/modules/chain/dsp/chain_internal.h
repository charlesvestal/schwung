/*
 * chain_internal.h — shared types and cross-TU declarations for the Signal
 * Chain DSP plugin (split out of chain_host.c, 2026-06 cleanup step 10).
 *
 * Everything marked CHAIN_INTERNAL is hidden-visibility: dsp.so's exported
 * symbol surface must stay exactly the 5 public entry points (see
 * chain_host.c) plus unified_log* — sub-plugins are dlopen'd and must never
 * be able to bind against these internals.
 */
#ifndef CHAIN_INTERNAL_H
#define CHAIN_INTERNAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <dlfcn.h>
#include <dirent.h>
#include <limits.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <malloc.h>

/* Sentinel for "channel field not present in patch file".
 * Distinguishes genuine absence from the legal 0 values used by
 * receive_channel (0=All) and forward_channel (0=ch 1 internal). */
#define PATCH_CHANNEL_UNSET INT_MIN

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"
#include "host/midi_fx_api_v1.h"
#include "host/lfo_common.h"
#include "../../../host/unified_log.h"
#include "../../../host/shadow_constants.h"

/* Limits */
#define MAX_PATCHES 32      /* Max patches to list in browser */
#define MAX_AUDIO_FX 4      /* Max FX loaded per active chain */
#define MAX_MIDI_FX 2       /* Max native MIDI FX modules per chain */
#define MAX_PATH_LEN 256
#define MAX_NAME_LEN 64

/* Optional file-based debug tracing for chain parsing/preset save diagnostics. */
#define CHAIN_DEBUG_FLAG_PATH "/data/UserData/schwung/chain_debug_on"
#define CHAIN_DEBUG_LOG_PATH "/data/UserData/schwung/chain_debug.log"
#define MOVE_SETTINGS_JSON_PATH "/data/UserData/settings/Settings.json"
#define CLOCK_SETTINGS_MAX_BYTES (256 * 1024)
#define CLOCK_SETTINGS_REFRESH_MS 1000
#define CLOCK_TICK_STALE_MS 750

/* MIDI input filter */
typedef enum {
    MIDI_INPUT_ANY = 0,
    MIDI_INPUT_PADS,
    MIDI_INPUT_EXTERNAL
} midi_input_t;

#define SAMPLE_RATE 44100
#define FRAMES_PER_BLOCK 128
#define MOVE_STEP_NOTE_MIN 16
#define MOVE_STEP_NOTE_MAX 31
#define MOVE_PAD_NOTE_MIN 68

/* Knob mapping constants */
#define MAX_KNOB_MAPPINGS 8
#define KNOB_CC_START 71
#define KNOB_CC_END 78
#define KNOB_ABS_CC_START 102
#define KNOB_ABS_CC_END 109
#define KNOB_STEP_FLOAT 0.0015f /* Base step for floats (~600 clicks for 0-1 at min speed) */
#define KNOB_STEP_INT 1        /* Base step for int params */

/* Knob acceleration settings */
#define KNOB_ACCEL_MIN_MULT 1    /* Multiplier for slow turns */
#define KNOB_ACCEL_MAX_MULT 4    /* Multiplier for fast turns (floats) */
#define KNOB_ACCEL_MAX_MULT_INT 2 /* Multiplier for fast turns (ints) */
#define KNOB_ACCEL_ENUM_MULT 1   /* Enums: always step by 1 (no acceleration) */
#define KNOB_ACCEL_SLOW_MS 250   /* Slower than this = min multiplier */
#define KNOB_ACCEL_FAST_MS 50    /* Faster than this = max multiplier */

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
#define MAX_CHAIN_PARAMS 256
#define MAX_ENUM_OPTIONS 128
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

#define MAX_MOD_TARGETS 32
#define MAX_MOD_SOURCES_PER_TARGET 8
#define MOD_PARAM_CACHE_REFRESH_MS 250
#define MOD_FLOAT_CHANGE_EPSILON 0.000001f
#define MOD_INT_ENUM_MIN_INTERVAL_MS 50

typedef struct mod_source_contribution {
    int active;
    char source_id[32];
    float contribution;
} mod_source_contribution_t;

/* Runtime modulation target state (non-destructive overlay). */
typedef struct mod_target_state {
    int active;
    int enabled;
    char target[16];
    char param[32];
    float base_value;
    mod_source_contribution_t sources[MAX_MOD_SOURCES_PER_TARGET];
    float effective_value;
    float last_applied_value;
    uint64_t last_applied_ms;
    int has_last_applied;
    float min_val;
    float max_val;
    knob_type_t type;
} mod_target_state_t;

#define MOVE_PAD_NOTE_MAX 99

/* MIDI FX parameter storage (key-value pairs for flexible configuration) */
#define MAX_MIDI_FX_PARAMS 8
typedef struct {
    char key[32];
    char val[32];
} midi_fx_param_t;

/* State storage size for FX plugins */
#define MAX_FX_STATE_LEN 8192


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

/* Synth state storage size - Surge XT needs ~8KB+ when pretty-printed with indent */
#define MAX_SYNTH_STATE_LEN 16384

/* LFO types, shapes, divisions, and waveform computation from lfo_common.h */

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
    midi_input_t midi_input;
    knob_mapping_t knob_mappings[MAX_KNOB_MAPPINGS];
    int knob_mapping_count;
    int receive_channel;   /* PATCH_CHANNEL_UNSET=absent, 0=All, 1-16=specific channel */
    int forward_channel;   /* PATCH_CHANNEL_UNSET=absent, -2=passthrough, -1=auto, 0-15=channel */
    int midi_fx_pre_mode;  /* 0 = Post (default), 1 = Pre (additive inject to Move MIDI_IN) */
    lfo_state_t lfos[LFO_COUNT];  /* LFO configuration */
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

/* LFO engine: shapes, divisions, and waveform computation now in lfo_common.h */

/* ============================================================================
 * V2 Instance-Based API
 * ============================================================================ */

/* Chain instance state - contains all per-instance data for v2 API */
typedef struct chain_instance {
    /* Module directory */
    char module_dir[MAX_PATH_LEN];

    /* Sub-plugin state - Synth */
    void *synth_handle;
    plugin_api_v2_t *synth_plugin_v2;
    void *synth_instance;
    char current_synth_module[MAX_NAME_LEN];
    int synth_default_forward_channel;  /* -1 = no default, 0-15 = channel */

    /* Audio FX state */
    void *fx_handles[MAX_AUDIO_FX];
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
    char fx_ui_hierarchy[MAX_AUDIO_FX][65536];  /* Cached ui_hierarchy JSON */

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
    char midi_fx_ui_hierarchy[MAX_MIDI_FX][65536];  /* Cached ui_hierarchy JSON */

    /* Knob mapping state */
    knob_mapping_t knob_mappings[MAX_KNOB_MAPPINGS];
    int knob_mapping_count;
    uint64_t knob_last_time_ms[MAX_KNOB_MAPPINGS];  /* For acceleration */

    /* Runtime modulation bus state */
    mod_target_state_t mod_targets[MAX_MOD_TARGETS];
    int mod_target_count;
    uint64_t mod_param_refresh_ms_synth;
    uint64_t mod_param_refresh_ms_fx[MAX_AUDIO_FX];
    uint64_t mod_param_refresh_ms_midi_fx[MAX_MIDI_FX];

    /* Per-slot LFO state */
    lfo_state_t lfos[LFO_COUNT];
    float lfo_base_values[LFO_COUNT];  /* Base value snapshot for LFO-to-LFO modulation */
    int lfo_base_valid[LFO_COUNT];     /* Whether base has been snapshotted */

    /* MIDI input filter */
    midi_input_t midi_input;

    /* Host APIs for sub-plugins */
    host_api_v1_t subplugin_host_api;

    /* Reference to host API (shared) */
    const host_api_v1_t *host;

    /* Parameter smoothing for synth and FX */
    param_smoother_t synth_smoother;
    param_smoother_t fx_smoothers[MAX_AUDIO_FX];

    /* Dirty flag: 1 = modified since last load/save */
    int dirty;

    /* External audio injection (e.g. Move track audio from Link Audio).
     * Set by host before render_block; mixed after synth, before FX. */
    int16_t *inject_audio;
    int inject_audio_frames;

    /* When set, render_block outputs raw synth only (no inject mix, no FX).
     * The shim calls chain_process_fx() separately for same-frame FX. */
    int external_fx_mode;

    /* Channel settings from last load_file (autosave restore).
     * Used as fallback when current_patch == -1 (file-based load, not library). */
    int loaded_receive_channel;   /* PATCH_CHANNEL_UNSET=absent, 0=All, 1-16=specific */
    int loaded_forward_channel;   /* PATCH_CHANNEL_UNSET=absent, -2=passthrough, -1=auto, 0-15=channel */

    /* MIDI FX placement: 0 = Post (default, output goes to slot synth only),
     * 1 = Pre (output also injected into Move's MIDI_IN cable 0 so Move's
     * native instrument on the slot's forward_channel plays it additively).
     * Only meaningful when a MIDI FX is loaded. */
    int midi_fx_pre_mode;

    /* Cached "pre_capable" hint from the loaded MIDI FX module.json.
     * Informs the Shadow UI default on first placement; does not gate the
     * per-slot toggle (legacy FX can still be switched to Pre manually). */
    int midi_fx_pre_capable[MAX_MIDI_FX];

    /* Pre-mode echo refcount: per-note counter tracking notes we injected
     * into Move's MIDI_IN cable 2. Move plays the injection and echoes it
     * back on MIDI_OUT cable 2, which the shim routes to slot chains — we
     * must drop those echoes before they re-enter MIDI FX processing or
     * the chain would transform and re-inject them (feedback loop). The
     * per-note refcount survives chord overlaps; note-off echoes decrement
     * so later note-ons on the same pitch aren't falsely filtered. */
    uint8_t pre_injected_notes[128];

    /* Pre-mode pad-held tracker: counts how many times each note is
     * currently held by a pad via cable-2 MIDI_OUT from Move. Tick-path
     * MIDI FX (arp) must NOT inject a note that's held by a pad, because
     * that would leave our refcount > 0 for the pad's pitch and the real
     * pad-release note-off would get mistaken for an injection echo and
     * eaten (symptom: arp keeps running after pad release). The set is
     * maintained in v2_on_midi after the echo filter so only real pad
     * events — not our own injection echoes — affect it. */
    uint8_t pre_pad_held[128];

    /* Per-component bypass flags. 1 = bypassed (skip processing), 0 = active. */
    int synth_bypassed;
    int midi_fx_bypassed[MAX_MIDI_FX];
    int fx_bypassed[MAX_AUDIO_FX];

    /* 1 = audio FX declared capabilities.requires_continuous_processing in
     * module.json; shim must never park the slot as fx_idle so stateful FX
     * (loopers, modulated delays) keep advancing internal time during silence. */
    int fx_requires_continuous[MAX_AUDIO_FX];
    
    /* Synth load error message */
    char synth_load_error[256];
} chain_instance_t;

#define CHAIN_INTERNAL __attribute__((visibility("hidden")))

/* Get current time in milliseconds (for knob acceleration) */
static inline uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Master preset registry — owned by chain_patch.c, read by v2_get_param. */
#define MAX_MASTER_PRESETS 64
CHAIN_INTERNAL extern char master_preset_names[MAX_MASTER_PRESETS][MAX_NAME_LEN];
CHAIN_INTERNAL extern char master_preset_paths[MAX_MASTER_PRESETS][MAX_PATH_LEN];
CHAIN_INTERNAL extern int master_preset_count;

/* ---- cross-TU internals (grouped by defining file) ---- */

/* chain_host.c */
CHAIN_INTERNAL void chain_log(const char *msg);
CHAIN_INTERNAL void parse_debug_log(const char *msg);
CHAIN_INTERNAL void v2_chain_log(chain_instance_t *inst, const char *msg);
CHAIN_INTERNAL int v2_load_audio_fx(chain_instance_t *inst, const char *fx_name);
CHAIN_INTERNAL int v2_load_synth(chain_instance_t *inst, const char *module_name);
CHAIN_INTERNAL void v2_synth_panic(chain_instance_t *inst);
CHAIN_INTERNAL void v2_unload_all_audio_fx(chain_instance_t *inst);
CHAIN_INTERNAL void v2_unload_synth(chain_instance_t *inst);

/* chain_json.c */
CHAIN_INTERNAL const char *bounded_strstr(const char *start, const char *end, const char *needle);
CHAIN_INTERNAL int json_get_float(const char *json, const char *key, float *out);
CHAIN_INTERNAL int json_get_int(const char *json, const char *key, int *out);
CHAIN_INTERNAL int json_get_int_in_section(const char *json, const char *section_key, const char *key, int *out);
CHAIN_INTERNAL int json_get_section_bounds(const char *json, const char *section_key, const char **out_start, const char **out_end);
CHAIN_INTERNAL int json_get_string(const char *json, const char *key, char *out, int out_len);
CHAIN_INTERNAL int json_get_string_in_section(const char *json, const char *section_key, const char *key, char *out, int out_len);

/* chain_params.c */
CHAIN_INTERNAL float dsp_value_to_float(const char *val_str, chain_param_info_t *pinfo, float fallback);
CHAIN_INTERNAL chain_param_info_t* find_param_by_key(chain_instance_t *inst, const char *target, const char *key);
CHAIN_INTERNAL chain_param_info_t *find_param_info(chain_param_info_t *params, int count, const char *key);
CHAIN_INTERNAL int format_param_value(chain_param_info_t *param, float value, char *buf, int buf_len);
CHAIN_INTERNAL int is_smoothable_float(const char *val, float *out_value);
CHAIN_INTERNAL chain_param_info_t *knob_find_param(chain_instance_t *inst, const char *target, const char *param);
CHAIN_INTERNAL void knob_forward_value(chain_instance_t *inst, const char *target, const char *param, const char *val_str);
CHAIN_INTERNAL int parse_chain_params(const char *module_path, chain_param_info_t *params, int *count);
CHAIN_INTERNAL int parse_chain_params_array_json(const char *json_array, chain_param_info_t *params, int max_params);
CHAIN_INTERNAL int parse_ui_hierarchy_cache(const char *module_path, char *out, int out_len);
CHAIN_INTERNAL void smoother_reset(param_smoother_t *smoother);
CHAIN_INTERNAL void smoother_set_target(param_smoother_t *smoother, const char *key, float value);
CHAIN_INTERNAL int smoother_update(param_smoother_t *smoother);

/* chain_mod.c */
CHAIN_INTERNAL void chain_mod_apply_effective_value(chain_instance_t *inst, mod_target_state_t *entry, int force_write);
CHAIN_INTERNAL void chain_mod_clear_source(void *ctx, const char *source_id);
CHAIN_INTERNAL void chain_mod_clear_target_entries(chain_instance_t *inst, const char *target, int restore_base);
CHAIN_INTERNAL int chain_mod_emit_value(void *ctx, const char *source_id, const char *target, const char *param, float signal, float depth, float offset, int bipolar, int enabled);
CHAIN_INTERNAL mod_target_state_t *chain_mod_find_target_entry(chain_instance_t *inst, const char *target, const char *param);
CHAIN_INTERNAL int chain_mod_get_base_for_subkey(chain_instance_t *inst, const char *target, const char *subkey, char *buf, int buf_len);
CHAIN_INTERNAL int chain_mod_get_modulated_for_subkey(chain_instance_t *inst, const char *target, const char *subkey, char *buf, int buf_len);
CHAIN_INTERNAL int chain_mod_is_target_active(chain_instance_t *inst, const char *target, const char *param);
CHAIN_INTERNAL int chain_mod_refresh_target_param_cache(chain_instance_t *inst, const char *target);
CHAIN_INTERNAL void chain_mod_update_base_from_set_param(chain_instance_t *inst, const char *target, const char *param, const char *val);

/* chain_midi.c */
CHAIN_INTERNAL int chain_get_clock_status(void);
CHAIN_INTERNAL int v2_load_midi_fx(chain_instance_t *inst, const char *fx_name);
CHAIN_INTERNAL void v2_on_midi(void *instance, const uint8_t *msg, int len, int source);
CHAIN_INTERNAL void v2_tick_midi_fx(chain_instance_t *inst, int frames);
CHAIN_INTERNAL void v2_unload_all_midi_fx(chain_instance_t *inst);

/* chain_patch.c */
CHAIN_INTERNAL int delete_master_preset(int index);
CHAIN_INTERNAL int load_master_preset_json(int index, char *buf, int buf_len);
CHAIN_INTERNAL int save_master_preset(const char *json_str);
CHAIN_INTERNAL void scan_master_presets(void);
CHAIN_INTERNAL int update_master_preset(int index, const char *json_str);
CHAIN_INTERNAL int v2_delete_patch(chain_instance_t *inst, int index);
CHAIN_INTERNAL int v2_load_from_patch_info(chain_instance_t *inst, patch_info_t *patch);
CHAIN_INTERNAL int v2_load_patch(chain_instance_t *inst, int patch_idx);
CHAIN_INTERNAL int v2_parse_patch_file(chain_instance_t *inst, const char *path, patch_info_t *patch);
CHAIN_INTERNAL int v2_save_patch(chain_instance_t *inst, const char *json_data);
CHAIN_INTERNAL int v2_scan_patches(chain_instance_t *inst);
CHAIN_INTERNAL int v2_update_patch(chain_instance_t *inst, int index, const char *json_data);


#endif /* CHAIN_INTERNAL_H */
