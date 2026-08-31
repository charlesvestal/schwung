/* shadow_chain_mgmt.h - Chain management, master FX, param handling, boot init
 * Extracted from schwung_shim.c for maintainability. */

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
#include "lfo_common.h"
#include "master_fx_key.h"
#include "fx_midi_filter.h"   /* FX_MIDI_CHANNEL_ALL, for master_fx_midi_channel below */

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Number of Master FX slots. Raising this should be a one-line change: all
 * "fx<N>:" key routing goes through master_fx_key.h with this passed in as
 * slot_count, and every loop over the slots is bounded by this name. Pinned by
 * tests/host/test_master_fx_slot_routing.sh, which reads the value out of this
 * line and proves the routing covers 1..MASTER_FX_SLOTS and rejects the slot
 * just past it. */
#define MASTER_FX_SLOTS 8

/* "fx%d" LFO target keys are formatted into MASTER_FX_TARGET_KEY_LEN buffers
 * and strcmp'd against lfo_state_t.target. A truncated format would compare
 * unequal and silently stop modulating, so pin the digit budget here rather
 * than discover it at slot 10000. */
_Static_assert(MASTER_FX_SLOTS > 0 && MASTER_FX_SLOTS <= 9999,
               "MASTER_FX_SLOTS must fit \"fx%d\" in MASTER_FX_TARGET_KEY_LEN");
#define SHADOW_CHAIN_MODULE_DIR "/data/UserData/schwung/modules/chain"
#define SHADOW_CHAIN_DSP_PATH "/data/UserData/schwung/modules/chain/dsp.so"

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

/* Size of one position's cached chain_params JSON. Shared by both Master FX
 * caches: the module.json snapshot below and the runtime refresh buffer in
 * shadow_chain_mgmt.c. */
#define MASTER_FX_CHAIN_PARAMS_MAX 65536

/* Master FX chain slot */
typedef struct {
    void *handle;                    /* dlopen handle */
    audio_fx_api_v2_t *api;          /* FX API pointer */
    void *instance;                  /* FX instance */
    char module_path[256];           /* Full DSP path */
    char module_id[64];              /* Module ID for display */
    shadow_capture_rules_t capture;  /* Capture rules for this FX */
    /* Cached chain_params to avoid file I/O in the audio thread.
     *
     * OWNED BUFFER, NEVER NULL. MASTER_FX_CHAIN_PARAMS_MAX bytes, allocated
     * once per position by shadow_master_fx_storage_ensure() and never freed.
     * It is a pointer rather than an inline array because Master FX is
     * becoming a list with insert/remove/move, and that reordering is a
     * PERMUTATION executed on the SPI callback (~900 us of budget after the
     * transfer). Rotating a pointer is free; memmoving 64 KB per position is
     * not. Nothing about this is a memory saving — the allocation is the same
     * bytes in a different place — so do not "simplify" it back to an inline
     * array without first moving the permutation off the audio thread.
     *
     * Vacating a position must ROTATE this pointer (hand it the buffer
     * displaced off the end of the shift) and clear its CONTENTS. Nulling it
     * instead is the exact mistake that took the SPI callback down on the slot
     * chain: v2_load_midi_fx_slot parsed a param table through the NULLed
     * pointer and SIGSEGV'd on the audio thread, and the shift silently leaked
     * the allocation it overwrote. See the PERMUTATION section of docs/CHAIN.md
     * and PERM_OWNED in src/host/chain_permute.h. */
    char *chain_params_cache;
    int chain_params_cached;         /* 1 if cache is valid */
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);  /* Optional MIDI handler */
    int bypassed;                    /* 1 = skip this MFX slot (dry passthrough), 0 = active */
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

    /* Tempo query — returns current BPM via sampler_get_bpm() fallback chain. */
    float (*get_bpm)(void);

    /* Transport beat position for LFO phase-lock (see host_api_v1). < 0 when
     * no transport is running; may be NULL. */
    double (*get_beat_position)(void);

    /* Web UI notification: called after any param set completes successfully.
     * Pushes the changed value to the web param notify ring for real-time
     * browser updates. May be NULL if web ring is not available. */
    void (*on_param_changed)(uint8_t slot, const char *key, const char *value);

    /* Queue a 4-byte USB-MIDI packet for the external port (cable 2).
     * Audio-thread safe: enqueues into the shim's lock-free ROUTE_EXTERNAL
     * ring, drained into the mailbox once per block. May be NULL on hosts
     * that have no external port — always guard. */
    int (*midi_send_external)(const uint8_t *msg, int len);
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
/* Optional: returns 1 if any audio FX in this chain instance opted out of
 * silence-skip via capabilities.requires_continuous_processing. NULL when the
 * loaded chain DSP is older than v0.3.12 — caller must null-check. */
