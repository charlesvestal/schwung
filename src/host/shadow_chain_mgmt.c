/* shadow_chain_mgmt.c - Chain management, master FX, param handling, boot init
 * Extracted from move_anything_shim.c for maintainability. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <math.h>
#include <strings.h>  /* strcasecmp */

#include "shadow_chain_mgmt.h"
#include "shadow_set_pages.h"
#include "shadow_sampler.h"
#include "shadow_dbus.h"
#include "unified_log.h"

/* ============================================================================
 * Globals
 * ============================================================================ */

/* Chain slot state */
shadow_chain_slot_t shadow_chain_slots[SHADOW_CHAIN_INSTANCES];
volatile int shadow_solo_count = 0;
const char *shadow_chain_default_patches[SHADOW_CHAIN_INSTANCES] = {
    "",  /* No default patch - user must select */
    "",
    "",
    ""
};

/* Chain DSP plugin state */
void *shadow_dsp_handle = NULL;
const plugin_api_v2_t *shadow_plugin_v2 = NULL;
void (*shadow_chain_set_inject_audio)(void *instance, int16_t *buf, int frames) = NULL;
void (*shadow_chain_set_external_fx_mode)(void *instance, int mode) = NULL;
void (*shadow_chain_process_fx)(void *instance, int16_t *buf, int frames) = NULL;
host_api_v1_t shadow_host_api;
int shadow_inprocess_ready = 0;

/* Master FX slots */
master_fx_slot_t shadow_master_fx_slots[MASTER_FX_SLOTS];

/* MIDI out log file */
FILE *shadow_midi_out_log = NULL;

/* ============================================================================
 * Static state
 * ============================================================================ */

static chain_mgmt_host_t host;
static volatile int chain_mgmt_initialized = 0;

/* UI request tracking */
static uint32_t shadow_ui_request_seen = 0;

/* ============================================================================
 * Initialization
 * ============================================================================ */

void chain_mgmt_init(const chain_mgmt_host_t *h) {
    host = *h;
    chain_mgmt_initialized = 1;
}

/* ============================================================================
 * Logging
 * ============================================================================ */

void shadow_log(const char *msg) {
    unified_log("shim", LOG_LEVEL_DEBUG, "%s", msg ? msg : "(null)");
}

int shadow_inprocess_log_enabled(void) {
    static int enabled = -1;
    static int check_counter = 0;
    if (enabled < 0 || (check_counter++ % 200 == 0)) {
        enabled = (access("/data/UserData/move-anything/shadow_inprocess_log_on", F_OK) == 0);
    }
    return enabled;
}

int shadow_midi_out_log_enabled(void) {
    static int enabled = 0;
    static int announced = 0;
    enabled = (access("/data/UserData/move-anything/shadow_midi_out_log_on", F_OK) == 0);
    if (!enabled && shadow_midi_out_log) {
        fclose(shadow_midi_out_log);
        shadow_midi_out_log = NULL;
    }
    if (enabled && !announced) {
        shadow_log("shadow_midi_out_log enabled");
        announced = 1;
    }
    return enabled;
}

void shadow_midi_out_logf(const char *fmt, ...) {
    if (!shadow_midi_out_log_enabled()) return;
    if (!shadow_midi_out_log) {
        shadow_midi_out_log = fopen("/data/UserData/move-anything/shadow_midi_out.log", "a");
        if (!shadow_midi_out_log) return;
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(shadow_midi_out_log, fmt, args);
    va_end(args);
    fputc('\n', shadow_midi_out_log);
    fflush(shadow_midi_out_log);
}

/* ============================================================================
 * Capture Rules
 * ============================================================================ */

void capture_set_bit(uint8_t *bitmap, int index) {
    if (index >= 0 && index < 128) {
        bitmap[index / 8] |= (1 << (index % 8));
    }
}

void capture_set_range(uint8_t *bitmap, int min, int max) {
    for (int i = min; i <= max && i < 128; i++) {
        if (i >= 0) {
            capture_set_bit(bitmap, i);
        }
    }
}

int capture_has_bit(const uint8_t *bitmap, int index) {
    if (index >= 0 && index < 128) {
        return (bitmap[index / 8] >> (index % 8)) & 1;
    }
    return 0;
}

int capture_has_note(const shadow_capture_rules_t *rules, uint8_t note) {
    return capture_has_bit(rules->notes, note);
}

int capture_has_cc(const shadow_capture_rules_t *rules, uint8_t cc) {
    return capture_has_bit(rules->ccs, cc);
}

void capture_clear(shadow_capture_rules_t *rules) {
    memset(rules->notes, 0, sizeof(rules->notes));
    memset(rules->ccs, 0, sizeof(rules->ccs));
}

void capture_apply_group(shadow_capture_rules_t *rules, const char *group) {
    if (!group || !rules) return;

    if (strcmp(group, "pads") == 0) {
        capture_set_range(rules->notes, CAPTURE_PADS_NOTE_MIN, CAPTURE_PADS_NOTE_MAX);
    } else if (strcmp(group, "steps") == 0) {
        capture_set_range(rules->notes, CAPTURE_STEPS_NOTE_MIN, CAPTURE_STEPS_NOTE_MAX);
    } else if (strcmp(group, "tracks") == 0) {
        capture_set_range(rules->ccs, CAPTURE_TRACKS_CC_MIN, CAPTURE_TRACKS_CC_MAX);
    } else if (strcmp(group, "knobs") == 0) {
        capture_set_range(rules->ccs, CAPTURE_KNOBS_CC_MIN, CAPTURE_KNOBS_CC_MAX);
    } else if (strcmp(group, "jog") == 0) {
        capture_set_bit(rules->ccs, CAPTURE_JOG_CC);
    }
}

void capture_parse_json(shadow_capture_rules_t *rules, const char *json) {
    if (!rules || !json) return;
    capture_clear(rules);

    /* Find "capture" object */
    const char *capture_start = strstr(json, "\"capture\"");
    if (!capture_start) return;

    const char *brace = strchr(capture_start, '{');
    if (!brace) return;

    /* Find matching closing brace */
    const char *end = strchr(brace, '}');
    if (!end) return;

    /* Parse "groups" array: ["steps", "pads"] */
    const char *groups = strstr(brace, "\"groups\"");
    if (groups && groups < end) {
        const char *arr_start = strchr(groups, '[');
        if (arr_start && arr_start < end) {
            const char *arr_end = strchr(arr_start, ']');
            if (arr_end && arr_end < end) {
                const char *p = arr_start;
                while (p < arr_end) {
                    const char *q1 = strchr(p, '"');
                    if (!q1 || q1 >= arr_end) break;
                    q1++;
                    const char *q2 = strchr(q1, '"');
                    if (!q2 || q2 >= arr_end) break;

                    char group[32];
                    size_t len = (size_t)(q2 - q1);
                    if (len < sizeof(group)) {
                        memcpy(group, q1, len);
                        group[len] = '\0';
                        capture_apply_group(rules, group);
                    }
                    p = q2 + 1;
                }
            }
        }
    }

    /* Parse "notes" array: [60, 61, 62] */
    const char *notes = strstr(brace, "\"notes\"");
    if (notes && notes < end) {
        const char *arr_start = strchr(notes, '[');
        if (arr_start && arr_start < end) {
            const char *arr_end = strchr(arr_start, ']');
            if (arr_end && arr_end < end) {
                const char *p = arr_start + 1;
                while (p < arr_end) {
                    while (p < arr_end && (*p == ' ' || *p == ',')) p++;
                    if (p >= arr_end) break;
                    int val = atoi(p);
                    if (val >= 0 && val < 128) {
                        capture_set_bit(rules->notes, val);
                    }
                    while (p < arr_end && *p != ',' && *p != ']') p++;
                }
            }
        }
    }

    /* Parse "note_ranges" array: [[68, 75], [80, 90]] */
    const char *note_ranges = strstr(brace, "\"note_ranges\"");
    if (note_ranges && note_ranges < end) {
        const char *arr_start = strchr(note_ranges, '[');
        if (arr_start && arr_start < end) {
            int depth = 1;
            const char *p = arr_start + 1;
            while (p < end && depth > 0) {
                if (*p == '[') depth++;
                else if (*p == ']') depth--;
                p++;
            }
            const char *arr_end = p - 1;

            p = arr_start + 1;
            while (p < arr_end) {
                const char *inner_start = strchr(p, '[');
                if (!inner_start || inner_start >= arr_end) break;
                const char *inner_end = strchr(inner_start, ']');
                if (!inner_end || inner_end >= arr_end) break;

                int min = -1, max = -1;
                const char *n = inner_start + 1;
                while (n < inner_end && (*n == ' ' || *n == ',')) n++;
                min = atoi(n);
                while (n < inner_end && *n != ',') n++;
                if (n < inner_end) {
                    n++;
                    while (n < inner_end && *n == ' ') n++;
                    max = atoi(n);
                }
                if (min >= 0 && max >= min && max < 128) {
                    capture_set_range(rules->notes, min, max);
                }
                p = inner_end + 1;
            }
        }
    }

    /* Parse "ccs" array: [118, 119] */
    const char *ccs = strstr(brace, "\"ccs\"");
    if (ccs && ccs < end) {
        const char *arr_start = strchr(ccs, '[');
        if (arr_start && arr_start < end) {
            const char *arr_end = strchr(arr_start, ']');
            if (arr_end && arr_end < end) {
                const char *p = arr_start + 1;
                while (p < arr_end) {
                    while (p < arr_end && (*p == ' ' || *p == ',')) p++;
                    if (p >= arr_end) break;
                    int val = atoi(p);
                    if (val >= 0 && val < 128) {
                        capture_set_bit(rules->ccs, val);
                    }
                    while (p < arr_end && *p != ',' && *p != ']') p++;
                }
            }
        }
    }

    /* Parse "cc_ranges" array: [[100, 110]] */
    const char *cc_ranges = strstr(brace, "\"cc_ranges\"");
    if (cc_ranges && cc_ranges < end) {
        const char *arr_start = strchr(cc_ranges, '[');
        if (arr_start && arr_start < end) {
            int depth = 1;
            const char *p = arr_start + 1;
            while (p < end && depth > 0) {
                if (*p == '[') depth++;
                else if (*p == ']') depth--;
                p++;
            }
            const char *arr_end = p - 1;

            p = arr_start + 1;
            while (p < arr_end) {
                const char *inner_start = strchr(p, '[');
                if (!inner_start || inner_start >= arr_end) break;
                const char *inner_end = strchr(inner_start, ']');
                if (!inner_end || inner_end >= arr_end) break;

                int min = -1, max = -1;
                const char *n = inner_start + 1;
                while (n < inner_end && (*n == ' ' || *n == ',')) n++;
                min = atoi(n);
                while (n < inner_end && *n != ',') n++;
                if (n < inner_end) {
                    n++;
                    while (n < inner_end && *n == ' ') n++;
                    max = atoi(n);
                }
                if (min >= 0 && max >= min && max < 128) {
                    capture_set_range(rules->ccs, min, max);
                }
                p = inner_end + 1;
            }
        }
    }
}

/* ============================================================================
 * Chain Management
 * ============================================================================ */

int shadow_chain_parse_channel(int ch) {
    if (ch == 0) return -1;  /* All channels */
    if (ch >= 1 && ch <= 16) return ch - 1;
    return ch;
}

void shadow_chain_defaults(void) {
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        shadow_chain_slots[i].instance = NULL;
        shadow_chain_slots[i].active = 0;
        shadow_chain_slots[i].patch_index = -1;
        shadow_chain_slots[i].channel = shadow_chain_parse_channel(1 + i);
        shadow_chain_slots[i].volume = 1.0f;
        shadow_chain_slots[i].muted = 0;
        shadow_chain_slots[i].soloed = 0;
        shadow_chain_slots[i].forward_channel = -1;
        capture_clear(&shadow_chain_slots[i].capture);
        strncpy(shadow_chain_slots[i].patch_name,
                shadow_chain_default_patches[i],
                sizeof(shadow_chain_slots[i].patch_name) - 1);
        shadow_chain_slots[i].patch_name[sizeof(shadow_chain_slots[i].patch_name) - 1] = '\0';
    }
    shadow_solo_count = 0;
    /* Clear all master FX slots */
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        memset(&shadow_master_fx_slots[i], 0, sizeof(master_fx_slot_t));
    }
}

