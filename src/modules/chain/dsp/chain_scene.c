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

/*
 * SLOT SETTINGS a scene can lock, beside the modules' own params. They have
 * no module, so their pairs carry the module id "chain" and are never
 * dormant; their shape comes from this table, not from chain_params.
 *
 *   target "slot"   key "buses:main_send<N>"  -- applied as an OFFSET
 *                   (scene_send_mod), never written into the saved level
 *   target "lfo1"   key "lfo1:<param>"        -- written into the LFO, with
 *   target "lfo2"                                its base kept for reads/saves
 */
#define CHAIN_SETTING_MODULE "chain"
typedef struct { const char *param; int kind; float min, max; } chain_setting_meta_t;
static const chain_setting_meta_t SLOT_SETTINGS[] = {
    { "main_send1", SCENE_KIND_INT, 0, BUS_MIX_SEND_LEVEL_MAX },
    { "main_send2", SCENE_KIND_INT, 0, BUS_MIX_SEND_LEVEL_MAX },
    { NULL, 0, 0, 0 },
};
static const chain_setting_meta_t LFO_SETTINGS[] = {
    { "enabled",      SCENE_KIND_ENUM, 0, 1 },
    { "shape",        SCENE_KIND_ENUM, 0, LFO_NUM_SHAPES - 1 },
    { "rate_hz",      SCENE_KIND_FLOAT, 0.1f, 20.0f },
    { "rate_div",     SCENE_KIND_ENUM, 0, LFO_NUM_DIVISIONS - 1 },
    { "sync",         SCENE_KIND_ENUM, 0, 1 },
    { "depth",        SCENE_KIND_FLOAT, -1.0f, 1.0f },
    { "polarity",     SCENE_KIND_ENUM, 0, 1 },
    { "phase_offset", SCENE_KIND_FLOAT, 0.0f, 1.0f },
    { NULL, 0, 0, 0 },
};

static int setting_lfo_index(const char *target) {
    if (strcmp(target, "lfo1") == 0) return 0;
    if (strcmp(target, "lfo2") == 0) return 1;
    return -1;
}

static const chain_setting_meta_t *chain_setting_meta(const char *target, const char *param) {
    const chain_setting_meta_t *t = strcmp(target, "slot") == 0 ? SLOT_SETTINGS
                                  : setting_lfo_index(target) >= 0 ? LFO_SETTINGS : NULL;
    if (!t || !param) return NULL;
    for (int i = 0; t[i].param; i++) if (strcmp(t[i].param, param) == 0) return &t[i];
    return NULL;
}

static float setting_clamp(const chain_setting_meta_t *m, float v) {
    if (v < m->min) v = m->min;
    if (v > m->max) v = m->max;
    return m->kind == SCENE_KIND_FLOAT ? v : roundf(v);
}

static float lfo_field_get(const lfo_state_t *l, const char *p) {
    if (!strcmp(p, "enabled")) return (float)l->enabled;
    if (!strcmp(p, "shape")) return (float)l->shape;
    if (!strcmp(p, "rate_hz")) return l->rate_hz;
    if (!strcmp(p, "rate_div")) return (float)l->rate_div;
    if (!strcmp(p, "sync")) return (float)l->sync;
    if (!strcmp(p, "depth")) return l->depth;
    if (!strcmp(p, "polarity")) return (float)l->bipolar;
    if (!strcmp(p, "phase_offset")) return l->phase_offset;
    return 0.0f;
}

/* Written directly, NOT through v2_set_param: that path stats a debug flag
 * file every 64th call, and a sweep writes every block. The side effects that
 * matter are mirrored: switching an LFO off takes its contribution down. */
