/*
 * Signal Chain — runtime modulation bus (non-destructive overlay).
 * Split from chain_host.c (2026-06 cleanup step 10); pure relocation,
 * no behavior change. Shared types/decls live in chain_internal.h.
 */

#include "chain_internal.h"

static float chain_mod_clampf(float value, float min_val, float max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

static int chain_mod_get_param_string(chain_instance_t *inst,
                                      const char *target,
                                      const char *param,
                                      char *buf,
                                      int buf_len) {
    if (!inst || !target || !param || !buf || buf_len < 2) return -1;

    if (strcmp(target, "synth") == 0) {
        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->get_param) {
            return inst->synth_plugin_v2->get_param(inst->synth_instance, param, buf, buf_len);
        }
        return -1;
    }

    if (strncmp(target, "fx", 2) == 0) {
        int fx_slot = atoi(target + 2) - 1;
        if (fx_slot < 0 || fx_slot >= MAX_AUDIO_FX || fx_slot >= inst->fx_count) return -1;

        if (inst->fx_is_v2[fx_slot] && inst->fx_plugins_v2[fx_slot] && inst->fx_instances[fx_slot]) {
            return inst->fx_plugins_v2[fx_slot]->get_param(inst->fx_instances[fx_slot], param, buf, buf_len);
        }
        return -1;
    }

    if (strncmp(target, "midi_fx", 7) == 0) {
        int midi_fx_slot = 0;
        if (target[7] != '\0') {
            midi_fx_slot = atoi(target + 7) - 1;
        }
        if (midi_fx_slot < 0 || midi_fx_slot >= MAX_MIDI_FX || midi_fx_slot >= inst->midi_fx_count) return -1;
        if (inst->midi_fx_plugins[midi_fx_slot] && inst->midi_fx_instances[midi_fx_slot]) {
            return inst->midi_fx_plugins[midi_fx_slot]->get_param(inst->midi_fx_instances[midi_fx_slot],
                                                                  param,
                                                                  buf,
                                                                  buf_len);
        }
        return -1;
    }

    return -1;
}

static int chain_mod_set_param_string(chain_instance_t *inst,
                                      const char *target,
                                      const char *param,
                                      const char *val) {
    if (!inst || !target || !param || !val) return -1;

    if (strcmp(target, "synth") == 0) {
        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->set_param) {
            inst->synth_plugin_v2->set_param(inst->synth_instance, param, val);
            return 0;
        }
        return -1;
    }

    if (strncmp(target, "fx", 2) == 0) {
        int fx_slot = atoi(target + 2) - 1;
        if (fx_slot < 0 || fx_slot >= MAX_AUDIO_FX || fx_slot >= inst->fx_count) return -1;

        if (inst->fx_is_v2[fx_slot] && inst->fx_plugins_v2[fx_slot] && inst->fx_instances[fx_slot]) {
            inst->fx_plugins_v2[fx_slot]->set_param(inst->fx_instances[fx_slot], param, val);
            return 0;
        }
        return -1;
    }

    if (strncmp(target, "midi_fx", 7) == 0) {
        int midi_fx_slot = 0;
        if (target[7] != '\0') {
            midi_fx_slot = atoi(target + 7) - 1;
        }
        if (midi_fx_slot < 0 || midi_fx_slot >= MAX_MIDI_FX || midi_fx_slot >= inst->midi_fx_count) return -1;
        if (inst->midi_fx_plugins[midi_fx_slot] && inst->midi_fx_instances[midi_fx_slot]) {
            inst->midi_fx_plugins[midi_fx_slot]->set_param(inst->midi_fx_instances[midi_fx_slot], param, val);
            return 0;
        }
        return -1;
    }

    return -1;
}

static int chain_mod_has_active_sources(const mod_target_state_t *entry) {
    if (!entry) return 0;

    for (int i = 0; i < MAX_MOD_SOURCES_PER_TARGET; i++) {
        if (entry->sources[i].active) return 1;
    }
    return 0;
}

static float chain_mod_sum_contributions(const mod_target_state_t *entry) {
    float sum = 0.0f;
    if (!entry) return sum;

    for (int i = 0; i < MAX_MOD_SOURCES_PER_TARGET; i++) {
        if (!entry->sources[i].active) continue;
        sum += entry->sources[i].contribution;
    }
    return sum;
}