void shadow_chain_load_config(void) {
    shadow_chain_defaults();

    FILE *f = fopen(SHADOW_CHAIN_CONFIG_PATH, "r");
    if (!f) {
        shadow_ui_state_refresh();
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 4096) {
        fclose(f);
        shadow_ui_state_refresh();
        return;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        shadow_ui_state_refresh();
        return;
    }

    size_t nread = fread(json, 1, size, f);
    json[nread] = '\0';
    fclose(f);

    char *cursor = json;
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        char *name_pos = strstr(cursor, "\"name\"");
        if (!name_pos) break;
        char *colon = strchr(name_pos, ':');
        if (colon) {
            char *q1 = strchr(colon, '"');
            if (q1) {
                q1++;
                char *q2 = strchr(q1, '"');
                if (q2 && q2 > q1) {
                    size_t len = (size_t)(q2 - q1);
                    if (len < sizeof(shadow_chain_slots[i].patch_name)) {
                        memcpy(shadow_chain_slots[i].patch_name, q1, len);
                        shadow_chain_slots[i].patch_name[len] = '\0';
                    }
                }
            }
        }

        char *chan_pos = strstr(name_pos, "\"channel\"");
        if (chan_pos) {
            char *chan_colon = strchr(chan_pos, ':');
            if (chan_colon) {
                int ch = atoi(chan_colon + 1);
                if (ch >= 0 && ch <= 16) {
                    shadow_chain_slots[i].channel = shadow_chain_parse_channel(ch);
                }
            }
            cursor = chan_pos + 8;
        } else {
            cursor = name_pos + 6;
        }

        /* Parse volume (0.0 - 1.0) */
        char *vol_pos = strstr(name_pos, "\"volume\"");
        if (vol_pos) {
            char *vol_colon = strchr(vol_pos, ':');
            if (vol_colon) {
                float vol = atof(vol_colon + 1);
                if (vol >= 0.0f && vol <= 1.0f) {
                    shadow_chain_slots[i].volume = vol;
                }
            }
        }

        /* Parse forward_channel (-2 = passthrough, -1 = auto, 1-16 = channel) */
        char *fwd_pos = strstr(name_pos, "\"forward_channel\"");
        if (fwd_pos) {
            char *fwd_colon = strchr(fwd_pos, ':');
            if (fwd_colon) {
                int ch = atoi(fwd_colon + 1);
                if (ch >= -2 && ch <= 16) {
                    shadow_chain_slots[i].forward_channel = (ch > 0) ? ch - 1 : ch;
                }
            }
        }
    }

    free(json);
    shadow_ui_state_refresh();
}

int shadow_chain_find_patch_index(void *instance, const char *name) {
    if (!shadow_plugin_v2 || !shadow_plugin_v2->get_param || !instance || !name || !name[0]) {
        return -1;
    }
    char buf[128];
    int len = shadow_plugin_v2->get_param(instance, "patch_count", buf, sizeof(buf));
    if (len <= 0) return -1;
    buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
    int count = atoi(buf);
    if (count <= 0) return -1;

    for (int i = 0; i < count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "patch_name_%d", i);
        len = shadow_plugin_v2->get_param(instance, key, buf, sizeof(buf));
        if (len <= 0) continue;
        buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
        if (strcmp(buf, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* ============================================================================
 * UI State
 * ============================================================================ */

void shadow_ui_state_update_slot(int slot) {
    shadow_ui_state_t *ui_state = host.shadow_ui_state_ptr ? *host.shadow_ui_state_ptr : NULL;
    if (!ui_state) return;
    if (slot < 0 || slot >= SHADOW_UI_SLOTS) return;
    int ch = shadow_chain_slots[slot].channel;
    ui_state->slot_channels[slot] = (ch < 0) ? 0 : (uint8_t)(ch + 1);
    ui_state->slot_volumes[slot] = (uint8_t)(shadow_chain_slots[slot].volume * 100.0f);
    ui_state->slot_forward_ch[slot] = (int8_t)shadow_chain_slots[slot].forward_channel;
    strncpy(ui_state->slot_names[slot],
            shadow_chain_slots[slot].patch_name,
            SHADOW_UI_NAME_LEN - 1);
    ui_state->slot_names[slot][SHADOW_UI_NAME_LEN - 1] = '\0';
}

void shadow_ui_state_refresh(void) {
    shadow_ui_state_t *ui_state = host.shadow_ui_state_ptr ? *host.shadow_ui_state_ptr : NULL;
    if (!ui_state) return;
    ui_state->slot_count = SHADOW_UI_SLOTS;
    for (int i = 0; i < SHADOW_UI_SLOTS; i++) {
        shadow_ui_state_update_slot(i);
    }
}

/* ============================================================================
 * Mute / Solo
 * ============================================================================ */

void shadow_apply_mute(int slot, int is_muted) {
    if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) return;
    if (is_muted == shadow_chain_slots[slot].muted) return;
    shadow_chain_slots[slot].muted = is_muted;
    shadow_ui_state_update_slot(slot);
    char msg[64];
    snprintf(msg, sizeof(msg), "Mute: slot %d %s", slot, is_muted ? "muted" : "unmuted");
    shadow_log(msg);
}

void shadow_toggle_solo(int slot) {
    if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) return;

    if (shadow_chain_slots[slot].soloed) {
        shadow_chain_slots[slot].soloed = 0;
        shadow_solo_count = 0;
        char msg[64];
        snprintf(msg, sizeof(msg), "Solo off: slot %d", slot);
        shadow_log(msg);
    } else {
        for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++)
            shadow_chain_slots[i].soloed = 0;
        shadow_chain_slots[slot].soloed = 1;
        shadow_solo_count = 1;
        char msg[64];
        snprintf(msg, sizeof(msg), "Solo on: slot %d", slot);
        shadow_log(msg);
    }
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        shadow_ui_state_update_slot(i);
    }
}

