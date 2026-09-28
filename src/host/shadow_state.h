/* shadow_state.h - Shadow slot state persistence
 * Extracted from schwung_shim.c for maintainability. */

#ifndef SHADOW_STATE_H
#define SHADOW_STATE_H

#include "shadow_chain_types.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

#define SHADOW_CONFIG_PATH "/data/UserData/schwung/shadow_chain_config.json"

/* ============================================================================
 * Callback struct
 * ============================================================================ */

typedef struct {
    void (*log)(const char *msg);
    shadow_chain_slot_t *chain_slots;
    int *solo_count;
} state_host_t;

/* ============================================================================
 * Public functions
 * ============================================================================ */

/* Initialize state module with host pointers. */
void state_init(const state_host_t *host);

/* Save slot volumes, forward channels, mute/solo to shadow_chain_config.json.
 * Preserves fields written by shadow_ui.js (patches, master_fx, etc.). */
void shadow_save_state(void);
/* Ask the shim WORKER to save (shadow_save_state_service). The slot mix
 * mutators run on the SPI callback and, before the live model, on the D-Bus
 * thread too -- a file rewrite from either raced the other (one reading the
 * file the other had just truncated dropped `patches` from the config) and put
 * file I/O on the audio callback. Now they only ask. */
void shadow_request_save_state(void);
void shadow_save_state_service(void);   /* worker: save if asked */

/* Load slot volumes, forward channels, mute/solo from shadow_chain_config.json. */
void shadow_load_state(void);

#endif /* SHADOW_STATE_H */
