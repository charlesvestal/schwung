/*
 * chain_fx_load.c — loading ONE audio FX or MIDI FX position, in the same three
 * steps as the synth (chain_synth_load.c), so changing one module touches that
 * module and nothing else in the slot.
 *
 *   STAGE   dlopen, create_instance, chain_params, ui_hierarchy, module.json —
 *           into a staging record and staging BUFFERS. Touches nothing on the
 *           chain instance but module_dir / subplugin_host_api, so it runs on
 *           the shim's slot loader while the callback keeps rendering the slot.
 *   COMMIT  pointer stores. The per-position parameter and hierarchy buffers
 *           (~1.1 MB + 64 KB, calloc'd by chain_alloc_position_storage) are
 *           SWAPPED with the staged ones rather than copied, so the commit is
 *           cheap on the callback and ownership only rotates — the buffer that
 *           leaves the position becomes the next stage's.
 *   RETIRE  destroy_instance + dlclose of what the commit detached.
 *
 * THE POSITION CROSSFADES, NOT THE SLOT. An audio FX swap fades that one
 * position from its wet output to its dry input (FX_SWAP_FADE_SAMPLES), swaps,
 * then fades from dry into the new module's output. The synth, every other FX
 * position and Move's track run straight through; only the module you changed
 * is ever out of the signal. A MIDI FX carries no audio and swaps at once.
 *
 * The synchronous entry points (v2_load_audio_fx_slot & co.) are the same
 * steps in sequence — one implementation, which is what keeps the async and
 * the boot/fallback paths from drifting.
 */

#include "chain_internal.h"

/* ============================================================ audio FX == */

typedef void (*fx_on_midi_fn_t)(void *, const uint8_t *, int, int);

typedef struct {
    int invalid;              /* refused name: commit changes NOTHING */
    int clear;                /* empty / "none": commit unloads and shrinks */
    int has_module;           /* a module was built */
    void *handle;
    audio_fx_api_v2_t *api;
    void *instance;
    fx_on_midi_fn_t on_midi;
    char module[MAX_NAME_LEN];
    int param_count;
    chain_param_info_t *params;   /* staging buffer, rotated at commit */
    char *hierarchy;              /* staging buffer, rotated at commit */
    chain_child_keys_t child_keys;
    int requires_continuous;
} fx_stage_t;

typedef struct {
    int midi;
    void *handle;
    void *api;                /* audio_fx_api_v2_t* or midi_fx_api_v1_t* */
    void *instance;
} fx_retire_t;

static fx_stage_t s_fx;
static fx_retire_t s_fx_retire;

static int stage_buffers(chain_param_info_t **params, char **hierarchy)
{
    if (!*params) *params = (chain_param_info_t *)calloc(MAX_CHAIN_PARAMS, sizeof(chain_param_info_t));
    if (!*hierarchy) *hierarchy = (char *)calloc(1, CHAIN_UI_HIERARCHY_LEN);
    return *params && *hierarchy;
}

/* Read capabilities.<flag> style hints out of module.json. */
static char *read_module_json(const char *dir)
{
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/module.json", dir);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size < 65536 && (buf = malloc(size + 1))) {
        size_t nr = fread(buf, 1, size, f);
        buf[nr] = '\0';
    }
    fclose(f);
    return buf;
}