static mod_source_contribution_t *chain_mod_find_source_contribution(mod_target_state_t *entry,
                                                                     const char *source_id) {
    if (!entry || !source_id || !source_id[0]) return NULL;

    for (int i = 0; i < MAX_MOD_SOURCES_PER_TARGET; i++) {
        mod_source_contribution_t *source_entry = &entry->sources[i];
        if (!source_entry->active) continue;
        if (strcmp(source_entry->source_id, source_id) == 0) return source_entry;
    }

    return NULL;
}

static mod_source_contribution_t *chain_mod_find_or_alloc_source_contribution(mod_target_state_t *entry,
                                                                               const char *source_id) {
    if (!entry || !source_id || !source_id[0]) return NULL;

    mod_source_contribution_t *source_entry = chain_mod_find_source_contribution(entry, source_id);
    if (source_entry) return source_entry;

    for (int i = 0; i < MAX_MOD_SOURCES_PER_TARGET; i++) {
        source_entry = &entry->sources[i];
        if (source_entry->active) continue;
        memset(source_entry, 0, sizeof(*source_entry));
        source_entry->active = 1;
        strncpy(source_entry->source_id, source_id, sizeof(source_entry->source_id) - 1);
        return source_entry;
    }

    return NULL;
}

static void chain_mod_remove_source_contribution(mod_target_state_t *entry, const char *source_id) {
    mod_source_contribution_t *source_entry = chain_mod_find_source_contribution(entry, source_id);
    if (!source_entry) return;
    memset(source_entry, 0, sizeof(*source_entry));
}

static void chain_mod_recompute_effective(mod_target_state_t *entry) {
    if (!entry) return;

    float effective = chain_mod_clampf(entry->base_value + chain_mod_sum_contributions(entry),
                                       entry->min_val,
                                       entry->max_val);
    if (entry->type == KNOB_TYPE_INT || entry->type == KNOB_TYPE_ENUM) {
        effective = (float)((int)effective);
    }
    entry->effective_value = chain_mod_clampf(effective, entry->min_val, entry->max_val);
}

mod_target_state_t *chain_mod_find_target_entry(chain_instance_t *inst,
                                                        const char *target,
                                                        const char *param) {
    if (!inst || !target || !param) return NULL;

    for (int i = 0; i < inst->mod_target_count && i < MAX_MOD_TARGETS; i++) {
        mod_target_state_t *entry = &inst->mod_targets[i];
        if (!entry->active) continue;
        if (strcmp(entry->target, target) == 0 && strcmp(entry->param, param) == 0) {
            return entry;
        }
    }
    return NULL;
}

static mod_target_state_t *chain_mod_alloc_target_entry(chain_instance_t *inst,
                                                         const char *target,
                                                         const char *param) {
    mod_target_state_t *entry = chain_mod_find_target_entry(inst, target, param);
    if (entry) return entry;
    if (!inst || !target || !param) return NULL;

    for (int i = 0; i < MAX_MOD_TARGETS; i++) {
        entry = &inst->mod_targets[i];
        if (entry->active) continue;

        memset(entry, 0, sizeof(*entry));
        entry->active = 1;
        strncpy(entry->target, target, sizeof(entry->target) - 1);
        strncpy(entry->param, param, sizeof(entry->param) - 1);
        entry->min_val = 0.0f;
        entry->max_val = 1.0f;
        entry->type = KNOB_TYPE_FLOAT;

        if (i >= inst->mod_target_count) {
            inst->mod_target_count = i + 1;
        }
        return entry;
    }

    return NULL;
}

int chain_mod_is_target_active(chain_instance_t *inst, const char *target, const char *param) {
    mod_target_state_t *entry = chain_mod_find_target_entry(inst, target, param);
    return (entry && entry->active && entry->enabled);
}

