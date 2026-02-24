/* shadow_chain_mgmt.h - Chain management, master FX, param handling, boot init
 * Extracted from move_anything_shim.c for maintainability. */

#ifndef SHADOW_CHAIN_MGMT_H
#define SHADOW_CHAIN_MGMT_H

#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include "shadow_constants.h"
#include "shadow_chain_types.h"
#include "plugin_api_v1.h"
#include "audio_fx_api_v2.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

#define MASTER_FX_SLOTS 4
#define SHADOW_CHAIN_MODULE_DIR "/data/UserData/move-anything/modules/chain"
#define SHADOW_CHAIN_DSP_PATH "/data/UserData/move-anything/modules/chain/dsp.so"

/* Capture group alias definitions */
#define CAPTURE_PADS_NOTE_MIN     68
#define CAPTURE_PADS_NOTE_MAX     99
#define CAPTURE_STEPS_NOTE_MIN    16
#define CAPTURE_STEPS_NOTE_MAX    31
#define CAPTURE_TRACKS_CC_MIN     40
#define CAPTURE_TRACKS_CC_MAX     43
#define CAPTURE_KNOBS_CC_MIN      71
#define CAPTURE_KNOBS_CC_MAX      78
#define CAPTURE_JOG_CC            14

/* ============================================================================
 * Types
 * ============================================================================ */

/* Master FX chain slot */
typedef struct {
    void *handle;                    /* dlopen handle */
    audio_fx_api_v2_t *api;          /* FX API pointer */
    void *instance;                  /* FX instance */
    char module_path[256];           /* Full DSP path */
    char module_id[64];              /* Module ID for display */
    shadow_capture_rules_t capture;  /* Capture rules for this FX */
    char chain_params_cache[2048];   /* Cached chain_params to avoid file I/O in audio thread */
    int chain_params_cached;         /* 1 if cache is valid */
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);  /* Optional MIDI handler */
} master_fx_slot_t;

/* ============================================================================
 * Callback struct - shim functions chain mgmt needs
 * ============================================================================ */

typedef struct {
    /* Shared state pointers (owned by shim) */
    shadow_control_t **shadow_control_ptr;
    shadow_param_t **shadow_param_ptr;
    shadow_ui_state_t **shadow_ui_state_ptr;
    uint8_t **global_mmap_addr_ptr;

    /* Boot callbacks */
    void (*overlay_sync)(void);
    int (*run_command)(const char *const argv[]);
    void (*launch_shadow_ui)(void);

    /* Boot state */
    bool *shadow_ui_enabled;
    int *startup_modwheel_countdown;
    int startup_modwheel_reset_frames;

    /* Param request: delegate shim-specific param prefixes (overtake_dsp, etc.)
     * The shim callback reads/writes shadow_param->key/value/error/result_len directly.
     * Returns 1 if handled, 0 if not. Caller publishes response if handled. */
    int (*handle_param_special)(uint8_t req_type, uint32_t req_id);
} chain_mgmt_host_t;

/* ============================================================================
 * Extern globals - chain state readable/writable by the shim
 * ============================================================================ */

/* Chain slot state */
extern shadow_chain_slot_t shadow_chain_slots[SHADOW_CHAIN_INSTANCES];
extern volatile int shadow_solo_count;
extern const char *shadow_chain_default_patches[SHADOW_CHAIN_INSTANCES];

/* Chain DSP plugin state */
extern void *shadow_dsp_handle;
extern const plugin_api_v2_t *shadow_plugin_v2;
extern void (*shadow_chain_set_inject_audio)(void *instance, int16_t *buf, int frames);
extern void (*shadow_chain_set_external_fx_mode)(void *instance, int mode);
extern void (*shadow_chain_process_fx)(void *instance, int16_t *buf, int frames);
extern host_api_v1_t shadow_host_api;
extern int shadow_inprocess_ready;

/* Master FX slots */
extern master_fx_slot_t shadow_master_fx_slots[MASTER_FX_SLOTS];