static void lfo_field_set(chain_instance_t *inst, int li, const char *p, float v) {
    lfo_state_t *l = &inst->lfos[li];
    const int iv = (int)lroundf(v);
    if (!strcmp(p, "enabled")) {
        l->enabled = iv ? 1 : 0;
        if (!l->enabled) {
            l->active = 0;
            chain_mod_clear_source(inst, li ? "lfo2" : "lfo1");
        } else {
            l->active = (l->target[0] && l->param[0]);
        }
    }
    else if (!strcmp(p, "shape")) l->shape = iv;
    else if (!strcmp(p, "rate_hz")) l->rate_hz = v;
    else if (!strcmp(p, "rate_div")) l->rate_div = iv;
    else if (!strcmp(p, "sync")) l->sync = iv ? 1 : 0;
    else if (!strcmp(p, "depth")) l->depth = v;
    else if (!strcmp(p, "polarity")) l->bipolar = iv ? 1 : 0;
    else if (!strcmp(p, "phase_offset")) l->phase_offset = v;
}

static int lfo_drive_find(chain_instance_t *inst, int li, const char *p) {
    for (int i = 0; i < 16; i++)
        if (inst->scene_lfo_drive[i].active && inst->scene_lfo_drive[i].lfo == li &&
            strcmp(inst->scene_lfo_drive[i].param, p) == 0) return i;
    return -1;
}

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
    memset(inst->scene_send_mod, 0, sizeof(inst->scene_send_mod));
    memset(inst->scene_lfo_drive, 0, sizeof(inst->scene_lfo_drive));
    memset(inst->scene_send_takeover, 0, sizeof(inst->scene_send_takeover));
}

