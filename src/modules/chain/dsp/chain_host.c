/*
 * Signal Chain Host DSP Plugin
 *
 * Orchestrates a signal chain: Input → MIDI FX → Sound Generator → Audio FX → Output
 */

#include "chain_internal.h"


/* ============================================================================
 * Global State (v1 compatibility)
 * ============================================================================ */

/* Host API provided by main host */
static const host_api_v1_t *g_host = NULL;


/* Logging helper */
/* Validate a module/FX name contains no path traversal sequences */
static int valid_module_name(const char *name) {
    if (!name || !name[0]) return 0;
    if (strstr(name, "..") != NULL) return 0;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return 0;
    return 1;
}

void chain_log(const char *msg) {
    /* Use unified log */
    unified_log("chain", LOG_LEVEL_DEBUG, "%s", msg);

    /* Also call host log if available */
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[chain] %s", msg);
        g_host->log(buf);
    }
}


/* ============================================================================
 * V2 Instance-Based API Implementation
 * ============================================================================ */


void v2_chain_log(chain_instance_t *inst, const char *msg) {
    if (inst && inst->host && inst->host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[chain-v2] %s", msg);
        inst->host->log(buf);
    }
}

/* Create a new chain instance */
static void* v2_create_instance(const char *module_dir, const char *config_json) {
    (void)config_json;

    chain_instance_t *inst = calloc(1, sizeof(chain_instance_t));
    if (!inst) return NULL;

    strncpy(inst->module_dir, module_dir, MAX_PATH_LEN - 1);

    /* Channel fields default to "absent" — getters return empty length until
     * a patch sets them, so callers won't clobber the shim's slot config. */
    inst->loaded_receive_channel = PATCH_CHANNEL_UNSET;
    inst->loaded_forward_channel = PATCH_CHANNEL_UNSET;

    /* Set up host API for sub-plugins */
    if (g_host) {
        inst->host = g_host;
        memcpy(&inst->subplugin_host_api, g_host, sizeof(host_api_v1_t));
        inst->subplugin_host_api.get_clock_status = chain_get_clock_status;
        inst->subplugin_host_api.mod_emit_value = chain_mod_emit_value;
        inst->subplugin_host_api.mod_clear_source = chain_mod_clear_source;
        inst->subplugin_host_api.mod_host_ctx = inst;
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

    /* Unload all plugins */
    v2_synth_panic(inst);
    v2_unload_all_audio_fx(inst);
    v2_unload_synth(inst);

    free(inst);
}

/* V2 synth panic - send all notes off */
void v2_synth_panic(chain_instance_t *inst) {
    if (!inst) return;

    for (int ch = 0; ch < 16; ch++) {
        uint8_t msg[3] = {(uint8_t)(0xB0 | ch), 123, 0};  /* All notes off */

        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->on_midi) {
            inst->synth_plugin_v2->on_midi(inst->synth_instance, msg, 3, MOVE_MIDI_SOURCE_HOST);
        }
    }
}

/* V2 get synth error */
static int v2_synth_get_error(chain_instance_t *inst, char *buf, int buf_len) {
    if (!inst) return 0;

    if (inst->synth_load_error[0] != '\0') {
        return snprintf(buf, buf_len, "%s", inst->synth_load_error);
    }

    if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->get_error) {
        return inst->synth_plugin_v2->get_error(inst->synth_instance, buf, buf_len);
    }
    return 0;  /* No error */
}

/* V2 unload synth */
void v2_unload_synth(chain_instance_t *inst) {
    if (!inst) return;
    inst->synth_load_error[0] = '\0';
    chain_mod_clear_target_entries(inst, "synth", 0);

    if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->destroy_instance) {
        inst->synth_plugin_v2->destroy_instance(inst->synth_instance);
    }

    if (inst->synth_handle) {
        dlclose(inst->synth_handle);
    }

    inst->synth_handle = NULL;
    inst->synth_plugin_v2 = NULL;
    inst->synth_instance = NULL;
    inst->current_synth_module[0] = '\0';
    inst->synth_param_count = 0;
    inst->mod_param_refresh_ms_synth = 0;
    inst->synth_default_forward_channel = -1;
    inst->synth_bypassed = 0;
}

/* V2 unload all audio FX */
void v2_unload_all_audio_fx(chain_instance_t *inst) {
    if (!inst) return;

    for (int i = 0; i < inst->fx_count; i++) {
        char target_name[16];
        snprintf(target_name, sizeof(target_name), "fx%d", i + 1);
        chain_mod_clear_target_entries(inst, target_name, 0);

        if (inst->fx_is_v2[i]) {
            if (inst->fx_plugins_v2[i] && inst->fx_instances[i] && inst->fx_plugins_v2[i]->destroy_instance) {
                inst->fx_plugins_v2[i]->destroy_instance(inst->fx_instances[i]);
            }
        }

        if (inst->fx_handles[i]) {
            dlclose(inst->fx_handles[i]);
        }

        inst->fx_handles[i] = NULL;
        inst->fx_plugins_v2[i] = NULL;
        inst->fx_instances[i] = NULL;
        inst->fx_is_v2[i] = 0;
        inst->fx_on_midi[i] = NULL;
        inst->fx_param_counts[i] = 0;
        inst->mod_param_refresh_ms_fx[i] = 0;
        inst->current_fx_modules[i][0] = '\0';
        inst->fx_ui_hierarchy[i][0] = '\0';
        inst->fx_bypassed[i] = 0;
    }
    inst->fx_count = 0;
}

