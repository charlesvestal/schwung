/*
 * Signal Chain -- SCENES for one slot.
 *
 * This slot's share of the set's scene bank (scene_morph.h), projected into
 * the modulation bus as a MORPH contribution per locked pair. The crossfader
 * arrives from the shim every frame (chain_set_scene_morph, dlsym'd); the work
 * runs in lfo_tick -- on idle frames too, through mod:tick -- and only when
 * something changed, plus a slow revalidation so a module that loads, swaps or
 * publishes its chain_params late is picked up without anyone telling us.
 *
 * Everything here runs on the SPI callback: no allocation, no logging, no
 * file I/O. The load scratch is a file-scope static for that reason (a table
 * is ~10 KB), safe because nothing here re-enters.
 *
 * Design: docs/superpowers/specs/2026-09-27-scene-morphing-design.md.
 */

#include "chain_internal.h"

#define SCENE_SOURCE_ID "scene"
/* Blocks between forced revalidations (~93 ms at 128 frames / 44.1 kHz). */
#define SCENE_REVALIDATE_BLOCKS 32

static scene_table_t s_load_scratch;

void chain_scene_init(chain_instance_t *inst) {
    if (!inst) return;
    memset(&inst->scenes, 0, sizeof(inst->scenes));
    inst->scene_a = SCENE_NONE;
    inst->scene_b = SCENE_NONE;
    inst->scene_edit = SCENE_NONE;
    inst->scene_edit_flags = 0;
    inst->scene_flash = SCENE_FLASH_NONE;
    inst->scene_x = 0.0f;
    inst->scene_dirty = 1;
    inst->scene_revalidate = 0;
    inst->scene_rev = 0;
}

/* The module loaded at a component position right now, or NULL. */
static const char *chain_scene_module_at(chain_instance_t *inst, const char *target) {
    if (strcmp(target, "synth") == 0) {
        return (inst->synth_instance && inst->current_synth_module[0]) ? inst->current_synth_module : NULL;
    }
    if (strncmp(target, "midi_fx", 7) == 0) {
        int i = atoi(target + 7) - 1;
        if (i < 0 || i >= MAX_MIDI_FX || i >= inst->midi_fx_count) return NULL;
        if (!inst->midi_fx_instances[i] || !inst->current_midi_fx_modules[i][0]) return NULL;
        return inst->current_midi_fx_modules[i];
    }
    if (strncmp(target, "fx", 2) == 0) {
        int i = atoi(target + 2) - 1;
        if (i < 0 || i >= MAX_AUDIO_FX || i >= inst->fx_count) return NULL;
        if (!inst->fx_instances[i] || !inst->current_fx_modules[i][0]) return NULL;
        return inst->current_fx_modules[i];
    }
    return NULL;
}

/* A pair is LIVE when the module it was locked under is the one loaded. */
static int chain_scene_pair_live(chain_instance_t *inst, const scene_pair_t *p) {
    const char *m = chain_scene_module_at(inst, p->target);
    return m && strcmp(m, p->module) == 0;
}

/* The ends this frame: the armed scene auditions at 100%, else the fader. */
static void chain_scene_ends(const chain_instance_t *inst, int *a, int *b, float *x) {
    if (inst->scene_edit != SCENE_NONE) {
        *a = inst->scene_edit;
        *b = SCENE_NONE;
        *x = 0.0f;
    } else {
        *a = inst->scene_a;
        *b = inst->scene_b;
        *x = inst->scene_x;
    }
}

static void chain_scene_changed(chain_instance_t *inst) {
    inst->scene_rev++;
    inst->scene_dirty = 1;
}

