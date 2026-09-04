/*
 * Signal Chain Host DSP Plugin
 *
 * Orchestrates a signal chain: Input → MIDI FX → Sound Generator → Audio FX → Output
 */

/* dlinfo()/struct link_map are GNU extensions and need the feature macro
 * BEFORE any libc header — chain_internal.h pulls in dlfcn.h. Used only to
 * log a dlopen'd module's load base, which is what turns the crash
 * handler's raw lr into an addr2line-able offset. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <link.h>
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

    /*
     * Per-position metadata storage, allocated EAGERLY for every position.
     *
     * These used to be inline arrays, so every position's buffer existed from
     * the moment the instance did and no call site ever checked for one —
     * `inst->fx_ui_hierarchy[i][0]` is read in a dozen places. Allocating
     * lazily on load would make each of those a null dereference in the audio
     * callback for an unloaded position. Eager keeps the old invariant exactly;
     * only the indirection is new (see chain_internal.h for why it is there).
     */
    if (!chain_alloc_position_storage(inst)) { free(inst); return NULL; }

    strncpy(inst->module_dir, module_dir, MAX_PATH_LEN - 1);

    /* Channel fields default to "absent" — getters return empty length until
     * a patch sets them, so callers won't clobber the shim's slot config. */
    inst->loaded_receive_channel = PATCH_CHANNEL_UNSET;
    inst->loaded_forward_channel = PATCH_CHANNEL_UNSET;

    /* No note has been played into the synth yet. calloc would say 0, which is
     * a real note number (C-1) and would name a voice nobody selected. */
    inst->synth_last_note = -1;

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
    v2_unload_all_midi_fx(inst);
    v2_unload_synth(inst);

    chain_free_position_storage(inst);
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
    inst->synth_last_note = -1;
    inst->synth_bypassed = 0;
}

