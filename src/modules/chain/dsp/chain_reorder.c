/*
 * chain_reorder.c — insert, remove and reorder a chain position by PERMUTING
 * the instance's per-position arrays, rather than reloading modules.
 *
 * See chain_permute.h for why (short version: a `<id>:module` write unloads and
 * dlopen()s, so renumbering by rewriting ids destroyed every module downstream
 * of the edit — a running arp lost its phase, a reverb lost its tail). This
 * file is the part that knows what a chain position IS.
 *
 * THREAD SAFETY. Everything here runs on the SPI audio thread, the same one
 * that calls render_block and on_midi: the shim services parameter requests
 * from shim_pre_transfer (shadow_inprocess_handle_param_request), in the same
 * callback and after shadow_mix_audio. There is no other thread that touches a
 * chain instance. So a permutation cannot interleave with a render — it is
 * atomic from the audio path's point of view for free, with no lock, no shadow
 * copy and no pending-request queue. (The same property is what lets the
 * existing module-load path dlopen() from this thread. That IS a real-time
 * violation — dlopen blocks, allocates and reads files under SCHED_FIFO 90 —
 * but it is pre-existing, and a permutation is strictly cheaper than the reload
 * it replaces.)
 */

#include "chain_internal.h"
#include "host/chain_permute.h"

/*
 * EVERY per-position field of a chain section, as data.
 *
 * This list is the whole correctness argument. A field left out of it keeps the
 * value belonging to whatever module USED to be at that index — a bypass flag
 * that follows the position instead of the module, or worse, param metadata
 * that makes one module's knob write another module's parameter. So it is
 * enumerated once, here, and tests/host/test_chain_permute.sh pins it against
 * the struct definition in chain_internal.h: a new `[MAX_AUDIO_FX]` or
 * `[MAX_MIDI_FX]` member that is not listed below fails the build's test suite
 * rather than misbehaving on hardware.
 *
 * `patches[]` and `patch_info_t.audio_fx[]` are deliberately NOT here: they are
 * the saved library, not the live chain, and they are written from the live
 * arrays at save time.
 */
/*
 * PERM_FIELD is a VALUE array: the position owns its bytes, and vacating one
 * zeroes them.
 *
 * PERM_OWNED is a pointer to a block allocated once per position by
 * chain_alloc_position_storage and NEVER NULL. Those must be rotated, not
 * zeroed — see chain_permute.h. The classification is load-bearing rather than
 * cosmetic (registering an owned array as a value one is a null dereference
 * inside the next module load, which is how it reached hardware), so
 * tests/host/test_chain_permute.sh cross-checks it against
 * chain_alloc_position_storage rather than trusting what is written here.
 */
#define PERM_FIELD(arr) { (void *)(arr), sizeof((arr)[0]), 0 }
#define PERM_OWNED(arr, bytes) { (void *)(arr), sizeof((arr)[0]), (bytes) }
#define PERM_PARAMS_BYTES (MAX_CHAIN_PARAMS * sizeof(chain_param_info_t))

static int chain_perm_collect_fx(chain_instance_t *inst, chain_perm_array_t *out) {
    int n = 0;
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->fx_handles);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->fx_plugins_v2);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->fx_instances);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->fx_is_v2);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->current_fx_modules);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->fx_on_midi);
    out[n++] = (chain_perm_array_t)PERM_OWNED(inst->fx_params, PERM_PARAMS_BYTES);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->fx_param_counts);
    out[n++] = (chain_perm_array_t)PERM_OWNED(inst->fx_ui_hierarchy, CHAIN_UI_HIERARCHY_LEN);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->mod_param_refresh_ms_fx);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->fx_smoothers);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->fx_bypassed);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->fx_requires_continuous);
    return n;
}

static int chain_perm_collect_midi_fx(chain_instance_t *inst, chain_perm_array_t *out) {
    int n = 0;
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->midi_fx_handles);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->midi_fx_plugins);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->midi_fx_instances);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->current_midi_fx_modules);
    out[n++] = (chain_perm_array_t)PERM_OWNED(inst->midi_fx_params, PERM_PARAMS_BYTES);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->midi_fx_param_counts);
    out[n++] = (chain_perm_array_t)PERM_OWNED(inst->midi_fx_ui_hierarchy, CHAIN_UI_HIERARCHY_LEN);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->mod_param_refresh_ms_midi_fx);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->midi_fx_pre_capable);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->midi_fx_wants_sysex);
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->midi_fx_bypassed);
    return n;
}