void chain_mod_update_base_from_set_param(chain_instance_t *inst,
                                                 const char *target,
                                                 const char *param,
                                                 const char *val) {
    if (!inst || !target || !param || !val) return;

    mod_target_state_t *entry = chain_mod_find_target_entry(inst, target, param);
    if (!entry || !entry->active) return;

    chain_param_info_t *pinfo = find_param_by_key(inst, target, param);
    if (pinfo) {
        entry->type = pinfo->type;
        entry->min_val = pinfo->min_val;
        entry->max_val = pinfo->max_val;
    }

    float base = entry->base_value;
    if (pinfo) {
        base = dsp_value_to_float(val, pinfo, base);
    } else {
        char *endptr = NULL;
        float parsed = strtof(val, &endptr);
        if (endptr != val) {
            base = parsed;
        }
    }

    entry->base_value = chain_mod_clampf(base, entry->min_val, entry->max_val);
}

void chain_mod_apply_effective_value(chain_instance_t *inst, mod_target_state_t *entry, int force_write) {
    if (!inst || !entry || !entry->active) return;

    chain_mod_recompute_effective(entry);

    uint64_t now_ms = 0;
    uint32_t min_interval_ms = 0;
    if (entry->type == KNOB_TYPE_INT || entry->type == KNOB_TYPE_ENUM) {
        min_interval_ms = MOD_INT_ENUM_MIN_INTERVAL_MS;
    }

    if (!force_write) {
        if (entry->has_last_applied) {
            if (entry->type == KNOB_TYPE_INT || entry->type == KNOB_TYPE_ENUM) {
                if ((int)entry->effective_value == (int)entry->last_applied_value) {
                    return;
                }
            } else if (fabsf(entry->effective_value - entry->last_applied_value) < MOD_FLOAT_CHANGE_EPSILON) {
                return;
            }
        }

        if (min_interval_ms > 0) {
            now_ms = get_time_ms();
            if (entry->last_applied_ms > 0 && (now_ms - entry->last_applied_ms) < min_interval_ms) {
                return;
            }
        }
    }

    char val_str[32];
    if (entry->type == KNOB_TYPE_INT || entry->type == KNOB_TYPE_ENUM) {
        snprintf(val_str, sizeof(val_str), "%d", (int)entry->effective_value);
    } else {
        snprintf(val_str, sizeof(val_str), "%.6f", entry->effective_value);
    }

    int rc = chain_mod_set_param_string(inst, entry->target, entry->param, val_str);
    if (rc == 0) {
        entry->last_applied_value = entry->effective_value;
        if (now_ms == 0) now_ms = get_time_ms();
        entry->last_applied_ms = now_ms;
        entry->has_last_applied = 1;
    }
}

static void chain_mod_clear_target_entry(chain_instance_t *inst, mod_target_state_t *entry, int restore_base) {
    if (!inst || !entry || !entry->active) return;

    entry->enabled = 0;
    if (restore_base) {
        entry->effective_value = entry->base_value;
        chain_mod_apply_effective_value(inst, entry, 1);
    }
    memset(entry, 0, sizeof(*entry));

    while (inst->mod_target_count > 0 &&
           !inst->mod_targets[inst->mod_target_count - 1].active) {
        inst->mod_target_count--;
    }
}

void chain_mod_clear_target_entries(chain_instance_t *inst, const char *target, int restore_base) {
    if (!inst || !target || !target[0]) return;

    for (int i = 0; i < inst->mod_target_count && i < MAX_MOD_TARGETS; i++) {
        mod_target_state_t *entry = &inst->mod_targets[i];
        if (!entry->active) continue;
        if (strcmp(entry->target, target) != 0) continue;
        chain_mod_clear_target_entry(inst, entry, restore_base);
    }
}

/* Optional getter helper: key suffix ':base' returns non-modulated base value.
 * Example: 'synth:cutoff:base' -> base cutoff while modulation is active. */
int chain_mod_get_base_for_subkey(chain_instance_t *inst,
                                         const char *target,
                                         const char *subkey,
                                         char *buf,
                                         int buf_len) {
    if (!inst || !target || !subkey || !buf || buf_len < 2) return -1;

    const size_t suffix_len = 5; /* ":base" */
    const size_t subkey_len = strlen(subkey);
    if (subkey_len <= suffix_len || strcmp(subkey + subkey_len - suffix_len, ":base") != 0) {
        return -1;
    }

    char param[64];
    const size_t param_len = subkey_len - suffix_len;
    if (param_len == 0 || param_len >= sizeof(param)) return -1;
    memcpy(param, subkey, param_len);
    param[param_len] = '\0';

    mod_target_state_t *entry = chain_mod_find_target_entry(inst, target, param);
    if (entry && entry->active) {
        if (entry->type == KNOB_TYPE_INT || entry->type == KNOB_TYPE_ENUM) {
            return snprintf(buf, buf_len, "%d", (int)entry->base_value);
        }
        return snprintf(buf, buf_len, "%.6f", entry->base_value);
    }

    /* If not modulated, return current plugin value for compatibility. */
    return chain_mod_get_param_string(inst, target, param, buf, buf_len);
}