/* V2 unload all audio FX */
void v2_unload_all_audio_fx(chain_instance_t *inst) {
    if (!inst) return;

    for (int i = 0; i < inst->fx_count; i++) {
        char target_name[16];
        chain_fx_component_id(target_name, sizeof(target_name), "fx", i);
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

/* V2 unload a single audio FX slot. Not static: chain_reorder.c removes a
 * position through it, so the dlclose and the modulation-entry clear stay in
 * one place rather than being restated there. */
void v2_unload_audio_fx_slot(chain_instance_t *inst, int slot) {
    if (!inst || slot < 0 || slot >= MAX_AUDIO_FX) return;
    char target_name[16];
    chain_fx_component_id(target_name, sizeof(target_name), "fx", slot);
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
    parse_ui_hierarchy_cache(fx_dir, inst->fx_ui_hierarchy[slot], CHAIN_UI_HIERARCHY_LEN);
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

    /*
     * The module's LOAD BASE, so a crash inside it can be attributed.
     *
     * The shim's SIGSEGV handler prints pc and lr, and both are raw runtime
     * addresses — useless on their own for a dlopen'd .so under ASLR. With the
     * base in the log, `lr - base` is a file offset you can hand straight to
     * addr2line and get the function and line that made the call.
     *
     * Logged for the SYNTH position specifically because that is where a
     * module runs the most code at load time (create_instance scans
     * directories, opens samples, mmaps files), and it is where a crash on
     * load strands the device in a boot loop: the position is restored at
     * every boot, so a module that segfaults here takes MoveOriginal down
     * before the UI can be used to remove it.
     */
    {
        struct link_map *lm = NULL;
        if (dlinfo(handle, RTLD_DI_LINKMAP, &lm) == 0 && lm) {
            snprintf(msg, sizeof(msg), "loaded %s base=0x%lx",
                     module_name, (unsigned long)lm->l_addr);
            v2_chain_log(inst, msg);
        }
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
    inst->synth_consumes_line_input = 0;       /* Default: not a line-input consumer */
    /* Reset per synth load: a stale note from the previous module would name a
     * voice in a list that no longer exists. */
    inst->synth_last_note = -1;
    inst->synth_wants_sysex = 0;               /* Default: no raw SysEx */
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
                    /* Parse capabilities to decide if this synth pulls audio in
                     * from line-in / internal mic (feedback risk on boot). Mirror
                     * the JS predicate consumesLineInput(): audio_in === true AND
                     * component_type ∉ {audio_fx, midi_fx}. audio_in is a JSON
                     * boolean, so json_get_int would mis-parse it (atoi("true")=0)
                     * — use json_get_bool_in_section. */
                    {
                        int audio_in = 0;
                        if (json_get_bool_in_section(json, "capabilities", "audio_in", &audio_in) == 0
                            && audio_in) {
                            char ctype[32] = "";
                            if (json_get_string_in_section(json, "capabilities", "component_type",
                                                           ctype, sizeof(ctype)) != 0) {
                                json_get_string(json, "component_type", ctype, sizeof(ctype));
                            }
                            if (strcmp(ctype, "audio_fx") != 0 && strcmp(ctype, "midi_fx") != 0) {
                                inst->synth_consumes_line_input = 1;
                                v2_chain_log(inst, "Synth consumes line input (feedback risk on boot)");
                            }
                        }
                    }
                    /* Opt-in for raw SysEx, same both-spellings rule as
                     * the MIDI FX path in chain_midi.c. */
                    {
                        int wants = 0;
                        if ((json_get_bool_in_section(json, "capabilities", "wants_sysex", &wants) == 0
                             || json_get_int_in_section(json, "capabilities", "wants_sysex", &wants) == 0)
                            && wants) {
                            inst->synth_wants_sysex = 1;
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
    parse_ui_hierarchy_cache(fx_dir, inst->fx_ui_hierarchy[slot], CHAIN_UI_HIERARCHY_LEN);
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

static void lfo_tick(chain_instance_t *inst, int frames);

static void v2_set_param(void *instance, const char *key, const char *val) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst) return;

    /*
     * "mod:tick" — advance modulation WITHOUT rendering audio.
     *
     * lfo_tick() normally runs inside render_block, and the shim skips
     * render_block entirely on a silent slot (one probe frame in 172, see
     * schwung_shim.c). A skipped frame advanced the LFO by nothing at all, so
     * an idle slot ran its LFOs ~172x too slow and in visible steps — which is
     * why modulation only animated while a note was sounding, and why an LFO
     * resumed from a stale phase at note-on.
     *
     * Routed through set_param rather than a new plugin_api_v2_t entry on
     * purpose: that struct is shared with third-party sub-plugins compiled
     * against the current header, and appending to it would have the host read
     * past the end of theirs. A string key costs one strcmp.
     *
     * FIRST statement in the function, before the debug log and every other
     * route, because this is called from the SPI callback on every silent
     * frame: no allocation, no logging, no file I/O on this path.
     */
    if (key && key[0] == 'm' && strcmp(key, "mod:tick") == 0) {
        lfo_tick(inst, val ? atoi(val) : 128);
        return;
    }

    {
        char dbg[256];
        snprintf(dbg, sizeof(dbg), "[v2_set_param] key='%s' val='%s'", key, val ? val : "null");
        parse_debug_log(dbg);
    }

    /*
     * ---- Section reorder verbs -------------------------------------------
     *
     * "fx:insert" / "fx:remove" / "fx:move", and the midi_fx spellings.
     * Positions are 1-BASED, matching the ids everything else speaks ("fx2"),
     * so a caller never has to convert; "move" takes "A>B".
     *
     * These exist so that changing a chain's SHAPE stops meaning "reload it".
     * The editor used to express an insert, a removal or a reorder as a run of
     * `<id>:module` writes, and each of those unloads the position and dlopen()s
     * a fresh instance — so adding a MIDI FX at the head rebuilt every MIDI FX
     * behind it, and removing a mid-chain reverb rebuilt everything downstream.
     * A running arp lost its phase; a delay lost its repeats. Here the arrays
     * are permuted and the instances are left alone (chain_permute.h).
     *
     * Ahead of every other route because they are the only keys whose subkey is
     * a verb rather than a parameter name, and a sub-plugin must never see one.
     */
    {
        int is_midi = -1;
        const char *verb = NULL;
        if (strncmp(key, "midi_fx:", 8) == 0)  { is_midi = 1; verb = key + 8; }
        else if (strncmp(key, "fx:", 3) == 0)  { is_midi = 0; verb = key + 3; }
        if (is_midi >= 0) {
            const char *v = val ? val : "";
            if (strcmp(verb, "insert") == 0) {
                chain_reorder_insert(inst, is_midi, atoi(v) - 1);
                return;
            }
            if (strcmp(verb, "remove") == 0) {
                chain_reorder_remove(inst, is_midi, atoi(v) - 1);
                return;
            }
            if (strcmp(verb, "move") == 0) {
                const char *sep = strchr(v, '>');
                if (sep) chain_reorder_move(inst, is_midi, atoi(v) - 1, atoi(sep + 1) - 1);
                return;
            }
            /* Anything else under these prefixes falls through on purpose —
             * "midi_fx:pre_capable" is an existing key that lives further
             * down, so this must claim the three verbs and nothing more. */
        }
    }

    /* Per-component bypass flags. Handled BEFORE the prefix routes below
     * so we don't forward "bypassed" down to the sub-plugin's set_param. */
    if (strcmp(key, "synth:bypassed") == 0) {
        inst->synth_bypassed = (val && atoi(val)) ? 1 : 0;
        return;
    }
    /*
     * BEHAVIOUR CHANGE at midi_fx2. The enumerated version handled only
     * "midi_fx1:bypassed"; "midi_fx2:bypassed" fell through to the generic
     * midi_fx2: route, which handed "bypassed" to the plugin as if it were one
     * of its own params and never set the flag — so MIDI FX 2 could not
     * actually be bypassed, even though chain_midi.c already reads
     * midi_fx_bypassed[] for every slot. Indexing it fixes that, and the
     * fixed-4 fxN:bypassed cases had the same shape of hole above fx4.
     */
    {
        const char *bsub = NULL;
        int bidx = chain_fx_index_from_key(key, "midi_fx", MAX_MIDI_FX, &bsub);
        if (bidx >= 0 && strcmp(bsub, "bypassed") == 0) {
            inst->midi_fx_bypassed[bidx] = (val && atoi(val)) ? 1 : 0;
            return;
        }
        bidx = chain_fx_index_from_key(key, "fx", MAX_AUDIO_FX, &bsub);
        if (bidx >= 0 && strcmp(bsub, "bypassed") == 0) {
            inst->fx_bypassed[bidx] = (val && atoi(val)) ? 1 : 0;
            return;
        }
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
            inst->knob_cc_out = temp_patch.knob_cc_out ? 1 : 0;
            knob_emit_cc_out_all(inst);
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
        /*
         * The LFOs go too. They are per-SLOT state, not per-module, so
         * unloading everything they could point at used to leave them running
         * and aimed at a component that no longer exists.
         *
         * Reported from the device: "when i created a new set, the LFO was
         * still active and targeting the now empty synth slot." A set switch
         * clears every slot through here, so the routing outlived the set that
         * defined it.
         *
         * Safe to zero rather than preserve: loading a patch assigns the whole
         * array from the patch (chain_patch.c, `inst->lfos[i] = patch->lfos[i]`),
         * so nothing a patch defines can be lost by clearing first. Zero is
         * inert -- no target, no depth.
         */
        memset(inst->lfos, 0, sizeof(inst->lfos));
        memset(inst->lfo_base_values, 0, sizeof(inst->lfo_base_values));
        memset(inst->lfo_base_valid, 0, sizeof(inst->lfo_base_valid));
        /*
         * Knob mappings go too, for the identical reason the LFOs do: they
         * are per-SLOT state, not per-module, so unloading everything they
         * could point at used to leave a mapping "assigned" to a component
         * that no longer exists.
         *
         * Reported from the device: a knob assigned to a param in one Set
         * still read (and, if turned, still forwarded to whatever now
         * occupies that position) after switching to an empty Set. Two-pass
         * set switching only calls load_file when the new Set has SAVED
         * slot state; an empty Set has none, so pass 2 never runs and this
         * clear is the only reset the mapping gets. Loading a patch assigns
         * the whole array from the patch (chain_patch.c, `memcpy(inst->
         * knob_mappings, patch->knob_mappings, ...)`), so nothing a patch
         * defines can be lost by clearing first.
         */
        memset(inst->knob_mappings, 0, sizeof(inst->knob_mappings));
        inst->knob_mapping_count = 0;
        inst->current_patch = -1;
        inst->dirty = 0;
        malloc_trim(0);
    }
    else if (strcmp(key, "knob_cc_out") == 0) {
        int new_mode = (val && atoi(val)) ? 1 : 0;
        if (new_mode != inst->knob_cc_out) {
            inst->knob_cc_out = new_mode;
            inst->dirty = 1;
            /* Turning it on owes the controller a full picture; turning it off
             * clears our record so a later re-enable re-sends everything. */
            for (int i = 0; i < inst->knob_mapping_count; i++) {
                inst->knob_mappings[i].last_cc_out = -1;
            }
            if (new_mode) knob_emit_cc_out_all(inst);
        }
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
            inst->pre_delay_count = 0;  /* drop any buffered clock-driven inject */
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
    else if (chain_fx_index_from_key(key, "fx", MAX_AUDIO_FX, NULL) >= 0) {
        const char *subkey = NULL;
        int fxi = chain_fx_index_from_key(key, "fx", MAX_AUDIO_FX, &subkey);
        if (strcmp(subkey, "module") == 0) {
            v2_load_audio_fx_slot(inst, fxi, val);
            smoother_reset(&inst->fx_smoothers[fxi]);
            inst->dirty = 1;
        } else if (inst->fx_count > fxi) {
            /* Anything below fx_count only: a param for a slot that holds no
             * FX is DROPPED, deliberately and silently. There is nothing to
             * forward it to, and the slot's module is set by "fxN:module"
             * above -- do not "fix" this into an auto-load. With eight slots
             * and a reorder UI, a stale key naming a slot that no longer
             * exists is an ordinary event, not a bug to be recovered from. */
            char fx_id[16];
            chain_fx_component_id(fx_id, sizeof(fx_id), "fx", fxi);
            if (chain_mod_is_target_active(inst, fx_id, subkey)) {
                chain_mod_update_base_from_set_param(inst, fx_id, subkey, val);
                mod_target_state_t *entry = chain_mod_find_target_entry(inst, fx_id, subkey);
                if (entry) {
                    chain_mod_apply_effective_value(inst, entry, 0);
                    inst->dirty = 1;
                    return;
                }
            }

            float fval;
            if (is_smoothable_float(val, &fval)) {
                chain_param_info_t *pinfo = find_param_info(inst->fx_params[fxi], inst->fx_param_counts[fxi], subkey);
                if (!pinfo || pinfo->type == KNOB_TYPE_FLOAT) {
                    smoother_set_target(&inst->fx_smoothers[fxi], subkey, fval);
                }
            }
            if (inst->fx_is_v2[fxi] && inst->fx_plugins_v2[fxi] && inst->fx_instances[fxi]) {
                inst->fx_plugins_v2[fxi]->set_param(inst->fx_instances[fxi], subkey, val);
            }
            if (strcmp(subkey, "plugin_id") == 0) {
                inst->fx_param_counts[fxi] = 0;
                inst->mod_param_refresh_ms_fx[fxi] = 0;
            }
            inst->dirty = 1;
        }
    }
    else if (chain_fx_index_from_key(key, "midi_fx", MAX_MIDI_FX, NULL) >= 0) {
        const char *subkey = NULL;
        int mfi = chain_fx_index_from_key(key, "midi_fx", MAX_MIDI_FX, &subkey);
        if (strcmp(subkey, "module") == 0) {
            /* The index is the SLOT, exactly as it is for "fxN:module" above.
             * This used to parse mfi and then discard it: the loader appended
             * at midi_fx_count, so on an empty chain "midi_fx4:module" and
             * "midi_fx1:module" were the same operation, and slot 1
             * additionally unloaded every other MIDI FX first. See
             * v2_load_midi_fx_slot in chain_midi.c for why slot 1 is no longer
             * special and what a caller shortening the chain must now do. */
            v2_load_midi_fx_slot(inst, mfi, val);
            inst->dirty = 1;
        } else if (inst->midi_fx_count > mfi && inst->midi_fx_plugins[mfi] && inst->midi_fx_instances[mfi]) {
            /* Dropped if the slot holds nothing — see the audio FX branch. */
            char mfx_id[16];
            chain_fx_component_id(mfx_id, sizeof(mfx_id), "midi_fx", mfi);
            if (chain_mod_is_target_active(inst, mfx_id, subkey)) {
                chain_mod_update_base_from_set_param(inst, mfx_id, subkey, val);
                mod_target_state_t *entry = chain_mod_find_target_entry(inst, mfx_id, subkey);
                if (entry) {
                    chain_mod_apply_effective_value(inst, entry, 0);
                    inst->dirty = 1;
                    return;
                }
            }
            inst->midi_fx_plugins[mfi]->set_param(inst->midi_fx_instances[mfi], subkey, val);
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
                /*
                 * Full depth, not half. An LFO you have just switched on should
                 * DO something — at 50% the effect was there but easy to miss,
                 * and the row-wide waveform now drawn on the LFO page reads as
                 * a half-height wave for no reason the user chose.
                 *
                 * The guard is what makes this safe: it fires only when depth is
                 * exactly 0 AND no target has been picked yet, i.e. a genuinely
                 * fresh LFO. Anything already configured, or restored from a
                 * saved slot, sets depth explicitly and is untouched.
                 */
                if (lfo->depth == 0.0f && !lfo->target[0] && !lfo->param[0]) {
                    lfo->depth = 1.0f;
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
                    /* Look up param info from the target's chain_params.
                     * knob_find_param parses the index out of the id, so
                     * fx4..fx8 / midi_fx3..midi_fx8 resolve too; the ladder
                     * this replaces stopped at fx3/midi_fx2 and silently left
                     * pinfo NULL (no step size, no range, no enum options). */
                    chain_param_info_t *pinfo = knob_find_param(inst, target, param);

                    /* Set mapping */
                    /* knob_N_set means "this knob has ONE whole-range
                     * destination, here" -- it collapses whatever the knob had
                     * before. That is what it has always meant for a knob with
                     * a single destination, and it is the contract the shadow
                     * UI and any external caller already rely on. */
                    if (found >= 0) {
                        /* Update existing (type/min/max looked up dynamically from pinfo) */
                        knob_mapping_t *km = &inst->knob_mappings[found];
                        knob_dest_assign(&km->dests[0], target, param);
                        km->dest_count = 1;
                        /* Remapped: this knob now means something else. */
                        km->last_cc_out = -1;
                        knob_emit_cc_out(inst, found);
                    } else if (inst->knob_mapping_count < MAX_KNOB_MAPPINGS) {
                        /* Add new */
                        int i = inst->knob_mapping_count++;
                        knob_mapping_t *km = &inst->knob_mappings[i];
                        memset(km, 0, sizeof(*km));   /* no inherited destinations */
                        km->cc = cc;
                        knob_dest_assign(&km->dests[0], target, param);
                        km->dest_count = 1;
                        km->dests[0].current_value = pinfo ? pinfo->default_val : 0.5f;
                        km->last_cc_out = -1;
                        knob_emit_cc_out(inst, i);
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
                        /* Blank the slot the shift vacated. It is past the
                         * count and so unread today, but a mapping added later
                         * lands on it, and only its first destination is
                         * written on that path -- any others would be
                         * inherited from whatever used to live here. */
                        memset(&inst->knob_mappings[inst->knob_mapping_count], 0,
                               sizeof(inst->knob_mappings[0]));
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
                        /*
                         * ONE detent per message, whatever `val` says.
                         *
                         * handleKnobTurn in shadow_ui.js sums every detent for
                         * this knob between frames, so `val` is an accumulated
                         * COUNT and reading only its sign discards a fast turn.
                         * Honouring the count would make the device's own
                         * encoders move further for the same gesture -- a
                         * change every existing user would feel on every chain
                         * knob, and not one to make in passing. The sign is
                         * what has always shipped here; max(1, accel) == accel,
                         * so this is bit-identical to it.
                         *
                         * 0.01f rather than KNOB_STEP_FLOAT is this path's
                         * historical fallback for a parameter that declares no
                         * step -- a 6.7x difference from the external-CC path
                         * on the same parameter, preserved for the same reason.
                         * See the note above chain_knob_accel.
                         */
                        knob_turn(inst, i, (delta_int > 0) ? 1 : -1, 0.01f);
                        knob_emit_cc_out(inst, i);
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
    /* Indexed for the same reason as the set_param side above: reading
     * "midi_fx2:bypassed" used to reach the plugin instead of our flag. */
    {
        const char *bsub = NULL;
        int bidx = chain_fx_index_from_key(key, "midi_fx", MAX_MIDI_FX, &bsub);
        if (bidx >= 0 && strcmp(bsub, "bypassed") == 0) {
            return snprintf(buf, buf_len, "%d", inst->midi_fx_bypassed[bidx] ? 1 : 0);
        }
        bidx = chain_fx_index_from_key(key, "fx", MAX_AUDIO_FX, &bsub);
        if (bidx >= 0 && strcmp(bsub, "bypassed") == 0) {
            return snprintf(buf, buf_len, "%d", inst->fx_bypassed[bidx] ? 1 : 0);
        }
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
    if (strcmp(key, "knob_cc_out") == 0) {
        return snprintf(buf, buf_len, "%d", inst->knob_cc_out ? 1 : 0);
    }
    if (strcmp(key, "midi_fx_pre_mode") == 0) {
        return snprintf(buf, buf_len, "%d", inst->midi_fx_pre_mode ? 1 : 0);
    }
    if (strcmp(key, "wants_sysex") == 0) {
        /* Does ANY component in this slot want raw SysEx?
         *
         * One answer per slot rather than per position, because SysEx carries
         * no channel and therefore nothing to route on -- there is no way to
         * address a position with it. The slot either has a component that
         * asked for SysEx or it does not, and the shim broadcasts to the ones
         * that did. A module distinguishes its own messages by manufacturer
         * ID, which is what that ID is for. */
        int want = inst->synth_wants_sysex;
        for (int i = 0; !want && i < inst->midi_fx_count; i++) {
            if (inst->midi_fx_wants_sysex[i]) want = 1;
        }
        return snprintf(buf, buf_len, "%d", want);
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
    /*
     * "fx<N>_module" / "midi_fx<N>_module" — what the editor asks to find out
     * what occupies a position. INDEXED, not enumerated: only fx1 and fx2 were
     * ever answered, so a third module loaded, ran and made sound while the
     * editor could not see it. `fx_count` said 3, `fx3_module` said nothing, an
     * unserved key reads back as "", and the reader drops a trailing empty.
     * Found on hardware 2026-08-20.
     */
    {
        int mi = chain_fx_index_from_suffixed(key, "midi_fx", MAX_MIDI_FX, "_module");
        if (mi >= 0) {
            return snprintf(buf, buf_len, "%s", inst->current_midi_fx_modules[mi]);
        }
        int fi = chain_fx_index_from_suffixed(key, "fx", MAX_AUDIO_FX, "_module");
        if (fi >= 0) {
            return snprintf(buf, buf_len, "%s", inst->current_fx_modules[fi]);
        }
    }
    if (strcmp(key, "midi_fx_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->midi_fx_count);
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
        /*
         * The patch's knob array, as FLAT ROWS.
         *
         * A knob with several destinations writes one ordinary object per
         * destination, all sharing its `cc`, distinguished by `dest`. Nothing
         * is nested, which matters: the parser below walks objects with a plain
         * scan for the next brace, and so does every build already in the
         * field. An older host reading this file sees several mappings on one
         * CC and uses the first, which is destination 0 -- it degrades to the
         * knob it would have had rather than to nonsense.
         *
         * `lo`/`hi` are written only when the destination is not whole-range,
         * and `dest`/`pos` only when there is more than one destination, so a
         * patch full of ordinary knobs re-saves BYTE-IDENTICAL to what shipped
         * before any of this. test_chain_patch_roundtrip asserts exactly that.
         *
         * Values are read back from the plugins rather than trusted from the
         * tracking copy, which may be stale if a parameter was changed through
         * the module's own UI or a state restore.
         */
        int off = 0;
        int rows = 0;
        off += snprintf(buf + off, buf_len - off, "[");
        for (int i = 0; i < inst->knob_mapping_count && i < MAX_KNOB_MAPPINGS; i++) {
            const knob_mapping_t *km = &inst->knob_mappings[i];
            int multi = knob_is_multi(km);

            for (int di = 0; di < km->dest_count && di < MAX_KNOB_DESTS; di++) {
                const char *target = km->dests[di].target;
                const char *param = km->dests[di].param;
                if (!param[0]) continue;
                float value = km->dests[di].current_value;

                /* Try to read actual value from DSP plugin.
                 * Indexed, not enumerated: this ladder stopped at fx2/midi_fx1, so
                 * a knob on fx3+ saved the STALE tracking value into the patch
                 * instead of the plugin's live one. The per-position plugin/
                 * instance checks stay -- fx_count is a high-water mark and an
                 * interior position can be empty. */
                char val_buf[64];
                int got = -1;
                if (strcmp(target, "synth") == 0 && inst->synth_plugin_v2 && inst->synth_instance) {
                    got = inst->synth_plugin_v2->get_param(inst->synth_instance, param, val_buf, sizeof(val_buf));
                } else {
                    int fxi = chain_fx_index_from_id(target, "fx", MAX_AUDIO_FX);
                    int mfi = chain_fx_index_from_id(target, "midi_fx", MAX_MIDI_FX);
                    if (fxi >= 0 && fxi < inst->fx_count &&
                        inst->fx_is_v2[fxi] && inst->fx_plugins_v2[fxi] && inst->fx_instances[fxi]) {
                        got = inst->fx_plugins_v2[fxi]->get_param(inst->fx_instances[fxi], param, val_buf, sizeof(val_buf));
                    } else if (mfi >= 0 && mfi < inst->midi_fx_count &&
                               inst->midi_fx_plugins[mfi] && inst->midi_fx_instances[mfi]) {
                        got = inst->midi_fx_plugins[mfi]->get_param(inst->midi_fx_instances[mfi], param, val_buf, sizeof(val_buf));
                    }
                }
                if (got > 0) {
                    chain_param_info_t *pinfo = find_param_by_key(inst, target, param);
                    value = dsp_value_to_float(val_buf, pinfo, value);
                }

                off += snprintf(buf + off, buf_len - off,
                    "%s{\"cc\":%d,\"target\":\"%s\",\"param\":\"%s\",\"value\":%.3f",
                    rows ? "," : "", km->cc, target, param, value);

                if (km->dests[di].lo != 0.0f || km->dests[di].hi != 1.0f) {
                    off += snprintf(buf + off, buf_len - off,
                        ",\"lo\":%.4f,\"hi\":%.4f", km->dests[di].lo, km->dests[di].hi);
                }
                if (multi) {
                    off += snprintf(buf + off, buf_len - off,
                        ",\"dest\":%d,\"pos\":%.4f", di, km->position);
                }
                off += snprintf(buf + off, buf_len - off, "}");
                rows++;

                /* Truncation would emit half an object, and JSON.parse in the
                 * shadow UI throws that into a silent catch -- the knob array
                 * would vanish from the saved patch with no error anywhere.
                 * Answer an empty array instead: visibly nothing, not
                 * invisibly broken. */
                if (off >= buf_len - 2) return snprintf(buf, buf_len, "[]");
            }
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
                    const char *target = inst->knob_mappings[i].dests[0].target;
                    const char *param = inst->knob_mappings[i].dests[0].param;
                    /* Indexed, not enumerated — see the knob_N_set site. This
                     * pinfo feeds the min/max/step/options answers below, so
                     * an unresolved fx4+ target made the whole knob undrivable. */
                    chain_param_info_t *pinfo = knob_find_param(inst, target, param);

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
                            return format_param_value(param_info, inst->knob_mappings[i].dests[0].current_value, buf, buf_len);
                        }
                        /* Fallback for params without metadata */
                        if (pinfo && pinfo->type == KNOB_TYPE_INT) {
                            return snprintf(buf, buf_len, "%d", (int)inst->knob_mappings[i].dests[0].current_value);
                        } else {
                            return snprintf(buf, buf_len, "%.2f", inst->knob_mappings[i].dests[0].current_value);
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
        int eff_result = chain_mod_get_effective_for_subkey(inst, "synth", subkey, buf, buf_len);
        if (eff_result >= 0) return eff_result;
        /* A plain read of an actively modulated key answers with the BASE —
         * the plugin holds the effective value the overlay keeps writing into
         * it, which is not what the user set (#276). */
        int plain_base = chain_mod_get_base_for_plain_key(inst, "synth", subkey, buf, buf_len);
        if (plain_base >= 0) return plain_base;

        /* Return synth's default forward channel from module.json capabilities */
        if (strcmp(subkey, "default_forward_channel") == 0) {
            return snprintf(buf, buf_len, "%d", inst->synth_default_forward_channel);
        }

        /* Whether this synth pulls audio in from line-in/mic (feedback risk on
         * boot). Parsed from module.json capabilities at synth load. */
        if (strcmp(subkey, "consumes_line_input") == 0) {
            return snprintf(buf, buf_len, "%d", inst->synth_consumes_line_input);
        }

        /* MIDI note last played into the synth, or -1. Resolved against the
         * module's declared voices by whoever holds that list. */
        if (strcmp(subkey, "last_note") == 0) {
            return snprintf(buf, buf_len, "%d", inst->synth_last_note);
        }

        /* For chain_params: try plugin first, fall back to parsed module.json data */
        if (strcmp(subkey, "chain_params") == 0) {
            /* Try plugin's own chain_params handler first */
            if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->get_param) {
                int result = inst->synth_plugin_v2->get_param(inst->synth_instance, subkey, buf, buf_len);
                if (chain_params_answer_is_useful(buf, result)) return result;  /* Plugin provided chain_params */
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

    /* Route fx<N>: prefixed params to that audio FX slot (strip prefix) */
    if (chain_fx_index_from_key(key, "fx", MAX_AUDIO_FX, NULL) >= 0) {
        const char *subkey = NULL;
        int fxi = chain_fx_index_from_key(key, "fx", MAX_AUDIO_FX, &subkey);
        char fx_id[16];
        chain_fx_component_id(fx_id, sizeof(fx_id), "fx", fxi);
        int base_result = chain_mod_get_base_for_subkey(inst, fx_id, subkey, buf, buf_len);
        if (base_result >= 0) return base_result;
        int mod_result = chain_mod_get_modulated_for_subkey(inst, fx_id, subkey, buf, buf_len);
        if (mod_result >= 0) return mod_result;
        int eff_result = chain_mod_get_effective_for_subkey(inst, fx_id, subkey, buf, buf_len);
        if (eff_result >= 0) return eff_result;
        int plain_base = chain_mod_get_base_for_plain_key(inst, fx_id, subkey, buf, buf_len);
        if (plain_base >= 0) return plain_base;

        /* For ui_hierarchy: return cached JSON from module.json, fall through to plugin if empty */
        if (strcmp(subkey, "ui_hierarchy") == 0 && inst->fx_count > fxi) {
            if (inst->fx_ui_hierarchy[fxi][0]) {
                int len = strlen(inst->fx_ui_hierarchy[fxi]);
                if (len < buf_len) {
                    strcpy(buf, inst->fx_ui_hierarchy[fxi]);
                    return len;
                }
            }
            /* Cache empty - fall through to plugin get_param below */
        }

        /* For chain_params: try plugin first, fall back to parsed module.json data */
        if (strcmp(subkey, "chain_params") == 0 && inst->fx_count > fxi) {
            /* Try plugin's own chain_params handler first */
            if (inst->fx_is_v2[fxi] && inst->fx_plugins_v2[fxi] && inst->fx_instances[fxi] && inst->fx_plugins_v2[fxi]->get_param) {
                int result = inst->fx_plugins_v2[fxi]->get_param(inst->fx_instances[fxi], subkey, buf, buf_len);
                if (chain_params_answer_is_useful(buf, result)) return result;
            }
            /* Fall back to parsed module.json data */
            if (inst->fx_param_counts[fxi] > 0) {
                int offset = 0;
                offset += snprintf(buf + offset, buf_len - offset, "[");
                for (int i = 0; i < inst->fx_param_counts[fxi] && offset < buf_len - 100; i++) {
                    chain_param_info_t *p = &inst->fx_params[fxi][i];
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

        if (inst->fx_count > fxi) {
            if (inst->fx_is_v2[fxi] && inst->fx_plugins_v2[fxi] && inst->fx_instances[fxi] && inst->fx_plugins_v2[fxi]->get_param) {
                return inst->fx_plugins_v2[fxi]->get_param(inst->fx_instances[fxi], subkey, buf, buf_len);
            }
        }
        return -1;
    }

    /* Route midi_fx<N>: prefixed params to that MIDI FX slot (strip prefix) */
    if (chain_fx_index_from_key(key, "midi_fx", MAX_MIDI_FX, NULL) >= 0) {
        const char *subkey = NULL;
        int mfi = chain_fx_index_from_key(key, "midi_fx", MAX_MIDI_FX, &subkey);
        char mfx_id[16];
        chain_fx_component_id(mfx_id, sizeof(mfx_id), "midi_fx", mfi);
        int base_result = chain_mod_get_base_for_subkey(inst, mfx_id, subkey, buf, buf_len);
        if (base_result >= 0) return base_result;
        int mod_result = chain_mod_get_modulated_for_subkey(inst, mfx_id, subkey, buf, buf_len);
        if (mod_result >= 0) return mod_result;
        int eff_result = chain_mod_get_effective_for_subkey(inst, mfx_id, subkey, buf, buf_len);
        if (eff_result >= 0) return eff_result;
        int plain_base = chain_mod_get_base_for_plain_key(inst, mfx_id, subkey, buf, buf_len);
        if (plain_base >= 0) return plain_base;
        /* For ui_hierarchy: return cached JSON from module.json, fall through to plugin if empty */
        if (strcmp(subkey, "ui_hierarchy") == 0 && inst->midi_fx_count > mfi) {
            if (inst->midi_fx_ui_hierarchy[mfi][0]) {
                int len = strlen(inst->midi_fx_ui_hierarchy[mfi]);
                if (len < buf_len) {
                    strcpy(buf, inst->midi_fx_ui_hierarchy[mfi]);
                    return len;
                }
            }
            /* Cache empty - fall through to plugin get_param below */
        }
        /* For chain_params: try plugin first, fall back to parsed module.json data */
        if (strcmp(subkey, "chain_params") == 0 && inst->midi_fx_count > mfi) {
            /* Try plugin's own chain_params handler first */
            if (inst->midi_fx_plugins[mfi] && inst->midi_fx_instances[mfi] && inst->midi_fx_plugins[mfi]->get_param) {
                int result = inst->midi_fx_plugins[mfi]->get_param(inst->midi_fx_instances[mfi], subkey, buf, buf_len);
                if (chain_params_answer_is_useful(buf, result)) return result;
            }
            /* Fall back to parsed module.json data */
            if (inst->midi_fx_param_counts[mfi] > 0) {
                int written = snprintf(buf, buf_len, "[");
                for (int i = 0; i < inst->midi_fx_param_counts[mfi] && written < buf_len - 10; i++) {
                    chain_param_info_t *p = &inst->midi_fx_params[mfi][i];
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
        if (inst->midi_fx_count > mfi && inst->midi_fx_plugins[mfi] && inst->midi_fx_instances[mfi] && inst->midi_fx_plugins[mfi]->get_param) {
            return inst->midi_fx_plugins[mfi]->get_param(inst->midi_fx_instances[mfi], subkey, buf, buf_len);
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

        /* Phase: when a transport is running, lock to song position (writing
         * lfo->phase keeps continuity — on stop, free-run resumes from the
         * locked phase instead of jumping). Otherwise free-run as before. */
        double bp = -1.0;
        if (lfo->sync && inst->host && inst->host->get_beat_position)
            bp = inst->host->get_beat_position();
        if (lfo->sync && bp >= 0.0) {
            lfo->phase = lfo_synced_phase(bp, lfo->rate_div);
        } else {
            float rate_hz;
            if (lfo->sync) {
                float bpm = 120.0f;
                if (inst->host && inst->host->get_bpm) bpm = inst->host->get_bpm();
                rate_hz = lfo_sync_rate_hz(bpm, lfo->rate_div);
            } else {
                rate_hz = lfo->rate_hz;
            }
            lfo->phase = lfo_advance_phase(lfo->phase, rate_hz, frames, sample_rate);
        }

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
