/*
 * chain_synth_load.c — loading a slot's sound generator in three steps, so the
 * expensive two can run anywhere but the SPI callback.
 *
 *   STAGE   resolve the module, dlopen it, create_instance, read its
 *           module.json and chain_params — everything slow — into a staging
 *           record. Touches NOTHING on the chain instance except two fields
 *           that never change after create (module_dir, subplugin_host_api),
 *           so it may run on the shim's slot loader WHILE the callback keeps
 *           rendering this instance, old synth and FX chain included.
 *   COMMIT  pointer stores and small copies: detach the old synth, install the
 *           staged one, rebuild the bookkeeping that names it. Callback-safe:
 *           no file, no dlopen, no allocation. It is the only step that
 *           writes the synth fields the render path reads, which is why it has
 *           to run on the callback (or with the callback not running, at boot).
 *   RETIRE  destroy_instance + dlclose of the detached old synth. Slow for
 *           some modules (threads to join, child processes to reap), so the
 *           async path does it back on the loader.
 *
 * v2_load_synth / v2_unload_synth are those steps in sequence, so there is ONE
 * implementation of loading, not a synchronous one and an async one that can
 * drift (the lesson of schwung#303, which this replaces at a fraction of the
 * size by staging only the synth).
 *
 * THE SWAP FADES. The async path does not commit the moment the stage lands:
 * chain_synth_async_swap_step first ramps the old synth's output down over
 * SYNTH_SWAP_FADE_SAMPLES (synth only — v2_render_block applies the gain
 * before the FX chain), then commits. The FX chain runs throughout, so a
 * reverb tail rings out across the swap instead of stopping, and the old
 * synth leaves without a click. The new synth starts from silence anyway.
 *
 * ONE stage and ONE retire slot, statics: loads are serialised by the shim
 * (one slot-load job in flight, which holds the param channel), and the
 * synchronous path runs only where the async one cannot be running (boot, or
 * the loader itself, or the fallback when there is no loader).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* dlinfo / struct link_map, as in chain_host.c */
#endif
#ifdef __linux__
#include <link.h>    /* glibc; tests/host builds this file on macOS too */
#endif
#include "chain_internal.h"
#include "host/split_voices_parse.h"

typedef void (*render_split_fn_t)(void *, int16_t *const *, int, int16_t *, int);

typedef struct {
    int has_module;                 /* 0: commit clears the synth */
    void *handle;
    plugin_api_v2_t *api;
    void *instance;                 /* NULL with has_module: the overflow case */
    render_split_fn_t render_split;
    char module[MAX_NAME_LEN];
    char load_error[sizeof(((chain_instance_t *)0)->synth_load_error)];
    int param_count;                /* params themselves in s_stage_params */
    chain_child_keys_t child_keys;
    char split_ids[SPLIT_VOICES_MAX][SPLIT_VOICE_ID_LEN];
    int split_count;
    int default_fwd;
    int consumes_line_input;
    int requires_continuous;
    int wants_sysex;
    int touch_observe;
} synth_stage_t;

typedef struct {
    void *handle;
    plugin_api_v2_t *api;
    void *instance;
} synth_retire_t;

/* ~1.1 MB, BSS: chain_param_info_t[MAX_CHAIN_PARAMS]. Not on any stack. */
static chain_param_info_t s_stage_params[MAX_CHAIN_PARAMS];
static synth_stage_t s_stage;
static synth_retire_t s_retire;
/* A stage waiting for its commit. Without it a second swap_step would install
 * the same staged module twice — two owners of one instance, two destroys. */
static int s_stage_ready;

/* ---------------------------------------------------------------- STAGE -- */

/* Returns 0 with st filled (has_module 0 for "none" or a failed load: the old
 * synth is still removed at commit, as it always was), never touching inst
 * beyond module_dir / subplugin_host_api and the log. */