void chain_scene_tick(chain_instance_t *inst) {
    if (!inst) return;
    if (++inst->scene_revalidate >= SCENE_REVALIDATE_BLOCKS) {
        inst->scene_revalidate = 0;
        /* Only worth a pass when there is something to project, or a stale
         * contribution to take down. */
        inst->scene_dirty = 1;
    }
    if (!inst->scene_dirty) return;
    inst->scene_dirty = 0;

    int a, b;
    float x;
    chain_scene_ends(inst, &a, &b, &x);

    /* 1. Take down every scene contribution that no longer contributes: its
     *    pair was unlocked, its end moved away, its module was swapped. A
     *    stale override would pin the parameter where the scene left it. */
    for (int i = 0; i < inst->mod_target_count && i < MAX_MOD_TARGETS; i++) {
        mod_target_state_t *e = &inst->mod_targets[i];
        if (!e->active || !chain_mod_has_source(e, SCENE_SOURCE_ID)) continue;
        int pi = scene_find(&inst->scenes, e->target, e->param);
        int keep = 0;
        if (pi >= 0 && chain_scene_pair_live(inst, &inst->scenes.pairs[pi])) {
            int ha, hb; float va, vb;
            keep = scene_resolve(&inst->scenes.pairs[pi], a, b, &ha, &va, &hb, &vb);
        }
        if (!keep) {
            char target[sizeof(e->target)], param[sizeof(e->param)];
            memcpy(target, e->target, sizeof(target));
            memcpy(param, e->param, sizeof(param));
            chain_mod_clear_source_at(inst, SCENE_SOURCE_ID, target, param);
        }
    }

    /* 2. Project every live, contributing pair. */
    for (int i = 0; i < inst->scenes.count; i++) {
        const scene_pair_t *p = &inst->scenes.pairs[i];
        if (!chain_scene_pair_live(inst, p)) continue;
        int ha, hb; float va, vb;
        if (!scene_resolve(p, a, b, &ha, &va, &hb, &vb)) continue;
        chain_mod_emit_morph(inst, SCENE_SOURCE_ID, p->target, p->param, ha, va, hb, vb, x);
    }

    /* 3. An int/enum write inside chain_mod's MOD_INT_ENUM_MIN_INTERVAL_MS is
     *    DROPPED, not queued -- so an enum that crossed 0.5 just after another
     *    write would sit on the wrong option until the next revalidation.
     *    Stay dirty until what the module holds is what the scene says. */
    for (int i = 0; i < inst->mod_target_count && i < MAX_MOD_TARGETS; i++) {
        const mod_target_state_t *e = &inst->mod_targets[i];
        if (!e->active || !chain_mod_has_source(e, SCENE_SOURCE_ID)) continue;
        if (!e->has_last_applied ||
            (int)e->effective_value != (int)e->last_applied_value ||
            fabsf(e->effective_value - e->last_applied_value) > 1e-6f) {
            inst->scene_dirty = 1;
            break;
        }
    }
}

/*
 * THE CROSSFADER, pushed by the shim once per frame for every slot. dlsym'd,
 * never a field on plugin_api_v2_t (see plugin_api_v1.h). Stores and marks
 * dirty only -- the work happens in lfo_tick. Returns this slot's scene
 * revision in the low 16 bits and a ONE-SHOT refusal code in bits 16-23,
 * which the shim folds into shadow_control_t for the UI.
 */
__attribute__((visibility("default")))
uint32_t chain_set_scene_morph(void *instance, uint8_t a, uint8_t b, float x, uint8_t edit,
                               uint8_t edit_flags) {
    return chain_scene_set_morph((chain_instance_t *)instance, a, b, x, edit, edit_flags);
}