extern int (*shadow_chain_fx_requires_continuous)(void *instance);
extern host_api_v1_t shadow_host_api;
extern int shadow_inprocess_ready;

/* Master FX slots */
extern master_fx_slot_t shadow_master_fx_slots[MASTER_FX_SLOTS];

/* Master FX LFOs */
#define MASTER_FX_LFO_COUNT 2
extern lfo_state_t shadow_master_fx_lfos[MASTER_FX_LFO_COUNT];
void shadow_master_fx_lfo_tick(int frames);

/* Direct param set (web UI ring buffer — doesn't touch shadow_param_t) */
void shadow_direct_set_param(uint8_t slot, const char *key, const char *value);

/* Legacy single-slot macros */
#define shadow_master_fx_handle (shadow_master_fx_slots[0].handle)
#define shadow_master_fx (shadow_master_fx_slots[0].api)
#define shadow_master_fx_instance (shadow_master_fx_slots[0].instance)
#define shadow_master_fx_module (shadow_master_fx_slots[0].module_path)
/* There is deliberately no `shadow_master_fx_capture` here any more. It named
 * position 0's capture rules, and shadow_midi.c cached a pointer to it at init
 * — so a Master FX module that declared `capture` was heard only if it sat
 * first, and a move gesture would silently take a MIDI-triggered module off
 * the air. Use shadow_master_fx_captures_note / _cc, which ask every loaded
 * position, per event, from the live array. */

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