static void synth_stage(chain_instance_t *inst, const char *module_name_in,
                        synth_stage_t *st, chain_param_info_t *params)
{
    char msg[256];
    char synth_path[MAX_PATH_LEN];
    char module_name[MAX_NAME_LEN];

    memset(st, 0, sizeof(*st));
    st->default_fwd = -1;
    if (!module_name_in || !module_name_in[0] || strcmp(module_name_in, "none") == 0)
        return;
    if (!valid_module_name(module_name_in)) {
        v2_chain_log(inst, "Invalid synth module name");
        return;
    }
    /* A copy at once: the caller's pointer may be a shared param buffer. */
    strncpy(module_name, module_name_in, MAX_NAME_LEN - 1);
    module_name[MAX_NAME_LEN - 1] = '\0';

    /* All sound generators live in modules/sound_generators/. A pack entry
     * ("rnbo-synth-graph-Test") resolves to its parent module directory, with
     * the pack path handed to create_instance as config JSON. */
    char *pack_config = NULL;
    char pack_config_buf[1024];
    snprintf(synth_path, sizeof(synth_path), "%s/../sound_generators/%s",
             inst->module_dir, module_name);
    struct stat path_st;
    if (stat(synth_path, &path_st) != 0 || !S_ISDIR(path_st.st_mode)) {
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
                        snprintf(synth_path, sizeof(synth_path), "%s/%s", sg_dir, ent->d_name);
                        snprintf(pack_config_buf, sizeof(pack_config_buf),
                                 "{\"pack\":\"%s/%s/packs/%s\"}", sg_dir, ent->d_name, pack_name);
                        pack_config = pack_config_buf;
                        snprintf(msg, sizeof(msg), "Resolved pack: %s -> %s", module_name, synth_path);
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
    snprintf(msg, sizeof(msg), "Loading synth: %s", dsp_path);
    v2_chain_log(inst, msg);

    void *handle = dlopen(dsp_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        snprintf(msg, sizeof(msg), "dlopen failed: %s", dlerror());
        v2_chain_log(inst, msg);
        return;
    }
    /* The module's LOAD BASE, so a crash inside it can be attributed: the
     * shim's SIGSEGV handler prints raw pc/lr, and `lr - base` is what
     * addr2line wants. Logged here because the synth position is where a
     * crash on load boot-loops the device (it is restored every boot). */
#ifdef __linux__
    {
        struct link_map *lm = NULL;
        if (dlinfo(handle, RTLD_DI_LINKMAP, &lm) == 0 && lm) {
            snprintf(msg, sizeof(msg), "loaded %s base=0x%lx",
                     module_name, (unsigned long)lm->l_addr);
            v2_chain_log(inst, msg);
        }
    }
#endif

    /* Optional per-voice render; NULL is the normal answer. The module
     * contract for it (render_split and render_block chosen PER FRAME, so both
     * must share voice/envelope state; it ACCUMULATES; main_out is the
     * no-voice output) is documented at move_plugin_render_split in
     * docs/MODULES.md and in bus_mix.h. Installed only at COMMIT, never while
     * the previous synth is live. */
    render_split_fn_t render_split_fn =
        (render_split_fn_t)dlsym(handle, "move_plugin_render_split");

    move_plugin_init_v2_fn init_v2 = (move_plugin_init_v2_fn)dlsym(handle, MOVE_PLUGIN_INIT_V2_SYMBOL);
    if (!init_v2) {
        snprintf(msg, sizeof(msg), "Synth %s does not support V2 API (V2 required)", module_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return;
    }
    plugin_api_v2_t *api = init_v2(&inst->subplugin_host_api);
    if (!api || api->api_version != MOVE_PLUGIN_API_VERSION_2) {
        snprintf(msg, sizeof(msg), "Synth %s V2 API version mismatch", module_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return;
    }
    void *synth_inst = api->create_instance(synth_path, pack_config);
    if (!synth_inst) {
        snprintf(msg, sizeof(msg), "Synth %s V2 create_instance failed", module_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return;
    }

    st->handle = handle;
    st->api = api;
    st->instance = synth_inst;
    st->render_split = render_split_fn;
    strncpy(st->module, module_name, MAX_NAME_LEN - 1);

    /* UI / parameter JSON over the param channel's limit: proceed with a NULL
     * instance so the UI loads and shows the error. */
    {
        char *temp_buf = (char *)malloc(262144);
        if (temp_buf) {
            int cp_len = 0, ui_len = 0;
            if (api->get_param) {
                cp_len = api->get_param(synth_inst, "chain_params", temp_buf, 262144);
                ui_len = api->get_param(synth_inst, "ui_hierarchy", temp_buf, 262144);
            }
            free(temp_buf);
            if (cp_len >= SHADOW_PARAM_VALUE_LEN - 1 || ui_len >= SHADOW_PARAM_VALUE_LEN - 1) {
                snprintf(msg, sizeof(msg),
                         "Synth %s UI or param JSON too large (chain_params: %d, ui_hierarchy: %d). Max %d.",
                         module_name, cp_len, ui_len, SHADOW_PARAM_VALUE_LEN - 1);
                v2_chain_log(inst, msg);
                snprintf(st->load_error, sizeof(st->load_error), "UI buffer overflow");
                api->destroy_instance(synth_inst);
                st->instance = NULL;
                parse_chain_params(synth_path, params, &st->param_count);
                chain_child_keys_load(&st->child_keys, synth_path);
                st->has_module = 1;
                return;
            }
        }
    }

    if (parse_chain_params(synth_path, params, &st->param_count) < 0) {
        v2_chain_log(inst, "ERROR: Failed to parse synth parameters");
        api->destroy_instance(synth_inst);
        dlclose(handle);
        memset(st, 0, sizeof(*st));
        st->default_fwd = -1;
        return;
    }
    chain_child_keys_load(&st->child_keys, synth_path);   /* `pad7_transpose` -> `transpose` */

    if (api->get_param) {
        char split_buf[4096];
        split_buf[0] = '\0';
        int got = api->get_param(synth_inst, "split_voices", split_buf, sizeof(split_buf));
        /* got <= 0: the module has no split support — a real answer. */
        if (got > 0) {
            /* Nothing NUL-terminates it after the plugin call. */
            split_buf[sizeof(split_buf) - 1] = '\0';
            st->split_count = split_voices_parse(split_buf, st->split_ids,
                                                 SPLIT_VOICES_MAX, SPLIT_VOICE_ID_LEN);
        }
    }

    /* capabilities from module.json */
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
                            st->default_fwd = -2;  /* passthrough (MPE) */
                            v2_chain_log(inst, "Synth default_forward_channel: passthrough");
                        } else if (fwd_ch >= 1 && fwd_ch <= 16) {
                            st->default_fwd = fwd_ch - 1;
                            snprintf(msg, sizeof(msg), "Synth default_forward_channel: %d", fwd_ch);
                            v2_chain_log(inst, msg);
                        }
                    }
                    /* Line input consumer (feedback risk on boot): mirrors the
                     * JS consumesLineInput() — audio_in is a JSON BOOLEAN, so
                     * json_get_int would read atoi("true") = 0. */
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
                                st->consumes_line_input = 1;
                                /* and keep-alive: nothing the shim sees would wake it */
                                st->requires_continuous = 1;
                                v2_chain_log(inst, "Synth consumes line input (feedback risk on boot)");
                            }
                        }
                    }
                    if (json_get_flag_in_section(json, "capabilities", "requires_continuous_processing"))
                        st->requires_continuous = 1;
                    if (st->requires_continuous)
                        v2_chain_log(inst, "Synth keep-alive: exempt from silence-skip");
                    if (json_get_flag_in_section(json, "capabilities", "wants_sysex"))
                        st->wants_sysex = 1;
                    if (json_get_flag_in_section(json, "capabilities", "touch_observe"))
                        st->touch_observe = 1;
                    free(json);
                }
            }
            fclose(f);
        }
    }
    chain_warm_render(api->render_block, synth_inst);
    st->has_module = 1;
}