uint32_t chain_scene_set_morph(chain_instance_t *inst, uint8_t a, uint8_t b, float x, uint8_t edit,
                               uint8_t edit_flags) {
    if (!inst) return 0;
    inst->scene_edit_flags = edit_flags;
    if (a != SCENE_NONE && a >= SCENE_COUNT) a = SCENE_NONE;
    if (b != SCENE_NONE && b >= SCENE_COUNT) b = SCENE_NONE;
    if (edit != SCENE_NONE && edit >= SCENE_COUNT) edit = SCENE_NONE;
    x = scene_clamp01(x);
    if (a != inst->scene_a || b != inst->scene_b || edit != inst->scene_edit ||
        fabsf(x - inst->scene_x) > 1e-6f) {
        inst->scene_a = a;
        inst->scene_b = b;
        inst->scene_edit = edit;
        inst->scene_x = x;
        inst->scene_dirty = 1;
    }
    uint32_t status = (uint32_t)inst->scene_rev | ((uint32_t)inst->scene_flash << 16);
    inst->scene_flash = SCENE_FLASH_NONE;
    return status;
}

/* "scenes:<verb>" writes. Returns 1 when the verb was ours. */
int chain_scene_set_param(chain_instance_t *inst, const char *verb, const char *val) {
    if (!inst || !verb) return 0;
    int rc;
    if (strcmp(verb, "lock") == 0) {
        rc = scene_apply_lock_verb(&inst->scenes, val);
        if (rc == SCENE_ERR_FULL) inst->scene_flash = SCENE_FLASH_FULL;
    } else if (strcmp(verb, "unlock") == 0) {
        rc = scene_apply_unlock_verb(&inst->scenes, val);
    } else if (strcmp(verb, "clear") == 0) {
        rc = scene_apply_clear_verb(&inst->scenes, val);
    } else if (strcmp(verb, "copy") == 0) {
        rc = scene_apply_copy_verb(&inst->scenes, val);
    } else if (strcmp(verb, "load") == 0) {
        rc = scene_load(&inst->scenes, &s_load_scratch, val ? val : "");
    } else {
        return 0;
    }
    if (rc == SCENE_OK) chain_scene_changed(inst);
    return 1;
}