static void fx_stage(chain_instance_t *inst, const char *fx_name, fx_stage_t *st)
{
    char msg[256], fx_path[MAX_PATH_LEN], fx_dir[MAX_PATH_LEN];
    chain_param_info_t *keep_params = st->params;
    char *keep_hier = st->hierarchy;
    memset(st, 0, sizeof(*st));
    st->params = keep_params;
    st->hierarchy = keep_hier;

    if (fx_name && fx_name[0] && strcmp(fx_name, "none") != 0 && !valid_module_name(fx_name)) {
        v2_chain_log(inst, "Invalid audio FX name");
        st->invalid = 1;
        return;
    }
    if (!fx_name || !fx_name[0] || strcmp(fx_name, "none") == 0) { st->clear = 1; return; }
    if (!stage_buffers(&st->params, &st->hierarchy)) {
        v2_chain_log(inst, "ERROR: no memory for audio FX metadata");
        return;
    }

    snprintf(fx_path, sizeof(fx_path), "%s/../audio_fx/%s/%s.so", inst->module_dir, fx_name, fx_name);
    snprintf(fx_dir, sizeof(fx_dir), "%s/../audio_fx/%s", inst->module_dir, fx_name);

    void *handle = dlopen(fx_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        snprintf(msg, sizeof(msg), "dlopen failed for FX %s: %s", fx_name, dlerror());
        v2_chain_log(inst, msg);
        return;
    }
    audio_fx_init_v2_fn init_v2 = (audio_fx_init_v2_fn)dlsym(handle, AUDIO_FX_INIT_V2_SYMBOL);
    if (!init_v2) {
        snprintf(msg, sizeof(msg), "Audio FX %s does not support V2 API (V2 required)", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return;
    }
    audio_fx_api_v2_t *api = init_v2(&inst->subplugin_host_api);
    if (!api || api->api_version != AUDIO_FX_API_VERSION_2) {
        snprintf(msg, sizeof(msg), "Audio FX %s V2 API version mismatch", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return;
    }
    void *fx_inst = api->create_instance(fx_dir, NULL);
    if (!fx_inst) {
        snprintf(msg, sizeof(msg), "Audio FX %s V2 create_instance failed", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return;
    }
    if (parse_chain_params(fx_dir, st->params, &st->param_count) < 0) {
        v2_chain_log(inst, "ERROR: Failed to parse audio FX parameters");
        api->destroy_instance(fx_inst);
        dlclose(handle);
        st->param_count = 0;
        return;
    }
    st->hierarchy[0] = '\0';
    parse_ui_hierarchy_cache(fx_dir, st->hierarchy, CHAIN_UI_HIERARCHY_LEN);
    /* Optional MIDI handler (e.g. ducker). */
    st->on_midi = (fx_on_midi_fn_t)dlsym(handle, "move_audio_fx_on_midi");
    /* Stateful FX (loopers, modulated delays) opt out of the shim's
     * silence-skip so their internal time keeps advancing. */
    char *mj = read_module_json(fx_dir);
    if (mj) {
        if (json_get_flag_in_section(mj, "capabilities", "requires_continuous_processing"))
            st->requires_continuous = 1;
        free(mj);
    }
    chain_child_keys_load(&st->child_keys, fx_dir);
    chain_warm_process(api->process_block, fx_inst);
    st->handle = handle;
    st->api = api;
    st->instance = fx_inst;
    strncpy(st->module, fx_name, MAX_NAME_LEN - 1);
    st->has_module = 1;
}

/* Callback-safe: detach position `slot` into *old and reset its fields. */
static void fx_detach(chain_instance_t *inst, int slot, fx_retire_t *old)
{
    old->midi = 0;
    old->handle = inst->fx_handles[slot];
    old->api = inst->fx_is_v2[slot] ? (void *)inst->fx_plugins_v2[slot] : NULL;
    old->instance = inst->fx_is_v2[slot] ? inst->fx_instances[slot] : NULL;

    char target_name[16];
    chain_fx_component_id(target_name, sizeof(target_name), "fx", slot);
    chain_mod_clear_target_entries(inst, target_name, 0);
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

/* Callback-safe. Returns 1 if a module was installed. */
static int fx_commit(chain_instance_t *inst, int slot, fx_stage_t *st, fx_retire_t *old)
{
    char msg[256];
    old->midi = 0;
    old->handle = old->api = old->instance = NULL;
    if (st->invalid) return 0;               /* a refused name changes nothing */
    fx_detach(inst, slot, old);
    if (st->clear) {
        while (inst->fx_count > 0 && inst->fx_handles[inst->fx_count - 1] == NULL)
            inst->fx_count--;
        snprintf(msg, sizeof(msg), "Audio FX slot %d cleared", slot);
        v2_chain_log(inst, msg);
        return 0;
    }
    if (!st->has_module) return 0;           /* failed: the position stays empty */

    inst->fx_handles[slot] = st->handle;
    inst->fx_plugins_v2[slot] = st->api;
    inst->fx_instances[slot] = st->instance;
    inst->fx_is_v2[slot] = 1;
    inst->fx_on_midi[slot] = st->on_midi;
    strncpy(inst->current_fx_modules[slot], st->module, MAX_NAME_LEN - 1);
    inst->current_fx_modules[slot][MAX_NAME_LEN - 1] = '\0';
    /* ROTATE the metadata buffers: the staged ones become the position's, the
     * position's become the next stage's. No copy on the callback. */
    chain_param_info_t *p = inst->fx_params[slot];
    inst->fx_params[slot] = st->params;
    st->params = p;
    char *h = inst->fx_ui_hierarchy[slot];
    inst->fx_ui_hierarchy[slot] = st->hierarchy;
    st->hierarchy = h;
    inst->fx_param_counts[slot] = st->param_count;
    inst->mod_param_refresh_ms_fx[slot] = 0;
    inst->fx_requires_continuous[slot] = st->requires_continuous;
    inst->fx_child_keys[slot] = st->child_keys;
    if (slot >= inst->fx_count) inst->fx_count = slot + 1;
    st->has_module = 0;                      /* handed over */
    snprintf(msg, sizeof(msg), "Audio FX v2 loaded: %s (slot %d, %d params)",
             inst->current_fx_modules[slot], slot, inst->fx_param_counts[slot]);
    v2_chain_log(inst, msg);
    return 1;
}

static void fx_retire(fx_retire_t *old)
{
    if (old->api && old->instance) {
        if (old->midi) {
            midi_fx_api_v1_t *m = (midi_fx_api_v1_t *)old->api;
            if (m->destroy_instance) m->destroy_instance(old->instance);
        } else {
            audio_fx_api_v2_t *a = (audio_fx_api_v2_t *)old->api;
            if (a->destroy_instance) a->destroy_instance(old->instance);
        }
    }
    if (old->handle) dlclose(old->handle);
    old->handle = old->api = old->instance = NULL;
}

/* A stage that was built but never committed (superseded, or the slot went
 * away) must not leak its instance and handle. */
static void fx_stage_discard(fx_stage_t *st, int midi)
{
    if (!st->has_module) return;
    fx_retire_t r = { midi, st->handle, st->api, st->instance };
    fx_retire(&r);
    st->has_module = 0;
}

/* ---- the synchronous entries (fxN:module on the callback, reorder) ---- */

void v2_unload_audio_fx_slot(chain_instance_t *inst, int slot)
{
    if (!inst || slot < 0 || slot >= MAX_AUDIO_FX) return;
    fx_retire_t old;
    fx_detach(inst, slot, &old);
    fx_retire(&old);
}

int v2_load_audio_fx_slot(chain_instance_t *inst, int slot, const char *fx_name)
{
    if (!inst || slot < 0 || slot >= MAX_AUDIO_FX) return -1;
    fx_stage_discard(&s_fx, 0);
    fx_stage(inst, fx_name, &s_fx);
    fx_retire_t old;
    int installed = fx_commit(inst, slot, &s_fx, &old);
    fx_retire(&old);
    if (s_fx.invalid) return -1;
    if (s_fx.clear) return 0;
    return installed ? 0 : -1;
}

/* ======================================================= the crossfade == */

/*
 * Process one main-chain audio FX position. Used by BOTH FX loops —
 * v2_render_block's and chain_process_fx's (same-frame mode) — so the swap
 * crossfade and the bypass discipline are written once.
 *
 * Bypass: always process so delay lines and reverb tails keep advancing, and
 * restore the dry so unbypass resumes cleanly.
 * Swap: out = dry + (wet - dry) * g, g ramping 1 -> 0 before the commit (the
 * old module leaves) and 0 -> 1 after it (the new one arrives).
 */
void chain_fx_run_position(chain_instance_t *inst, int i, int16_t *buf, int frames)
{
    if (frames > FRAMES_PER_BLOCK) frames = FRAMES_PER_BLOCK;
    const int bypassed = (i < MAX_AUDIO_FX && inst->fx_bypassed[i]);
    const int swapping = (inst->fx_swap_phase != 0 && inst->fx_swap_pos == i);
    int16_t dry[FRAMES_PER_BLOCK * 2];
    if (bypassed || swapping) memcpy(dry, buf, (size_t)frames * 2 * sizeof(int16_t));
    /* All loaded FX are v2 — the loader hard-requires it. */
    if (inst->fx_plugins_v2[i] && inst->fx_instances[i] && inst->fx_plugins_v2[i]->process_block)
        inst->fx_plugins_v2[i]->process_block(inst->fx_instances[i], buf, frames);
    if (bypassed) {
        memcpy(buf, dry, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }
    if (!swapping) return;
    inst->fx_swap_rendered = 1;
    float g = inst->fx_swap_gain;
    const float step = (inst->fx_swap_phase == 1 ? -1.0f : 1.0f) / (float)FX_SWAP_FADE_SAMPLES;
    for (int s = 0; s < frames; s++) {
        for (int c = 0; c < 2; c++) {
            const int k = 2 * s + c;
            const float d = (float)dry[k];
            float v = d + ((float)buf[k] - d) * g;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            buf[k] = (int16_t)v;
        }
        g += step;
        if (g < 0.0f) g = 0.0f;
        if (g > 1.0f) g = 1.0f;
    }
    inst->fx_swap_gain = g;
    if (inst->fx_swap_phase == 2 && g >= 1.0f) inst->fx_swap_phase = 0;   /* arrived */
}

/* ============================================================ MIDI FX == */

typedef struct {
    int clear;
    int has_module;
    void *handle;
    midi_fx_api_v1_t *api;
    void *instance;
    char module[MAX_NAME_LEN];
    int param_count;
    chain_param_info_t *params;
    char *hierarchy;
    chain_child_keys_t child_keys;
    int pre_capable;
    int wants_sysex;
} mfx_stage_t;

static mfx_stage_t s_mfx;

static void mfx_stage(chain_instance_t *inst, const char *fx_name, mfx_stage_t *st)
{
    char msg[256], fx_path[MAX_PATH_LEN], fx_dir[MAX_PATH_LEN];
    chain_param_info_t *keep_params = st->params;
    char *keep_hier = st->hierarchy;
    memset(st, 0, sizeof(*st));
    st->params = keep_params;
    st->hierarchy = keep_hier;

    if (!fx_name || !fx_name[0] || strcmp(fx_name, "none") == 0) { st->clear = 1; return; }
    if (!stage_buffers(&st->params, &st->hierarchy)) {
        v2_chain_log(inst, "ERROR: no memory for MIDI FX metadata");
        return;
    }
    snprintf(fx_path, sizeof(fx_path), "%s/../midi_fx/%s/dsp.so", inst->module_dir, fx_name);
    snprintf(fx_dir, sizeof(fx_dir), "%s/../midi_fx/%s", inst->module_dir, fx_name);
    snprintf(msg, sizeof(msg), "Loading MIDI FX: %s", fx_path);
    v2_chain_log(inst, msg);

    void *handle = dlopen(fx_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        snprintf(msg, sizeof(msg), "dlopen failed: %s", dlerror());
        v2_chain_log(inst, msg);
        return;
    }
    midi_fx_init_fn init_fn = (midi_fx_init_fn)dlsym(handle, MIDI_FX_INIT_SYMBOL);
    if (!init_fn) {
        snprintf(msg, sizeof(msg), "MIDI FX %s missing init symbol", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return;
    }
    midi_fx_api_v1_t *api = init_fn(&inst->subplugin_host_api);
    if (!api || api->api_version != MIDI_FX_API_VERSION) {
        snprintf(msg, sizeof(msg), "MIDI FX %s API version mismatch", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return;
    }
    void *instance = api->create_instance(fx_dir, NULL);
    if (!instance) {
        snprintf(msg, sizeof(msg), "MIDI FX %s create_instance failed", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return;
    }
    if (parse_chain_params(fx_dir, st->params, &st->param_count) < 0) {
        v2_chain_log(inst, "ERROR: Failed to parse MIDI FX parameters");
        api->destroy_instance(instance);
        dlclose(handle);
        st->param_count = 0;
        return;
    }
    chain_child_keys_load(&st->child_keys, fx_dir);
    st->hierarchy[0] = '\0';
    parse_ui_hierarchy_cache(fx_dir, st->hierarchy, CHAIN_UI_HIERARCHY_LEN);
    /* pre_capable informs the Shadow UI's Pre/Post default on first placement;
     * wants_sysex accepts the JSON boolean AND 1/0 — json_get_int_in_section
     * reads atoi("true") as 0, the audio_in trap. */
    char *mj = read_module_json(fx_dir);
    if (mj) {
        int cap = 0;
        if (json_get_int_in_section(mj, "capabilities", "pre_capable", &cap) == 0 && cap)
            st->pre_capable = 1;
        int wants = 0;
        if ((json_get_bool_in_section(mj, "capabilities", "wants_sysex", &wants) == 0 ||
             json_get_int_in_section(mj, "capabilities", "wants_sysex", &wants) == 0) && wants)
            st->wants_sysex = 1;
        free(mj);
    }
    st->handle = handle;
    st->api = api;
    st->instance = instance;
    strncpy(st->module, fx_name, MAX_NAME_LEN - 1);
    st->has_module = 1;
}

static void mfx_detach(chain_instance_t *inst, int slot, fx_retire_t *old)
{
    old->midi = 1;
    old->handle = inst->midi_fx_handles[slot];
    old->api = inst->midi_fx_plugins[slot];
    old->instance = inst->midi_fx_instances[slot];

    char target[16];
    snprintf(target, sizeof(target), "midi_fx%d", slot + 1);
    chain_mod_clear_target_entries(inst, target, 0);
    inst->midi_fx_handles[slot] = NULL;
    inst->midi_fx_plugins[slot] = NULL;
    inst->midi_fx_instances[slot] = NULL;
    inst->current_midi_fx_modules[slot][0] = '\0';
    inst->midi_fx_param_counts[slot] = 0;
    inst->mod_param_refresh_ms_midi_fx[slot] = 0;
    inst->midi_fx_ui_hierarchy[slot][0] = '\0';
    inst->midi_fx_pre_capable[slot] = 0;
    inst->midi_fx_wants_sysex[slot] = 0;
    inst->midi_fx_bypassed[slot] = 0;
    /* Stale refcount entries from an unloaded MIDI FX would orphan future
     * note-ons; the pad-held tracker and a buffered clock-driven inject batch
     * go with it. Chain-wide, not per slot — the tracker is not keyed by slot,
     * so unloading one FX can orphan a note another is holding (a stuck note
     * on Move until the next unload); keying it by slot would fix that. */
    memset(inst->pre_injected_notes, 0, sizeof(inst->pre_injected_notes));
    memset(inst->pre_pad_held, 0, sizeof(inst->pre_pad_held));
    inst->pre_delay_count = 0;
}

static int mfx_commit(chain_instance_t *inst, int slot, mfx_stage_t *st, fx_retire_t *old)
{
    char msg[256];
    mfx_detach(inst, slot, old);
    if (st->clear) {
        /* Trailing empties shrink the mark; an interior hole leaves it. */
        while (inst->midi_fx_count > 0 && inst->midi_fx_handles[inst->midi_fx_count - 1] == NULL)
            inst->midi_fx_count--;
        snprintf(msg, sizeof(msg), "MIDI FX slot %d cleared", slot);
        v2_chain_log(inst, msg);
        return 0;
    }
    if (!st->has_module) return 0;
    inst->midi_fx_handles[slot] = st->handle;
    inst->midi_fx_plugins[slot] = st->api;
    inst->midi_fx_instances[slot] = st->instance;
    inst->mod_param_refresh_ms_midi_fx[slot] = 0;
    strncpy(inst->current_midi_fx_modules[slot], st->module, MAX_NAME_LEN - 1);
    inst->current_midi_fx_modules[slot][MAX_NAME_LEN - 1] = '\0';
    chain_param_info_t *p = inst->midi_fx_params[slot];
    inst->midi_fx_params[slot] = st->params;
    st->params = p;
    char *h = inst->midi_fx_ui_hierarchy[slot];
    inst->midi_fx_ui_hierarchy[slot] = st->hierarchy;
    st->hierarchy = h;
    inst->midi_fx_param_counts[slot] = st->param_count;
    inst->midi_fx_child_keys[slot] = st->child_keys;
    inst->midi_fx_pre_capable[slot] = st->pre_capable;
    inst->midi_fx_wants_sysex[slot] = st->wants_sysex;
    if (slot >= inst->midi_fx_count) inst->midi_fx_count = slot + 1;
    st->has_module = 0;
    snprintf(msg, sizeof(msg), "MIDI FX loaded: %s (slot %d)", inst->current_midi_fx_modules[slot], slot);
    v2_chain_log(inst, msg);
    return 1;
}

/*
 * Tear down one slot. Does NOT touch midi_fx_count: the caller decides whether
 * the mark moves, because clearing an interior slot must not shorten the list.
 */
void v2_unload_midi_fx_slot(chain_instance_t *inst, int slot)
{
    if (!inst || slot < 0 || slot >= MAX_MIDI_FX) return;
    fx_retire_t old;
    mfx_detach(inst, slot, &old);
    fx_retire(&old);
}

/*
 * Load into exactly `slot` ("midi_fx<N>:module" lands in slot N). Whatever
 * was there goes first — including on failure, so a bad name leaves a clean
 * slot rather than a half-loaded one. A rewrite that SHORTENS the chain must
 * clear the tail explicitly ("midi_fx<N>:module=none"), as audio FX always
 * required.
 */
int v2_load_midi_fx_slot(chain_instance_t *inst, int slot, const char *fx_name)
{
    if (!inst || slot < 0 || slot >= MAX_MIDI_FX) return -1;
    if (s_mfx.has_module) {
        fx_retire_t r = { 1, s_mfx.handle, s_mfx.api, s_mfx.instance };
        fx_retire(&r);
        s_mfx.has_module = 0;
    }
    mfx_stage(inst, fx_name, &s_mfx);
    fx_retire_t old;
    int installed = mfx_commit(inst, slot, &s_mfx, &old);
    fx_retire(&old);
    if (s_mfx.clear) return 0;
    return installed ? 0 : -1;
}

/* ===================================================== async (the shim) == */

/*
 * One staged load at a time, any kind. The shim's slot loader drives these,
 * dlsym'd like chain_set_clip_phase (never a host_api_v1_t field):
 *
 *   chain_load_stage      LOADER. Build the module for `key` (synth:module,
 *                         fx<N>:module, midi_fx<N>:module). Returns 1 if the
 *                         key is one this file stages, 0 if not — the shim
 *                         only posts keys it knows are stageable.
 *   chain_load_swap_step  CALLBACK, every frame until it returns 1: fade the
 *                         outgoing module, then commit. `force` commits now.
 *   chain_load_retire     LOADER. Destroy what the commit detached.
 */
enum { LOAD_NONE = 0, LOAD_SYNTH, LOAD_FX, LOAD_MIDI_FX };
static int s_kind;
static int s_pos;
static int s_ready;

__attribute__((visibility("default")))
int chain_load_stage(void *instance, const char *key, const char *value)
{
    chain_instance_t *inst = (chain_instance_t *)instance;
    s_ready = 0;
    s_kind = LOAD_NONE;
    if (!inst || !key) return 0;
    if (strcmp(key, "synth:module") == 0) {
        chain_synth_async_stage(inst, value);
        s_kind = LOAD_SYNTH;
    } else {
        const char *sub = NULL;
        int pos = chain_fx_index_from_key(key, "fx", MAX_AUDIO_FX, &sub);
        if (pos >= 0 && sub && strcmp(sub, "module") == 0) {
            fx_stage_discard(&s_fx, 0);
            fx_stage(inst, value, &s_fx);
            s_kind = LOAD_FX;
            s_pos = pos;
        } else {
            pos = chain_fx_index_from_key(key, "midi_fx", MAX_MIDI_FX, &sub);
            if (pos < 0 || !sub || strcmp(sub, "module") != 0) return 0;
            if (s_mfx.has_module) {
                fx_retire_t r = { 1, s_mfx.handle, s_mfx.api, s_mfx.instance };
                fx_retire(&r);
                s_mfx.has_module = 0;
            }
            mfx_stage(inst, value, &s_mfx);
            s_kind = LOAD_MIDI_FX;
            s_pos = pos;
        }
    }
    s_ready = 1;
    return 1;
}

__attribute__((visibility("default")))
int chain_load_swap_step(void *instance, int force)
{
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst || !s_ready) return 1;   /* nothing staged: nothing to do */

    if (s_kind == LOAD_SYNTH) {
        if (!chain_synth_async_swap_step(inst, force)) return 0;
        s_ready = 0;
        return 1;
    }
    if (s_kind == LOAD_MIDI_FX) {
        /* No audio, nothing to fade: swap now. */
        mfx_commit(inst, s_pos, &s_mfx, &s_fx_retire);
        inst->dirty = 1;
        s_ready = 0;
        return 1;
    }

    /* LOAD_FX */
    const int pos = s_pos;
    if (inst->fx_swap_phase == 0 || inst->fx_swap_pos != pos || inst->fx_swap_phase == 2) {
        /* Start (or take over from another position's fade-in, which simply
         * snaps to full). Nothing audible to fade out: an empty position, one
         * past the chain, or a bypassed one (its output is the dry already). */
        const int audible = pos < inst->fx_count && inst->fx_instances[pos] &&
                            !inst->fx_bypassed[pos];
        /* Continuing a fade-in at this same position: fade out from where it
         * got to, not from full — a jump is the click this exists to avoid. */
        const float from = (inst->fx_swap_phase == 2 && inst->fx_swap_pos == pos)
                           ? inst->fx_swap_gain : 1.0f;
        inst->fx_swap_pos = pos;
        inst->fx_swap_gain = from;
        inst->fx_swap_frames = 0;
        inst->fx_swap_rendered = 0;
        inst->fx_swap_phase = 1;
        if (!audible) force = 1;
    }
    /* A fade the render is not advancing (the shim's idle gate skips a silent
     * slot's FX) must not hold the swap; the block bound is the backstop. */
    if (++inst->fx_swap_frames > 1 && !inst->fx_swap_rendered) force = 1;
    inst->fx_swap_rendered = 0;
    if (inst->fx_swap_frames > FX_SWAP_FADE_MAX_BLOCKS) force = 1;
    if (!force && inst->fx_swap_gain > 0.0f) return 0;

    int installed = fx_commit(inst, pos, &s_fx, &s_fx_retire);
    smoother_reset(&inst->fx_smoothers[pos]);
    inst->dirty = 1;
    if (installed) {
        inst->fx_swap_gain = 0.0f;        /* fade the new module in from dry */
        inst->fx_swap_phase = 2;
    } else {
        inst->fx_swap_phase = 0;
    }
    s_ready = 0;
    return 1;
}

__attribute__((visibility("default")))
void chain_load_retire(void)
{
    if (s_kind == LOAD_SYNTH) chain_synth_async_retire();
    else fx_retire(&s_fx_retire);
    s_kind = LOAD_NONE;
}