/* --------------------------------------------------------------- COMMIT -- */

/* Detach whatever synth is loaded into *old and reset every field that names
 * it. No destroy, no dlclose: that is the retire's. Callback-safe. */
static void synth_detach(chain_instance_t *inst, synth_retire_t *old)
{
    old->handle = inst->synth_handle;
    old->api = inst->synth_plugin_v2;
    old->instance = inst->synth_instance;

    inst->synth_load_error[0] = '\0';
    chain_mod_clear_target_entries(inst, "synth", 0);
    inst->synth_handle = NULL;
    inst->synth_plugin_v2 = NULL;
    inst->synth_instance = NULL;
    /* Cleared with the handle it was resolved against: a function pointer
     * into a mapping about to be dlclose'd. */
    inst->synth_render_split = NULL;
    inst->current_synth_module[0] = '\0';
    inst->synth_param_count = 0;
    chain_child_keys_load(&inst->synth_child_keys, NULL);
    inst->mod_param_refresh_ms_synth = 0;
    inst->synth_default_forward_channel = -1;
    inst->synth_consumes_line_input = 0;
    inst->synth_requires_continuous = 0;
    inst->synth_wants_sysex = 0;
    inst->synth_touch_observe = 0;
    inst->synth_last_note = -1;   /* would name a voice in a list that is gone */
    inst->synth_bypassed = 0;
    memset(inst->synth_split_voice_ids, 0, sizeof(inst->synth_split_voice_ids));
    inst->synth_split_voice_count = 0;
    chain_reset_voice_bus(inst);
    chain_voice_sends_load(inst);   /* with no plugin, exactly the clear */
    /* The orphan counts are a fact about resolving a bus's stored ids AGAINST
     * A MODULE; with none they would report the departed module's mismatch.
     * Recomputed by chain_bus_rebuild_voice_map at the next load. */
    for (int b = 0; b < SLOT_BUSES; b++) inst->buses[b].orphan_count = 0;
    /* BUSES SURVIVE A SYNTH SWAP, deliberately: they are the user's routing
     * for this SLOT, not a property of the synth. Only the voice->bus map is
     * reset (above), because the new module's voice list differs. */
}