/* ============================================================================
 * Master FX
 * ============================================================================ */

void shadow_master_fx_slot_unload(int slot) {
    if (slot < 0 || slot >= MASTER_FX_SLOTS) return;
    master_fx_slot_t *s = &shadow_master_fx_slots[slot];

    if (s->instance && s->api && s->api->destroy_instance) {
        s->api->destroy_instance(s->instance);
    }
    s->instance = NULL;
    s->api = NULL;
    s->on_midi = NULL;
    if (s->handle) {
        dlclose(s->handle);
        s->handle = NULL;
    }
    s->module_path[0] = '\0';
    s->module_id[0] = '\0';
    capture_clear(&s->capture);
}

void shadow_master_fx_unload_all(void) {
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        shadow_master_fx_slot_unload(i);
    }
}

int shadow_master_fx_slot_load(int slot, const char *dsp_path) {
    return shadow_master_fx_slot_load_with_config(slot, dsp_path, NULL);
}

int shadow_master_fx_slot_load_with_config(int slot, const char *dsp_path, const char *config_json) {
    if (slot < 0 || slot >= MASTER_FX_SLOTS) return -1;
    master_fx_slot_t *s = &shadow_master_fx_slots[slot];

    if (!dsp_path || !dsp_path[0]) {
        shadow_master_fx_slot_unload(slot);
        return 0;
    }

    /* Already loaded? (skip check if config_json provided) */
    if (!config_json && strcmp(s->module_path, dsp_path) == 0 && s->instance) {
        return 0;
    }

    shadow_master_fx_slot_unload(slot);

    s->handle = dlopen(dsp_path, RTLD_NOW | RTLD_LOCAL);
    if (!s->handle) {
        fprintf(stderr, "Shadow master FX[%d]: failed to load %s: %s\n", slot, dsp_path, dlerror());
        return -1;
    }

    audio_fx_init_v2_fn init_fn = (audio_fx_init_v2_fn)dlsym(s->handle, AUDIO_FX_INIT_V2_SYMBOL);
    if (!init_fn) {
        fprintf(stderr, "Shadow master FX[%d]: %s not found in %s\n", slot, AUDIO_FX_INIT_V2_SYMBOL, dsp_path);
        dlclose(s->handle);
        s->handle = NULL;
        return -1;
    }

    s->api = init_fn(&shadow_host_api);
    if (!s->api || !s->api->create_instance) {
        fprintf(stderr, "Shadow master FX[%d]: init failed for %s\n", slot, dsp_path);
        dlclose(s->handle);
        s->handle = NULL;
        s->api = NULL;
        return -1;
    }

    /* Extract module directory from dsp_path */
    char module_dir[256];
    strncpy(module_dir, dsp_path, sizeof(module_dir) - 1);
    module_dir[sizeof(module_dir) - 1] = '\0';
    char *last_slash = strrchr(module_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
    }

    s->instance = s->api->create_instance(module_dir, config_json);
    if (!s->instance) {
        fprintf(stderr, "Shadow master FX[%d]: create_instance failed for %s\n", slot, dsp_path);
        dlclose(s->handle);
        s->handle = NULL;
        s->api = NULL;
        return -1;
    }

    strncpy(s->module_path, dsp_path, sizeof(s->module_path) - 1);
    s->module_path[sizeof(s->module_path) - 1] = '\0';

    /* Extract module ID from path */
    const char *id_start = strrchr(module_dir, '/');
    if (id_start) {
        strncpy(s->module_id, id_start + 1, sizeof(s->module_id) - 1);
    } else {
        strncpy(s->module_id, module_dir, sizeof(s->module_id) - 1);
    }
    s->module_id[sizeof(s->module_id) - 1] = '\0';

    /* Load capture rules from module.json capabilities */
    char module_json_path[512];
    snprintf(module_json_path, sizeof(module_json_path), "%s/module.json", module_dir);
    s->chain_params_cached = 0;
    s->chain_params_cache[0] = '\0';
    FILE *f = fopen(module_json_path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0 && size < 16384) {
            char *json = malloc(size + 1);
            if (json) {
                size_t nread = fread(json, 1, size, f);
                json[nread] = '\0';
                const char *caps = strstr(json, "\"capabilities\"");
                if (caps) {
                    capture_parse_json(&s->capture, caps);
                }
                /* Cache chain_params */
                const char *chain_params = strstr(json, "\"chain_params\"");
                if (chain_params) {
                    const char *arr_start = strchr(chain_params, '[');
                    if (arr_start) {
                        int depth = 1;
                        const char *arr_end = arr_start + 1;
                        while (*arr_end && depth > 0) {
                            if (*arr_end == '[') depth++;
                            else if (*arr_end == ']') depth--;
                            arr_end++;
                        }
                        int len = (int)(arr_end - arr_start);
                        if (len > 0 && len < (int)sizeof(s->chain_params_cache) - 1) {
                            memcpy(s->chain_params_cache, arr_start, len);
                            s->chain_params_cache[len] = '\0';
                            s->chain_params_cached = 1;
                        }
                    }
                }
                free(json);
            }
        }
        fclose(f);
    }

    /* Check for optional MIDI handler */
    {
        typedef void (*fx_on_midi_fn)(void *, const uint8_t *, int, int);
        s->on_midi = (fx_on_midi_fn)dlsym(s->handle, "move_audio_fx_on_midi");
    }

    fprintf(stderr, "Shadow master FX[%d]: loaded %s\n", slot, dsp_path);
    return 0;
}

int shadow_master_fx_load(const char *dsp_path) {
    return shadow_master_fx_slot_load(0, dsp_path);
}

void shadow_master_fx_unload(void) {
    shadow_master_fx_slot_unload(0);
}

void shadow_master_fx_forward_midi(const uint8_t *msg, int len, int source) {
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        master_fx_slot_t *s = &shadow_master_fx_slots[i];
        if (s->on_midi && s->instance) {
            s->on_midi(s->instance, msg, len, source);
        }
    }
}

/* ============================================================================
 * Capture Loading
 * ============================================================================ */

static void capture_debug_log(const char *msg) {
    FILE *log = fopen("/data/UserData/move-anything/shadow_capture_debug.log", "a");
    if (log) {
        fprintf(log, "%s\n", msg);
        fclose(log);
    }
}

