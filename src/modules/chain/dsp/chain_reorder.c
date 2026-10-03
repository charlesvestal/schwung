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
 * callback and after shadow_mix_audio. So a permutation cannot interleave with
 * a render — it is atomic from the audio path's point of view for free, with
 * no lock, no shadow copy and no pending-request queue.
 *
 * The one other thread that touches a chain instance is the shim's slot loader
 * (slot_load_job.h), and it cannot meet a permutation: it only ever holds an
 * instance the callback has PARKED (taken out of shadow_chain_slots[]), and the
 * load holds the param channel, so no verb reaches this file until the
 * instance is handed back.
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
_Static_assert(sizeof(chain_child_keys_t) <= CHAIN_PERM_MAX_ELEM,
               "chain_child_keys_t permutes by value: over CHAIN_PERM_MAX_ELEM every fx:move is refused");
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
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->fx_child_keys);
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
    out[n++] = (chain_perm_array_t)PERM_FIELD(inst->midi_fx_child_keys);
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
/* Follow one table of lanes through the permutation. A lane whose module LEFT
 * keeps its target string (see below) and is marked orphaned AND module_gone:
 * `module_gone` is the sticky half, because lane_tick un-orphans on a clip
 * fingerprint match and a fingerprint says nothing about which MODULE a lane
 * belongs to -- orphaned alone lasted exactly one block, after which the lane
 * drove whatever slid into the position. `is_live` is 0 for the copies (undo,
 * edit base, stash), which hold no override of their own. */
static void chain_perm_retarget_lanes(lane_t *lanes, int n, const char *prefix,
                                      int max, const int *map, int count,
                                      int is_live) {
    for (int i = 0; i < n; i++) {
        lane_t *ln = &lanes[i];
        if (!ln->used) continue;
        char keep[sizeof(ln->target)];
        snprintf(keep, sizeof(keep), "%s", ln->target);
        if (chain_perm_retarget(ln->target, sizeof(ln->target),
                                prefix, max, map, count) < 0) {
            snprintf(ln->target, sizeof(ln->target), "%s", keep);
            ln->orphaned = 1;
            ln->module_gone = 1;
            /* Its override went with the module's mod entries (the unloader
             * clears them), so there is nothing left to release. */
            if (is_live) ln->driving = 0;
        }
    }
}

/* The journals hold lane NAMES too (lane_span_rec_t). A record whose module
 * left voids its whole entry: applying it would re-create automation aimed at
 * whatever now sits at that position, and a half-applied entry is the desync
 * lane_paste_span refuses everywhere else. The host treats a voided id as
 * "too far back" (the id check in the journal verb). */
static void chain_perm_retarget_journal(lane_journal_entry_t *ring, int depth,
                                        const char *prefix, int max,
                                        const int *map, int count) {
    for (int k = 0; k < depth; k++) {
        lane_journal_entry_t *je = &ring[k];
        if (!je->id) continue;
        for (int r = 0; r < je->nrec && r < LANE_JOURNAL_LANES; r++) {
            if (chain_perm_retarget(je->rec[r].target, sizeof(je->rec[r].target),
                                    prefix, max, map, count) < 0) {
                je->id = 0;
                break;
            }
        }
    }
}

/* A lane's mod-bus source id is "lane:<target>:<param>" (chain_lanes.c), so a
 * renamed ENTRY must rename the lane source inside it too. Left under the old
 * name, the next lane_tick emitted the new id as a SECOND override on the same
 * entry, and the release only ever removed the new one: the old override kept
 * the parameter frozen and the knob dead. Every lane source on an entry is for
 * that entry's own target:param, so the new id is derived from the entry. */
static void chain_perm_rename_lane_sources(mod_target_state_t *e) {
    char sid[MOD_SOURCE_ID_LEN];
    int n = snprintf(sid, sizeof(sid), "lane:%s:%s", e->target, e->param);
    if (n <= 0 || n >= (int)sizeof(sid)) return;
    for (int s = 0; s < MAX_MOD_SOURCES_PER_TARGET; s++) {
        mod_source_contribution_t *src = &e->sources[s];
        if (!src->active || strncmp(src->source_id, "lane:", 5) != 0) continue;
        memcpy(src->source_id, sid, (size_t)n + 1);
    }
}