/* Advance the fade envelope by one sample. Call once per stereo frame in mix loop. */
static inline void shadow_fade_advance(int slot) {
    slot_fade_t *f = &shadow_chain_slots[slot].fade;
    if (f->gain < f->target) {
        f->gain += f->step;
        if (f->gain > f->target) f->gain = f->target;
    } else if (f->gain > f->target) {
        f->gain -= f->step;
        if (f->gain < f->target) f->gain = f->target;
    }
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

/* Does this chain instance hold ANY loaded component — synth, audio FX, or
 * MIDI FX, in any position? This is the shim's definition of an "active" slot,
 * and every activation site shares it. Costs three in-process get_param calls
 * regardless of the FX caps: it asks the DSP for its list LENGTHS rather than
 * probing each position, so MAX_AUDIO_FX / MAX_MIDI_FX are never restated on
 * this side. Callers run inside the SPI callback — keep it that cheap. */
int shadow_slot_has_loaded_component(const plugin_api_v2_t *pv2, void *instance);

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

/* Give every Master FX position its owned chain_params buffers (the struct
 * member above and the runtime refresh cache inside shadow_chain_mgmt.c).
 * Idempotent and gap-filling: it allocates only positions that do not have a
 * buffer yet, so calling it twice is free and a partial failure is retryable.
 * Returns 1 when every position has both buffers, 0 otherwise.
 *
 * Call it from a thread that can cope with malloc failing. chain_mgmt_init()
 * and shadow_chain_defaults() both do, and both run at shim startup — the
 * point is that no allocation ever happens on the SPI callback, where a NULL
 * return has nowhere to go. shadow_master_fx_slot_load_with_config() refuses
 * to bring a position up while this returns 0, so a reader can never reach a
 * missing buffer. */
int shadow_master_fx_storage_ensure(void);

void shadow_master_fx_slot_unload(int slot);
void shadow_master_fx_unload_all(void);
int shadow_master_fx_slot_load(int slot, const char *dsp_path);
int shadow_master_fx_slot_load_with_config(int slot, const char *dsp_path,
                                            const char *config_json);
int shadow_master_fx_load(const char *dsp_path);
void shadow_master_fx_unload(void);

/* --- Master FX shape: insert / remove / move, as an ARRAY PERMUTATION ---
 *
 * The instances keep running and only their index changes. Expressed as a run
 * of `fxN:module` writes instead, each position behind the edit would be
 * unloaded and dlopen'd afresh — a reverb loses the tail that was ringing.
 * All three take 0-based positions and return 1 on success, 0 when refused
 * (out of range, chain full, from == to). A refused edit changes nothing.
 *
 * Insert only opens the hole; the caller follows with the ordinary `fxN:module`
 * write. Remove unloads the occupant through shadow_master_fx_slot_unload —
 * never a memset, which would leak the dlopen handle and the FX instance.
 *
 * Reached from the shadow UI as `master_fx:fx:insert` / `:remove` / `:move`,
 * spelled to match the slot chain's `fx:insert` exactly so one shared emitter
 * serves both editors. */
int shadow_master_fx_insert(int at);
int shadow_master_fx_remove(int at);
int shadow_master_fx_move(int from, int to);

/* How long the Master FX chain is, holes included — NOT the cap. Published as
 * `master_fx:fx_count`. Once a position can be removed, the cap no longer says
 * where the chain ends; bound loops by this. Never reports less than
 * "highest loaded position + 1", so a module can never run unseen. */
int shadow_master_fx_count(void);

/* Master FX listen channel, 0-based (0 = MIDI ch 1); FX_MIDI_CHANNEL_ALL (-1)
 * = every channel, the default. Enforced inside shadow_master_fx_forward_midi
 * rather than at its callers — see the comment there. */
extern volatile int master_fx_midi_channel;

void shadow_master_fx_forward_midi(const uint8_t *msg, int len, int source);

/* Does ANY loaded Master FX position capture this note / CC?
 *
 * A UNION over positions, deliberately: capture belongs to the MODULE, not to
 * the index it currently sits at. The predicate this replaced read position 0
 * only (shadow_midi.c cached, at init, a raw pointer to
 * shadow_master_fx_slots[0].capture), which was invisible while
 * Master FX was a fixed array nobody reordered. With the move gesture it turns
 * into: drag a MIDI-triggered module — a ducker is the obvious one on a master
 * bus — off position 0 and it silently stops receiving MIDI, with no swap and
 * no reload to blame it on.
 *
 * Evaluated from the live array on every event; nothing caches a pointer into
 * an array whose contents permute. Runs on the SPI callback: no allocation, no
 * I/O, no locks — at most MASTER_FX_SLOTS pointer tests and one bit test. */
int shadow_master_fx_captures_note(uint8_t note);
int shadow_master_fx_captures_cc(uint8_t cc);

/* --- Capture loading --- */
void shadow_slot_load_capture(int slot, int patch_index);

/* --- Boot --- */
int shadow_inprocess_load_chain(void);

/* Round-robin refresh of per-slot capabilities.wants_sysex. Call once per
 * SPI frame; it advances one slot per call. */
void shadow_chain_refresh_wants_sysex_tick(void);

/* --- UI requests --- */
void shadow_inprocess_handle_ui_request(void);

/* --- Fade completions --- */
void shadow_process_fade_completions(void);

/* --- Param handling --- */
int shadow_handle_slot_param_set(int slot, const char *key, const char *value);
int shadow_handle_slot_param_get(int slot, const char *key, char *buf, int buf_len);
int shadow_param_publish_response(uint32_t req_id);
void shadow_inprocess_handle_param_request(void);

#endif /* SHADOW_CHAIN_MGMT_H */