/* V2 unload a single audio FX slot */
static void v2_unload_audio_fx_slot(chain_instance_t *inst, int slot) {
    if (!inst || slot < 0 || slot >= MAX_AUDIO_FX) return;
    char target_name[16];
    snprintf(target_name, sizeof(target_name), "fx%d", slot + 1);
    chain_mod_clear_target_entries(inst, target_name, 0);

    if (inst->fx_is_v2[slot]) {
        if (inst->fx_plugins_v2[slot] && inst->fx_instances[slot] && inst->fx_plugins_v2[slot]->destroy_instance) {
            inst->fx_plugins_v2[slot]->destroy_instance(inst->fx_instances[slot]);
        }
    }

    if (inst->fx_handles[slot]) {
        dlclose(inst->fx_handles[slot]);
    }

    inst->fx_handles[slot] = NULL;
    inst->fx_plugins_v2[slot] = NULL;
    inst->fx_instances[slot] = NULL;
    inst->fx_is_v2[slot] = 0;
    inst->fx_on_midi[slot] = NULL;
    inst->fx_param_counts[slot] = 0;
    inst->mod_param_refresh_ms_fx[slot] = 0;
    inst->current_fx_modules[slot][0] = '\0';
    inst->fx_ui_hierarchy[slot][0] = '\0';
    inst->fx_bypassed[slot] = 0;
    inst->fx_requires_continuous[slot] = 0;
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
        inst->fx_ui_hierarchy[slot][0] = '\0';
        return -1;
    }
    parse_ui_hierarchy_cache(fx_dir, inst->fx_ui_hierarchy[slot], sizeof(inst->fx_ui_hierarchy[slot]));
    inst->mod_param_refresh_ms_fx[slot] = 0;

    /* Read capabilities.requires_continuous_processing from module.json — stateful
     * FX (loopers, modulated delays) opt out of the shim's silence-skip so their
     * internal time advances even when audio I/O has been silent for >1s. */
    inst->fx_requires_continuous[slot] = 0;
    {
        char mj_path[MAX_PATH_LEN];
        snprintf(mj_path, sizeof(mj_path), "%s/module.json", fx_dir);
        FILE *mj = fopen(mj_path, "r");
        if (mj) {
            fseek(mj, 0, SEEK_END);
            long mj_size = ftell(mj);
            fseek(mj, 0, SEEK_SET);
            if (mj_size > 0 && mj_size < 65536) {
                char *mj_buf = malloc(mj_size + 1);
                if (mj_buf) {
                    size_t nr = fread(mj_buf, 1, mj_size, mj);
                    mj_buf[nr] = '\0';
                    int cap = 0;
                    if (json_get_int_in_section(mj_buf, "capabilities",
                                                "requires_continuous_processing", &cap) == 0
                        && cap) {
                        inst->fx_requires_continuous[slot] = 1;
                    }
                    free(mj_buf);
                }
            }
            fclose(mj);
        }
    }

    /* Update fx_count to include this slot */
    if (slot >= inst->fx_count) {
        inst->fx_count = slot + 1;
    }

    snprintf(msg, sizeof(msg), "Audio FX v2 loaded: %s (slot %d, %d params)", fx_name, slot, inst->fx_param_counts[slot]);
    v2_chain_log(inst, msg);
    return 0;
}