void shadow_slot_load_capture(int slot, int patch_index) {
    char dbg[512];
    snprintf(dbg, sizeof(dbg), "shadow_slot_load_capture: slot=%d patch_index=%d", slot, patch_index);
    capture_debug_log(dbg);

    if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) return;
    if (!shadow_chain_slots[slot].instance) {
        capture_debug_log("  -> no instance");
        return;
    }
    if (!shadow_plugin_v2 || !shadow_plugin_v2->get_param) {
        capture_debug_log("  -> no plugin_v2/get_param");
        return;
    }

    capture_clear(&shadow_chain_slots[slot].capture);

    char key[32];
    char path[512];
    snprintf(key, sizeof(key), "patch_path_%d", patch_index);
    int len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance, key, path, sizeof(path));
    snprintf(dbg, sizeof(dbg), "  -> get_param(%s) len=%d", key, len);
    capture_debug_log(dbg);
    if (len <= 0) return;
    path[len < (int)sizeof(path) ? len : (int)sizeof(path) - 1] = '\0';
    snprintf(dbg, sizeof(dbg), "  -> path: %s", path);
    capture_debug_log(dbg);

    FILE *f = fopen(path, "r");
    if (!f) {
        capture_debug_log("  -> fopen failed");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 16384) {
        fclose(f);
        return;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return;
    }

    size_t nread = fread(json, 1, size, f);
    json[nread] = '\0';
    fclose(f);

    capture_parse_json(&shadow_chain_slots[slot].capture, json);
    free(json);

    /* Log capture rules summary */
    int has_notes = 0, has_ccs = 0;
    for (int b = 0; b < 16; b++) {
        if (shadow_chain_slots[slot].capture.notes[b]) has_notes = 1;
        if (shadow_chain_slots[slot].capture.ccs[b]) has_ccs = 1;
    }
    snprintf(dbg, sizeof(dbg), "  -> capture parsed: has_notes=%d has_ccs=%d", has_notes, has_ccs);
    capture_debug_log(dbg);
    snprintf(dbg, sizeof(dbg), "  -> note 16 captured: %d", capture_has_note(&shadow_chain_slots[slot].capture, 16));
    capture_debug_log(dbg);
    if (has_notes || has_ccs) {
        snprintf(dbg, sizeof(dbg), "Slot %d capture loaded: notes=%d ccs=%d",
                 slot, has_notes, has_ccs);
        shadow_log(dbg);
    }
}

/* ============================================================================
 * Boot - Load Chain
 * ============================================================================ */

