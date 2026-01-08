/*
 * Module Manager - discovers, loads, and manages DSP modules
 */

#ifndef MODULE_MANAGER_H
#define MODULE_MANAGER_H

#include <stdint.h>
#include "plugin_api_v1.h"

#define MAX_MODULES 32
#define MAX_MODULE_ID_LEN 64
#define MAX_MODULE_NAME_LEN 128
#define MAX_PATH_LEN 512

/* Module metadata parsed from module.json */
typedef struct module_info {
    char id[MAX_MODULE_ID_LEN];
    char name[MAX_MODULE_NAME_LEN];
    char version[32];
    char ui_script[MAX_PATH_LEN];
    char dsp_path[MAX_PATH_LEN];
    char module_dir[MAX_PATH_LEN];
    int api_version;

    /* Capabilities */
    int cap_audio_out;
    int cap_audio_in;
    int cap_midi_in;
    int cap_midi_out;
    int cap_aftertouch;
    int cap_claims_master_knob;  /* If true, module handles volume knob */

    /* Defaults JSON string (for passing to plugin) */
    char defaults_json[1024];
} module_info_t;

/* Module manager state */
typedef struct module_manager {
    /* Discovered modules */
    module_info_t modules[MAX_MODULES];
    int module_count;

    /* Currently loaded module */
    int current_module_index;  /* -1 if none */
    void *dsp_handle;          /* dlopen handle */
    plugin_api_v1_t *plugin;   /* plugin API returned by init */

    /* Host API instance (passed to plugins) */
    host_api_v1_t host_api;

    /* Audio output buffer */
    int16_t audio_out_buffer[MOVE_FRAMES_PER_BLOCK * 2];

    /* Host-level volume (0-100, default 100) */
    int host_volume;

} module_manager_t;

/* Initialize module manager with host resources */
void mm_init(module_manager_t *mm, uint8_t *mapped_memory,
             int (*midi_send_internal)(const uint8_t*, int),
             int (*midi_send_external)(const uint8_t*, int));

/* Scan a directory for modules (e.g., "/data/UserData/move-anything/modules") */
int mm_scan_modules(module_manager_t *mm, const char *modules_dir);

/* Get module count */
int mm_get_module_count(module_manager_t *mm);

/* Get module info by index */
const module_info_t* mm_get_module_info(module_manager_t *mm, int index);

/* Find module by ID, returns index or -1 */
int mm_find_module(module_manager_t *mm, const char *module_id);

/* Load a module by index, returns 0 on success */
int mm_load_module(module_manager_t *mm, int index);

/* Load a module by ID, returns 0 on success */
int mm_load_module_by_id(module_manager_t *mm, const char *module_id);

/* Unload current module */
void mm_unload_module(module_manager_t *mm);

/* Check if a module is currently loaded */
int mm_is_module_loaded(module_manager_t *mm);

/* Get current module info, or NULL if none loaded */
const module_info_t* mm_get_current_module(module_manager_t *mm);

/* Send MIDI to current module's DSP plugin */
void mm_on_midi(module_manager_t *mm, const uint8_t *msg, int len, int source);

/* Set parameter on current module */
void mm_set_param(module_manager_t *mm, const char *key, const char *val);

/* Get parameter from current module */
int mm_get_param(module_manager_t *mm, const char *key, char *buf, int buf_len);

/* Render audio block from current module, writes to mailbox */
void mm_render_block(module_manager_t *mm);

/* Host volume control (0-100) */
void mm_set_host_volume(module_manager_t *mm, int volume);
int mm_get_host_volume(module_manager_t *mm);

/* Check if current module claims the master knob */
int mm_module_claims_master_knob(module_manager_t *mm);

/* Cleanup */
void mm_destroy(module_manager_t *mm);

#endif /* MODULE_MANAGER_H */
