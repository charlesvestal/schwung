/* shadow_set_pages.h - Set page switching and per-set state management
 * Extracted from move_anything_shim.c for maintainability. */

#ifndef SHADOW_SET_PAGES_H
#define SHADOW_SET_PAGES_H

#include <stdint.h>
#include "shadow_constants.h"
#include "shadow_chain_types.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

#define SET_PAGES_DIR "/data/UserData/move-anything/set_pages"
#define SET_PAGES_CURRENT_PATH SET_PAGES_DIR "/current_page.txt"
#define SET_PAGES_TOTAL 8
#define SET_PAGE_OVERLAY_FRAMES 120  /* ~2 seconds at 60fps */

/* Path constants used by set/config management */
#define SHADOW_CHAIN_CONFIG_FILENAME "shadow_chain_config.json"
#define SHADOW_CHAIN_CONFIG_PATH "/data/UserData/move-anything/" SHADOW_CHAIN_CONFIG_FILENAME
#define SET_STATE_DIR  "/data/UserData/move-anything/set_state"
#define SLOT_STATE_DIR "/data/UserData/move-anything/slot_state"
#define ACTIVE_SET_PATH "/data/UserData/move-anything/active_set.txt"

/* ============================================================================
 * Callback struct - shim functions set pages needs
 * ============================================================================ */

typedef struct {
    void (*log)(const char *msg);
    void (*announce)(const char *msg);
    void (*overlay_sync)(void);
    int (*run_command)(const char *const argv[]);
    void (*save_state)(void);
    int (*read_set_mute_states)(const char *set_name, int muted_out[4], int soloed_out[4]);
    float (*read_set_tempo)(const char *set_name);
    void (*ui_state_update_slot)(int slot);
    void (*ui_state_refresh)(void);
    int (*chain_parse_channel)(int ch);
    /* Shared state pointers */
    shadow_chain_slot_t *chain_slots;
    shadow_control_t **shadow_control_ptr;
    volatile int *solo_count;
} set_pages_host_t;

/* ============================================================================
 * Extern globals - set page state readable/writable by the shim
 * ============================================================================ */

extern int set_page_current;
extern int set_page_overlay_active;
extern int set_page_overlay_timeout;
extern int set_page_loading;
extern volatile int set_page_change_in_flight;

/* Set tracking globals (shared with sampler for tempo) */
extern float sampler_set_tempo;
extern char sampler_current_set_name[128];
extern char sampler_current_set_uuid[64];
extern int sampler_last_song_index;
extern int sampler_pending_song_index;
extern uint32_t sampler_pending_set_seq;

/* ============================================================================
 * Public functions
 * ============================================================================ */

/* Initialize set pages subsystem with callbacks to shim functions.
 * Must be called before any other set pages function. */
void set_pages_init(const set_pages_host_t *host);

/* Utility: ensure a directory exists (mkdir -p) */
void shadow_ensure_dir(const char *dir);

/* Utility: copy a single file. Returns 1 on success. */
int shadow_copy_file(const char *src_path, const char *dst_path);

/* Batch migration: seed per-set state for all existing sets */
void shadow_batch_migrate_sets(void);

/* Save shadow chain config to a specific directory */
void shadow_save_config_to_dir(const char *dir);

/* Load shadow chain config from a specific directory. Returns 1 if loaded. */
int shadow_load_config_from_dir(const char *dir);

/* Handle a set being loaded (from Settings.json poll) */
void shadow_handle_set_loaded(const char *set_name, const char *uuid);

/* Poll Settings.json for set changes */
void shadow_poll_current_set(void);

/* Read current page from disk (returns 0 if not found) */
int set_page_read_persisted(void);

/* Change to a new set page (non-blocking: spawns background thread) */
void shadow_change_set_page(int new_page);

#endif /* SHADOW_SET_PAGES_H */