int shadow_inprocess_load_chain(void) {
    if (shadow_inprocess_ready) return 0;

    shadow_dsp_handle = dlopen(SHADOW_CHAIN_DSP_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!shadow_dsp_handle) {
        fprintf(stderr, "Shadow inprocess: failed to load %s: %s\n",
                SHADOW_CHAIN_DSP_PATH, dlerror());
        return -1;
    }

    uint8_t *global_mmap = host.global_mmap_addr_ptr ? *host.global_mmap_addr_ptr : NULL;

    memset(&shadow_host_api, 0, sizeof(shadow_host_api));
    shadow_host_api.api_version = MOVE_PLUGIN_API_VERSION;
    shadow_host_api.sample_rate = MOVE_SAMPLE_RATE;
    shadow_host_api.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    shadow_host_api.mapped_memory = global_mmap;
    shadow_host_api.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    shadow_host_api.audio_in_offset = MOVE_AUDIO_IN_OFFSET;
    shadow_host_api.log = shadow_log;

    move_plugin_init_v2_fn init_v2 = (move_plugin_init_v2_fn)dlsym(
        shadow_dsp_handle, MOVE_PLUGIN_INIT_V2_SYMBOL);
    if (!init_v2) {
        fprintf(stderr, "Shadow inprocess: %s not found\n", MOVE_PLUGIN_INIT_V2_SYMBOL);
        dlclose(shadow_dsp_handle);
        shadow_dsp_handle = NULL;
        return -1;
    }

    shadow_plugin_v2 = init_v2(&shadow_host_api);
    if (!shadow_plugin_v2 || !shadow_plugin_v2->create_instance) {
        fprintf(stderr, "Shadow inprocess: chain v2 init failed\n");
        dlclose(shadow_dsp_handle);
        shadow_dsp_handle = NULL;
        shadow_plugin_v2 = NULL;
        return -1;
    }

    /* Look up optional chain exports for Link Audio routing + same-frame FX */
    shadow_chain_set_inject_audio = (void (*)(void *, int16_t *, int))
        dlsym(shadow_dsp_handle, "chain_set_inject_audio");
    shadow_chain_set_external_fx_mode = (void (*)(void *, int))
        dlsym(shadow_dsp_handle, "chain_set_external_fx_mode");
    shadow_chain_process_fx = (void (*)(void *, int16_t *, int))
        dlsym(shadow_dsp_handle, "chain_process_fx");

    unified_log("shim", LOG_LEVEL_INFO, "chain dlsym: inject=%p ext_fx_mode=%p process_fx=%p same_frame=%d",
            (void*)shadow_chain_set_inject_audio,
            (void*)shadow_chain_set_external_fx_mode,
            (void*)shadow_chain_process_fx,
            (shadow_chain_set_external_fx_mode && shadow_chain_process_fx) ? 1 : 0);

    /* Set pages: read persisted page on boot */
    set_page_current = set_page_read_persisted();
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "SetPage: boot page = %d", set_page_current + 1);
        shadow_log(msg);
    }

    /* Show page toast on boot */
    set_page_overlay_active = 1;
    set_page_overlay_timeout = SET_PAGE_OVERLAY_FRAMES;
    if (host.overlay_sync) host.overlay_sync();

    /* Run batch migration for per-set state support */
    shadow_batch_migrate_sets();

    /* Determine boot state directory */
    char boot_state_dir[512];
    snprintf(boot_state_dir, sizeof(boot_state_dir), "%s", SLOT_STATE_DIR);
    {
        FILE *asf = fopen(ACTIVE_SET_PATH, "r");
        if (asf) {
            char boot_uuid[128] = "";
            if (fgets(boot_uuid, sizeof(boot_uuid), asf)) {
                char *end = boot_uuid + strlen(boot_uuid) - 1;
                while (end > boot_uuid && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
                if (boot_uuid[0]) {
                    char set_dir[512];
                    snprintf(set_dir, sizeof(set_dir), SET_STATE_DIR "/%s", boot_uuid);
                    char test_slot[768];
                    snprintf(test_slot, sizeof(test_slot), "%s/slot_0.json", set_dir);
                    char test_cfg[768];
                    snprintf(test_cfg, sizeof(test_cfg), "%s/shadow_chain_config.json", set_dir);
                    struct stat st;
                    if (stat(test_slot, &st) == 0 || stat(test_cfg, &st) == 0) {
                        snprintf(boot_state_dir, sizeof(boot_state_dir), "%s", set_dir);
                        snprintf(sampler_current_set_uuid, sizeof(sampler_current_set_uuid),
                                 "%s", boot_uuid);
                        char boot_name[128] = "";
                        if (fgets(boot_name, sizeof(boot_name), asf)) {
                            char *ne = boot_name + strlen(boot_name) - 1;
                            while (ne > boot_name && (*ne == '\n' || *ne == '\r' || *ne == ' ')) *ne-- = '\0';
                            if (boot_name[0]) {
                                snprintf(sampler_current_set_name, sizeof(sampler_current_set_name),
                                         "%s", boot_name);
                            }
                        }
                        char m[256];
                        snprintf(m, sizeof(m), "Boot: using per-set state dir %s", set_dir);
                        shadow_log(m);
                    }
                }
            }
            fclose(asf);
        }
    }

    shadow_chain_load_config();
    if (strcmp(boot_state_dir, SLOT_STATE_DIR) != 0) {
        shadow_load_config_from_dir(boot_state_dir);
    }

    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        shadow_chain_slots[i].instance = shadow_plugin_v2->create_instance(
            SHADOW_CHAIN_MODULE_DIR, NULL);
        if (!shadow_chain_slots[i].instance) {
            continue;
        }

        /* Check for autosave file */
        char autosave_path[256];
        snprintf(autosave_path, sizeof(autosave_path),
                 "%s/slot_%d.json", boot_state_dir, i);
        FILE *af = fopen(autosave_path, "r");
        if (af) {
            fseek(af, 0, SEEK_END);
            long asize = ftell(af);
            fclose(af);
            if (asize > 10) {
                shadow_plugin_v2->set_param(
                    shadow_chain_slots[i].instance, "load_file", autosave_path);
                shadow_chain_slots[i].active = 1;
                shadow_chain_slots[i].patch_index = -1;
                /* Query channel settings from loaded autosave */
                if (shadow_chain_slots[i].forward_channel == -1 && shadow_plugin_v2->get_param) {
                    char fwd_buf[16];
                    int len = shadow_plugin_v2->get_param(shadow_chain_slots[i].instance,
                        "synth:default_forward_channel", fwd_buf, sizeof(fwd_buf));
                    if (len > 0) {
                        fwd_buf[len < (int)sizeof(fwd_buf) ? len : (int)sizeof(fwd_buf) - 1] = '\0';
                        int default_fwd = atoi(fwd_buf);
                        if (default_fwd >= 0 && default_fwd <= 15) {
                            shadow_chain_slots[i].forward_channel = default_fwd;
                        }
                    }
                }
                if (shadow_plugin_v2->get_param) {
                    char ch_buf[16];
                    int len;
                    len = shadow_plugin_v2->get_param(shadow_chain_slots[i].instance,
                        "patch:receive_channel", ch_buf, sizeof(ch_buf));
                    if (len > 0) {
                        ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
                        int recv_ch = atoi(ch_buf);
                        if (recv_ch != 0) {
                            shadow_chain_slots[i].channel = (recv_ch >= 1 && recv_ch <= 16) ? recv_ch - 1 : -1;
                        }
                    }
                    len = shadow_plugin_v2->get_param(shadow_chain_slots[i].instance,
                        "patch:forward_channel", ch_buf, sizeof(ch_buf));
                    if (len > 0) {
                        ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
                        int fwd_ch = atoi(ch_buf);
                        if (fwd_ch != 0) {
                            shadow_chain_slots[i].forward_channel = (fwd_ch > 0) ? fwd_ch - 1 : fwd_ch;
                        }
                    }
                }
                {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Shadow inprocess: slot %d loaded from autosave", i);
                    shadow_log(msg);
                }
                continue;
            }
        }

        /* Fall back to name-based lookup from config */
        if (strcasecmp(shadow_chain_slots[i].patch_name, "none") == 0 ||
            shadow_chain_slots[i].patch_name[0] == '\0') {
            shadow_chain_slots[i].active = 0;
            shadow_chain_slots[i].patch_index = -1;
            continue;
        }
        int idx = shadow_chain_find_patch_index(shadow_chain_slots[i].instance,
                                                shadow_chain_slots[i].patch_name);
        shadow_chain_slots[i].patch_index = idx;
        if (idx >= 0 && shadow_plugin_v2->set_param) {
            char idx_str[16];
            snprintf(idx_str, sizeof(idx_str), "%d", idx);
            shadow_plugin_v2->set_param(shadow_chain_slots[i].instance, "load_patch", idx_str);
            shadow_chain_slots[i].active = 1;
            shadow_slot_load_capture(i, idx);
            if (shadow_chain_slots[i].forward_channel == -1 && shadow_plugin_v2->get_param) {
                char fwd_buf[16];
                int len = shadow_plugin_v2->get_param(shadow_chain_slots[i].instance,
                    "synth:default_forward_channel", fwd_buf, sizeof(fwd_buf));
                if (len > 0) {
                    fwd_buf[len < (int)sizeof(fwd_buf) ? len : (int)sizeof(fwd_buf) - 1] = '\0';
                    int default_fwd = atoi(fwd_buf);
                    if (default_fwd >= 0 && default_fwd <= 15) {
                        shadow_chain_slots[i].forward_channel = default_fwd;
                    }
                }
            }
            if (shadow_plugin_v2->get_param) {
                char ch_buf[16];
                int len;
                len = shadow_plugin_v2->get_param(shadow_chain_slots[i].instance,
                    "patch:receive_channel", ch_buf, sizeof(ch_buf));
                if (len > 0) {
                    ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
                    int recv_ch = atoi(ch_buf);
                    if (recv_ch != 0) {
                        shadow_chain_slots[i].channel = (recv_ch >= 1 && recv_ch <= 16) ? recv_ch - 1 : -1;
                    }
                }
                len = shadow_plugin_v2->get_param(shadow_chain_slots[i].instance,
                    "patch:forward_channel", ch_buf, sizeof(ch_buf));
                if (len > 0) {
                    ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
                    int fwd_ch = atoi(ch_buf);
                    if (fwd_ch != 0) {
                        shadow_chain_slots[i].forward_channel = (fwd_ch > 0) ? fwd_ch - 1 : fwd_ch;
                    }
                }
            }
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "Shadow inprocess: patch not found: %s",
                     shadow_chain_slots[i].patch_name);
            shadow_log(msg);
        }
    }

    /* Load master FX slots from state files */
    for (int mfx = 0; mfx < MASTER_FX_SLOTS; mfx++) {
        char mfx_path[256];
        snprintf(mfx_path, sizeof(mfx_path), "%s/master_fx_%d.json", boot_state_dir, mfx);
        FILE *mf = fopen(mfx_path, "r");
        if (!mf) continue;

        fseek(mf, 0, SEEK_END);
        long msize = ftell(mf);
        fseek(mf, 0, SEEK_SET);

        if (msize <= 10) {
            fclose(mf);
            continue;
        }

        char *mjson = malloc(msize + 1);
        if (!mjson) { fclose(mf); continue; }
        size_t mnread = fread(mjson, 1, msize, mf);
        mjson[mnread] = '\0';
        fclose(mf);

        /* Extract module_path */
        char dsp_path[256] = "";
        {
            char *mp = strstr(mjson, "\"module_path\":");
            if (mp) {
                mp = strchr(mp, ':');
                if (mp) {
                    mp++;
                    while (*mp == ' ' || *mp == '"') mp++;
                    char *end = mp;
                    while (*end && *end != '"') end++;
                    int len = end - mp;
                    if (len > 0 && len < (int)sizeof(dsp_path) - 1) {
                        strncpy(dsp_path, mp, len);
                        dsp_path[len] = '\0';
                    }
                }
            }
        }

        if (!dsp_path[0]) {
            free(mjson);
            continue;
        }

        /* Extract plugin_id from params */
        char config_json_buf[512] = "";
        char *params_start = strstr(mjson, "\"params\":");
        if (params_start) {
            char *pid_key = strstr(params_start, "\"plugin_id\"");
            if (pid_key) {
                char *pc = strchr(pid_key + 11, ':');
                if (pc) {
                    pc++;
                    while (*pc == ' ') pc++;
                    if (*pc == '"') {
                        pc++;
                        char *pe = strchr(pc, '"');
                        if (pe) {
                            int plen = pe - pc;
                            if (plen > 0 && plen < 256) {
                                char pid_val[256];
                                strncpy(pid_val, pc, plen);
                                pid_val[plen] = '\0';
                                snprintf(config_json_buf, sizeof(config_json_buf),
                                         "{\"plugin_id\":\"%s\"}", pid_val);
                            }
                        }
                    }
                }
            }
        }

        int load_result = shadow_master_fx_slot_load_with_config(mfx, dsp_path,
            config_json_buf[0] ? config_json_buf : NULL);
        if (load_result != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "MFX boot: slot %d failed to load %s", mfx, dsp_path);
            shadow_log(msg);
            free(mjson);
            continue;
        }

        master_fx_slot_t *s = &shadow_master_fx_slots[mfx];

        /* Restore state if available */
        char *state_start = strstr(mjson, "\"state\":");
        if (state_start && s->api && s->instance && s->api->set_param) {
            char *obj_start = strchr(state_start, '{');
            if (obj_start) {
                int depth = 1;
                char *obj_end = obj_start + 1;
                while (*obj_end && depth > 0) {
                    if (*obj_end == '{') depth++;
                    else if (*obj_end == '}') depth--;
                    obj_end++;
                }
                int slen = obj_end - obj_start;
                char *state_buf = malloc(slen + 1);
                if (state_buf) {
                    memcpy(state_buf, obj_start, slen);
                    state_buf[slen] = '\0';
                    s->api->set_param(s->instance, "state", state_buf);
                    free(state_buf);
                }
            }
        } else if (params_start && s->api && s->instance && s->api->set_param) {
            /* Fall back to individual params */
            char *obj_start = strchr(params_start, '{');
            if (obj_start) {
                int depth = 1;
                char *obj_end = obj_start + 1;
                while (*obj_end && depth > 0) {
                    if (*obj_end == '{') depth++;
                    else if (*obj_end == '}') depth--;
                    obj_end++;
                }
                char *p = obj_start + 1;
                while (p < obj_end - 1) {
                    char *kstart = strchr(p, '"');
                    if (!kstart || kstart >= obj_end) break;
                    kstart++;
                    char *kend = strchr(kstart, '"');
                    if (!kend || kend >= obj_end) break;

                    char param_key[128];
                    int klen = kend - kstart;
                    if (klen >= (int)sizeof(param_key)) { p = kend + 1; continue; }
                    strncpy(param_key, kstart, klen);
                    param_key[klen] = '\0';

                    char *colon = strchr(kend, ':');
                    if (!colon || colon >= obj_end) break;
                    colon++;
                    while (*colon == ' ') colon++;

                    char param_val[256];
                    if (*colon == '"') {
                        colon++;
                        char *vend = strchr(colon, '"');
                        if (!vend || vend >= obj_end) break;
                        int vlen = vend - colon;
                        if (vlen >= (int)sizeof(param_val)) { p = vend + 1; continue; }
                        strncpy(param_val, colon, vlen);
                        param_val[vlen] = '\0';
                        p = vend + 1;
                    } else {
                        char *vend = colon;
                        while (*vend && *vend != ',' && *vend != '}' && *vend != '\n') vend++;
                        int vlen = vend - colon;
                        if (vlen >= (int)sizeof(param_val)) { p = vend; continue; }
                        strncpy(param_val, colon, vlen);
                        param_val[vlen] = '\0';
                        while (vlen > 0 && (param_val[vlen-1] == ' ' || param_val[vlen-1] == '\r')) {
                            param_val[--vlen] = '\0';
                        }
                        p = vend;
                    }

                    if (strcmp(param_key, "plugin_id") != 0) {
                        s->api->set_param(s->instance, param_key, param_val);
                    }
                }
            }
        }

        {
            char msg[256];
            snprintf(msg, sizeof(msg), "MFX boot: slot %d loaded %s%s",
                     mfx, s->module_id,
                     state_start ? " (with state)" : (strstr(mjson, "\"params\":") ? " (with params)" : ""));
            shadow_log(msg);
        }
        free(mjson);
    }

    shadow_ui_state_refresh();

    /* Pre-create directories */
    {
        struct stat st;
        if (stat(SAMPLER_RECORDINGS_DIR, &st) != 0) {
            const char *mkdir_argv[] = { "mkdir", "-p", SAMPLER_RECORDINGS_DIR, NULL };
            if (host.run_command) host.run_command(mkdir_argv);
        }
        if (stat(SKIPBACK_DIR, &st) != 0) {
            const char *mkdir_argv[] = { "mkdir", "-p", SKIPBACK_DIR, NULL };
            if (host.run_command) host.run_command(mkdir_argv);
        }
        if (stat(SLOT_STATE_DIR, &st) != 0) {
            const char *mkdir_argv[] = { "mkdir", "-p", SLOT_STATE_DIR, NULL };
            if (host.run_command) host.run_command(mkdir_argv);
        }
        if (stat(SET_STATE_DIR, &st) != 0) {
            const char *mkdir_argv[] = { "mkdir", "-p", SET_STATE_DIR, NULL };
            if (host.run_command) host.run_command(mkdir_argv);
        }
    }

    shadow_inprocess_ready = 1;
    if (host.startup_modwheel_countdown) {
        *host.startup_modwheel_countdown = host.startup_modwheel_reset_frames;
    }
    shadow_control_t *ctrl = host.shadow_control_ptr ? *host.shadow_control_ptr : NULL;
    if (ctrl) {
        ctrl->shadow_ready = 1;
    }
    if (host.shadow_ui_enabled && *host.shadow_ui_enabled && host.launch_shadow_ui) {
        host.launch_shadow_ui();
    }
    shadow_log("Shadow inprocess: chain loaded");
    return 0;
}