/* Legacy single-slot macros */
#define shadow_master_fx_handle (shadow_master_fx_slots[0].handle)
#define shadow_master_fx (shadow_master_fx_slots[0].api)
#define shadow_master_fx_instance (shadow_master_fx_slots[0].instance)
#define shadow_master_fx_module (shadow_master_fx_slots[0].module_path)
#define shadow_master_fx_capture (shadow_master_fx_slots[0].capture)

/* MIDI out log file (for log_enabled check in shim) */
extern FILE *shadow_midi_out_log;

/* ============================================================================
 * Inline functions - used by both shim and chain_mgmt
 * ============================================================================ */

/* Effective volume: combines volume, mute, and solo.
 * Solo wins over mute (matching Ableton/Move behavior). */
static inline float shadow_effective_volume(int slot) {
    if (shadow_solo_count > 0) {
        return shadow_chain_slots[slot].soloed ? shadow_chain_slots[slot].volume : 0.0f;
    }
    if (shadow_chain_slots[slot].muted) return 0.0f;
    return shadow_chain_slots[slot].volume;
}

/* Check if any master FX slot is active */
static inline int shadow_master_fx_chain_active(void) {
    for (int fx = 0; fx < MASTER_FX_SLOTS; fx++) {
        master_fx_slot_t *s = &shadow_master_fx_slots[fx];
        if (s->instance && s->api && s->api->process_block) {
            return 1;
        }
    }
    return 0;
}

/* ============================================================================
 * Public functions
 * ============================================================================ */

/* Initialize chain management with callbacks to shim functions.
 * Must be called before any other chain_mgmt function. */
void chain_mgmt_init(const chain_mgmt_host_t *host);

/* --- Logging --- */
void shadow_log(const char *msg);
int shadow_inprocess_log_enabled(void);
int shadow_midi_out_log_enabled(void);
void shadow_midi_out_logf(const char *fmt, ...);

/* --- Capture rules --- */
void capture_set_bit(uint8_t *bitmap, int index);
void capture_set_range(uint8_t *bitmap, int min, int max);
int capture_has_bit(const uint8_t *bitmap, int index);
int capture_has_note(const shadow_capture_rules_t *rules, uint8_t note);
int capture_has_cc(const shadow_capture_rules_t *rules, uint8_t cc);
void capture_clear(shadow_capture_rules_t *rules);
void capture_apply_group(shadow_capture_rules_t *rules, const char *group);
void capture_parse_json(shadow_capture_rules_t *rules, const char *json);

/* --- Chain management --- */
int shadow_chain_parse_channel(int ch);
void shadow_chain_defaults(void);
void shadow_chain_load_config(void);
int shadow_chain_find_patch_index(void *instance, const char *name);

/* --- UI state --- */
void shadow_ui_state_update_slot(int slot);
void shadow_ui_state_refresh(void);

/* --- Mute/solo --- */
void shadow_apply_mute(int slot, int is_muted);
void shadow_toggle_solo(int slot);

/* --- Master FX --- */
void shadow_master_fx_slot_unload(int slot);
void shadow_master_fx_unload_all(void);
int shadow_master_fx_slot_load(int slot, const char *dsp_path);
int shadow_master_fx_slot_load_with_config(int slot, const char *dsp_path,
                                            const char *config_json);
int shadow_master_fx_load(const char *dsp_path);
void shadow_master_fx_unload(void);
void shadow_master_fx_forward_midi(const uint8_t *msg, int len, int source);

/* --- Capture loading --- */
void shadow_slot_load_capture(int slot, int patch_index);

/* --- Boot --- */
int shadow_inprocess_load_chain(void);

/* --- UI requests --- */
void shadow_inprocess_handle_ui_request(void);

/* --- Param handling --- */
int shadow_handle_slot_param_set(int slot, const char *key, const char *value);
int shadow_handle_slot_param_get(int slot, const char *key, char *buf, int buf_len);
int shadow_param_publish_response(uint32_t req_id);
void shadow_inprocess_handle_param_request(void);

#endif /* SHADOW_CHAIN_MGMT_H */