static void chain_perm_retarget_all(chain_instance_t *inst, const char *prefix,
                                    int max, const int *map, int count) {
    for (int i = 0; i < inst->mod_target_count && i < MAX_MOD_TARGETS; i++) {
        mod_target_state_t *e = &inst->mod_targets[i];
        if (!e->active) continue;
        const int r = chain_perm_retarget(e->target, sizeof(e->target), prefix, max, map, count);
        if (r < 0) {
            e->active = 0;
            e->param[0] = '\0';
        } else if (r > 0) {
            chain_perm_rename_lane_sources(e);
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
    /* SCENE LOCKS follow their module too (a fourth table naming a position
     * by string). A lock on a position that LEFT is dropped with it: the
     * position is gone, and a dormant lock keyed to a renumbered index would
     * wake on whatever module next lands there. */
    int scenes_moved = 0;
    for (int i = 0; i < inst->scenes.count; i++) {
        scene_pair_t *p = &inst->scenes.pairs[i];
        char before[SCENE_TARGET_LEN];
        memcpy(before, p->target, sizeof(before));
        if (chain_perm_retarget(p->target, sizeof(p->target), prefix, max, map, count) < 0) {
            p->mask = 0;
            scenes_moved = 1;
        } else if (strcmp(before, p->target) != 0) {
            scenes_moved = 1;
        }
    }
    if (scenes_moved) {
        scene_compact(&inst->scenes);
        inst->scene_rev++;
        inst->scene_dirty = 1;
    }

    /* AND THE AUTOMATION LANES, which were the FOURTH table and were missed.
     *
     * A lane names its position by the same string ("fx3"), so this is exactly
     * the failure chain_perm_retarget's own comment describes: a permutation
     * that moves the arrays and not the routings "would silently re-aim every
     * routing at whatever slid into the position it named". Insert a module at
     * fx1 and a lane recorded against fx3's `mix` drove the module now sitting
     * at fx3 — and `mix`, `level` and `feedback` are ubiquitous, so it usually
     * FOUND a parameter to drive. The clip fingerprint cannot catch it: it
     * checks which CLIP the lane belongs to, never which module.
     *
     * A lane whose module LEFT is marked `orphaned` rather than emptied.
     * chain_perm_retarget clears the id on -1, which is right for a routing —
     * there is nowhere to point — but a lane holds the user's recorded
     * automation, and an empty target is one the lock map cannot show and the
     * clear verbs cannot name. `orphaned` already means exactly what is wanted
     * here: retained, SILENT (lane_eval refuses it), and restarted rather than
     * resurrected by the next write, because a different module at that
     * position is a different thing. So the target is put back and the flag
     * set instead. */
    chain_perm_retarget_lanes(inst->lanes.lanes, LANE_MAX, prefix, max, map, count, 1);

    /* AND EVERY COPY OF THE STORE, which name positions the same way. Left
     * behind, Slot Settings' Undo (a store swap), Move's Undo of a clip
     * delete (unstash) and the unified-Undo journal all brought lanes back
     * aimed at whichever module now occupies the old position. */
    chain_perm_retarget_lanes(inst->lanes_undo.lanes, LANE_MAX, prefix, max, map, count, 0);
    chain_perm_retarget_lanes(inst->lanes_edit_base.lanes, LANE_MAX, prefix, max, map, count, 0);
    for (int k = 0; k < LANE_STASH_DEPTH; k++) {
        lane_stash_t *sh = &inst->lanes_stash[k];
        if (!sh->id) continue;
        chain_perm_retarget_lanes(sh->lanes, sh->n < LANE_MAX ? sh->n : LANE_MAX,
                                  prefix, max, map, count, 0);
    }
    chain_perm_retarget_journal(inst->lanes_journal, LANE_JOURNAL_DEPTH, prefix, max, map, count);
    chain_perm_retarget_journal(inst->lanes_sjournal, LANE_SJOURNAL_DEPTH, prefix, max, map, count);
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
    /* A swap's fade-in names a POSITION (chain_fx_load.c); after a permute
     * that position holds something else. Land it at full instead. */
    if (!is_midi) inst->fx_swap_phase = 0;
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
    /* A swap's fade-in names a POSITION (chain_fx_load.c); after a permute
     * that position holds something else. Land it at full instead. */
    if (!is_midi) inst->fx_swap_phase = 0;
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
    /* A swap's fade-in names a POSITION (chain_fx_load.c); after a permute
     * that position holds something else. Land it at full instead. */
    if (!is_midi) inst->fx_swap_phase = 0;
    int map[CHAIN_PERM_MAX_POS];
    int now = chain_perm_move(s.arrays, s.n, *s.count, from, to, map);
    if (now < 0) return 0;
    chain_perm_retarget_all(inst, s.prefix, s.cap, map, *s.count);
    inst->dirty = 1;
    return 1;
}