/* ============================================================================
 * UI Request Handling
 * ============================================================================ */

void shadow_inprocess_handle_ui_request(void) {
    shadow_control_t *ctrl = host.shadow_control_ptr ? *host.shadow_control_ptr : NULL;
    if (!ctrl || !shadow_plugin_v2 || !shadow_plugin_v2->set_param) return;

    uint32_t request_id = ctrl->ui_request_id;
    if (request_id == shadow_ui_request_seen) return;
    shadow_ui_request_seen = request_id;

    int slot = ctrl->ui_slot;
    int patch_index = ctrl->ui_patch_index;

    {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "UI request: slot=%d patch=%d instance=%p",
                 slot, patch_index, shadow_chain_slots[slot < SHADOW_CHAIN_INSTANCES ? slot : 0].instance);
        shadow_log(dbg);
    }

    if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) return;
    if (patch_index < 0) return;
    if (!shadow_chain_slots[slot].instance) {
        shadow_log("UI request: slot instance is NULL, aborting");
        return;
    }

    /* Handle "none" special value */
    if (patch_index == SHADOW_PATCH_INDEX_NONE) {
        if (shadow_plugin_v2->set_param && shadow_chain_slots[slot].instance) {
            shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance, "synth:module", "");
            shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance, "fx1:module", "");
            shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance, "fx2:module", "");
        }
        shadow_chain_slots[slot].active = 0;
        shadow_chain_slots[slot].patch_index = -1;
        capture_clear(&shadow_chain_slots[slot].capture);
        strncpy(shadow_chain_slots[slot].patch_name, "", sizeof(shadow_chain_slots[slot].patch_name) - 1);
        shadow_chain_slots[slot].patch_name[sizeof(shadow_chain_slots[slot].patch_name) - 1] = '\0';
        shadow_ui_state_t *ui_state = host.shadow_ui_state_ptr ? *host.shadow_ui_state_ptr : NULL;
        if (ui_state && slot < SHADOW_UI_SLOTS) {
            strncpy(ui_state->slot_names[slot], "", SHADOW_UI_NAME_LEN - 1);
            ui_state->slot_names[slot][SHADOW_UI_NAME_LEN - 1] = '\0';
        }
        return;
    }

    if (shadow_plugin_v2->get_param) {
        char buf[32];
        int len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance,
                                              "patch_count", buf, sizeof(buf));
        if (len > 0) {
            buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
            int patch_count = atoi(buf);
            if (patch_count > 0 && patch_index >= patch_count) {
                return;
            }
        }
    }

    char idx_str[16];
    snprintf(idx_str, sizeof(idx_str), "%d", patch_index);
    shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance, "load_patch", idx_str);
    shadow_chain_slots[slot].patch_index = patch_index;
    shadow_chain_slots[slot].active = 1;

    if (shadow_plugin_v2->get_param) {
        char key[32];
        char buf[128];
        int len = 0;
        snprintf(key, sizeof(key), "patch_name_%d", patch_index);
        len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance, key, buf, sizeof(buf));
        if (len > 0) {
            buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
            strncpy(shadow_chain_slots[slot].patch_name, buf, sizeof(shadow_chain_slots[slot].patch_name) - 1);
            shadow_chain_slots[slot].patch_name[sizeof(shadow_chain_slots[slot].patch_name) - 1] = '\0';
        }
    }

    shadow_slot_load_capture(slot, patch_index);

    /* Apply channel settings saved in patch */
    if (shadow_plugin_v2->get_param) {
        char ch_buf[16];
        int len;
        len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance,
            "patch:receive_channel", ch_buf, sizeof(ch_buf));
        if (len > 0) {
            ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
            int recv_ch = atoi(ch_buf);
            if (recv_ch != 0) {
                shadow_chain_slots[slot].channel = (recv_ch >= 1 && recv_ch <= 16) ? recv_ch - 1 : -1;
            }
        }
        len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance,
            "patch:forward_channel", ch_buf, sizeof(ch_buf));
        if (len > 0) {
            ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
            int fwd_ch = atoi(ch_buf);
            if (fwd_ch != 0) {
                shadow_chain_slots[slot].forward_channel = (fwd_ch > 0) ? fwd_ch - 1 : fwd_ch;
            }
        }
    }

    shadow_ui_state_update_slot(slot);
}

/* ============================================================================
 * Param Handling
 * ============================================================================ */

