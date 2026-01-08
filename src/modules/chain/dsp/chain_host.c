/*
 * Signal Chain Host DSP Plugin
 *
 * Orchestrates a signal chain: Input → MIDI FX → Sound Generator → Audio FX → Output
 * Phase 2: Patch files with synth module selection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <dirent.h>

#include "host/plugin_api_v1.h"

/* Maximum patches we can track */
#define MAX_PATCHES 32
#define MAX_PATH_LEN 256
#define MAX_NAME_LEN 64

/* Patch info */
typedef struct {
    char name[MAX_NAME_LEN];
    char path[MAX_PATH_LEN];
    char synth_module[MAX_NAME_LEN];
    int synth_preset;
} patch_info_t;

/* Host API provided by main host */
static const host_api_v1_t *g_host = NULL;

/* Sub-plugin state */
static void *g_synth_handle = NULL;
static plugin_api_v1_t *g_synth_plugin = NULL;
static char g_current_synth_module[MAX_NAME_LEN] = "";

/* Patch state */
static patch_info_t g_patches[MAX_PATCHES];
static int g_patch_count = 0;
static int g_current_patch = 0;
static char g_module_dir[MAX_PATH_LEN] = "";

/* Our host API for sub-plugins (forwards to main host) */
static host_api_v1_t g_subplugin_host_api;

/* Plugin API we return to host */
static plugin_api_v1_t g_plugin_api;

/* Logging helper */
static void chain_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[chain] %s", msg);
        g_host->log(buf);
    } else {
        printf("[chain] %s\n", msg);
    }
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

    /* Get init function */
    move_plugin_init_v1_fn init_fn = (move_plugin_init_v1_fn)dlsym(g_synth_handle, MOVE_PLUGIN_INIT_SYMBOL);
    if (!init_fn) {
        snprintf(msg, sizeof(msg), "dlsym failed: %s", dlerror());
        chain_log(msg);
        dlclose(g_synth_handle);
        g_synth_handle = NULL;
        return -1;
    }

    /* Initialize sub-plugin with our forwarding host API */
    g_synth_plugin = init_fn(&g_subplugin_host_api);
    if (!g_synth_plugin) {
        chain_log("Synth plugin init returned NULL");
        dlclose(g_synth_handle);
        g_synth_handle = NULL;
        return -1;
    }

    /* Verify API version */
    if (g_synth_plugin->api_version != MOVE_PLUGIN_API_VERSION) {
        snprintf(msg, sizeof(msg), "Synth API version mismatch: %d vs %d",
                 g_synth_plugin->api_version, MOVE_PLUGIN_API_VERSION);
        chain_log(msg);
        dlclose(g_synth_handle);
        g_synth_handle = NULL;
        g_synth_plugin = NULL;
        return -1;
    }

    /* Call on_load */
    if (g_synth_plugin->on_load) {
        int ret = g_synth_plugin->on_load(module_path, config_json);
        if (ret != 0) {
            snprintf(msg, sizeof(msg), "Synth on_load failed: %d", ret);
            chain_log(msg);
            dlclose(g_synth_handle);
            g_synth_handle = NULL;
            g_synth_plugin = NULL;
            return -1;
        }
    }

    chain_log("Synth loaded successfully");
    return 0;
}

/* Unload synth sub-plugin */
static void unload_synth(void) {
    if (g_synth_plugin && g_synth_plugin->on_unload) {
        g_synth_plugin->on_unload();
    }
    if (g_synth_handle) {
        dlclose(g_synth_handle);
        g_synth_handle = NULL;
    }
    g_synth_plugin = NULL;
    g_current_synth_module[0] = '\0';
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

    if (size > 4096) {
        fclose(f);
        chain_log("Patch file too large");
        return -1;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return -1;
    }

    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);

    /* Parse fields */
    strncpy(patch->path, path, MAX_PATH_LEN - 1);

    if (json_get_string(json, "name", patch->name, MAX_NAME_LEN) != 0) {
        strcpy(patch->name, "Unnamed");
    }

    if (json_get_string(json, "module", patch->synth_module, MAX_NAME_LEN) != 0) {
        strcpy(patch->synth_module, "sf2");  /* Default to SF2 */
    }

    if (json_get_int(json, "preset", &patch->synth_preset) != 0) {
        patch->synth_preset = 0;
    }

    free(json);

    snprintf(msg, sizeof(msg), "Parsed patch: %s -> %s preset %d",
             patch->name, patch->synth_module, patch->synth_preset);
    chain_log(msg);

    return 0;
}