/* Optional getter helper: key suffix ':modulated' returns whether a target
 * currently has at least one active modulation source. */
int chain_mod_get_modulated_for_subkey(chain_instance_t *inst,
                                              const char *target,
                                              const char *subkey,
                                              char *buf,
                                              int buf_len) {
    if (!inst || !target || !subkey || !buf || buf_len < 2) return -1;

    const size_t suffix_len = 10; /* ":modulated" */
    const size_t subkey_len = strlen(subkey);
    if (subkey_len <= suffix_len || strcmp(subkey + subkey_len - suffix_len, ":modulated") != 0) {
        return -1;
    }

    char param[64];
    const size_t param_len = subkey_len - suffix_len;
    if (param_len == 0 || param_len >= sizeof(param)) return -1;
    memcpy(param, subkey, param_len);
    param[param_len] = '\0';

    mod_target_state_t *entry = chain_mod_find_target_entry(inst, target, param);
    if (entry && entry->active && chain_mod_has_active_sources(entry)) {
        return snprintf(buf, buf_len, "1");
    }
    return snprintf(buf, buf_len, "0");
}

/* Runtime modulation callback (initial stateful implementation).
 * Applies non-destructive contribution math and stores effective values. */
int chain_mod_emit_value(void *ctx,
                                const char *source_id,
                                const char *target,
                                const char *param,
                                float signal,
                                float depth,
                                float offset,
                                int bipolar,
                                int enabled) {
    chain_instance_t *inst = (chain_instance_t *)ctx;
    if (!inst || !source_id || !target || !param) return -1;

    if (!enabled) {
        chain_mod_clear_source(inst, source_id);
        return 0;
    }

    chain_param_info_t *pinfo = find_param_by_key(inst, target, param);
    if (!pinfo) {
        mod_target_state_t *stale = chain_mod_find_target_entry(inst, target, param);
        if (stale && stale->active) {
            chain_mod_remove_source_contribution(stale, source_id);
            if (!chain_mod_has_active_sources(stale)) {
                chain_mod_clear_target_entry(inst, stale, 0);
            }
        }
        return -1;
    }
    mod_target_state_t *entry = chain_mod_alloc_target_entry(inst, target, param);
    if (!entry) return -1;

    mod_source_contribution_t *source_entry = chain_mod_find_or_alloc_source_contribution(entry, source_id);
    if (!source_entry) return -1;

    if (!entry->enabled) {
        float base = pinfo->default_val;
        char val_buf[64];
        if (chain_mod_get_param_string(inst, target, param, val_buf, sizeof(val_buf)) > 0) {
            base = dsp_value_to_float(val_buf, pinfo, base);
        }
        entry->base_value = chain_mod_clampf(base, pinfo->min_val, pinfo->max_val);
    }

    entry->type = pinfo->type;
    entry->min_val = pinfo->min_val;
    entry->max_val = pinfo->max_val;

    /* For unipolar sources, map [-1,1] into [0,1] before depth scaling. */
    float mod_signal = signal;
    if (!bipolar) {
        mod_signal = (signal + 1.0f) * 0.5f;
    }

    /* Scale source depth/offset by target parameter range so a depth of 1.0
     * has meaningful effect on large-range params (e.g. 0..127). */
    float range_span = entry->max_val - entry->min_val;
    if (range_span <= 0.0f) {
        range_span = 1.0f;
    }
    float range_scale = bipolar ? (0.5f * range_span) : range_span;
    source_entry->contribution = ((mod_signal * depth) + offset) * range_scale;
    entry->enabled = chain_mod_has_active_sources(entry);
    chain_mod_apply_effective_value(inst, entry, 0);
    return 0;
}