int shadow_handle_slot_param_set(int slot, const char *key, const char *value) {
    if (strcmp(key, "slot:volume") == 0) {
        float vol = atof(value);
        if (vol < 0.0f) vol = 0.0f;
        if (vol > 1.0f) vol = 1.0f;
        shadow_chain_slots[slot].volume = vol;
        shadow_ui_state_update_slot(slot);
        return 1;
    }
    if (strcmp(key, "slot:muted") == 0) {
        shadow_apply_mute(slot, atoi(value));
        return 1;
    }
    if (strcmp(key, "slot:soloed") == 0) {
        int val = atoi(value);
        if (val && !shadow_chain_slots[slot].soloed) {
            for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++)
                shadow_chain_slots[i].soloed = 0;
            shadow_chain_slots[slot].soloed = 1;
            shadow_solo_count = 1;
        } else if (!val && shadow_chain_slots[slot].soloed) {
            shadow_chain_slots[slot].soloed = 0;
            shadow_solo_count = 0;
        }
        for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++)
            shadow_ui_state_update_slot(i);
        return 1;
    }
    if (strcmp(key, "slot:forward_channel") == 0) {
        int ch = atoi(value);
        if (ch < -2) ch = -2;
        if (ch > 15) ch = 15;
        shadow_chain_slots[slot].forward_channel = ch;
        shadow_ui_state_update_slot(slot);
        return 1;
    }
    if (strcmp(key, "slot:receive_channel") == 0) {
        int ch = atoi(value);
        if (ch == 0) {
            shadow_chain_slots[slot].channel = -1;
            shadow_ui_state_update_slot(slot);
        } else if (ch >= 1 && ch <= 16) {
            shadow_chain_slots[slot].channel = ch - 1;
            shadow_ui_state_update_slot(slot);
        }
        return 1;
    }
    return 0;
}

int shadow_handle_slot_param_get(int slot, const char *key, char *buf, int buf_len) {
    if (strcmp(key, "slot:volume") == 0) {
        return snprintf(buf, buf_len, "%.2f", shadow_chain_slots[slot].volume);
    }
    if (strcmp(key, "slot:muted") == 0) {
        return snprintf(buf, buf_len, "%d", shadow_chain_slots[slot].muted);
    }
    if (strcmp(key, "slot:soloed") == 0) {
        return snprintf(buf, buf_len, "%d", shadow_chain_slots[slot].soloed);
    }
    if (strcmp(key, "slot:forward_channel") == 0) {
        return snprintf(buf, buf_len, "%d", shadow_chain_slots[slot].forward_channel);
    }
    if (strcmp(key, "slot:receive_channel") == 0) {
        int ch = shadow_chain_slots[slot].channel;
        return snprintf(buf, buf_len, "%d", (ch < 0) ? 0 : ch + 1);
    }
    return -1;
}

int shadow_param_publish_response(uint32_t req_id) {
    shadow_param_t *param = host.shadow_param_ptr ? *host.shadow_param_ptr : NULL;
    if (!param) return 0;
    if (param->request_id != req_id) {
        return 0;
    }
    param->response_id = req_id;
    param->response_ready = 1;
    param->request_type = 0;
    return 1;
}

