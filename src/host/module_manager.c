/*
 * Module Manager - discovers, loads, and manages DSP modules
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include "module_manager.h"

/* Simple JSON parsing helpers (minimal, for module.json only) */
static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    pos = strchr(pos + strlen(search), ':');
    if (!pos) return -1;

    /* Skip whitespace */
    while (*pos && (*pos == ':' || *pos == ' ' || *pos == '\t' || *pos == '\n')) pos++;

    if (*pos != '"') return -1;
    pos++; /* skip opening quote */

    int i = 0;
    while (*pos && *pos != '"' && i < out_len - 1) {
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return i;
}

static int json_get_int(const char *json, const char *key, int *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    pos = strchr(pos + strlen(search), ':');
    if (!pos) return -1;

    /* Skip whitespace */
    while (*pos && (*pos == ':' || *pos == ' ' || *pos == '\t' || *pos == '\n')) pos++;

    *out = atoi(pos);
    return 0;
}

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

/* Extract "defaults" object as raw JSON string */
static int json_get_defaults(const char *json, char *out, int out_len) {
    const char *pos = strstr(json, "\"defaults\"");
    if (!pos) return -1;

    pos = strchr(pos, '{');
    if (!pos) return -1;

    int depth = 0;
    int i = 0;
    while (*pos && i < out_len - 1) {
        out[i++] = *pos;
        if (*pos == '{') depth++;
        if (*pos == '}') {
            depth--;
            if (depth == 0) break;
        }
        pos++;
    }
    out[i] = '\0';
    return (depth == 0) ? i : -1;
}

/* Host log callback */
static void host_log(const char *msg) {
    printf("[plugin] %s\n", msg);
}