/*
 * Re-aim everything that names a position BY STRING.
 *
 * Three tables do, and all three had to be found by reading rather than by
 * grepping for one spelling: the runtime modulation targets (whose entries also
 * carry the modulation BASE — which is exactly why it no longer needs carrying
 * anywhere, the entry simply stays valid), the two per-slot LFOs, and the knob
 * mappings. A routing whose module LEFT is cleared in both halves; leaving
 * `param` naming a departed module's parameter is how a later `target` write
 * silently revives half a routing.
 */
static void chain_perm_retarget_all(chain_instance_t *inst, const char *prefix,
                                    int max, const int *map, int count) {
    for (int i = 0; i < inst->mod_target_count && i < MAX_MOD_TARGETS; i++) {
        mod_target_state_t *e = &inst->mod_targets[i];
        if (!e->active) continue;
        if (chain_perm_retarget(e->target, sizeof(e->target), prefix, max, map, count) < 0) {
            e->active = 0;
            e->param[0] = '\0';
        }
    }
    for (int i = 0; i < LFO_COUNT; i++) {
        lfo_state_t *l = &inst->lfos[i];
        if (chain_perm_retarget(l->target, sizeof(l->target), prefix, max, map, count) < 0) {
            l->param[0] = '\0';
            l->active = 0;
        }
    }
    for (int i = 0; i < inst->knob_mapping_count && i < MAX_KNOB_MAPPINGS; i++) {
        knob_mapping_t *k = &inst->knob_mappings[i];
        if (chain_perm_retarget(k->target, sizeof(k->target), prefix, max, map, count) < 0) {
            k->param[0] = '\0';
        }
    }
}

/* Which section a request names, resolved once so the three verbs below cannot
 * disagree about a cap or a prefix. */
typedef struct {
    chain_perm_array_t arrays[24];
    int n;
    int *count;
    int cap;
    const char *prefix;
} chain_section_t;

static int chain_section_resolve(chain_instance_t *inst, int is_midi,
                                 chain_section_t *s) {
    if (!inst || !s) return 0;
    if (is_midi) {
        s->n = chain_perm_collect_midi_fx(inst, s->arrays);
        s->count = &inst->midi_fx_count;
        s->cap = MAX_MIDI_FX;
        s->prefix = "midi_fx";
    } else {
        s->n = chain_perm_collect_fx(inst, s->arrays);
        s->count = &inst->fx_count;
        s->cap = MAX_AUDIO_FX;
        s->prefix = "fx";
    }
    return 1;
}

/*
 * Open an empty position at `at` (0-based), shifting the rest along.
 *
 * Nothing is loaded: the caller follows with the ordinary `<id>:module` write,
 * and for the one audio frame in between the chain simply has a hole in it,
 * which both walks skip. Returns 1 on success.
 */
int chain_reorder_insert(chain_instance_t *inst, int is_midi, int at) {
    chain_section_t s;
    if (!chain_section_resolve(inst, is_midi, &s)) return 0;
    int map[CHAIN_PERM_MAX_POS];
    int now = chain_perm_insert(s.arrays, s.n, *s.count, s.cap, at, map);
    if (now < 0) return 0;
    chain_perm_retarget_all(inst, s.prefix, s.cap, map, *s.count);
    *s.count = now;
    inst->dirty = 1;
    return 1;
}

/*
 * Unload position `at` and close the gap.
 *
 * The unload is real — the module is leaving, so its instance must be destroyed
 * and its modulation entries dropped, exactly as before. What is NEW is that
 * everything BEHIND it is only renumbered, not rebuilt.
 */
int chain_reorder_remove(chain_instance_t *inst, int is_midi, int at) {
    chain_section_t s;
    if (!chain_section_resolve(inst, is_midi, &s)) return 0;
    if (at < 0 || at >= *s.count) return 0;

    /* Destroy the occupant first, through the section's own unloader, so the
     * dlclose and the modulation-entry clear stay in one place. */
    if (is_midi) v2_unload_midi_fx_slot(inst, at);
    else         v2_unload_audio_fx_slot(inst, at);

    int map[CHAIN_PERM_MAX_POS];
    int before = *s.count;
    int now = chain_perm_remove(s.arrays, s.n, before, at, map);
    if (now < 0) return 0;
    chain_perm_retarget_all(inst, s.prefix, s.cap, map, before);
    *s.count = now;
    inst->dirty = 1;
    return 1;
}

/* Move position `from` to position `to`, both 0-based. */
int chain_reorder_move(chain_instance_t *inst, int is_midi, int from, int to) {
    chain_section_t s;
    if (!chain_section_resolve(inst, is_midi, &s)) return 0;
    int map[CHAIN_PERM_MAX_POS];
    int now = chain_perm_move(s.arrays, s.n, *s.count, from, to, map);
    if (now < 0) return 0;
    chain_perm_retarget_all(inst, s.prefix, s.cap, map, *s.count);
    inst->dirty = 1;
    return 1;
}