/* The module loaded at a component position right now, or NULL. */
static const char *chain_scene_module_at(chain_instance_t *inst, const char *target) {
    if (strcmp(target, "slot") == 0 || setting_lfo_index(target) >= 0) return CHAIN_SETTING_MODULE;
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

/* The two slot sends: an OFFSET against the LIVE level, every block, so a
 * send knob turned mid-morph moves the unlocked end immediately. */
static void chain_scene_sends(chain_instance_t *inst) {
    for (int sd = 0; sd < BUS_MIX_SENDS; sd++) inst->scene_send_mod[sd] = 0;
    if (inst->scenes.count == 0) return;
    int a, b;
    float x;
    chain_scene_ends(inst, &a, &b, &x);
    for (int i = 0; i < inst->scenes.count; i++) {
        const scene_pair_t *p = &inst->scenes.pairs[i];
        if (strcmp(p->target, "slot") != 0) continue;
        int sd = !strcmp(p->param, "main_send1") ? 0 : !strcmp(p->param, "main_send2") ? 1 : -1;
        if (sd < 0 || sd >= BUS_MIX_SENDS) continue;
        int ha, hb; float va, vb;
        if (!scene_resolve(p, a, b, &ha, &va, &hb, &vb)) continue;
        const float base = (float)inst->main_send_level[sd];
        scene_takeover_t *to = &inst->scene_send_takeover[sd];
        if (scene_takeover_expired(to, x)) to->on = 0;
        const float v = to->on
            ? scene_takeover_value(to, ha ? va : base, hb ? vb : base, x, SCENE_KIND_INT)
            : scene_morph_value(ha, va, hb, vb, base, x, SCENE_KIND_INT);
        inst->scene_send_mod[sd] += (int)lroundf(v - base);
    }
}

/* The LFO fields: engaged with their base captured, written when the morph
 * changes them, handed back to the base when nothing drives them. */
static void chain_scene_lfos(chain_instance_t *inst, int a, int b, float x) {
    /* release what no longer contributes */
    for (int d = 0; d < 16; d++) {
        typeof(inst->scene_lfo_drive[0]) *dr = &inst->scene_lfo_drive[d];
        if (!dr->active) continue;
        char target[8];
        snprintf(target, sizeof(target), "lfo%d", dr->lfo + 1);
        int pi = scene_find(&inst->scenes, target, dr->param);
        int ha, hb; float va, vb;
        if (pi >= 0 && scene_resolve(&inst->scenes.pairs[pi], a, b, &ha, &va, &hb, &vb)) continue;
        lfo_field_set(inst, dr->lfo, dr->param, dr->base);
        memset(dr, 0, sizeof(*dr));
    }
    /* drive what contributes */
    for (int i = 0; i < inst->scenes.count; i++) {
        const scene_pair_t *p = &inst->scenes.pairs[i];
        const int li = setting_lfo_index(p->target);
        if (li < 0) continue;
        const chain_setting_meta_t *m = chain_setting_meta(p->target, p->param);
        if (!m) continue;
        int ha, hb; float va, vb;
        if (!scene_resolve(p, a, b, &ha, &va, &hb, &vb)) continue;
        int d = lfo_drive_find(inst, li, p->param);
        if (d < 0) {
            for (d = 0; d < 16 && inst->scene_lfo_drive[d].active; d++) {}
            if (d >= 16) continue;
            typeof(inst->scene_lfo_drive[0]) *dr = &inst->scene_lfo_drive[d];
            memset(dr, 0, sizeof(*dr));
            dr->active = 1;
            dr->lfo = li;
            snprintf(dr->param, sizeof(dr->param), "%s", p->param);
            dr->base = lfo_field_get(&inst->lfos[li], p->param);
        }
        typeof(inst->scene_lfo_drive[0]) *dr = &inst->scene_lfo_drive[d];
        if (scene_takeover_expired(&dr->takeover, x)) dr->takeover.on = 0;
        const float v = setting_clamp(m, dr->takeover.on
            ? scene_takeover_value(&dr->takeover, ha ? va : dr->base, hb ? vb : dr->base, x, m->kind)
            : scene_morph_value(ha, va, hb, vb, dr->base, x, m->kind));
        if (dr->has_last && fabsf(v - dr->last) < 1e-6f) continue;
        lfo_field_set(inst, li, p->param, v);
        dr->last = v;
        dr->has_last = 1;
    }
}

void chain_scene_tick(chain_instance_t *inst) {
    if (!inst) return;
    chain_scene_sends(inst);
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

    /* 2. Project every live, contributing pair. The slot settings are not
     *    modulation targets: sends ride their own offset (above), LFO fields
     *    are driven below. */
    chain_scene_lfos(inst, a, b, x);
    for (int i = 0; i < inst->scenes.count; i++) {
        const scene_pair_t *p = &inst->scenes.pairs[i];
        if (!strcmp(p->module, CHAIN_SETTING_MODULE)) continue;
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
    if (a != inst->scene_a || b != inst->scene_b || edit != inst->scene_edit) {
        /* Another scene, or an edit: every live anchor goes. They were a
         * position between THESE two ends, and mean nothing between others. */
        chain_mod_clear_takeovers(inst, SCENE_SOURCE_ID);
        for (int d = 0; d < 16; d++) inst->scene_lfo_drive[d].takeover.on = 0;
        for (int sd = 0; sd < BUS_MIX_SENDS; sd++) inst->scene_send_takeover[sd].on = 0;
    }
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
    /* Slot settings: "buses:main_send<N>" is target "slot"; "lfo<N>:<p>" is
     * its own target. Only the params chain_setting_meta knows count. */
    if (strcmp(target, "buses") == 0) {
        snprintf(target, target_len, "slot");
        *subkey = colon + 1;
        return chain_setting_meta("slot", *subkey) != NULL;
    }
    if (setting_lfo_index(target) >= 0) {
        *subkey = colon + 1;
        return chain_setting_meta(target, *subkey) != NULL;
    }
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
    float v;
    const chain_setting_meta_t *sm = chain_setting_meta(target, subkey);
    if (sm) {
        char *end = NULL;
        v = strtof(val, &end);
        if (!end || end == val) return 0;
        v = setting_clamp(sm, v);
    } else {
        chain_param_info_t *pinfo = find_param_by_key(inst, target, subkey);
        if (!pinfo) {
            inst->scene_flash = SCENE_FLASH_NA;
            return 0;
        }
        v = dsp_value_to_float(val, pinfo, pinfo->default_val);
        if (v < pinfo->min_val) v = pinfo->min_val;
        if (v > pinfo->max_val) v = pinfo->max_val;
        if (chain_scene_kind(pinfo) != SCENE_KIND_FLOAT) v = roundf(v);
    }

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
 * A STATE READ SAVES THE KNOB, NOT THE MORPH.
 *
 * Every save path -- the slot autosave, User Presets, the snapshot -- reads a
 * component's opaque `<comp>:state`, and a module writes into it whatever it
 * holds right now. Under a scene that is the MORPHED value, so saving with the
 * fader anywhere but "nothing locked" replaced the knob with the scene: after
 * a reboot the unlocked end of every morph was wherever the fader had been.
 *
 * So around that one read, the base goes back into the module and the morph is
 * re-applied straight after. Same call, same thread (the SPI callback), no
 * audio block in between -- nothing is heard. Scene-driven params only: an
 * LFO's save behaviour is not this feature's to change.
 */
/* A read of a slot setting answers WHAT A WRITE WOULD CHANGE: the lock
 * while armed, else the base of a driven LFO field (the struct holds the
 * morph). Sends need nothing -- their level is never written. -1 = not ours. */
static int chain_scene_setting_read(chain_instance_t *inst, const char *key, char *buf, int buf_len) {
    char target[SCENE_TARGET_LEN];
    const char *subkey = NULL;
    const char *colon = key ? strchr(key, ':') : NULL;
    if (!colon || (strncmp(key, "buses:", 6) && strncmp(key, "lfo1:", 5) && strncmp(key, "lfo2:", 5))) return -1;
    if (!chain_scene_split_key(key, target, sizeof(target), &subkey)) return -1;
    const chain_setting_meta_t *m = chain_setting_meta(target, subkey);
    if (!m) return -1;
    float v;
    int have = 0;
    if (inst->scene_edit != SCENE_NONE) {
        int i = scene_find(&inst->scenes, target, subkey);
        if (i >= 0 && (inst->scenes.pairs[i].mask & (1u << inst->scene_edit))) {
            v = inst->scenes.pairs[i].values[inst->scene_edit];
            have = 1;
        }
    }
    const int li = setting_lfo_index(target);
    if (!have && li >= 0) {
        int d = lfo_drive_find(inst, li, subkey);
        if (d >= 0) { v = inst->scene_lfo_drive[d].base; have = 1; }
    }
    if (!have) return -1;
    return m->kind == SCENE_KIND_FLOAT ? snprintf(buf, buf_len, "%.6f", v)
                                       : snprintf(buf, buf_len, "%d", (int)lroundf(v));
}

int chain_scene_get_around_state(chain_instance_t *inst, const char *key, char *buf, int buf_len,
                                 chain_get_param_fn impl) {
    if (inst && inst->scenes.count) {
        int r = chain_scene_setting_read(inst, key, buf, buf_len);
        if (r >= 0) return r;
    }
    /* The LFO config a patch saves: the driven fields go back to their base
     * for the read, exactly as a module's state does below. Plain struct
     * writes, so nothing is heard. */
    if (inst && key && strcmp(key, "lfo_config") == 0) {
        int any = 0;
        for (int d = 0; d < 16; d++) {
            if (!inst->scene_lfo_drive[d].active) continue;
            lfo_field_set(inst, inst->scene_lfo_drive[d].lfo, inst->scene_lfo_drive[d].param,
                          inst->scene_lfo_drive[d].base);
            any = 1;
        }
        int r = impl(inst, key, buf, buf_len);
        if (any) {
            for (int d = 0; d < 16; d++) inst->scene_lfo_drive[d].has_last = 0;
            inst->scene_dirty = 1;
        }
        return r;
    }
    size_t n = key ? strlen(key) : 0;
    if (!inst || n <= 6 || strcmp(key + n - 6, ":state") != 0 || inst->scenes.count == 0)
        return impl(inst, key, buf, buf_len);
    char target[SCENE_TARGET_LEN];
    const char *subkey = NULL;
    if (!chain_scene_split_key(key, target, sizeof(target), &subkey) || strcmp(subkey, "state") != 0)
        return impl(inst, key, buf, buf_len);
    int swapped = 0;
    for (int i = 0; i < inst->mod_target_count && i < MAX_MOD_TARGETS; i++) {
        mod_target_state_t *e = &inst->mod_targets[i];
        if (!e->active || strcmp(e->target, target) != 0 || !chain_mod_has_source(e, SCENE_SOURCE_ID)) continue;
        chain_mod_write_base(inst, e);
        swapped++;
    }
    int r = impl(inst, key, buf, buf_len);
    if (swapped) {
        for (int i = 0; i < inst->mod_target_count && i < MAX_MOD_TARGETS; i++) {
            mod_target_state_t *e = &inst->mod_targets[i];
            if (!e->active || strcmp(e->target, target) != 0 || !chain_mod_has_source(e, SCENE_SOURCE_ID)) continue;
            chain_mod_apply_effective_value(inst, e, 1);
        }
    }
    return r;
}

/* The set_param route, so chain_host.c carries one line of it: the table
 * verbs, then the edit arm. Returns 1 when the write was consumed. */
int chain_scene_route_set(chain_instance_t *inst, const char *key, const char *val) {
    if (!inst || !key) return 0;
    if (strncmp(key, "scenes:", 7) == 0) {
        chain_scene_set_param(inst, key + 7, val);
        return 1;
    }
    if (inst->scene_edit != SCENE_NONE && chain_scene_edit_write(inst, key, val)) {
        inst->dirty = 1;
        return 1;
    }
    if (!val || inst->scenes.count == 0) return 0;
    /*
     * A KNOB TURN ON A PARAMETER THE SCENE IS DRIVING is heard: the LIVE
     * TAKEOVER (scene_morph.h) anchors it at the fader, and the fader then
     * morphs from there toward whichever end it heads for. Measured BEFORE
     * the write lands, so the old base is still here to measure against.
     */
    /* An LFO field: that is also its new BASE. The write itself still goes
     * through (the LFO takes it now); the next tick puts the anchor back. */
    if ((key[0] == 'l') && (!strncmp(key, "lfo1:", 5) || !strncmp(key, "lfo2:", 5))) {
        const int li = key[3] - '1';
        int d = lfo_drive_find(inst, li, key + 5);
        const chain_setting_meta_t *m = chain_setting_meta(li ? "lfo2" : "lfo1", key + 5);
        if (d >= 0 && m) {
            typeof(inst->scene_lfo_drive[0]) *dr = &inst->scene_lfo_drive[d];
            const float nb = setting_clamp(m, strtof(val, NULL));
            const float heard = dr->has_last ? dr->last : dr->base;
            dr->takeover.k = scene_takeover_k(heard, dr->base, nb, m->kind, m->min, m->max);
            dr->takeover.x0 = inst->scene_x;
            dr->takeover.on = 1;
            dr->base = nb;
            dr->has_last = 0;
            inst->scene_dirty = 1;
        }
        return 0;
    }
    /* A slot send: an offset beside the level, so the anchor is all there is. */
    if (!strncmp(key, "buses:main_send", 15)) {
        const int sd = !strcmp(key + 15, "1") ? 0 : !strcmp(key + 15, "2") ? 1 : -1;
        if (sd >= 0 && sd < BUS_MIX_SENDS && scene_find(&inst->scenes, "slot", key + 6) >= 0) {
            const float old = (float)inst->main_send_level[sd];
            const float heard = old + (float)inst->scene_send_mod[sd];
            float nb = strtof(val, NULL);
            if (nb < 0) nb = 0;
            if (nb > BUS_MIX_SEND_LEVEL_MAX) nb = BUS_MIX_SEND_LEVEL_MAX;
            scene_takeover_t *to = &inst->scene_send_takeover[sd];
            to->k = scene_takeover_k(heard, old, nb, SCENE_KIND_INT, 0, BUS_MIX_SEND_LEVEL_MAX);
            to->x0 = inst->scene_x;
            to->on = 1;
        }
        return 0;
    }
    /* A module's own parameter: the anchor sits on the scene's morph. */
    {
        char target[SCENE_TARGET_LEN];
        const char *subkey = NULL;
        if (chain_scene_split_key(key, target, sizeof(target), &subkey) &&
            !chain_setting_meta(target, subkey)) {
            chain_param_info_t *pinfo = find_param_by_key(inst, target, subkey);
            if (pinfo)
                chain_mod_scene_takeover(inst, SCENE_SOURCE_ID, target, subkey,
                                         dsp_value_to_float(val, pinfo, pinfo->default_val));
        }
    }
    return 0;
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