/* Parse a single module.json file */
static int parse_module_json(const char *module_dir, module_info_t *info) {
    char json_path[MAX_PATH_LEN];
    snprintf(json_path, sizeof(json_path), "%s/module.json", module_dir);

    FILE *f = fopen(json_path, "r");
    if (!f) {
        printf("mm: cannot open %s\n", json_path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len > 8192) {
        printf("mm: module.json too large: %s\n", json_path);
        fclose(f);
        return -1;
    }

    char *json = malloc(len + 1);
    if (!json) {
        fclose(f);
        return -1;
    }

    fread(json, 1, len, f);
    json[len] = '\0';
    fclose(f);

    memset(info, 0, sizeof(*info));
    strncpy(info->module_dir, module_dir, MAX_PATH_LEN - 1);

    /* Parse required fields */
    if (json_get_string(json, "id", info->id, MAX_MODULE_ID_LEN) < 0) {
        printf("mm: missing 'id' in %s\n", json_path);
        free(json);
        return -1;
    }

    json_get_string(json, "name", info->name, MAX_MODULE_NAME_LEN);
    if (info->name[0] == '\0') {
        strncpy(info->name, info->id, MAX_MODULE_NAME_LEN - 1);
    }

    json_get_string(json, "version", info->version, sizeof(info->version));

    /* UI and DSP paths */
    char ui_file[128] = "ui.js";
    char dsp_file[128] = "dsp.so";
    json_get_string(json, "ui", ui_file, sizeof(ui_file));
    json_get_string(json, "dsp", dsp_file, sizeof(dsp_file));

    snprintf(info->ui_script, MAX_PATH_LEN, "%s/%s", module_dir, ui_file);
    snprintf(info->dsp_path, MAX_PATH_LEN, "%s/%s", module_dir, dsp_file);

    /* API version */
    info->api_version = 1;
    json_get_int(json, "api_version", &info->api_version);

    /* Capabilities */
    json_get_bool(json, "audio_out", &info->cap_audio_out);
    json_get_bool(json, "audio_in", &info->cap_audio_in);
    json_get_bool(json, "midi_in", &info->cap_midi_in);
    json_get_bool(json, "midi_out", &info->cap_midi_out);
    json_get_bool(json, "aftertouch", &info->cap_aftertouch);
    json_get_bool(json, "claims_master_knob", &info->cap_claims_master_knob);
    json_get_bool(json, "raw_midi", &info->cap_raw_midi);
    json_get_bool(json, "raw_ui", &info->cap_raw_ui);

    /* Component type for categorization (sound_generator, audio_fx, midi_fx, utility, etc.) */
    json_get_string(json, "component_type", info->component_type, sizeof(info->component_type));

    /* Defaults */
    json_get_defaults(json, info->defaults_json, sizeof(info->defaults_json));

    free(json);

    printf("mm: parsed module '%s' (%s) v%s\n", info->name, info->id, info->version);
    return 0;
}

void mm_init(module_manager_t *mm, uint8_t *mapped_memory,
             int (*midi_send_internal)(const uint8_t*, int),
             int (*midi_send_external)(const uint8_t*, int)) {
    memset(mm, 0, sizeof(*mm));
    mm->current_module_index = -1;
    mm->dsp_handle = NULL;
    mm->plugin = NULL;
    mm->host_volume = 100;  /* Default to full volume */

    /* Initialize host API */
    mm->host_api.api_version = MOVE_PLUGIN_API_VERSION;
    mm->host_api.sample_rate = MOVE_SAMPLE_RATE;
    mm->host_api.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    mm->host_api.mapped_memory = mapped_memory;
    mm->host_api.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    mm->host_api.audio_in_offset = MOVE_AUDIO_IN_OFFSET;
    mm->host_api.log = host_log;
    mm->host_api.midi_send_internal = midi_send_internal;
    mm->host_api.midi_send_external = midi_send_external;
}

/* Helper to scan a single directory for modules */
static int scan_directory(module_manager_t *mm, const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return 0;  /* Not an error - directory may not exist */
    }

    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && mm->module_count < MAX_MODULES) {
        if (entry->d_name[0] == '.') continue;

        char module_path[MAX_PATH_LEN];
        snprintf(module_path, sizeof(module_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(module_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        /* Check for module.json */
        char json_path[MAX_PATH_LEN];
        snprintf(json_path, sizeof(json_path), "%s/module.json", module_path);
        if (stat(json_path, &st) != 0) continue;

        if (parse_module_json(module_path, &mm->modules[mm->module_count]) == 0) {
            mm->module_count++;
            found++;
        }
    }

    closedir(dir);
    return found;
}

int mm_scan_modules(module_manager_t *mm, const char *modules_dir) {
    mm->module_count = 0;

    /* Scan main modules directory */
    int main_count = scan_directory(mm, modules_dir);
    if (main_count < 0) {
        printf("mm: cannot open modules directory: %s\n", modules_dir);
    }

    /* Scan component subdirectories */
    char subdir[MAX_PATH_LEN];

    snprintf(subdir, sizeof(subdir), "%s/sound_generators", modules_dir);
    scan_directory(mm, subdir);

    snprintf(subdir, sizeof(subdir), "%s/audio_fx", modules_dir);
    scan_directory(mm, subdir);

    snprintf(subdir, sizeof(subdir), "%s/midi_fx", modules_dir);
    scan_directory(mm, subdir);

    snprintf(subdir, sizeof(subdir), "%s/utilities", modules_dir);
    scan_directory(mm, subdir);

    snprintf(subdir, sizeof(subdir), "%s/other", modules_dir);
    scan_directory(mm, subdir);

    printf("mm: found %d modules\n", mm->module_count);
    return mm->module_count;
}

int mm_get_module_count(module_manager_t *mm) {
    return mm->module_count;
}

const module_info_t* mm_get_module_info(module_manager_t *mm, int index) {
    if (index < 0 || index >= mm->module_count) return NULL;
    return &mm->modules[index];
}

int mm_find_module(module_manager_t *mm, const char *module_id) {
    for (int i = 0; i < mm->module_count; i++) {
        if (strcmp(mm->modules[i].id, module_id) == 0) {
            return i;
        }
    }
    return -1;
}

int mm_load_module(module_manager_t *mm, int index) {
    if (index < 0 || index >= mm->module_count) {
        printf("mm: invalid module index %d\n", index);
        return -1;
    }

    /* Unload any current module first */
    mm_unload_module(mm);

    module_info_t *info = &mm->modules[index];

    /* Check API version - support both v1 and v2 */
    if (info->api_version != MOVE_PLUGIN_API_VERSION &&
        info->api_version != MOVE_PLUGIN_API_VERSION_2) {
        printf("mm: module '%s' requires API v%d, host supports v%d and v%d\n",
               info->id, info->api_version, MOVE_PLUGIN_API_VERSION, MOVE_PLUGIN_API_VERSION_2);
        return -1;
    }

    /* Check if DSP exists - DSP is optional for UI-only modules */
    struct stat st;
    int has_dsp = (stat(info->dsp_path, &st) == 0);

    if (has_dsp) {
        /* Load DSP plugin */
        printf("mm: loading DSP plugin: %s\n", info->dsp_path);
        mm->dsp_handle = dlopen(info->dsp_path, RTLD_NOW | RTLD_LOCAL);
        if (!mm->dsp_handle) {
            printf("mm: dlopen failed: %s\n", dlerror());
            return -1;
        }

        const char *defaults = info->defaults_json[0] ? info->defaults_json : NULL;
        int plugin_loaded = 0;

        /* Try v2 API first */
        move_plugin_init_v2_fn init_v2 = (move_plugin_init_v2_fn)dlsym(mm->dsp_handle, MOVE_PLUGIN_INIT_V2_SYMBOL);
        if (init_v2) {
            mm->plugin_v2 = init_v2(&mm->host_api);
            if (mm->plugin_v2 && mm->plugin_v2->api_version == MOVE_PLUGIN_API_VERSION_2) {
                mm->plugin_instance = mm->plugin_v2->create_instance(info->module_dir, defaults);
                if (mm->plugin_instance) {
                    printf("mm: loaded v2 plugin for '%s'\n", info->id);
                    plugin_loaded = 1;
                } else {
                    printf("mm: v2 create_instance failed, trying v1\n");
                    mm->plugin_v2 = NULL;
                }
            } else {
                mm->plugin_v2 = NULL;
            }
        }

        /* Fall back to v1 API */
        if (!plugin_loaded) {
            move_plugin_init_v1_fn init_fn = (move_plugin_init_v1_fn)dlsym(mm->dsp_handle, MOVE_PLUGIN_INIT_SYMBOL);
            if (!init_fn) {
                printf("mm: dlsym failed for %s: %s\n", MOVE_PLUGIN_INIT_SYMBOL, dlerror());
                dlclose(mm->dsp_handle);
                mm->dsp_handle = NULL;
                return -1;
            }

            /* Initialize plugin */
            mm->plugin = init_fn(&mm->host_api);
            if (!mm->plugin) {
                printf("mm: plugin init returned NULL\n");
                dlclose(mm->dsp_handle);
                mm->dsp_handle = NULL;
                return -1;
            }

            /* Verify plugin API version */
            if (mm->plugin->api_version != MOVE_PLUGIN_API_VERSION) {
                printf("mm: plugin reports API v%d, expected v%d\n",
                       mm->plugin->api_version, MOVE_PLUGIN_API_VERSION);
                dlclose(mm->dsp_handle);
                mm->dsp_handle = NULL;
                mm->plugin = NULL;
                return -1;
            }

            /* Call on_load */
            if (mm->plugin->on_load) {
                int ret = mm->plugin->on_load(info->module_dir, defaults);
                if (ret != 0) {
                    printf("mm: plugin on_load failed with %d\n", ret);
                    dlclose(mm->dsp_handle);
                    mm->dsp_handle = NULL;
                    mm->plugin = NULL;
                    return -1;
                }
            }
            printf("mm: loaded v1 plugin for '%s'\n", info->id);
        }
    } else {
        printf("mm: no DSP plugin for module '%s' (UI-only)\n", info->id);
    }

    mm->current_module_index = index;
    printf("mm: module '%s' loaded successfully\n", info->name);
    return 0;
}

int mm_load_module_by_id(module_manager_t *mm, const char *module_id) {
    int index = mm_find_module(mm, module_id);
    if (index < 0) {
        printf("mm: module not found: %s\n", module_id);
        return -1;
    }
    return mm_load_module(mm, index);
}

void mm_unload_module(module_manager_t *mm) {
    /* Clean up v2 plugin */
    if (mm->plugin_v2 && mm->plugin_instance) {
        if (mm->plugin_v2->destroy_instance) {
            mm->plugin_v2->destroy_instance(mm->plugin_instance);
        }
        mm->plugin_instance = NULL;
        mm->plugin_v2 = NULL;
    }

    /* Clean up v1 plugin */
    if (mm->plugin && mm->plugin->on_unload) {
        mm->plugin->on_unload();
    }
    mm->plugin = NULL;

    if (mm->dsp_handle) {
        dlclose(mm->dsp_handle);
        mm->dsp_handle = NULL;
    }

    mm->current_module_index = -1;
}

int mm_is_module_loaded(module_manager_t *mm) {
    return mm->current_module_index >= 0;
}

const module_info_t* mm_get_current_module(module_manager_t *mm) {
    if (mm->current_module_index < 0) return NULL;
    return &mm->modules[mm->current_module_index];
}

void mm_on_midi(module_manager_t *mm, const uint8_t *msg, int len, int source) {
    if (mm->plugin_v2 && mm->plugin_instance && mm->plugin_v2->on_midi) {
        mm->plugin_v2->on_midi(mm->plugin_instance, msg, len, source);
    } else if (mm->plugin && mm->plugin->on_midi) {
        mm->plugin->on_midi(msg, len, source);
    }
}

void mm_set_param(module_manager_t *mm, const char *key, const char *val) {
    if (mm->plugin_v2 && mm->plugin_instance && mm->plugin_v2->set_param) {
        mm->plugin_v2->set_param(mm->plugin_instance, key, val);
    } else if (mm->plugin && mm->plugin->set_param) {
        mm->plugin->set_param(key, val);
    }
}

int mm_get_param(module_manager_t *mm, const char *key, char *buf, int buf_len) {
    if (mm->plugin_v2 && mm->plugin_instance && mm->plugin_v2->get_param) {
        return mm->plugin_v2->get_param(mm->plugin_instance, key, buf, buf_len);
    } else if (mm->plugin && mm->plugin->get_param) {
        return mm->plugin->get_param(key, buf, buf_len);
    }
    return -1;
}

void mm_render_block(module_manager_t *mm) {
    if (mm->plugin_v2 && mm->plugin_instance && mm->plugin_v2->render_block) {
        mm->plugin_v2->render_block(mm->plugin_instance, mm->audio_out_buffer, MOVE_FRAMES_PER_BLOCK);
    } else if (mm->plugin && mm->plugin->render_block) {
        mm->plugin->render_block(mm->audio_out_buffer, MOVE_FRAMES_PER_BLOCK);
    } else {
        /* No plugin or no render function - output silence */
        memset(mm->audio_out_buffer, 0, sizeof(mm->audio_out_buffer));
    }

    /* Apply host volume if not at 100% */
    if (mm->host_volume < 100) {
        for (int i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++) {
            mm->audio_out_buffer[i] = (int16_t)((mm->audio_out_buffer[i] * mm->host_volume) / 100);
        }
    }

    /* Write to mailbox */
    if (mm->host_api.mapped_memory) {
        int16_t *dst = (int16_t *)(mm->host_api.mapped_memory + MOVE_AUDIO_OUT_OFFSET);
        memcpy(dst, mm->audio_out_buffer, MOVE_FRAMES_PER_BLOCK * 2 * sizeof(int16_t));
    }
}

void mm_set_host_volume(module_manager_t *mm, int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    mm->host_volume = volume;
}

int mm_get_host_volume(module_manager_t *mm) {
    return mm->host_volume;
}

int mm_module_claims_master_knob(module_manager_t *mm) {
    if (mm->current_module_index < 0) return 0;
    return mm->modules[mm->current_module_index].cap_claims_master_knob;
}

int mm_module_wants_raw_midi(module_manager_t *mm) {
    if (mm->current_module_index < 0) return 0;
    return mm->modules[mm->current_module_index].cap_raw_midi;
}

int mm_module_wants_raw_ui(module_manager_t *mm) {
    if (mm->current_module_index < 0) return 0;
    return mm->modules[mm->current_module_index].cap_raw_ui;
}

void mm_destroy(module_manager_t *mm) {
    mm_unload_module(mm);
    memset(mm, 0, sizeof(*mm));
    mm->current_module_index = -1;
}