/* "scenes:<verb>" reads. Returns bytes written, or -1 when not ours. */
int chain_scene_get_param(chain_instance_t *inst, const char *verb, char *buf, int buf_len) {
    if (!inst || !verb || !buf || buf_len < 2) return -1;
    if (strcmp(verb, "dump") == 0) {
        int n = scene_dump(&inst->scenes, buf, buf_len);
        /* An overflow must not read as an empty bank: "" means served-and-empty. */
        return n < 0 ? -1 : n;
    }
    if (strcmp(verb, "count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->scenes.count);
    }
    if (strcmp(verb, "rev") == 0) {
        return snprintf(buf, buf_len, "%u", (unsigned)inst->scene_rev);
    }
    if (strcmp(verb, "locks") == 0) {
        /* Per-scene lock counts, "n0,n1,...,n15" -- what the Scenes screen draws. */
        int off = 0;
        for (int n = 0; n < SCENE_COUNT; n++) {
            int w = snprintf(buf + off, (size_t)(buf_len - off), n ? ",%d" : "%d",
                             scene_lock_count(&inst->scenes, n));
            if (w < 0 || w >= buf_len - off) return -1;
            off += w;
        }
        return off;
    }
    return -1;
}

/* Split "synth:cutoff" / "fx2:mix" / "midi_fx1:rate" into component + subkey.
 * Returns 0 for anything that is not a component parameter write. */
static int chain_scene_split_key(const char *key, char *target, size_t target_len,
                                 const char **subkey) {
    const char *colon = strchr(key, ':');
    if (!colon) return 0;
    size_t n = (size_t)(colon - key);
    if (n == 0 || n >= target_len) return 0;
    memcpy(target, key, n);
    target[n] = '\0';
    if (strcmp(target, "synth") != 0) {
        const char *digits = NULL;
        if (strncmp(target, "midi_fx", 7) == 0) digits = target + 7;
        else if (strncmp(target, "fx", 2) == 0) digits = target + 2;
        else return 0;
        if (!*digits) return 0;
        for (const char *d = digits; *d; d++) if (*d < '0' || *d > '9') return 0;
    }
    *subkey = colon + 1;
    return 1;
}

static int chain_scene_kind(const chain_param_info_t *p) {
    return p->type == KNOB_TYPE_ENUM ? SCENE_KIND_ENUM
         : p->type == KNOB_TYPE_INT  ? SCENE_KIND_INT : SCENE_KIND_FLOAT;
}

/*
 * THE EDIT ARM. While a scene is armed, a write to a component's own
 * parameter becomes a LOCK in that scene instead of a change to the base.
 * Decided HERE, below every UI, because a module that draws its own screen
 * (9W9's ui_chain.js) brings its own io and never passes through the host's
 * write wrapper -- a UI-side hook would work on the host grid and silently do
 * nothing there.
 *
 * Returns 1 when the write was consumed as a lock. Anything else returns 0
 * and goes to the base as before -- a knob never goes dead -- with a flash
 * when it was a real parameter we could not take (budget full, not declared).
 */
int chain_scene_edit_write(chain_instance_t *inst, const char *key, const char *val) {
    if (!inst || !key || !val || inst->scene_edit == SCENE_NONE) return 0;
    char target[SCENE_TARGET_LEN];
    const char *subkey = NULL;
    if (!chain_scene_split_key(key, target, sizeof(target), &subkey)) return 0;
    if (!scene_edit_subkey_eligible(subkey)) return 0;

    const char *module = chain_scene_module_at(inst, target);
    if (!module) return 0;
    chain_param_info_t *pinfo = find_param_by_key(inst, target, subkey);
    if (!pinfo) {
        inst->scene_flash = SCENE_FLASH_NA;
        return 0;
    }
    float v = dsp_value_to_float(val, pinfo, pinfo->default_val);
    if (v < pinfo->min_val) v = pinfo->min_val;
    if (v > pinfo->max_val) v = pinfo->max_val;
    if (chain_scene_kind(pinfo) != SCENE_KIND_FLOAT) v = roundf(v);

    /* Delete held: the same gesture REMOVES this parameter from the armed
     * scene, and the write does not reach the base either -- the knob was
     * turned to pick the parameter, not to set it. */
    if (inst->scene_edit_flags & SCENE_EDIT_UNLOCK) {
        if (scene_unlock(&inst->scenes, inst->scene_edit, target, subkey) == SCENE_OK) {
            chain_scene_changed(inst);
            chain_scene_tick(inst);
        }
        return 1;
    }
    int rc = scene_lock(&inst->scenes, inst->scene_edit, target, subkey, v, module);
    if (rc == SCENE_ERR_FULL) {
        inst->scene_flash = SCENE_FLASH_FULL;
        return 0;
    }
    if (rc != SCENE_OK) return 0;
    chain_scene_changed(inst);
    /* Heard NOW rather than at the next tick: the knob and the sound move
     * together, which is the whole feel of editing a scene. */
    chain_scene_tick(inst);
    return 1;
}

/*
 * A plain read while armed answers WHAT A WRITE WOULD CHANGE: the lock, for a
 * parameter the armed scene locks. Returns bytes written, or -1 to fall
 * through (not armed, not locked, dormant).
 */
int chain_scene_edit_read(chain_instance_t *inst, const char *target, const char *subkey,
                          char *buf, int buf_len) {
    if (!inst || inst->scene_edit == SCENE_NONE || !target || !subkey || !buf || buf_len < 2) return -1;
    int i = scene_find(&inst->scenes, target, subkey);
    if (i < 0) return -1;
    const scene_pair_t *p = &inst->scenes.pairs[i];
    if (!(p->mask & (1u << inst->scene_edit)) || !chain_scene_pair_live(inst, p)) return -1;
    float v = p->values[inst->scene_edit];
    chain_param_info_t *pinfo = find_param_by_key(inst, target, subkey);
    if (pinfo && chain_scene_kind(pinfo) != SCENE_KIND_FLOAT) {
        return snprintf(buf, buf_len, "%d", (int)lroundf(v));
    }
    return snprintf(buf, buf_len, "%.6f", v);
}