/* Scan patches directory and populate patch list */
static int scan_patches(const char *module_dir) {
    char patches_dir[MAX_PATH_LEN];
    char msg[256];

    snprintf(patches_dir, sizeof(patches_dir), "%s/patches", module_dir);

    snprintf(msg, sizeof(msg), "Scanning patches in: %s", patches_dir);
    chain_log(msg);

    DIR *dir = opendir(patches_dir);
    if (!dir) {
        chain_log("No patches directory found");
        return 0;
    }

    g_patch_count = 0;
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

    snprintf(msg, sizeof(msg), "Found %d patches", g_patch_count);
    chain_log(msg);

    return g_patch_count;
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

    /* Check if we need to switch synth modules */
    if (strcmp(g_current_synth_module, patch->synth_module) != 0) {
        /* Unload current synth */
        unload_synth();

        /* Build path to new synth module */
        char synth_path[MAX_PATH_LEN];
        strncpy(synth_path, g_module_dir, sizeof(synth_path) - 1);
        char *last_slash = strrchr(synth_path, '/');
        if (last_slash) {
            snprintf(last_slash + 1, sizeof(synth_path) - (last_slash - synth_path) - 1,
                     "%s", patch->synth_module);
        }

        /* Load new synth */
        if (load_synth(synth_path, NULL) != 0) {
            snprintf(msg, sizeof(msg), "Failed to load synth: %s", patch->synth_module);
            chain_log(msg);
            return -1;
        }

        strncpy(g_current_synth_module, patch->synth_module, MAX_NAME_LEN - 1);
    }

    /* Set preset on synth */
    if (g_synth_plugin && g_synth_plugin->set_param) {
        char preset_str[16];
        snprintf(preset_str, sizeof(preset_str), "%d", patch->synth_preset);
        g_synth_plugin->set_param("preset", preset_str);
    }

    g_current_patch = index;

    snprintf(msg, sizeof(msg), "Loaded patch %d: %s", index, patch->name);
    chain_log(msg);

    return 0;
}

/* === Plugin API Implementation === */

static int plugin_on_load(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Chain host loading from: %s", module_dir);
    chain_log(msg);

    /* Store module directory for later use */
    strncpy(g_module_dir, module_dir, MAX_PATH_LEN - 1);

    /* Scan for patches */
    scan_patches(module_dir);

    if (g_patch_count > 0) {
        /* Load first patch */
        if (load_patch(0) != 0) {
            chain_log("Failed to load first patch, falling back to SF2");
            goto fallback;
        }
    } else {
        chain_log("No patches found, using default SF2");
        goto fallback;
    }

    chain_log("Chain host initialized with patches");
    return 0;

fallback:
    /* Fallback: Load SF2 directly */
    {
        char synth_path[MAX_PATH_LEN];
        strncpy(synth_path, module_dir, sizeof(synth_path) - 1);
        char *last_slash = strrchr(synth_path, '/');
        if (last_slash) {
            strcpy(last_slash + 1, "sf2");
        } else {
            strcpy(synth_path, "modules/sf2");
        }

        if (load_synth(synth_path, NULL) != 0) {
            chain_log("Failed to load SF2 synth");
            return -1;
        }
        strncpy(g_current_synth_module, "sf2", MAX_NAME_LEN - 1);
    }

    chain_log("Chain host initialized (fallback)");
    return 0;
}

static void plugin_on_unload(void) {
    chain_log("Chain host unloading");
    unload_synth();
}

static void plugin_on_midi(const uint8_t *msg, int len, int source) {
    /* Forward MIDI to synth */
    if (g_synth_plugin && g_synth_plugin->on_midi) {
        g_synth_plugin->on_midi(msg, len, source);
    }
}

static void plugin_set_param(const char *key, const char *val) {
    /* Handle chain-level params */
    if (strcmp(key, "patch") == 0) {
        int index = atoi(val);
        load_patch(index);
        return;
    }
    if (strcmp(key, "next_patch") == 0) {
        int next = (g_current_patch + 1) % g_patch_count;
        if (g_patch_count > 0) load_patch(next);
        return;
    }
    if (strcmp(key, "prev_patch") == 0) {
        int prev = (g_current_patch - 1 + g_patch_count) % g_patch_count;
        if (g_patch_count > 0) load_patch(prev);
        return;
    }

    /* Forward to synth */
    if (g_synth_plugin && g_synth_plugin->set_param) {
        g_synth_plugin->set_param(key, val);
    }
}

static int plugin_get_param(const char *key, char *buf, int buf_len) {
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
    if (strcmp(key, "synth_module") == 0) {
        snprintf(buf, buf_len, "%s", g_current_synth_module);
        return 0;
    }

    /* Forward to synth */
    if (g_synth_plugin && g_synth_plugin->get_param) {
        return g_synth_plugin->get_param(key, buf, buf_len);
    }
    return -1;
}

static void plugin_render_block(int16_t *out_interleaved_lr, int frames) {
    if (g_synth_plugin && g_synth_plugin->render_block) {
        /* Get audio from synth */
        g_synth_plugin->render_block(out_interleaved_lr, frames);

        /* TODO Phase 3: Process through audio FX chain here */
    } else {
        /* No synth loaded - output silence */
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
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