static void synth_install(chain_instance_t *inst, const synth_stage_t *st,
                          const chain_param_info_t *params)
{
    if (!st->has_module) return;
    inst->synth_handle = st->handle;
    inst->synth_plugin_v2 = st->api;
    inst->synth_instance = st->instance;
    inst->synth_render_split = st->render_split;
    strncpy(inst->current_synth_module, st->module, MAX_NAME_LEN - 1);
    inst->current_synth_module[MAX_NAME_LEN - 1] = '\0';
    memcpy(inst->synth_load_error, st->load_error, sizeof(inst->synth_load_error));
    /* Only the entries in use: a few hundred KB at most, usually far less. */
    memcpy(inst->synth_params, params, sizeof(chain_param_info_t) * (size_t)st->param_count);
    inst->synth_param_count = st->param_count;
    inst->synth_child_keys = st->child_keys;
    inst->mod_param_refresh_ms_synth = 0;
    inst->synth_default_forward_channel = st->default_fwd;
    inst->synth_consumes_line_input = st->consumes_line_input;
    inst->synth_requires_continuous = st->requires_continuous;
    inst->synth_wants_sysex = st->wants_sysex;
    inst->synth_touch_observe = st->touch_observe;
    memcpy(inst->synth_split_voice_ids, st->split_ids, sizeof(st->split_ids));
    inst->synth_split_voice_count = st->split_count;
    if (!st->instance) return;   /* the overflow case: named, nothing to map */
    /* A voice id resolves against the module loaded NOW; the buses' stored ids
     * survive untouched and an undeclared one becomes an orphan. */
    chain_bus_rebuild_voice_map(inst);
    /* AFTER the params: a send level's range comes out of synth_params. */
    chain_voice_sends_load(inst);
}

/* What a `synth:module` write does once the new module is staged: panic and
 * detach the old, install the new. Callback-safe. */