void shadow_inprocess_handle_param_request(void) {
    shadow_param_t *shadow_param = host.shadow_param_ptr ? *host.shadow_param_ptr : NULL;
    if (!shadow_param) return;

    uint8_t req_type = shadow_param->request_type;
    if (req_type == 0) return;
    uint32_t req_id = shadow_param->request_id;

    /* Handle master FX chain params */
    if (strncmp(shadow_param->key, "master_fx:", 10) == 0) {
        const char *fx_key = shadow_param->key + 10;
        int mfx_slot = -1;
        int has_slot_prefix = 0;
        const char *param_key = fx_key;

        /* Parse slot prefix */
        if (strncmp(fx_key, "fx1:", 4) == 0) { mfx_slot = 0; param_key = fx_key + 4; has_slot_prefix = 1; }
        else if (strncmp(fx_key, "fx2:", 4) == 0) { mfx_slot = 1; param_key = fx_key + 4; has_slot_prefix = 1; }
        else if (strncmp(fx_key, "fx3:", 4) == 0) { mfx_slot = 2; param_key = fx_key + 4; has_slot_prefix = 1; }
        else if (strncmp(fx_key, "fx4:", 4) == 0) { mfx_slot = 3; param_key = fx_key + 4; has_slot_prefix = 1; }
        else { mfx_slot = 0; param_key = fx_key; }

        /* Delegate shim-specific params (resample_bridge, link_audio_routing) */
        if (!has_slot_prefix && host.handle_param_special) {
            if (strcmp(param_key, "resample_bridge") == 0 ||
                strcmp(param_key, "link_audio_routing") == 0) {
                if (host.handle_param_special(req_type, req_id)) {
                    shadow_param_publish_response(req_id);
                    return;
                }
            }
        }

        master_fx_slot_t *mfx = &shadow_master_fx_slots[mfx_slot];

        if (req_type == 1) {  /* SET */
            if (strcmp(param_key, "module") == 0) {
                int result = shadow_master_fx_slot_load(mfx_slot, shadow_param->value);
                shadow_param->error = (result == 0) ? 0 : 7;
                shadow_param->result_len = 0;
            } else if (strcmp(param_key, "param") == 0 && mfx->api && mfx->instance) {
                char *eq = strchr(shadow_param->value, '=');
                if (eq && mfx->api->set_param) {
                    *eq = '\0';
                    mfx->api->set_param(mfx->instance, shadow_param->value, eq + 1);
                    *eq = '=';
                    shadow_param->error = 0;
                } else {
                    shadow_param->error = 8;
                }
                shadow_param->result_len = 0;
            } else if (mfx->api && mfx->instance && mfx->api->set_param) {
                mfx->api->set_param(mfx->instance, param_key, shadow_param->value);
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else {
                shadow_param->error = 9;
                shadow_param->result_len = -1;
            }
        } else if (req_type == 2) {  /* GET */
            if (strcmp(param_key, "module") == 0) {
                strncpy(shadow_param->value, mfx->module_path, SHADOW_PARAM_VALUE_LEN - 1);
                shadow_param->value[SHADOW_PARAM_VALUE_LEN - 1] = '\0';
                shadow_param->error = 0;
                shadow_param->result_len = strlen(shadow_param->value);
            } else if (strcmp(param_key, "name") == 0) {
                strncpy(shadow_param->value, mfx->module_id, SHADOW_PARAM_VALUE_LEN - 1);
                shadow_param->value[SHADOW_PARAM_VALUE_LEN - 1] = '\0';
                shadow_param->error = 0;
                shadow_param->result_len = strlen(shadow_param->value);
            } else if (strcmp(param_key, "error") == 0) {
                shadow_param->value[0] = '\0';
                shadow_param->error = 0;
                shadow_param->result_len = 0;
                if (mfx->api && mfx->instance && mfx->api->get_param) {
                    int len = mfx->api->get_param(mfx->instance, "load_error",
                                                   shadow_param->value, SHADOW_PARAM_VALUE_LEN);
                    if (len > 0) {
                        shadow_param->result_len = len;
                    }
                }
            } else if (strcmp(param_key, "chain_params") == 0) {
                if (mfx->api && mfx->instance && mfx->api->get_param) {
                    int len = mfx->api->get_param(mfx->instance, "chain_params",
                                                   shadow_param->value, SHADOW_PARAM_VALUE_LEN);
                    if (len > 2) {
                        shadow_param->error = 0;
                        shadow_param->result_len = len;
                        shadow_param_publish_response(req_id);
                        return;
                    }
                }
                if (mfx->chain_params_cached && mfx->chain_params_cache[0]) {
                    int len = strlen(mfx->chain_params_cache);
                    if (len < SHADOW_PARAM_VALUE_LEN - 1) {
                        memcpy(shadow_param->value, mfx->chain_params_cache, len + 1);
                        shadow_param->error = 0;
                        shadow_param->result_len = len;
                        shadow_param_publish_response(req_id);
                        return;
                    }
                }
                shadow_param->value[0] = '[';
                shadow_param->value[1] = ']';
                shadow_param->value[2] = '\0';
                shadow_param->error = 0;
                shadow_param->result_len = 2;
            } else if (strcmp(param_key, "ui_hierarchy") == 0) {
                if (mfx->api && mfx->instance && mfx->api->get_param) {
                    int len = mfx->api->get_param(mfx->instance, "ui_hierarchy",
                                                   shadow_param->value, SHADOW_PARAM_VALUE_LEN);
                    if (len > 2) {
                        shadow_param->error = 0;
                        shadow_param->result_len = len;
                        shadow_param_publish_response(req_id);
                        return;
                    }
                }
                /* Fall back to reading ui_hierarchy from module.json */
                char module_dir[256];
                strncpy(module_dir, mfx->module_path, sizeof(module_dir) - 1);
                module_dir[sizeof(module_dir) - 1] = '\0';
                char *last_slash = strrchr(module_dir, '/');
                if (last_slash) *last_slash = '\0';

                char json_path[512];
                snprintf(json_path, sizeof(json_path), "%s/module.json", module_dir);

                FILE *f = fopen(json_path, "r");
                if (f) {
                    fseek(f, 0, SEEK_END);
                    long size = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    if (size > 0 && size < 32768) {
                        char *json = malloc(size + 1);
                        if (json) {
                            size_t nread = fread(json, 1, size, f);
                            json[nread] = '\0';

                            const char *ui_hier = strstr(json, "\"ui_hierarchy\"");
                            if (ui_hier) {
                                const char *obj_start = strchr(ui_hier + 14, '{');
                                if (obj_start) {
                                    int depth = 1;
                                    const char *obj_end = obj_start + 1;
                                    while (*obj_end && depth > 0) {
                                        if (*obj_end == '{') depth++;
                                        else if (*obj_end == '}') depth--;
                                        obj_end++;
                                    }
                                    int len = (int)(obj_end - obj_start);
                                    if (len > 0 && len < SHADOW_PARAM_VALUE_LEN - 1) {
                                        memcpy(shadow_param->value, obj_start, len);
                                        shadow_param->value[len] = '\0';
                                        shadow_param->error = 0;
                                        shadow_param->result_len = len;
                                        free(json);
                                        fclose(f);
                                        shadow_param_publish_response(req_id);
                                        return;
                                    }
                                }
                            }
                            free(json);
                        }
                    }
                    fclose(f);
                }
                shadow_param->error = 12;
                shadow_param->result_len = -1;
            } else if (mfx->api && mfx->instance && mfx->api->get_param) {
                int len = mfx->api->get_param(mfx->instance, param_key,
                                               shadow_param->value, SHADOW_PARAM_VALUE_LEN);
                if (len >= 0) {
                    shadow_param->error = 0;
                    shadow_param->result_len = len;
                } else {
                    shadow_param->error = 10;
                    shadow_param->result_len = -1;
                }
            } else {
                shadow_param->error = 11;
                shadow_param->result_len = -1;
            }
        } else {
            shadow_param->error = 6;
            shadow_param->result_len = -1;
        }
        shadow_param_publish_response(req_id);
        return;
    }

    /* Handle overtake DSP params - delegate to shim */
    if (strncmp(shadow_param->key, "overtake_dsp:", 13) == 0) {
        if (host.handle_param_special && host.handle_param_special(req_type, req_id)) {
            shadow_param_publish_response(req_id);
            return;
        }
        /* Fallback if no handler */
        shadow_param->error = 13;
        shadow_param->result_len = -1;
        shadow_param_publish_response(req_id);
        return;
    }

    int slot = shadow_param->slot;
    if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) {
        shadow_param->error = 1;
        shadow_param->result_len = -1;
        shadow_param_publish_response(req_id);
        return;
    }

    /* Handle slot-level params first */
    if (req_type == 1) {
        if (shadow_handle_slot_param_set(slot, shadow_param->key, shadow_param->value)) {
            shadow_param->error = 0;
            shadow_param->result_len = 0;
            shadow_param_publish_response(req_id);
            return;
        }
    }
    else if (req_type == 2) {
        int len = shadow_handle_slot_param_get(slot, shadow_param->key,
                                                shadow_param->value, SHADOW_PARAM_VALUE_LEN);
        if (len >= 0) {
            shadow_param->error = 0;
            shadow_param->result_len = len;
            shadow_param_publish_response(req_id);
            return;
        }
    }

    /* Not a slot param - forward to plugin */
    if (!shadow_plugin_v2 || !shadow_chain_slots[slot].instance) {
        shadow_param->error = 2;
        shadow_param->result_len = -1;
        shadow_param_publish_response(req_id);
        return;
    }

    if (req_type == 1) {  /* SET param */
        if (shadow_plugin_v2->set_param) {
            char key_copy[SHADOW_PARAM_KEY_LEN];
            static char value_copy[SHADOW_PARAM_VALUE_LEN];
            strncpy(key_copy, shadow_param->key, sizeof(key_copy) - 1);
            key_copy[sizeof(key_copy) - 1] = '\0';
            strncpy(value_copy, shadow_param->value, sizeof(value_copy) - 1);
            value_copy[sizeof(value_copy) - 1] = '\0';

            shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance,
                                        key_copy, value_copy);
            shadow_param->error = 0;
            shadow_param->result_len = 0;

            if (strcmp(key_copy, "synth:module") == 0) {
                if (value_copy[0] != '\0') {
                    shadow_chain_slots[slot].active = 1;
                    if (shadow_chain_slots[slot].forward_channel == -1 && shadow_plugin_v2->get_param) {
                        char fwd_buf[16];
                        int len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance,
                            "synth:default_forward_channel", fwd_buf, sizeof(fwd_buf));
                        if (len > 0) {
                            fwd_buf[len < (int)sizeof(fwd_buf) ? len : (int)sizeof(fwd_buf) - 1] = '\0';
                            int default_fwd = atoi(fwd_buf);
                            if (default_fwd >= 0 && default_fwd <= 15) {
                                shadow_chain_slots[slot].forward_channel = default_fwd;
                                shadow_ui_state_update_slot(slot);
                            }
                        }
                    }
                }
            }
            if (!shadow_chain_slots[slot].active &&
                (strcmp(key_copy, "fx1:module") == 0 ||
                 strcmp(key_copy, "fx2:module") == 0) &&
                value_copy[0] != '\0') {
                shadow_chain_slots[slot].active = 1;
            }
            if (strcmp(key_copy, "load_patch") == 0 ||
                strcmp(key_copy, "patch") == 0) {
                int idx = atoi(value_copy);
                if (idx < 0 || idx == SHADOW_PATCH_INDEX_NONE) {
                    shadow_chain_slots[slot].active = 0;
                    shadow_chain_slots[slot].patch_index = -1;
                    capture_clear(&shadow_chain_slots[slot].capture);
                    shadow_chain_slots[slot].patch_name[0] = '\0';
                } else {
                    shadow_chain_slots[slot].active = 1;
                    shadow_chain_slots[slot].patch_index = idx;
                    shadow_slot_load_capture(slot, idx);

                    if (shadow_chain_slots[slot].forward_channel == -1 && shadow_plugin_v2->get_param) {
                        char fwd_buf[16];
                        int len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance,
                            "synth:default_forward_channel", fwd_buf, sizeof(fwd_buf));
                        if (len > 0) {
                            fwd_buf[len < (int)sizeof(fwd_buf) ? len : (int)sizeof(fwd_buf) - 1] = '\0';
                            int default_fwd = atoi(fwd_buf);
                            if (default_fwd >= 0 && default_fwd <= 15) {
                                shadow_chain_slots[slot].forward_channel = default_fwd;
                            }
                        }
                    }
                }
                shadow_ui_state_update_slot(slot);
            }

            if (shadow_midi_out_log_enabled()) {
                if (strcmp(key_copy, "synth:module") == 0 ||
                    strcmp(key_copy, "fx1:module") == 0 ||
                    strcmp(key_copy, "fx2:module") == 0 ||
                    strcmp(key_copy, "midi_fx1:module") == 0) {
                    shadow_midi_out_logf("param_set: slot=%d key=%s val=%s active=%d",
                        slot, key_copy, value_copy, shadow_chain_slots[slot].active);
                }
            }
        } else {
            shadow_param->error = 3;
            shadow_param->result_len = -1;
        }
    }
    else if (req_type == 2) {  /* GET param */
        if (shadow_plugin_v2->get_param) {
            memset(shadow_param->value, 0, 256);
            int len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance,
                                                  shadow_param->key,
                                                  shadow_param->value,
                                                  SHADOW_PARAM_VALUE_LEN);
            if (len >= 0) {
                if (len < SHADOW_PARAM_VALUE_LEN) {
                    shadow_param->value[len] = '\0';
                } else {
                    shadow_param->value[SHADOW_PARAM_VALUE_LEN - 1] = '\0';
                }
                shadow_param->error = 0;
                shadow_param->result_len = len;
            } else {
                shadow_param->error = 4;
                shadow_param->result_len = -1;
            }
        } else {
            shadow_param->error = 5;
            shadow_param->result_len = -1;
        }
    }
    else {
        shadow_param->error = 6;
        shadow_param->result_len = -1;
    }

    shadow_param_publish_response(req_id);
}