void chain_mod_clear_source(void *ctx, const char *source_id) {
    chain_instance_t *inst = (chain_instance_t *)ctx;
    if (!inst) return;

    const int clear_all = (!source_id || source_id[0] == '\0');
    for (int i = 0; i < inst->mod_target_count && i < MAX_MOD_TARGETS; i++) {
        mod_target_state_t *entry = &inst->mod_targets[i];
        if (!entry->active) continue;

        if (clear_all) {
            chain_mod_clear_target_entry(inst, entry, 1);
            continue;
        }

        chain_mod_remove_source_contribution(entry, source_id);
        if (!chain_mod_has_active_sources(entry)) {
            chain_mod_clear_target_entry(inst, entry, 1);
            continue;
        }

        entry->enabled = 1;
        chain_mod_apply_effective_value(inst, entry, 0);
    }
}


int chain_mod_refresh_target_param_cache(chain_instance_t *inst, const char *target) {
    if (!inst || !target) return -1;

    char buf[32768];
    int result = -1;
    chain_param_info_t parsed[MAX_CHAIN_PARAMS];
    int parsed_count = -1;

    if (strcmp(target, "synth") == 0) {
        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->get_param) {
            result = inst->synth_plugin_v2->get_param(inst->synth_instance, "chain_params", buf, sizeof(buf));
        }
        if (result <= 0) return -1;
        parsed_count = parse_chain_params_array_json(buf, parsed, MAX_CHAIN_PARAMS);
        if (parsed_count < 0) return -1;
        memcpy(inst->synth_params, parsed, sizeof(chain_param_info_t) * (size_t)parsed_count);
        inst->synth_param_count = parsed_count;
        return parsed_count;
    }

    if (strncmp(target, "fx", 2) == 0) {
        int fx_slot = atoi(target + 2) - 1;
        if (fx_slot < 0 || fx_slot >= MAX_AUDIO_FX || fx_slot >= inst->fx_count) return -1;

        if (inst->fx_is_v2[fx_slot] && inst->fx_plugins_v2[fx_slot] &&
            inst->fx_instances[fx_slot] && inst->fx_plugins_v2[fx_slot]->get_param) {
            result = inst->fx_plugins_v2[fx_slot]->get_param(inst->fx_instances[fx_slot], "chain_params", buf, sizeof(buf));
        }
        if (result <= 0) return -1;
        parsed_count = parse_chain_params_array_json(buf, parsed, MAX_CHAIN_PARAMS);
        if (parsed_count < 0) return -1;
        memcpy(inst->fx_params[fx_slot], parsed, sizeof(chain_param_info_t) * (size_t)parsed_count);
        inst->fx_param_counts[fx_slot] = parsed_count;
        return parsed_count;
    }

    if (strncmp(target, "midi_fx", 7) == 0) {
        int midi_fx_slot = 0;
        if (target[7] != '\0') {
            midi_fx_slot = atoi(target + 7) - 1;
        }
        if (midi_fx_slot < 0 || midi_fx_slot >= MAX_MIDI_FX || midi_fx_slot >= inst->midi_fx_count) return -1;
        if (!inst->midi_fx_plugins[midi_fx_slot] || !inst->midi_fx_instances[midi_fx_slot]) return -1;

        result = inst->midi_fx_plugins[midi_fx_slot]->get_param(inst->midi_fx_instances[midi_fx_slot],
                                                                "chain_params",
                                                                buf,
                                                                sizeof(buf));
        if (result <= 0) return -1;
        parsed_count = parse_chain_params_array_json(buf, parsed, MAX_CHAIN_PARAMS);
        if (parsed_count < 0) return -1;
        memcpy(inst->midi_fx_params[midi_fx_slot], parsed, sizeof(chain_param_info_t) * (size_t)parsed_count);
        inst->midi_fx_param_counts[midi_fx_slot] = parsed_count;
        return parsed_count;
    }

    return -1;
}

/*
 * Match a modulation key against chain_params metadata.
 * Supports exact matches and child-prefixed keys (e.g. nvram_tone_0_cutofffrequency)
 * where metadata may only expose the suffix key (e.g. cutofffrequency).
 */