static void synth_commit(chain_instance_t *inst, const synth_stage_t *st,
                         const chain_param_info_t *params, synth_retire_t *old)
{
    v2_synth_panic(inst);
    synth_detach(inst, old);
    smoother_reset(&inst->synth_smoother);
    synth_install(inst, st, params);
    if (!st->has_module) inst->knob_mapping_count = 0;
    inst->dirty = 1;
    inst->synth_swap_fading = 0;
    inst->synth_swap_gain = 1.0f;
    inst->synth_swap_frames = 0;
    inst->synth_swap_rendered = 0;
    char msg[256];
    snprintf(msg, sizeof(msg), st->has_module ? "Synth v2 loaded: %s (%d params)"
                                              : "Synth cleared%s", st->module, st->param_count);
    v2_chain_log(inst, msg);
}

/* --------------------------------------------------------------- RETIRE -- */

static void synth_retire(synth_retire_t *old)
{
    if (old->api && old->instance && old->api->destroy_instance)
        old->api->destroy_instance(old->instance);
    if (old->handle) dlclose(old->handle);
    old->handle = NULL;
    old->api = NULL;
    old->instance = NULL;
}

/* -------------------------------------------------- synchronous entries -- */

void v2_unload_synth(chain_instance_t *inst)
{
    if (!inst) return;
    synth_retire_t old;
    synth_detach(inst, &old);
    synth_retire(&old);
}

int v2_load_synth(chain_instance_t *inst, const char *module_name)
{
    if (!inst || !module_name || !module_name[0]) return -1;
    synth_stage(inst, module_name, &s_stage, s_stage_params);
    if (!s_stage.has_module) return -1;   /* failed: the caller already unloaded */
    synth_retire_t old;
    synth_commit(inst, &s_stage, s_stage_params, &old);
    synth_retire(&old);
    return 0;
}

/* The `synth:module` write, synchronously: chain_host.c's set_param. */
void chain_synth_set_module(chain_instance_t *inst, const char *val)
{
    synth_stage(inst, val, &s_stage, s_stage_params);
    synth_retire_t old;
    synth_commit(inst, &s_stage, s_stage_params, &old);
    synth_retire(&old);
}

/* ------------------------------------------------------- async (shim) -- */

/*
 * The shim's slot loader drives these three through chain_fx_load.c's
 * generic exports (chain_load_stage / _swap_step / _retire):
 *
 *   chain_synth_async_stage    LOADER. Build the new synth while the callback keeps
 *                        rendering this instance untouched.
 *   chain_synth_async_swap_step CALLBACK, every frame until it returns 1. Fades the
 *                        old synth (only) out, then commits. `force` commits
 *                        at once — for a slot the shim is not rendering, whose
 *                        fade would never advance.
 *   chain_synth_async_retire   LOADER. Destroy what the commit detached.
 */
void chain_synth_async_stage(chain_instance_t *inst, const char *module_name)
{
    if (!inst) { memset(&s_stage, 0, sizeof(s_stage)); s_stage_ready = 0; return; }
    synth_stage(inst, module_name, &s_stage, s_stage_params);
    s_stage_ready = 1;
}

int chain_synth_async_swap_step(chain_instance_t *inst, int force)
{
    if (!inst || !s_stage_ready) return 1;   /* nothing staged: nothing to do */
    /* Nothing to fade from: commit now. */
    if (!inst->synth_instance) force = 1;
    if (!inst->synth_swap_fading) {
        inst->synth_swap_gain = 1.0f;   /* before the flag: render reads both */
        inst->synth_swap_frames = 0;
        inst->synth_swap_fading = 1;
    }
    /* A fade the render path is not advancing must not hold the swap: the
     * shim's idle gate skips a SILENT slot's render_block, and a silent synth
     * has nothing to fade. No render since the last step means commit now;
     * the block bound is the backstop. */
    if (++inst->synth_swap_frames > 1 && !inst->synth_swap_rendered) force = 1;
    inst->synth_swap_rendered = 0;
    if (inst->synth_swap_frames > SYNTH_SWAP_FADE_MAX_BLOCKS) force = 1;
    if (!force && inst->synth_swap_gain > 0.0f) return 0;
    synth_commit(inst, &s_stage, s_stage_params, &s_retire);
    s_stage_ready = 0;
    return 1;
}

void chain_synth_async_retire(void)
{
    synth_retire(&s_retire);
}