/* V2 load synth - loads a sound generator module */
int v2_load_synth(chain_instance_t *inst, const char *module_name) {
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

    /* Build path to synth module - all sound generators in modules/sound_generators/.
     * For pack entries (e.g. "rnbo-synth-graph-Test"), resolve to the parent module
     * directory and pass the pack path as config JSON. */
    char *pack_config = NULL;
    char pack_config_buf[1024];

    snprintf(synth_path, sizeof(synth_path), "%s/../sound_generators/%s",
             inst->module_dir, module_name);

    struct stat path_st;
    if (stat(synth_path, &path_st) != 0 || !S_ISDIR(path_st.st_mode)) {
        /* Directory not found — resolve as pack entry */
        char sg_dir[MAX_PATH_LEN];
        snprintf(sg_dir, sizeof(sg_dir), "%s/../sound_generators", inst->module_dir);
        DIR *sgd = opendir(sg_dir);
        if (sgd) {
            struct dirent *ent;
            while ((ent = readdir(sgd)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                size_t plen = strlen(ent->d_name);
                if (strncmp(module_name, ent->d_name, plen) == 0 &&
                    module_name[plen] == '-') {
                    const char *pack_name = module_name + plen + 1;
                    char check[MAX_PATH_LEN];
                    snprintf(check, sizeof(check), "%s/%s/packs/%s/info.json",
                             sg_dir, ent->d_name, pack_name);
                    if (stat(check, &path_st) == 0) {
                        snprintf(synth_path, sizeof(synth_path),
                                 "%s/%s", sg_dir, ent->d_name);
                        snprintf(pack_config_buf, sizeof(pack_config_buf),
                                 "{\"pack\":\"%s/%s/packs/%s\"}",
                                 sg_dir, ent->d_name, pack_name);
                        pack_config = pack_config_buf;
                        snprintf(msg, sizeof(msg), "Resolved pack: %s -> %s",
                                 module_name, synth_path);
                        v2_chain_log(inst, msg);
                        break;
                    }
                }
            }
            closedir(sgd);
        }
    }

    char dsp_path[MAX_PATH_LEN];
    snprintf(dsp_path, sizeof(dsp_path), "%s/dsp.so", synth_path);

    inst->synth_load_error[0] = '\0';
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

    void *synth_inst = api->create_instance(synth_path, pack_config);
    if (!synth_inst) {
        snprintf(msg, sizeof(msg), "Synth %s V2 create_instance failed", module_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    /* Check UI and parameters JSON buffer size limits */
    char *temp_buf = (char*)malloc(262144);
    if (temp_buf) {
        int cp_len = 0;
        int ui_len = 0;
        if (api->get_param) {
            cp_len = api->get_param(synth_inst, "chain_params", temp_buf, 262144);
            ui_len = api->get_param(synth_inst, "ui_hierarchy", temp_buf, 262144);
        }
        free(temp_buf);

        if (cp_len >= SHADOW_PARAM_VALUE_LEN - 1 || ui_len >= SHADOW_PARAM_VALUE_LEN - 1) {
            snprintf(msg, sizeof(msg), "Synth %s UI or param JSON too large (chain_params: %d, ui_hierarchy: %d). Max %d.", 
                     module_name, cp_len, ui_len, SHADOW_PARAM_VALUE_LEN - 1);
            v2_chain_log(inst, msg);
            snprintf(inst->synth_load_error, sizeof(inst->synth_load_error), "UI buffer overflow");
            
            api->destroy_instance(synth_inst);
            
            /* Proceed as success with NULL synth_instance so UI loads and displays error */
            inst->synth_handle = handle;
            inst->synth_plugin_v2 = api;
            inst->synth_instance = NULL;
            strncpy(inst->current_synth_module, module_name, MAX_NAME_LEN - 1);
            
            parse_chain_params(synth_path, inst->synth_params, &inst->synth_param_count);
            inst->mod_param_refresh_ms_synth = 0;
            return 0;
        }
    }

    inst->synth_handle = handle;
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
    inst->mod_param_refresh_ms_synth = 0;

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
                        if (fwd_ch == -2) {
                            inst->synth_default_forward_channel = -2;  /* Passthrough (for MPE) */
                            v2_chain_log(inst, "Synth default_forward_channel: passthrough");
                        } else if (fwd_ch >= 1 && fwd_ch <= 16) {
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
int v2_load_audio_fx(chain_instance_t *inst, const char *fx_name) {
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
        inst->fx_ui_hierarchy[slot][0] = '\0';
        return -1;
    }
    parse_ui_hierarchy_cache(fx_dir, inst->fx_ui_hierarchy[slot], sizeof(inst->fx_ui_hierarchy[slot]));
    inst->mod_param_refresh_ms_fx[slot] = 0;

    inst->fx_count++;

    snprintf(msg, sizeof(msg), "Audio FX v2 loaded: %s (slot %d, %d params)", fx_name, slot, inst->fx_param_counts[slot]);
    v2_chain_log(inst, msg);
    return 0;
}


/* Debug logging helper for parsing */
void parse_debug_log(const char *msg) {
    /* Cached flag check: this runs on every v2_set_param (every knob tick,
     * on the SPI thread) — a stat() per call is RT-path file I/O. Re-check
     * the flag every 64th call instead. */
    static int cached = -1;
    static unsigned counter = 0;
    if (cached < 0 || (counter++ % 64 == 0)) {
        struct stat st;
        cached = (stat(CHAIN_DEBUG_FLAG_PATH, &st) == 0);
    }
    if (!cached) return;
    FILE *dbg = fopen(CHAIN_DEBUG_LOG_PATH, "a");
    if (dbg) {
        fprintf(dbg, "%s\n", msg);
        fclose(dbg);
    }
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst) return;

    {
        char dbg[256];
        snprintf(dbg, sizeof(dbg), "[v2_set_param] key='%s' val='%s'", key, val ? val : "null");
        parse_debug_log(dbg);
    }

    /* Per-component bypass flags. Handled BEFORE the prefix routes below
     * so we don't forward "bypassed" down to the sub-plugin's set_param. */
    if (strcmp(key, "synth:bypassed") == 0) {
        inst->synth_bypassed = (val && atoi(val)) ? 1 : 0;
        return;
    }
    if (strcmp(key, "midi_fx1:bypassed") == 0) {
        inst->midi_fx_bypassed[0] = (val && atoi(val)) ? 1 : 0;
        return;
    }
    if (strcmp(key, "fx1:bypassed") == 0) {
        inst->fx_bypassed[0] = (val && atoi(val)) ? 1 : 0;
        return;
    }
    if (strcmp(key, "fx2:bypassed") == 0) {
        inst->fx_bypassed[1] = (val && atoi(val)) ? 1 : 0;
        return;
    }
    if (strcmp(key, "fx3:bypassed") == 0) {
        inst->fx_bypassed[2] = (val && atoi(val)) ? 1 : 0;
        return;
    }
    if (strcmp(key, "fx4:bypassed") == 0) {
        inst->fx_bypassed[3] = (val && atoi(val)) ? 1 : 0;
        return;
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
        inst->dirty = 0;
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
            inst->dirty = 0;
        }
    }
    else if (strcmp(key, "load_file") == 0) {
        /* Load patch from arbitrary file path (used for autosave restore) */
        patch_info_t temp_patch;
        memset(&temp_patch, 0, sizeof(temp_patch));
        if (v2_parse_patch_file(inst, val, &temp_patch) == 0) {
            v2_load_from_patch_info(inst, &temp_patch);
            inst->current_patch = -1;  /* Not from library */
            /* Preserve channel settings for getter fallback (current_patch == -1) */
            inst->loaded_receive_channel = temp_patch.receive_channel;
            inst->loaded_forward_channel = temp_patch.forward_channel;
            inst->midi_fx_pre_mode = temp_patch.midi_fx_pre_mode ? 1 : 0;
            /* Check for "modified" field to restore dirty state */
            FILE *mf = fopen(val, "r");
            if (mf) {
                char mbuf[256];
                inst->dirty = 0;
                while (fgets(mbuf, sizeof(mbuf), mf)) {
                    if (strstr(mbuf, "\"modified\"") && strstr(mbuf, "true")) {
                        inst->dirty = 1;
                        break;
                    }
                }
                fclose(mf);
            }
        }
    }
    else if (strcmp(key, "clear") == 0) {
        /* Clear all DSP (synth + FX) without loading anything new.
         * Used by two-pass set switching to free memory before loading. */
        v2_synth_panic(inst);
        v2_unload_all_midi_fx(inst);
        v2_unload_all_audio_fx(inst);
        v2_unload_synth(inst);
        inst->current_patch = -1;
        inst->dirty = 0;
        malloc_trim(0);
    }
    else if (strcmp(key, "midi_fx_pre_mode") == 0) {
        int new_mode = (val && atoi(val)) ? 1 : 0;
        if (new_mode != inst->midi_fx_pre_mode) {
            inst->midi_fx_pre_mode = new_mode;
            inst->dirty = 1;
            /* Toggling clears any in-flight refcount so a stale echo can't
             * orphan a future note-on once Pre is re-enabled. The pad-held
             * set also resets — off→on re-enters Pre with clean state; on→off
             * means we stop tracking anyway (but a leftover count would
             * suppress the first inject after a later toggle-on). */
            memset(inst->pre_injected_notes, 0, sizeof(inst->pre_injected_notes));
            memset(inst->pre_pad_held, 0, sizeof(inst->pre_pad_held));
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
            v2_synth_panic(inst);
            v2_unload_synth(inst);
            smoother_reset(&inst->synth_smoother);  /* Reset smoother on module change */
            if (val && val[0] != '\0' && strcmp(val, "none") != 0) {
                v2_load_synth(inst, val);
            } else {
                /* Clearing synth - also clear knob mappings */
                inst->knob_mapping_count = 0;
            }
            inst->dirty = 1;
        } else {
            if (chain_mod_is_target_active(inst, "synth", subkey)) {
                chain_mod_update_base_from_set_param(inst, "synth", subkey, val);
                mod_target_state_t *entry = chain_mod_find_target_entry(inst, "synth", subkey);
                if (entry) {
                    chain_mod_apply_effective_value(inst, entry, 0);
                    inst->dirty = 1;
                    return;
                }
            }

            /* Only smooth float params — int/enum values must not be interpolated */
            float fval;
            if (is_smoothable_float(val, &fval)) {
                chain_param_info_t *pinfo = find_param_info(inst->synth_params, inst->synth_param_count, subkey);
                if (!pinfo || pinfo->type == KNOB_TYPE_FLOAT) {
                    smoother_set_target(&inst->synth_smoother, subkey, fval);
                }
            }
            /* Always forward immediately (smoother will override with interpolated values) */
            if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->set_param) {
                inst->synth_plugin_v2->set_param(inst->synth_instance, subkey, val);
            }
            inst->dirty = 1;
        }
    }
    else if (strncmp(key, "fx1:", 4) == 0) {
        const char *subkey = key + 4;
        /* Intercept module change to swap FX1 dynamically */
        if (strcmp(subkey, "module") == 0) {
            v2_load_audio_fx_slot(inst, 0, val);
            smoother_reset(&inst->fx_smoothers[0]);  /* Reset smoother on module change */
            inst->dirty = 1;
        } else if (inst->fx_count > 0) {
            if (chain_mod_is_target_active(inst, "fx1", subkey)) {
                chain_mod_update_base_from_set_param(inst, "fx1", subkey, val);
                mod_target_state_t *entry = chain_mod_find_target_entry(inst, "fx1", subkey);
                if (entry) {
                    chain_mod_apply_effective_value(inst, entry, 0);
                    inst->dirty = 1;
                    return;
                }
            }

            float fval;
            if (is_smoothable_float(val, &fval)) {
                chain_param_info_t *pinfo = find_param_info(inst->fx_params[0], inst->fx_param_counts[0], subkey);
                if (!pinfo || pinfo->type == KNOB_TYPE_FLOAT) {
                    smoother_set_target(&inst->fx_smoothers[0], subkey, fval);
                }
            }
            if (inst->fx_is_v2[0] && inst->fx_plugins_v2[0] && inst->fx_instances[0]) {
                inst->fx_plugins_v2[0]->set_param(inst->fx_instances[0], subkey, val);
            }
            if (strcmp(subkey, "plugin_id") == 0) {
                inst->fx_param_counts[0] = 0;
                inst->mod_param_refresh_ms_fx[0] = 0;
            }
            inst->dirty = 1;
        }
    }
    else if (strncmp(key, "fx2:", 4) == 0) {
        const char *subkey = key + 4;
        /* Intercept module change to swap FX2 dynamically */
        if (strcmp(subkey, "module") == 0) {
            v2_load_audio_fx_slot(inst, 1, val);
            smoother_reset(&inst->fx_smoothers[1]);  /* Reset smoother on module change */
            inst->dirty = 1;
        } else if (inst->fx_count > 1) {
            if (chain_mod_is_target_active(inst, "fx2", subkey)) {
                chain_mod_update_base_from_set_param(inst, "fx2", subkey, val);
                mod_target_state_t *entry = chain_mod_find_target_entry(inst, "fx2", subkey);
                if (entry) {
                    chain_mod_apply_effective_value(inst, entry, 0);
                    inst->dirty = 1;
                    return;
                }
            }

            float fval;
            if (is_smoothable_float(val, &fval)) {
                chain_param_info_t *pinfo = find_param_info(inst->fx_params[1], inst->fx_param_counts[1], subkey);
                if (!pinfo || pinfo->type == KNOB_TYPE_FLOAT) {
                    smoother_set_target(&inst->fx_smoothers[1], subkey, fval);
                }
            }
            if (inst->fx_is_v2[1] && inst->fx_plugins_v2[1] && inst->fx_instances[1]) {
                inst->fx_plugins_v2[1]->set_param(inst->fx_instances[1], subkey, val);
            }
            if (strcmp(subkey, "plugin_id") == 0) {
                inst->fx_param_counts[1] = 0;
                inst->mod_param_refresh_ms_fx[1] = 0;
            }
            inst->dirty = 1;
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
            inst->dirty = 1;
        } else if (inst->midi_fx_count > 0 && inst->midi_fx_plugins[0] && inst->midi_fx_instances[0]) {
            if (chain_mod_is_target_active(inst, "midi_fx1", subkey)) {
                chain_mod_update_base_from_set_param(inst, "midi_fx1", subkey, val);
                mod_target_state_t *entry = chain_mod_find_target_entry(inst, "midi_fx1", subkey);
                if (entry) {
                    chain_mod_apply_effective_value(inst, entry, 0);
                    inst->dirty = 1;
                    return;
                }
            }
            inst->midi_fx_plugins[0]->set_param(inst->midi_fx_instances[0], subkey, val);
            inst->dirty = 1;
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
            inst->dirty = 1;
        } else if (inst->midi_fx_count > 1 && inst->midi_fx_plugins[1] && inst->midi_fx_instances[1]) {
            if (chain_mod_is_target_active(inst, "midi_fx2", subkey)) {
                chain_mod_update_base_from_set_param(inst, "midi_fx2", subkey, val);
                mod_target_state_t *entry = chain_mod_find_target_entry(inst, "midi_fx2", subkey);
                if (entry) {
                    chain_mod_apply_effective_value(inst, entry, 0);
                    inst->dirty = 1;
                    return;
                }
            }
            inst->midi_fx_plugins[1]->set_param(inst->midi_fx_instances[1], subkey, val);
            inst->dirty = 1;
        }
    }
    /* LFO configuration: lfo1:* and lfo2:* */
    else if (strncmp(key, "lfo1:", 5) == 0 || strncmp(key, "lfo2:", 5) == 0) {
        int lfo_idx = (key[3] == '1') ? 0 : 1;
        lfo_state_t *lfo = &inst->lfos[lfo_idx];
        const char *subkey = key + 5;
        char source_id[8];
        snprintf(source_id, sizeof(source_id), "lfo%d", lfo_idx + 1);

        if (strcmp(subkey, "enabled") == 0) {
            lfo->enabled = atoi(val);
            if (!lfo->enabled) {
                lfo->active = 0;
                chain_mod_clear_source(inst, source_id);
            } else {
                /* Set sensible defaults if this is a fresh LFO (rate_hz still 0) */
                if (lfo->rate_hz < 0.1f && !lfo->sync) {
                    lfo->rate_hz = 1.0f;
                }
                if (lfo->depth == 0.0f && !lfo->target[0] && !lfo->param[0]) {
                    lfo->depth = 0.5f;
                }
                lfo->active = (lfo->target[0] && lfo->param[0]);
            }
        } else if (strcmp(subkey, "shape") == 0) {
            lfo->shape = atoi(val);
            if (lfo->shape < 0) lfo->shape = 0;
            if (lfo->shape >= LFO_NUM_SHAPES) lfo->shape = LFO_NUM_SHAPES - 1;
        } else if (strcmp(subkey, "rate_hz") == 0) {
            lfo->rate_hz = strtof(val, NULL);
            if (lfo->rate_hz < 0.1f) lfo->rate_hz = 0.1f;
            if (lfo->rate_hz > 20.0f) lfo->rate_hz = 20.0f;
        } else if (strcmp(subkey, "rate_div") == 0) {
            lfo->rate_div = atoi(val);
            if (lfo->rate_div < 0) lfo->rate_div = 0;
            if (lfo->rate_div >= LFO_NUM_DIVISIONS) lfo->rate_div = LFO_NUM_DIVISIONS - 1;
        } else if (strcmp(subkey, "sync") == 0) {
            lfo->sync = atoi(val);
            /* Default to 1/1 (index 15) if rate_div is still at 0 (16bar) */
            if (lfo->sync && lfo->rate_div == 0) lfo->rate_div = 15;
        } else if (strcmp(subkey, "depth") == 0) {
            lfo->depth = strtof(val, NULL);
            if (lfo->depth < -1.0f) lfo->depth = -1.0f;
            if (lfo->depth > 1.0f) lfo->depth = 1.0f;
        } else if (strcmp(subkey, "polarity") == 0) {
            lfo->bipolar = atoi(val) ? 1 : 0;
        } else if (strcmp(subkey, "phase_offset") == 0) {
            lfo->phase_offset = strtof(val, NULL);
            if (lfo->phase_offset < 0.0f) lfo->phase_offset = 0.0f;
            if (lfo->phase_offset > 1.0f) lfo->phase_offset = 1.0f;
        } else if (strcmp(subkey, "target") == 0) {
            /* Clear old modulation source before changing target */
            if (lfo->target[0]) {
                chain_mod_clear_source(inst, source_id);
            }
            strncpy(lfo->target, val, sizeof(lfo->target) - 1);
            lfo->target[sizeof(lfo->target) - 1] = '\0';
            lfo->active = (lfo->enabled && lfo->target[0] && lfo->param[0]);
            inst->lfo_base_valid[lfo_idx] = 0;  /* Re-snapshot base */
        } else if (strcmp(subkey, "target_param") == 0) {
            /* Clear old modulation source before changing param */
            if (lfo->param[0]) {
                chain_mod_clear_source(inst, source_id);
            }
            strncpy(lfo->param, val, sizeof(lfo->param) - 1);
            lfo->param[sizeof(lfo->param) - 1] = '\0';
            lfo->active = (lfo->enabled && lfo->target[0] && lfo->param[0]);
            inst->lfo_base_valid[lfo_idx] = 0;  /* Re-snapshot base */
        } else if (strcmp(subkey, "retrigger") == 0) {
            lfo->retrigger = atoi(val);
            lfo->held_count = 0;  /* Reset on toggle */
        }
        inst->dirty = 1;
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
                inst->dirty = 1;
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
                        inst->dirty = 1;
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
                        chain_param_info_t *pinfo = knob_find_param(inst, target, param);
                        if (!pinfo) continue;

                        /* Calculate acceleration based on time between events */
                        uint64_t now = get_time_ms();
                        uint64_t last = inst->knob_last_time_ms[i];
                        inst->knob_last_time_ms[i] = now;
                        int accel = KNOB_ACCEL_MIN_MULT;
                        if (last > 0) {
                            uint64_t elapsed = now - last;
                            if (elapsed <= KNOB_ACCEL_FAST_MS) {
                                accel = KNOB_ACCEL_MAX_MULT;
                            } else if (elapsed < KNOB_ACCEL_SLOW_MS) {
                                float ratio = (float)(KNOB_ACCEL_SLOW_MS - elapsed) /
                                              (float)(KNOB_ACCEL_SLOW_MS - KNOB_ACCEL_FAST_MS);
                                accel = KNOB_ACCEL_MIN_MULT + (int)(ratio * (KNOB_ACCEL_MAX_MULT - KNOB_ACCEL_MIN_MULT));
                            }
                        }

                        /* Cap acceleration: enums never accelerate, ints limited */
                        int is_int = (pinfo->type == KNOB_TYPE_INT || pinfo->type == KNOB_TYPE_ENUM);
                        if (pinfo->type == KNOB_TYPE_ENUM) {
                            accel = KNOB_ACCEL_ENUM_MULT;
                        } else if (is_int && accel > KNOB_ACCEL_MAX_MULT_INT) {
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

                        knob_forward_value(inst, target, param, val_str);
                        inst->dirty = 1;
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
    }
}

/*
 * Convert a DSP get_param return string to a float value.
 * Handles numeric strings directly. For non-numeric strings (enum labels),
 * looks up the index in the param's options list.
 * Returns the float value, or fallback if conversion fails.
 */
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst) return -1;

    /* Per-component bypass flags. Handled BEFORE the prefix routes below
     * so we return our cached flag instead of forwarding to the sub-plugin. */
    if (strcmp(key, "synth:bypassed") == 0) {
        return snprintf(buf, buf_len, "%d", inst->synth_bypassed ? 1 : 0);
    }
    if (strcmp(key, "midi_fx1:bypassed") == 0) {
        return snprintf(buf, buf_len, "%d", inst->midi_fx_bypassed[0] ? 1 : 0);
    }
    if (strcmp(key, "fx1:bypassed") == 0) {
        return snprintf(buf, buf_len, "%d", inst->fx_bypassed[0] ? 1 : 0);
    }
    if (strcmp(key, "fx2:bypassed") == 0) {
        return snprintf(buf, buf_len, "%d", inst->fx_bypassed[1] ? 1 : 0);
    }
    if (strcmp(key, "fx3:bypassed") == 0) {
        return snprintf(buf, buf_len, "%d", inst->fx_bypassed[2] ? 1 : 0);
    }
    if (strcmp(key, "fx4:bypassed") == 0) {
        return snprintf(buf, buf_len, "%d", inst->fx_bypassed[3] ? 1 : 0);
    }

    if (strcmp(key, "dirty") == 0) {
        return snprintf(buf, buf_len, "%d", inst->dirty);
    }
    if (strcmp(key, "patch_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->patch_count);
    }
    if (strcmp(key, "current_patch") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_patch);
    }
    if (strcmp(key, "patch:receive_channel") == 0) {
        int v = PATCH_CHANNEL_UNSET;
        if (inst->current_patch >= 0 && inst->current_patch < inst->patch_count) {
            v = inst->patches[inst->current_patch].receive_channel;
        } else if (inst->loaded_receive_channel != PATCH_CHANNEL_UNSET) {
            v = inst->loaded_receive_channel;
        }
        if (v == PATCH_CHANNEL_UNSET) return 0;  /* absent — caller skips */
        return snprintf(buf, buf_len, "%d", v);
    }
    if (strcmp(key, "patch:forward_channel") == 0) {
        int v = PATCH_CHANNEL_UNSET;
        if (inst->current_patch >= 0 && inst->current_patch < inst->patch_count) {
            v = inst->patches[inst->current_patch].forward_channel;
        } else if (inst->loaded_forward_channel != PATCH_CHANNEL_UNSET) {
            v = inst->loaded_forward_channel;
        }
        if (v == PATCH_CHANNEL_UNSET) return 0;  /* absent — caller skips */
        return snprintf(buf, buf_len, "%d", v);
    }
    if (strcmp(key, "midi_fx_pre_mode") == 0) {
        return snprintf(buf, buf_len, "%d", inst->midi_fx_pre_mode ? 1 : 0);
    }
    if (strcmp(key, "midi_fx:pre_capable") == 0) {
        /* Hint from the loaded MIDI FX's module.json. Aggregated as OR
         * across slots — in practice only one MIDI FX is loaded per slot. */
        int cap = 0;
        for (int i = 0; i < inst->midi_fx_count; i++) {
            if (inst->midi_fx_pre_capable[i]) { cap = 1; break; }
        }
        return snprintf(buf, buf_len, "%d", cap);
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
    if (strcmp(key, "synth_error") == 0 || strcmp(key, "load_error") == 0) {
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

    /* LFO configuration queries */
    if (strncmp(key, "lfo1:", 5) == 0 || strncmp(key, "lfo2:", 5) == 0) {
        int lfo_idx = (key[3] == '1') ? 0 : 1;
        lfo_state_t *lfo = &inst->lfos[lfo_idx];
        const char *subkey = key + 5;

        if (strcmp(subkey, "enabled") == 0)
            return snprintf(buf, buf_len, "%d", lfo->enabled);
        if (strcmp(subkey, "active") == 0)
            return snprintf(buf, buf_len, "%d", lfo->active);
        if (strcmp(subkey, "shape") == 0)
            return snprintf(buf, buf_len, "%d", lfo->shape);
        if (strcmp(subkey, "shape_name") == 0)
            return snprintf(buf, buf_len, "%s",
                   (lfo->shape >= 0 && lfo->shape < LFO_NUM_SHAPES)
                   ? lfo_shape_names[lfo->shape] : "sine");
        if (strcmp(subkey, "rate_hz") == 0)
            return snprintf(buf, buf_len, "%.1f", lfo->rate_hz);
        if (strcmp(subkey, "rate_div") == 0)
            return snprintf(buf, buf_len, "%d", lfo->rate_div);
        if (strcmp(subkey, "rate_div_label") == 0)
            return snprintf(buf, buf_len, "%s",
                   (lfo->rate_div >= 0 && lfo->rate_div < LFO_NUM_DIVISIONS)
                   ? lfo_divisions[lfo->rate_div].label : "1/4");
        if (strcmp(subkey, "sync") == 0)
            return snprintf(buf, buf_len, "%d", lfo->sync);
        if (strcmp(subkey, "depth") == 0)
            return snprintf(buf, buf_len, "%.2f", lfo->depth);
        if (strcmp(subkey, "polarity") == 0)
            return snprintf(buf, buf_len, "%d", lfo->bipolar);
        if (strcmp(subkey, "phase_offset") == 0)
            return snprintf(buf, buf_len, "%.2f", lfo->phase_offset);
        if (strcmp(subkey, "target") == 0)
            return snprintf(buf, buf_len, "%s", lfo->target);
        if (strcmp(subkey, "target_param") == 0)
            return snprintf(buf, buf_len, "%s", lfo->param);
        if (strcmp(subkey, "retrigger") == 0)
            return snprintf(buf, buf_len, "%d", lfo->retrigger);
        return -1;
    }
    /* LFO config as JSON (for patch save) */
    if (strcmp(key, "lfo_config") == 0) {
        int off = 0;
        off += snprintf(buf + off, buf_len - off, "{");
        for (int i = 0; i < LFO_COUNT; i++) {
            lfo_state_t *lfo = &inst->lfos[i];
            if (i > 0) off += snprintf(buf + off, buf_len - off, ",");
            if (!lfo->enabled && !lfo->target[0]) {
                off += snprintf(buf + off, buf_len - off, "\"lfo%d\":null", i + 1);
            } else {
                off += snprintf(buf + off, buf_len - off,
                    "\"lfo%d\":{\"enabled\":%d,\"shape\":%d,\"sync\":%d,"
                    "\"rate_hz\":%.1f,\"rate_div\":%d,\"depth\":%.2f,\"polarity\":%d,"
                    "\"phase_offset\":%.2f,\"target\":\"%s\",\"target_param\":\"%s\","
                    "\"retrigger\":%d,\"division_table_version\":%d}",
                    i + 1, lfo->enabled, lfo->shape, lfo->sync,
                    lfo->rate_hz, lfo->rate_div, lfo->depth, lfo->bipolar,
                    lfo->phase_offset, lfo->target, lfo->param,
                    lfo->retrigger, LFO_NUM_DIVISIONS);
            }
        }
        off += snprintf(buf + off, buf_len - off, "}");
        return off;
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
        int base_result = chain_mod_get_base_for_subkey(inst, "synth", subkey, buf, buf_len);
        if (base_result >= 0) return base_result;
        int mod_result = chain_mod_get_modulated_for_subkey(inst, "synth", subkey, buf, buf_len);
        if (mod_result >= 0) return mod_result;

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
        }
        return -1;
    }

    /* Route fx1: prefixed params to FX1 (strip prefix) */
    if (strncmp(key, "fx1:", 4) == 0) {
        const char *subkey = key + 4;
        int base_result = chain_mod_get_base_for_subkey(inst, "fx1", subkey, buf, buf_len);
        if (base_result >= 0) return base_result;
        int mod_result = chain_mod_get_modulated_for_subkey(inst, "fx1", subkey, buf, buf_len);
        if (mod_result >= 0) return mod_result;

        /* For ui_hierarchy: return cached JSON from module.json, fall through to plugin if empty */
        if (strcmp(subkey, "ui_hierarchy") == 0 && inst->fx_count > 0) {
            if (inst->fx_ui_hierarchy[0][0]) {
                int len = strlen(inst->fx_ui_hierarchy[0]);
                if (len < buf_len) {
                    strcpy(buf, inst->fx_ui_hierarchy[0]);
                    return len;
                }
            }
            /* Cache empty - fall through to plugin get_param below */
        }

        /* For chain_params: try plugin first, fall back to parsed module.json data */
        if (strcmp(subkey, "chain_params") == 0 && inst->fx_count > 0) {
            /* Try plugin's own chain_params handler first */
            if (inst->fx_is_v2[0] && inst->fx_plugins_v2[0] && inst->fx_instances[0] && inst->fx_plugins_v2[0]->get_param) {
                int result = inst->fx_plugins_v2[0]->get_param(inst->fx_instances[0], subkey, buf, buf_len);
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
            }
        }
        return -1;
    }

    /* Route fx2: prefixed params to FX2 (strip prefix) */
    if (strncmp(key, "fx2:", 4) == 0) {
        const char *subkey = key + 4;
        int base_result = chain_mod_get_base_for_subkey(inst, "fx2", subkey, buf, buf_len);
        if (base_result >= 0) return base_result;
        int mod_result = chain_mod_get_modulated_for_subkey(inst, "fx2", subkey, buf, buf_len);
        if (mod_result >= 0) return mod_result;

        /* For ui_hierarchy: return cached JSON from module.json, fall through to plugin if empty */
        if (strcmp(subkey, "ui_hierarchy") == 0 && inst->fx_count > 1) {
            if (inst->fx_ui_hierarchy[1][0]) {
                int len = strlen(inst->fx_ui_hierarchy[1]);
                if (len < buf_len) {
                    strcpy(buf, inst->fx_ui_hierarchy[1]);
                    return len;
                }
            }
            /* Cache empty - fall through to plugin get_param below */
        }

        /* For chain_params: try plugin first, fall back to parsed module.json data */
        if (strcmp(subkey, "chain_params") == 0 && inst->fx_count > 1) {
            /* Try plugin's own chain_params handler first */
            if (inst->fx_is_v2[1] && inst->fx_plugins_v2[1] && inst->fx_instances[1] && inst->fx_plugins_v2[1]->get_param) {
                int result = inst->fx_plugins_v2[1]->get_param(inst->fx_instances[1], subkey, buf, buf_len);
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
            }
        }
        return -1;
    }

    /* Route midi_fx1: prefixed params to MIDI FX1 (strip prefix) */
    if (strncmp(key, "midi_fx1:", 9) == 0) {
        const char *subkey = key + 9;
        int base_result = chain_mod_get_base_for_subkey(inst, "midi_fx1", subkey, buf, buf_len);
        if (base_result >= 0) return base_result;
        int mod_result = chain_mod_get_modulated_for_subkey(inst, "midi_fx1", subkey, buf, buf_len);
        if (mod_result >= 0) return mod_result;
        /* For ui_hierarchy: return cached JSON from module.json, fall through to plugin if empty */
        if (strcmp(subkey, "ui_hierarchy") == 0 && inst->midi_fx_count > 0) {
            if (inst->midi_fx_ui_hierarchy[0][0]) {
                int len = strlen(inst->midi_fx_ui_hierarchy[0]);
                if (len < buf_len) {
                    strcpy(buf, inst->midi_fx_ui_hierarchy[0]);
                    return len;
                }
            }
            /* Cache empty - fall through to plugin get_param below */
        }
        /* For chain_params: try plugin first, fall back to parsed module.json data */
        if (strcmp(subkey, "chain_params") == 0 && inst->midi_fx_count > 0) {
            /* Try plugin's own chain_params handler first */
            if (inst->midi_fx_plugins[0] && inst->midi_fx_instances[0] && inst->midi_fx_plugins[0]->get_param) {
                int result = inst->midi_fx_plugins[0]->get_param(inst->midi_fx_instances[0], subkey, buf, buf_len);
                if (result > 0) return result;
            }
            /* Fall back to parsed module.json data */
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
        int base_result = chain_mod_get_base_for_subkey(inst, "midi_fx2", subkey, buf, buf_len);
        if (base_result >= 0) return base_result;
        int mod_result = chain_mod_get_modulated_for_subkey(inst, "midi_fx2", subkey, buf, buf_len);
        if (mod_result >= 0) return mod_result;
        /* For ui_hierarchy: return cached JSON from module.json, fall through to plugin if empty */
        if (strcmp(subkey, "ui_hierarchy") == 0 && inst->midi_fx_count > 1) {
            if (inst->midi_fx_ui_hierarchy[1][0]) {
                int len = strlen(inst->midi_fx_ui_hierarchy[1]);
                if (len < buf_len) {
                    strcpy(buf, inst->midi_fx_ui_hierarchy[1]);
                    return len;
                }
            }
            /* Cache empty - fall through to plugin get_param below */
        }
        /* For chain_params: try plugin first, fall back to parsed module.json data */
        if (strcmp(subkey, "chain_params") == 0 && inst->midi_fx_count > 1) {
            /* Try plugin's own chain_params handler first */
            if (inst->midi_fx_plugins[1] && inst->midi_fx_instances[1] && inst->midi_fx_plugins[1]->get_param) {
                int result = inst->midi_fx_plugins[1]->get_param(inst->midi_fx_instances[1], subkey, buf, buf_len);
                if (result > 0) return result;
            }
            /* Fall back to parsed module.json data */
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
    }

    return -1;
}

/* ============================================================================
 * LFO Engine - ticked once per render_block (~344 Hz at 128 frames / 44100 Hz)
 * ============================================================================ */

/* LFO param metadata for LFO-to-LFO modulation in slot context */
typedef struct {
    const char *key;
    float min_val;
    float max_val;
} slot_lfo_param_meta_t;

static const slot_lfo_param_meta_t slot_lfo_param_meta[] = {
    { "depth",       -1.0f, 1.0f  },
    { "rate_hz",      0.1f, 20.0f },
    { "phase_offset", 0.0f, 1.0f  },
};
#define SLOT_LFO_PARAM_META_COUNT 3

static void lfo_tick(chain_instance_t *inst, int frames) {
    if (!inst) return;
    float sample_rate = (float)(inst->host ? inst->host->sample_rate : MOVE_SAMPLE_RATE);

    for (int i = 0; i < LFO_COUNT; i++) {
        lfo_state_t *lfo = &inst->lfos[i];
        if (!lfo->enabled || !lfo->active) continue;

        /* Phase accumulation */
        float rate_hz;
        if (lfo->sync) {
            float bpm = 120.0f;
            if (inst->host && inst->host->get_bpm) {
                bpm = inst->host->get_bpm();
            }
            rate_hz = lfo_sync_rate_hz(bpm, lfo->rate_div);
        } else {
            rate_hz = lfo->rate_hz;
        }

        lfo->phase = lfo_advance_phase(lfo->phase, rate_hz, frames, sample_rate);

        /* Compute waveform with phase offset */
        double effective_phase = fmod(lfo->phase + (double)lfo->phase_offset, 1.0);
        float signal = lfo_compute_shape(lfo->shape, effective_phase, lfo);

        /* Check for LFO-to-LFO targeting: "lfo1" or "lfo2" */
        int target_lfo = -1;
        if (lfo->target[0] == 'l' && lfo->target[1] == 'f' && lfo->target[2] == 'o' &&
            lfo->target[3] >= '1' && lfo->target[3] <= '2' && lfo->target[4] == '\0') {
            target_lfo = lfo->target[3] - '1';
        }

        if (target_lfo >= 0 && target_lfo != i) {
            /* LFO-to-LFO: directly modify target LFO's param */
            lfo_state_t *tgt = &inst->lfos[target_lfo];
            const slot_lfo_param_meta_t *meta = NULL;
            for (int j = 0; j < SLOT_LFO_PARAM_META_COUNT; j++) {
                if (strcmp(slot_lfo_param_meta[j].key, lfo->param) == 0) {
                    meta = &slot_lfo_param_meta[j];
                    break;
                }
            }
            if (!meta) continue;

            /* Read current value for base */
            float cur;
            if (strcmp(lfo->param, "depth") == 0) cur = tgt->depth;
            else if (strcmp(lfo->param, "rate_hz") == 0) cur = tgt->rate_hz;
            else if (strcmp(lfo->param, "phase_offset") == 0) cur = tgt->phase_offset;
            else continue;

            /* Use base from lfo_base_values if not yet snapshotted.
             * We store base in inst->lfo_base_values[i] and track validity
             * with inst->lfo_base_valid[i]. */
            if (!inst->lfo_base_valid[i]) {
                inst->lfo_base_values[i] = cur;
                inst->lfo_base_valid[i] = 1;
            }

            float base = inst->lfo_base_values[i];
            float half_range = (meta->max_val - meta->min_val) / 2.0f;
            float modulated = base + signal * lfo->depth * half_range;
            if (modulated < meta->min_val) modulated = meta->min_val;
            if (modulated > meta->max_val) modulated = meta->max_val;

            if (strcmp(lfo->param, "depth") == 0) tgt->depth = modulated;
            else if (strcmp(lfo->param, "rate_hz") == 0) tgt->rate_hz = modulated;
            else if (strcmp(lfo->param, "phase_offset") == 0) tgt->phase_offset = modulated;
        } else if (target_lfo < 0) {
            /* Normal FX/synth target: emit modulation via existing runtime */
            char source_id[8];
            snprintf(source_id, sizeof(source_id), "lfo%d", i + 1);
            chain_mod_emit_value(inst, source_id, lfo->target, lfo->param,
                                 signal, lfo->depth, 0.0f, lfo->bipolar, 1 /*enabled*/);
        }
        /* target_lfo == i: self-targeting, skip */
    }
}

/* V2 render_block handler */
static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst) {
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
                        }
                    }
                }
            }
        }
    }

    /* Tick LFOs — emit modulation before audio render */
    lfo_tick(inst, frames);

    /* Process MIDI FX tick (for arpeggiator timing) */
    v2_tick_midi_fx(inst, frames);

    /* Always render so synth state advances (envelopes, LFOs, phases).
     * If bypassed, zero the buffer afterward — downstream FX still see
     * silence as input but the synth's internal time doesn't freeze, so
     * unbypass resumes cleanly without a burst. */
    if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->render_block) {
        inst->synth_plugin_v2->render_block(inst->synth_instance, out_interleaved_lr, frames);
    } else {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
    }
    if (inst->synth_bypassed) {
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

    /* Process through audio FX chain.
     * Always process so FX state advances (delay buffers, reverb tails).
     * If bypassed, save the dry input and restore it after process_block,
     * so audio passes through unchanged but FX internals stay live. */
    for (int i = 0; i < inst->fx_count; i++) {
        int bypassed = (i < MAX_AUDIO_FX && inst->fx_bypassed[i]);
        int16_t fx_dry[FRAMES_PER_BLOCK * 2];
        if (bypassed) {
            memcpy(fx_dry, out_interleaved_lr, frames * 2 * sizeof(int16_t));
        }
        /* All loaded FX are v2 — v2_load_audio_fx_slot hard-requires it. */
        if (inst->fx_plugins_v2[i] && inst->fx_instances[i] && inst->fx_plugins_v2[i]->process_block) {
            inst->fx_plugins_v2[i]->process_block(inst->fx_instances[i], out_interleaved_lr, frames);
        }
        if (bypassed) {
            memcpy(out_interleaved_lr, fx_dry, frames * 2 * sizeof(int16_t));
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
        int bypassed = (i < MAX_AUDIO_FX && inst->fx_bypassed[i]);
        int16_t fx_dry[FRAMES_PER_BLOCK * 2];
        if (bypassed) {
            memcpy(fx_dry, buf, frames * 2 * sizeof(int16_t));
        }
        /* All loaded FX are v2 — v2_load_audio_fx_slot hard-requires it. */
        if (inst->fx_plugins_v2[i] && inst->fx_instances[i] && inst->fx_plugins_v2[i]->process_block) {
            inst->fx_plugins_v2[i]->process_block(inst->fx_instances[i], buf, frames);
        }
        if (bypassed) {
            memcpy(buf, fx_dry, frames * 2 * sizeof(int16_t));
        }
    }
}

/* Exported: 1 if any audio FX slot opted out of the shim's silence-skip via
 * capabilities.requires_continuous_processing in its module.json. Read by the
 * shim per SPI frame to keep stateful FX (loopers, modulated delays) running
 * during silence. */
int chain_fx_requires_continuous(void *instance) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst) return 0;
    for (int i = 0; i < inst->fx_count; i++) {
        if (inst->fx_requires_continuous[i]) return 1;
    }
    return 0;
}
