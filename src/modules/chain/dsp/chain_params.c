/*
 * Signal Chain — parameter metadata, parsing, smoothing, knob mapping.
 * Split from chain_host.c (2026-06 cleanup step 10); pure relocation,
 * no behavior change. Shared types/decls live in chain_internal.h.
 */

#include "chain_internal.h"
#include "relative_cc.h"

/*
 * Format a parameter value for display based on its metadata.
 * Returns length of formatted string, or -1 on error.
 */
int format_param_value(chain_param_info_t *param, float value, char *buf, int buf_len) {
    if (!param || !buf || buf_len < 2) return -1;

    if (param->type == KNOB_TYPE_ENUM) {
        /* Use option label for enums */
        int idx = (int)value;
        if (idx >= 0 && idx < param->option_count) {
            int len = strlen(param->options[idx]);
            if (len >= buf_len) len = buf_len - 1;
            memcpy(buf, param->options[idx], len);
            buf[len] = '\0';
            return len;
        }
        /* Fallback for out-of-range enum */
        snprintf(buf, buf_len, "%d", idx);
        return strlen(buf);
    }

    /* Scale 0-1 values to 0-100 for percentage display */
    float display_value = value;
    if (strcmp(param->unit, "%") == 0 && param->max_val <= 1.0f) {
        display_value = value * 100.0f;
    }

    /* Format numeric value */
    char val_str[32];
    if (param->display_format[0]) {
        /* Use custom format */
        snprintf(val_str, sizeof(val_str), param->display_format, display_value);
    } else {
        /* Use defaults based on type */
        if (param->type == KNOB_TYPE_FLOAT) {
            snprintf(val_str, sizeof(val_str), "%.2f", display_value);
        } else {
            snprintf(val_str, sizeof(val_str), "%d", (int)display_value);
        }
    }

    /* Add unit suffix if present */
    if (param->unit[0]) {
        snprintf(buf, buf_len, "%s %s", val_str, param->unit);
    } else {
        snprintf(buf, buf_len, "%s", val_str);
    }

    return strlen(buf);
}

/* Find or create a smoothed parameter slot */
static smooth_param_t* smoother_get_param(param_smoother_t *smoother, const char *key) {
    /* Look for existing */
    for (int i = 0; i < smoother->count; i++) {
        if (strcmp(smoother->params[i].key, key) == 0) {
            return &smoother->params[i];
        }
    }
    /* Create new if space */
    if (smoother->count < MAX_SMOOTH_PARAMS) {
        smooth_param_t *p = &smoother->params[smoother->count++];
        strncpy(p->key, key, MAX_NAME_LEN - 1);
        p->key[MAX_NAME_LEN - 1] = '\0';
        p->target = 0.0f;
        p->current = 0.0f;
        p->active = 0;
        return p;
    }
    return NULL;
}

/* Set a parameter target value for smoothing */
void smoother_set_target(param_smoother_t *smoother, const char *key, float value) {
    smooth_param_t *p = smoother_get_param(smoother, key);
    if (p) {
        /* Always jump current to new value.  The hierarchy editor uses a
         * read-modify-write cycle: it reads the plugin's current value,
         * applies a delta, and writes back.  If current lags behind target
         * (as it does with interpolation), render_block overwrites the
         * plugin value with the lagged current, and the next UI read sees
         * that lagged value — making the parameter appear stuck near 0. */
        p->current = value;
        p->target = value;
        p->active = 1;
    }
}

/* Update all smoothed parameters toward their targets, returns 1 if any changed */
int smoother_update(param_smoother_t *smoother) {
    int changed = 0;
    for (int i = 0; i < smoother->count; i++) {
        smooth_param_t *p = &smoother->params[i];
        if (p->active) {
            float diff = p->target - p->current;
            if (fabsf(diff) > 0.0001f) {
                p->current += diff * SMOOTH_COEFF;
                changed = 1;
            } else {
                p->current = p->target;
            }
        }
    }
    return changed;
}

/* Reset smoother state */
void smoother_reset(param_smoother_t *smoother) {
    smoother->count = 0;
    memset(smoother->params, 0, sizeof(smoother->params));
}

/* Check if a string looks like a float value (for smoothing eligibility) */
int is_smoothable_float(const char *val, float *out_value) {
    if (!val || !val[0]) return 0;

    /* Skip if it's clearly not a number */
    char c = val[0];
    if (c != '-' && c != '.' && (c < '0' || c > '9')) return 0;

    char *endptr;
    float f = strtof(val, &endptr);

    /* Must have parsed something and no trailing garbage (except whitespace) */
    if (endptr == val) return 0;
    while (*endptr == ' ' || *endptr == '\t') endptr++;
    if (*endptr != '\0') return 0;

    /* Don't smooth integer-like values (presets, indices) */
    if (f == (int)f && f >= 0 && f < 1000) {
        /* Could be an index - only smooth if it's in 0-1 range or has decimal */
        if (strchr(val, '.') == NULL && (f < 0.0f || f > 1.0f)) {
            return 0;  /* Likely an integer index, don't smooth */
        }
    }

    if (out_value) *out_value = f;
    return 1;
}

static int parse_param_object(const char *param_json, chain_param_info_t *param) {
    memset(param, 0, sizeof(chain_param_info_t));

    /* Find end of this JSON object (brace-depth tracking) */
    int brace_depth = 0;
    const char *param_obj_end = param_json;
    do {
        if (*param_obj_end == '{') brace_depth++;
        if (*param_obj_end == '}') brace_depth--;
        param_obj_end++;
    } while (brace_depth > 0 && *param_obj_end);

    /* Extract key (required) */
    const char *key_start = bounded_strstr(param_json, param_obj_end, "\"key\"");
    if (!key_start) return -1;
    key_start = strchr(key_start, ':');
    if (!key_start) return -1;
    key_start = strchr(key_start, '"');
    if (!key_start) return -1;
    key_start++;
    const char *key_end = strchr(key_start, '"');
    if (!key_end) return -1;
    int key_len = key_end - key_start;
    if (key_len >= sizeof(param->key)) key_len = sizeof(param->key) - 1;
    memcpy(param->key, key_start, key_len);
    param->key[key_len] = '\0';

    /* Extract label/name (required) */
    const char *label_start = bounded_strstr(param_json, param_obj_end, "\"label\"");
    if (!label_start) label_start = bounded_strstr(param_json, param_obj_end, "\"name\"");
    if (label_start) {
        label_start = strchr(label_start, ':');
        if (label_start) {
            label_start = strchr(label_start, '"');
            if (label_start) {
                label_start++;
                const char *label_end = strchr(label_start, '"');
                if (label_end) {
                    int len = label_end - label_start;
                    if (len >= sizeof(param->name)) len = sizeof(param->name) - 1;
                    memcpy(param->name, label_start, len);
                    param->name[len] = '\0';
                }
            }
        }
    }

    /* Extract type (required) */
    const char *type_start = bounded_strstr(param_json, param_obj_end, "\"type\"");
    if (!type_start) return -1;
    type_start = strchr(type_start, ':');
    if (!type_start) return -1;
    type_start = strchr(type_start, '"');
    if (!type_start) return -1;
    type_start++;

    if (strncmp(type_start, "float", 5) == 0) {
        param->type = KNOB_TYPE_FLOAT;
    } else if (strncmp(type_start, "int", 3) == 0) {
        param->type = KNOB_TYPE_INT;
    } else if (strncmp(type_start, "enum", 4) == 0) {
        param->type = KNOB_TYPE_ENUM;
    } else {
        return -1;
    }

    /* Extract min (optional for enum) */
    const char *min_start = bounded_strstr(param_json, param_obj_end, "\"min\"");
    if (min_start) {
        min_start = strchr(min_start, ':');
        if (min_start) {
            param->min_val = atof(min_start + 1);
        }
    }

    /* Extract max (optional for enum) */
    const char *max_start = bounded_strstr(param_json, param_obj_end, "\"max\"");
    if (max_start) {
        max_start = strchr(max_start, ':');
        if (max_start) {
            param->max_val = atof(max_start + 1);
        }
    }

    /* Extract default (optional) */
    const char *default_start = bounded_strstr(param_json, param_obj_end, "\"default\"");
    if (default_start) {
        default_start = strchr(default_start, ':');
        if (default_start) {
            param->default_val = atof(default_start + 1);
        }
    } else {
        /* Default to min for numeric, 0 for enum */
        param->default_val = (param->type == KNOB_TYPE_ENUM) ? 0 : param->min_val;
    }

    /* Extract step (optional) */
    const char *step_start = bounded_strstr(param_json, param_obj_end, "\"step\"");
    if (step_start) {
        step_start = strchr(step_start, ':');
        if (step_start) {
            param->step = atof(step_start + 1);
        }
    } else {
        /* Default step values */
        if (param->type == KNOB_TYPE_FLOAT) {
            param->step = 0.0015f;
        } else {
            param->step = 1.0f;
        }
    }

    /* Extract unit (optional) */
    const char *unit_start = bounded_strstr(param_json, param_obj_end, "\"unit\"");
    if (unit_start) {
        unit_start = strchr(unit_start, ':');
        if (unit_start) {
            unit_start = strchr(unit_start, '"');
            if (unit_start) {
                unit_start++;
                const char *unit_end = strchr(unit_start, '"');
                if (unit_end) {
                    int len = unit_end - unit_start;
                    if (len >= sizeof(param->unit)) len = sizeof(param->unit) - 1;
                    memcpy(param->unit, unit_start, len);
                    param->unit[len] = '\0';
                }
            }
        }
    }

    /* Extract display_format (optional) */
    const char *format_start = bounded_strstr(param_json, param_obj_end, "\"display_format\"");
    if (format_start) {
        format_start = strchr(format_start, ':');
        if (format_start) {
            format_start = strchr(format_start, '"');
            if (format_start) {
                format_start++;
                const char *format_end = strchr(format_start, '"');
                if (format_end) {
                    int len = format_end - format_start;
                    if (len >= sizeof(param->display_format)) len = sizeof(param->display_format) - 1;
                    memcpy(param->display_format, format_start, len);
                    param->display_format[len] = '\0';
                }
            }
        }
    }

    /* Extract options array (for enums) */
    if (param->type == KNOB_TYPE_ENUM) {
        const char *options_start = bounded_strstr(param_json, param_obj_end, "\"options\"");
        if (options_start) {
            options_start = strchr(options_start, '[');
            if (options_start) {
                options_start++;
                param->option_count = 0;

                /* Parse each option string */
                const char *opt = options_start;
                while (param->option_count < MAX_ENUM_OPTIONS) {
                    opt = strchr(opt, '"');
                    if (!opt || opt > strstr(options_start, "]")) break;
                    opt++;
                    const char *opt_end = strchr(opt, '"');
                    if (!opt_end) break;

                    int len = opt_end - opt;
                    if (len >= 32) len = 31;
                    memcpy(param->options[param->option_count], opt, len);
                    param->options[param->option_count][len] = '\0';
                    param->option_count++;

                    opt = opt_end + 1;
                }
            }
        }

        /* Set max_val for enums to option_count - 1 */
        if (param->option_count > 0) {
            param->max_val = (float)(param->option_count - 1);
        }
    }

    /*
     * Extract max_param — RECORDED BUT NOT IMPLEMENTED, deliberately.
     *
     * Eight modules declare it (sf2, hush1, hera, surge, moog, minijv, helm,
     * eucalypso) and NOTHING has ever consumed it: it is parsed into the struct
     * here and read by no one, in C or in JS. Its only effect was the marker
     * below, `max_val = -1`, which chain_host serialises literally — so sf2
     * shipped `{"min":0,"max":-1}`, an inverted range, and the rest lost their
     * declared bound. A field that silently corrupts what it decorates is worse
     * than one that does nothing, so the marker is gone and the declared max
     * (or the type default) now stands.
     *
     * It is NOT implemented because the two real uses disagree about what the
     * referenced key means, and picking one would be guessing at someone else's
     * intent:
     *
     *   preset       max_param="preset_count"  -> wants count - 1  (7 modules)
     *   laneN_pulses max_param="laneN_steps"   -> wants the value  (eucalypso)
     *
     * Modules should publish a real `max` instead. Every one of these builds
     * its chain_params string at runtime, so it already knows the number at the
     * moment it serialises — see docs/MODULES.md.
     */
    const char *max_param_start = bounded_strstr(param_json, param_obj_end, "\"max_param\"");
    if (max_param_start) {
        max_param_start = strchr(max_param_start, ':');
        if (max_param_start) {
            max_param_start = strchr(max_param_start, '"');
            if (max_param_start) {
                max_param_start++;
                const char *max_param_end = strchr(max_param_start, '"');
                if (max_param_end) {
                    int len = max_param_end - max_param_start;
                    if (len >= sizeof(param->max_param)) len = sizeof(param->max_param) - 1;
                    memcpy(param->max_param, max_param_start, len);
                    param->max_param[len] = '\0';
                    /*
                     * These declarations pair max_param with NO literal "max"
                     * (sf2, hush1), so dropping the marker would leave max_val
                     * at its memset 0 — a 0..0 knob that cannot move, which is
                     * no better than the inverted range we are removing. Fall
                     * back to the same default a max-less int already gets in
                     * parse_chain_params_array_json below, so the param behaves
                     * like every other unbounded int rather than like a bug.
                     * Only reached when max_param is declared; nothing else
                     * changes.
                     */
                    if (!bounded_strstr(param_json, param_obj_end, "\"max\"")) {
                        param->max_val = (param->type == KNOB_TYPE_FLOAT) ? 1.0f : 9999.0f;
                    }
                }
            }
        }
    }

    return 0;
}

/*
 * Parse params array from a single level.
 * Recursively processes nested levels if needed.
 */
static int parse_level_params(const char *level_json, chain_param_info_t *out_params, int *param_count, int max_params) {
    /* Find params array in this level */
    const char *params = strstr(level_json, "\"params\"");
    if (!params) return 0;

    const char *arr_open = strchr(params, '[');
    if (!arr_open) return 0;

    /* Find matching ] for the params array using bracket-depth tracking.
     * This prevents iteration from escaping into knobs or other fields. */
    const char *arr_end = arr_open + 1;
    int bracket_depth = 1;
    while (*arr_end && bracket_depth > 0) {
        if (*arr_end == '[') bracket_depth++;
        else if (*arr_end == ']') bracket_depth--;
        if (bracket_depth > 0) arr_end++;
    }
    /* arr_end now points at the matching ] */

    /* Iterate through params array, bounded by arr_end */
    const char *param_start = arr_open + 1;
    while (*param_count < max_params && param_start < arr_end) {
        /* Skip whitespace */
        while (param_start < arr_end && (*param_start == ' ' || *param_start == '\t' || *param_start == '\n' || *param_start == '\r')) param_start++;

        /* Check for end of array */
        if (param_start >= arr_end || *param_start == ']') break;

        /* Check if this is an object */
        if (*param_start == '{') {
            /* Find end of this object */
            int brace_depth = 0;
            const char *param_end = param_start;
            do {
                if (*param_end == '{') brace_depth++;
                if (*param_end == '}') brace_depth--;
                param_end++;
            } while (brace_depth > 0 && *param_end);

            /* Only parse if object is within the params array */
            if (param_end <= arr_end + 1) {
                /* Check if this is a param definition (has "type" key within this object) */
                if (bounded_strstr(param_start, param_end, "\"type\"")) {
                    if (parse_param_object(param_start, &out_params[*param_count]) == 0) {
                        (*param_count)++;
                    }
                }
            }
            /* Skip navigation items (they don't define params) */

            param_start = param_end;
        } else if (*param_start == '"') {
            /* String reference - skip (already defined elsewhere) */
            const char *close_quote = strchr(param_start + 1, '"');
            if (close_quote && close_quote < arr_end) {
                param_start = close_quote + 1;
            } else {
                break;
            }
        } else {
            param_start++;
            continue;
        }

        /* Skip comma (bounded) */
        while (param_start < arr_end && *param_start != ',' && *param_start != ']') param_start++;
        if (param_start >= arr_end || *param_start == ']') break;
        param_start++;
    }

    return 0;
}

/*
 * Parse parameters from ui_hierarchy structure.
 * Extracts param definitions from shared_params and all levels.
 */
static int parse_hierarchy_params(const char *json, chain_param_info_t *out_params, int max_params) {
    int param_count = 0;

    /* Find ui_hierarchy section */
    const char *hierarchy = strstr(json, "\"ui_hierarchy\"");
    if (!hierarchy) return 0;

    /* Parse shared_params if present */
    const char *shared = strstr(hierarchy, "\"shared_params\"");
    if (shared) {
        shared = strchr(shared, '[');
        if (shared) {
            shared++;

            /* Iterate through shared_params array */
            const char *param_start = shared;
            while (param_count < max_params) {
                /* Skip whitespace */
                while (*param_start == ' ' || *param_start == '\t' || *param_start == '\n') param_start++;

                /* Check for end of array */
                if (*param_start == ']') break;

                /* Check if this is an object (not a string reference) */
                if (*param_start == '{') {
                    /* Find end of this object */
                    int brace_depth = 0;
                    const char *param_end = param_start;
                    do {
                        if (*param_end == '{') brace_depth++;
                        if (*param_end == '}') brace_depth--;
                        param_end++;
                    } while (brace_depth > 0 && *param_end);

                    /* Parse this param object */
                    if (parse_param_object(param_start, &out_params[param_count]) == 0) {
                        param_count++;
                    }

                    param_start = param_end;
                } else if (*param_start == '"') {
                    /* String reference - skip for now (just advance past it) */
                    param_start = strchr(param_start + 1, '"');
                    if (param_start) param_start++;
                }

                /* Skip comma */
                param_start = strchr(param_start, ',');
                if (!param_start) break;
                param_start++;
            }
        }
    }

    /* Parse params from all levels */
    const char *levels = strstr(hierarchy, "\"levels\"");
    if (levels) {
        const char *levels_open = strchr(levels, '{');
        if (levels_open) {
            /* Find end of levels object using brace-depth tracking */
            int levels_depth = 0;
            const char *levels_end = levels_open;
            do {
                if (*levels_end == '{') levels_depth++;
                if (*levels_end == '}') levels_depth--;
                levels_end++;
            } while (levels_depth > 0 && *levels_end);

            /* Iterate through each level object, bounded by levels_end */
            const char *level_start = levels_open + 1;
            while (param_count < max_params && level_start < levels_end) {
                /* Skip to next level definition */
                level_start = strchr(level_start, '{');
                if (!level_start || level_start >= levels_end) break;

                /* Find end of this level */
                int brace_depth = 0;
                const char *level_end = level_start;
                do {
                    if (*level_end == '{') brace_depth++;
                    if (*level_end == '}') brace_depth--;
                    level_end++;
                } while (brace_depth > 0 && *level_end);

                /* Parse params from this level */
                parse_level_params(level_start, out_params, &param_count, max_params);

                level_start = level_end;
            }
        }
    }

    /* Validate no duplicate keys */
    for (int i = 0; i < param_count; i++) {
        for (int j = i + 1; j < param_count; j++) {
            if (strcmp(out_params[i].key, out_params[j].key) == 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "ERROR: Duplicate parameter key '%s' in ui_hierarchy", out_params[i].key);
                chain_log(msg);
                return -1; /* Signal error */
            }
        }
    }

    return param_count;
}

/*
 * Parse parameter definitions from module.json.
 * First tries ui_hierarchy (new format), falls back to chain_params (legacy).
 */
int parse_chain_params(const char *module_path, chain_param_info_t *params, int *count) {
    char json_path[MAX_PATH_LEN];
    snprintf(json_path, sizeof(json_path), "%s/module.json", module_path);

    FILE *f = fopen(json_path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 65536) {
        fclose(f);
        return -1;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return -1;
    }

    { size_t nr = fread(json, 1, size, f); json[nr] = '\0'; }
    fclose(f);

    /* Try ui_hierarchy first */
    const char *hierarchy = strstr(json, "\"ui_hierarchy\"");
    if (hierarchy) {
        *count = parse_hierarchy_params(json, params, MAX_CHAIN_PARAMS);
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg), "Parsed ui_hierarchy params: count=%d", *count);
        chain_log(log_msg);
        for (int i = 0; i < *count && i < 10; i++) {
            snprintf(log_msg, sizeof(log_msg), "  Param[%d]: key=%s, name=%s, type=%d",
                     i, params[i].key, params[i].name, params[i].type);
            chain_log(log_msg);
        }
        if (*count > 0) {
            free(json);
            return 0;
        }
        /* count == 0: hierarchy had no inline params (string refs only).
         * Fall through to chain_params for metadata. */
        chain_log("No inline params in ui_hierarchy, falling through to chain_params");
    }

    /* Fall back to legacy chain_params */
    *count = 0;

    /* Find chain_params array */
    const char *chain_params_str = strstr(json, "\"chain_params\"");
    if (!chain_params_str) {
        free(json);
        return 0;  /* No params is OK */
    }

    const char *arr_start = strchr(chain_params_str, '[');
    if (!arr_start) {
        free(json);
        return 0;
    }

    /* Find matching ] */
    int depth = 1;
    const char *arr_end = arr_start + 1;
    while (*arr_end && depth > 0) {
        if (*arr_end == '[') depth++;
        else if (*arr_end == ']') depth--;
        arr_end++;
    }

    /* Parse each parameter object (legacy chain_params format) */
    const char *pos = arr_start + 1;
    while (pos < arr_end && *count < MAX_CHAIN_PARAMS) {
        const char *obj_start = strchr(pos, '{');
        if (!obj_start || obj_start >= arr_end) break;

        const char *obj_end = strchr(obj_start, '}');
        if (!obj_end || obj_end >= arr_end) break;

        chain_param_info_t *p = &params[*count];
        memset(p, 0, sizeof(*p));
        p->type = KNOB_TYPE_FLOAT;  /* Default */
        p->min_val = 0.0f;
        p->max_val = 1.0f;

        /* Parse key - look for "key": to find field, not value */
        const char *key_pos = strstr(obj_start, "\"key\":");
        if (key_pos && key_pos < obj_end) {
            const char *q1 = strchr(key_pos + 6, '"');
            if (q1 && q1 < obj_end) {
                q1++;
                const char *q2 = strchr(q1, '"');
                if (q2 && q2 < obj_end) {
                    int len = (int)(q2 - q1);
                    if (len > 31) len = 31;
                    strncpy(p->key, q1, len);
                }
            }
        }

        /* Parse name - look for "name": to find field */
        const char *name_pos = strstr(obj_start, "\"name\":");
        if (name_pos && name_pos < obj_end) {
            const char *q1 = strchr(name_pos + 7, '"');
            if (q1 && q1 < obj_end) {
                q1++;
                const char *q2 = strchr(q1, '"');
                if (q2 && q2 < obj_end) {
                    int len = (int)(q2 - q1);
                    if (len > 31) len = 31;
                    strncpy(p->name, q1, len);
                }
            }
        }

        /* Parse type - look for "type": to find field, not value */
        const char *type_pos = strstr(obj_start, "\"type\":");
        if (type_pos && type_pos < obj_end) {
            const char *q1 = strchr(type_pos + 7, '"');
            if (q1 && q1 < obj_end) {
                q1++;
                if (strncmp(q1, "int", 3) == 0) {
                    p->type = KNOB_TYPE_INT;
                    p->max_val = 9999.0f;  /* Default for int */
                } else if (strncmp(q1, "enum", 4) == 0) {
                    p->type = KNOB_TYPE_ENUM;
                }
            }
        }

        /* Parse options (for enum type) */
        const char *options_pos = strstr(obj_start, "\"options\":");
        if (options_pos && options_pos < obj_end) {
            const char *arr_start2 = strchr(options_pos, '[');
            if (arr_start2 && arr_start2 < obj_end) {
                const char *arr_end2 = strchr(arr_start2, ']');
                if (arr_end2 && arr_end2 < obj_end) {
                    const char *opt_pos = arr_start2 + 1;
                    while (opt_pos < arr_end2 && p->option_count < MAX_ENUM_OPTIONS) {
                        const char *oq1 = strchr(opt_pos, '"');
                        if (!oq1 || oq1 >= arr_end2) break;
                        oq1++;
                        const char *oq2 = strchr(oq1, '"');
                        if (!oq2 || oq2 >= arr_end2) break;
                        int olen = (int)(oq2 - oq1);
                        if (olen > 31) olen = 31;
                        strncpy(p->options[p->option_count], oq1, olen);
                        p->options[p->option_count][olen] = '\0';
                        p->option_count++;
                        opt_pos = oq2 + 1;
                    }
                }
            }
        }

        /* Parse min */
        const char *min_pos = strstr(obj_start, "\"min\":");
        if (min_pos && min_pos < obj_end) {
            const char *colon = strchr(min_pos, ':');
            if (colon && colon < obj_end) {
                p->min_val = (float)atof(colon + 1);
            }
        }

        /* Parse max - look for "max": but not "max_param": */
        const char *max_pos = strstr(obj_start, "\"max\":");
        if (max_pos && max_pos < obj_end) {
            const char *colon = strchr(max_pos, ':');
            if (colon && colon < obj_end) {
                p->max_val = (float)atof(colon + 1);
            }
        }

        /* Parse max_param. UNIMPLEMENTED — see the note at the other parse
         * site; recorded for diagnostics only, and it must NOT touch max_val. */
        const char *max_param_pos = strstr(obj_start, "\"max_param\":");
        if (max_param_pos && max_param_pos < obj_end) {
            const char *q1 = strchr(max_param_pos + 12, '"');
            if (q1 && q1 < obj_end) {
                q1++;
                const char *q2 = strchr(q1, '"');
                if (q2 && q2 < obj_end) {
                    int len = (int)(q2 - q1);
                    if (len > 31) len = 31;
                    strncpy(p->max_param, q1, len);
                }
            }
        }

        /* Parse default */
        const char *def_pos = strstr(obj_start, "\"default\":");
        if (def_pos && def_pos < obj_end) {
            const char *colon = strchr(def_pos, ':');
            if (colon && colon < obj_end) {
                p->default_val = (float)atof(colon + 1);
            }
        }

        if (p->key[0]) {
            (*count)++;
        }

        pos = obj_end + 1;
    }

    free(json);
    return 0;
}

/* Parse ui_hierarchy object from module.json and cache it as raw JSON. */
int parse_ui_hierarchy_cache(const char *module_path, char *out, int out_len) {
    char json_path[MAX_PATH_LEN];
    if (!module_path || !out || out_len < 2) return -1;

    out[0] = '\0';
    snprintf(json_path, sizeof(json_path), "%s/module.json", module_path);

    FILE *f = fopen(json_path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size >= 65536) {
        fclose(f);
        return -1;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return -1;
    }

    { size_t nr = fread(json, 1, size, f); json[nr] = '\0'; }
    fclose(f);

    int rc = -1;
    const char *hier_start = strstr(json, "\"ui_hierarchy\"");
    if (hier_start) {
        const char *obj_start = strchr(hier_start, '{');
        if (obj_start) {
            int depth = 1;
            const char *obj_end = obj_start + 1;
            while (*obj_end && depth > 0) {
                if (*obj_end == '{') depth++;
                else if (*obj_end == '}') depth--;
                obj_end++;
            }
            int len = (int)(obj_end - obj_start);
            if (depth == 0 && len > 0 && len < out_len) {
                memcpy(out, obj_start, (size_t)len);
                out[len] = '\0';
                rc = len;
            }
        }
    }

    free(json);
    return rc;
}

/*
 * Parse a runtime chain_params JSON array (as returned by plugin get_param).
 * Returns parsed count on success (including 0), or -1 on malformed input.
 */
int parse_chain_params_array_json(const char *json_array, chain_param_info_t *params, int max_params) {
    if (!json_array || !params || max_params <= 0) return -1;

    const char *arr_start = strchr(json_array, '[');
    if (!arr_start) return -1;

    int depth = 1;
    const char *arr_end = arr_start + 1;
    while (*arr_end && depth > 0) {
        if (*arr_end == '[') depth++;
        else if (*arr_end == ']') depth--;
        arr_end++;
    }
    if (depth != 0) return -1;

    int count = 0;
    const char *pos = arr_start + 1;
    while (pos < arr_end && count < max_params) {
        const char *obj_start = strchr(pos, '{');
        if (!obj_start || obj_start >= arr_end) break;

        int obj_depth = 0;
        const char *obj_end = obj_start;
        do {
            if (*obj_end == '{') obj_depth++;
            else if (*obj_end == '}') obj_depth--;
            obj_end++;
        } while (obj_end < arr_end && obj_depth > 0);

        if (obj_depth != 0) break;

        chain_param_info_t parsed;
        if (parse_param_object(obj_start, &parsed) == 0) {
            params[count++] = parsed;
        }

        pos = obj_end;
    }

    return count;
}

/* Look up parameter info by key in a param list */
chain_param_info_t *find_param_info(chain_param_info_t *params, int count, const char *key) {
    for (int i = 0; i < count; i++) {
        if (strcmp(params[i].key, key) == 0) {
            return &params[i];
        }
    }
    return NULL;
}

/*
 * Look up param metadata from a knob/modulation target string
 * ("synth", "fx1".."fxN", "midi_fx1".."midi_fxN").
 *
 * Indexed, not enumerated: this used to stop at fx3/midi_fx2, which is how a
 * five-FX chain got a target it could route but not modulate. The bound is
 * MAX_AUDIO_FX / MAX_MIDI_FX so the next cap bump needs no edit here.
 */
chain_param_info_t *knob_find_param(chain_instance_t *inst, const char *target, const char *param) {
    if (!inst || !target) return NULL;
    if (strcmp(target, "synth") == 0)
        return find_param_info(inst->synth_params, inst->synth_param_count, param);

    int fx = chain_fx_index_from_id(target, "fx", MAX_AUDIO_FX);
    if (fx >= 0) {
        if (fx >= inst->fx_count) return NULL;
        return find_param_info(inst->fx_params[fx], inst->fx_param_counts[fx], param);
    }

    int mfx = chain_fx_index_from_id(target, "midi_fx", MAX_MIDI_FX);
    if (mfx >= 0) {
        if (mfx >= inst->midi_fx_count) return NULL;
        return find_param_info(inst->midi_fx_params[mfx], inst->midi_fx_param_counts[mfx], param);
    }

    return NULL;
}

/*
 * Point a destination at a parameter, whole-range.
 *
 * One owner for the three fields that must move together: a truncated name and
 * an unreset range are both silent, and the strncpy-then-terminate spelling
 * this replaces was six lines per assignment and appeared twice.
 */
void knob_dest_assign(knob_dest_t *d, const char *target, const char *param) {
    if (!d) return;
    snprintf(d->target, sizeof(d->target), "%s", target ? target : "");
    snprintf(d->param, sizeof(d->param), "%s", param ? param : "");
    d->lo = 0.0f;
    d->hi = 1.0f;
}

/* ---- One knob-turn law ---------------------------------------------------
 *
 * Three paths turn a chain knob and each had hand-rolled the same time-based
 * acceleration curve: the relative CC decode and the absolute CC in
 * chain_midi.c, and knob_N_adjust in chain_host.c -- which is the path the
 * device's own encoders use, so it is the common case rather than an edge.
 *
 * Three copies of one curve is three places for it to drift, and it already
 * had. The two spellings nested their bounds differently (`elapsed <
 * KNOB_ACCEL_SLOW_MS` outside, vs `elapsed <= KNOB_ACCEL_FAST_MS` first) and
 * happened to compute the same answer -- but they still disagree on the
 * FALLBACK BASE STEP used when a parameter declares no step of its own:
 * chain_midi.c uses KNOB_STEP_FLOAT (0.0015), chain_host.c uses 0.01f, a 6.7x
 * difference between an external controller and the device's own encoder on
 * the same parameter. That one is deliberately NOT resolved here: either value
 * is a behaviour change for somebody, and picking one is a judgement call for
 * review rather than a silent side effect of deduplication.
 *
 * Extracted here rather than into a header because chain_params.c IS compiled
 * natively by tests/host (unlike chain_midi.c and chain_host.c, which dlopen
 * plugins -- that asymmetry is why relative_cc.h exists at all). The curve is
 * split from the clock read for the same reason relative_cc.h split the decode
 * from its call site: a source-level pin cannot tell 4 from 8.
 *
 * Shape and name taken from #347, which reached the same extraction first.
 */

/*
 * Time-based acceleration multiplier for one knob message, against the FLOAT
 * ceiling. Callers that need the int or enum cap apply chain_knob_accel_cap
 * afterwards, because only they know the parameter's type. `last_ms` is read
 * and updated in place.
 */
int chain_knob_accel_for_gap(uint64_t elapsed_ms) {
    if (elapsed_ms >= KNOB_ACCEL_SLOW_MS) return KNOB_ACCEL_MIN_MULT;
    if (elapsed_ms <= KNOB_ACCEL_FAST_MS) return KNOB_ACCEL_MAX_MULT;

    float ratio = (float)(KNOB_ACCEL_SLOW_MS - elapsed_ms) /
                  (float)(KNOB_ACCEL_SLOW_MS - KNOB_ACCEL_FAST_MS);
    return KNOB_ACCEL_MIN_MULT +
           (int)(ratio * (KNOB_ACCEL_MAX_MULT - KNOB_ACCEL_MIN_MULT));
}

int chain_knob_accel(uint64_t *last_ms) {
    uint64_t now = get_time_ms();
    uint64_t last = *last_ms;
    *last_ms = now;

    /* No previous message: the first turn of a session is a slow one. */
    if (last == 0) return KNOB_ACCEL_MIN_MULT;
    return chain_knob_accel_for_gap(now - last);
}

/*
 * Cap the multiplier for a stepped parameter. Enums never accelerate on TIME
 * (a deliberate slow turn must not overshoot a list of options); ints are
 * limited rather than pinned. Both rules predate this extraction and are
 * unchanged -- see relative_cc_multiplier for why a DETENT count is still
 * honoured for an enum even though the time multiplier is not.
 */
int chain_knob_accel_cap(int accel, int type) {
    if (type == KNOB_TYPE_ENUM) return KNOB_ACCEL_ENUM_MULT;
    if (type == KNOB_TYPE_INT && accel > KNOB_ACCEL_MAX_MULT_INT)
        return KNOB_ACCEL_MAX_MULT_INT;
    return accel;
}

/* ---- Destination windows -------------------------------------------------
 *
 * A destination's lo/hi are fractions of its parameter's own range. These four
 * convert between the two, and they are the only place that arithmetic lives.
 */

/* Fraction (0..1 of the parameter's range) -> a value in the parameter's own
 * units, quantised the way that parameter is quantised. */
float knob_frac_to_value(float frac, const chain_param_info_t *pinfo) {
    if (!pinfo) return frac;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    float v = pinfo->min_val + frac * (pinfo->max_val - pinfo->min_val);
    if (pinfo->type == KNOB_TYPE_INT || pinfo->type == KNOB_TYPE_ENUM) {
        /* +0.5 before truncation: an enum sub-range must be able to REACH its
         * top option, and plain truncation leaves the last one unreachable
         * except at exactly 1.0. */
        v = (float)((int)(v + 0.5f));
    }
    if (v < pinfo->min_val) v = pinfo->min_val;
    if (v > pinfo->max_val) v = pinfo->max_val;
    return v;
}

/* The inverse. A degenerate range answers 0 rather than dividing by it. */
float knob_value_to_frac(float value, const chain_param_info_t *pinfo) {
    if (!pinfo) return 0.0f;
    float span = pinfo->max_val - pinfo->min_val;
    if (span <= 0.0f) return 0.0f;
    float f = (value - pinfo->min_val) / span;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

/* Where a destination sits when the knob is at `position`. lo > hi is an
 * inverted destination and needs no special case: the interpolation simply
 * runs backwards. */
float knob_dest_value_at(const knob_dest_t *d, float position,
                         const chain_param_info_t *pinfo) {
    if (!d) return 0.0f;
    if (position < 0.0f) position = 0.0f;
    if (position > 1.0f) position = 1.0f;
    return knob_frac_to_value(d->lo + position * (d->hi - d->lo), pinfo);
}

/* Clamp a value into a destination's window, in the parameter's own units.
 * This is the whole of what a range means for a SINGLE-destination knob: the
 * parameter keeps its own step and feel and is simply bounded. `lo > hi` names
 * the same window from the other end, so the bounds are ordered here. */
float knob_dest_clamp(const knob_dest_t *d, float value,
                      const chain_param_info_t *pinfo) {
    if (!d || !pinfo) return value;
    float a = knob_frac_to_value(d->lo, pinfo);
    float b = knob_frac_to_value(d->hi, pinfo);
    float lo = (a < b) ? a : b;
    float hi = (a < b) ? b : a;
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/* The base step for one detent: what the parameter declares, or a fallback.
 * `float_fallback` is a PARAMETER because the two knob paths disagree about it
 * -- chain_midi.c has always used KNOB_STEP_FLOAT and chain_host.c 0.01f, a
 * 6.7x difference on the same parameter. Passing it in keeps that divergence
 * visible at both call sites instead of quietly resolving it here; see the
 * note above chain_knob_accel. */
float knob_base_step(const chain_param_info_t *pinfo, float float_fallback) {
    if (!pinfo) return float_fallback;
    if (pinfo->step > 0) return pinfo->step;
    if (pinfo->type == KNOB_TYPE_INT || pinfo->type == KNOB_TYPE_ENUM)
        return (float)KNOB_STEP_INT;
    return float_fallback;
}

/*
 * Forward a formatted value string to the plugin identified by target.
 *
 * A knob turn is an EDIT of the parameter's RESTING value, exactly as an edit
 * from the parameter's own page is -- so it has to reach the modulation bus by
 * the same route. It did not, and the consequence was a knob that looked dead.
 *
 * A modulated parameter is not owned by whoever wrote it last. The bus holds a
 * base and rewrites `base + modulation` to the plugin on every tick
 * (chain_mod_apply_effective_value). The prefixed `synth:` / `fxN:` /
 * `midi_fxN:` set_param routes therefore update the base FIRST, so the wobble
 * follows the new setting. This function did not: it wrote straight through to
 * the plugin and told the bus nothing, so the next tick recomputed from the
 * stale base and erased the turn -- within milliseconds, every time. Assign a
 * knob to a parameter an LFO is driving and the knob reads as dead, or moves
 * and snaps back.
 *
 * REALTIME: the added call is the same one the prefixed routes already make
 * from this same thread, and chain_mod_apply_effective_value allocates nothing.
 */
void knob_forward_value(chain_instance_t *inst, const char *target, const char *param, const char *val_str) {
    if (!inst || !target || !param) return;

    if (chain_mod_is_target_active(inst, target, param)) {
        chain_mod_update_base_from_set_param(inst, target, param, val_str);
        mod_target_state_t *entry = chain_mod_find_target_entry(inst, target, param);
        if (entry) {
            chain_mod_apply_effective_value(inst, entry, 0);
            return;
        }
    }

    if (strcmp(target, "synth") == 0) {
        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->set_param)
            inst->synth_plugin_v2->set_param(inst->synth_instance, param, val_str);
        return;
    }

    int fx = chain_fx_index_from_id(target, "fx", MAX_AUDIO_FX);
    if (fx >= 0) {
        if (fx >= inst->fx_count) return;
        if (inst->fx_is_v2[fx] && inst->fx_plugins_v2[fx] && inst->fx_instances[fx] &&
            inst->fx_plugins_v2[fx]->set_param)
            inst->fx_plugins_v2[fx]->set_param(inst->fx_instances[fx], param, val_str);
        return;
    }

    int mfx = chain_fx_index_from_id(target, "midi_fx", MAX_MIDI_FX);
    if (mfx >= 0) {
        if (mfx >= inst->midi_fx_count) return;
        if (inst->midi_fx_plugins[mfx] && inst->midi_fx_instances[mfx] &&
            inst->midi_fx_plugins[mfx]->set_param)
            inst->midi_fx_plugins[mfx]->set_param(inst->midi_fx_instances[mfx], param, val_str);
        return;
    }
}


/* Format a value the way its parameter is spelled, and forward it. Kept
 * separate so the two branches of knob_turn cannot format differently. */
static void knob_format_and_forward(chain_instance_t *inst, const knob_dest_t *d,
                                    const chain_param_info_t *pinfo, float value) {
    char val_str[16];
    if (pinfo && (pinfo->type == KNOB_TYPE_INT || pinfo->type == KNOB_TYPE_ENUM))
        snprintf(val_str, sizeof(val_str), "%d", (int)value);
    else
        snprintf(val_str, sizeof(val_str), "%.3f", value);
    knob_forward_value(inst, d->target, d->param, val_str);
}

/* ---- Destination editing -------------------------------------------------
 *
 * One owner for every change to a knob's destination list, so the two things
 * that must happen alongside a change cannot be forgotten at a call site:
 * seeding the position when a knob first gains a second destination, and
 * re-writing the destinations when a window moves.
 */

/*
 * Find the knob mapping for a CC, creating one if there is room.
 * Returns NULL when the table is full.
 */
knob_mapping_t *knob_mapping_for_cc(chain_instance_t *inst, int cc, int create) {
    if (!inst) return NULL;
    for (int i = 0; i < inst->knob_mapping_count; i++)
        if (inst->knob_mappings[i].cc == cc) return &inst->knob_mappings[i];
    if (!create || inst->knob_mapping_count >= MAX_KNOB_MAPPINGS) return NULL;

    knob_mapping_t *km = &inst->knob_mappings[inst->knob_mapping_count++];
    memset(km, 0, sizeof(*km));
    km->cc = cc;
    km->last_cc_out = -1;
    return km;
}

/*
 * Seed the knob's position from its FIRST destination's live value.
 *
 * Called at the moment a knob gains a second destination. Without it the
 * position is wherever it was left -- 0 for a new mapping -- and the first
 * turn would yank every destination to the bottom of its window. Seeding from
 * destination 0 means nothing moves at the moment of adding.
 *
 * NOT called from a poll, and that is deliberate: re-deriving the position
 * from a destination's own quantisation grid between detents would stall a
 * slow turn on a coarse destination, which is the same arithmetic that keeps a
 * single destination off this path in the first place.
 */
void knob_seed_position(chain_instance_t *inst, knob_mapping_t *km) {
    if (!inst || !km || km->dest_count < 1) return;
    const knob_dest_t *d = &km->dests[0];
    chain_param_info_t *pinfo = knob_find_param(inst, d->target, d->param);
    if (!pinfo) return;

    float span = d->hi - d->lo;
    if (span == 0.0f) { km->position = 0.0f; return; }

    float pos = (knob_value_to_frac(d->current_value, pinfo) - d->lo) / span;
    if (pos < 0.0f) pos = 0.0f;
    if (pos > 1.0f) pos = 1.0f;
    km->position = pos;
}

/* A mapping's index in the table, or -1. The knob helpers take a pointer, but
 * the CC-out and position calls are indexed. */
int knob_mapping_index(chain_instance_t *inst, const knob_mapping_t *km) {
    if (!inst || !km) return -1;
    for (int i = 0; i < inst->knob_mapping_count; i++)
        if (&inst->knob_mappings[i] == km) return i;
    return -1;
}

/* Remove a mapping from the table, blanking the slot the shift vacates -- a
 * mapping added later lands on it and only writes its first destination. */
void knob_mapping_drop(chain_instance_t *inst, knob_mapping_t *km) {
    int i = knob_mapping_index(inst, km);
    if (i < 0) return;
    for (int j = i; j < inst->knob_mapping_count - 1; j++)
        inst->knob_mappings[j] = inst->knob_mappings[j + 1];
    inst->knob_mapping_count--;
    memset(&inst->knob_mappings[inst->knob_mapping_count], 0, sizeof(inst->knob_mappings[0]));
}

/* Point destination `di` (0-based) at a parameter, keeping its window -- a
 * window is a fraction of whatever parameter is there, so re-pointing is
 * exactly the case it was designed to survive. `di == dest_count` appends.
 * Returns 0 on success. */
int knob_dest_point(chain_instance_t *inst, knob_mapping_t *km, int di,
                    const char *target, const char *param) {
    if (!inst || !km || di < 0 || di >= MAX_KNOB_DESTS) return -1;
    if (di > km->dest_count) return -1;          /* no gaps */

    int appending = (di == km->dest_count);
    if (appending) {
        memset(&km->dests[di], 0, sizeof(km->dests[di]));
        km->dests[di].lo = 0.0f;
        km->dests[di].hi = 1.0f;
    }

    float lo = km->dests[di].lo, hi = km->dests[di].hi;
    knob_dest_assign(&km->dests[di], target, param);
    km->dests[di].lo = lo;
    km->dests[di].hi = hi;

    chain_param_info_t *pinfo = knob_find_param(inst, target, param);
    if (pinfo) {
        char val_buf[64];
        int got = -1;
        if (strcmp(target, "synth") == 0 && inst->synth_plugin_v2 && inst->synth_instance)
            got = inst->synth_plugin_v2->get_param(inst->synth_instance, param, val_buf, sizeof(val_buf));
        km->dests[di].current_value = (got > 0)
            ? dsp_value_to_float(val_buf, pinfo, pinfo->default_val)
            : pinfo->default_val;
    }

    if (appending) km->dest_count = di + 1;

    /* Crossing from one destination to two is where the position starts to
     * mean something. */
    if (km->dest_count == 2 && appending) knob_seed_position(inst, km);
    return 0;
}

/* Remove destination `di`, closing the gap. Returns the remaining count. */
int knob_dest_remove(chain_instance_t *inst, knob_mapping_t *km, int di) {
    (void)inst;
    if (!km || di < 0 || di >= km->dest_count) return km ? km->dest_count : 0;
    for (int j = di; j < km->dest_count - 1; j++) km->dests[j] = km->dests[j + 1];
    memset(&km->dests[km->dest_count - 1], 0, sizeof(km->dests[0]));
    km->dest_count--;
    return km->dest_count;
}

/*
 * Set destination `di`'s window and APPLY IT NOW.
 *
 * Applying immediately is the point: the window is being adjusted by ear, and
 * a range you cannot hear until the next turn is a range you are setting
 * blind. For a multi-destination knob that means re-deriving this destination
 * from the current position; for a single one it means clamping the value into
 * the new window, which can move the parameter -- deliberately, for the same
 * reason.
 */
void knob_dest_set_window(chain_instance_t *inst, knob_mapping_t *km, int di,
                          float lo, float hi) {
    if (!inst || !km || di < 0 || di >= km->dest_count) return;
    if (lo < 0.0f) lo = 0.0f;
    if (lo > 1.0f) lo = 1.0f;
    if (hi < 0.0f) hi = 0.0f;
    if (hi > 1.0f) hi = 1.0f;

    knob_dest_t *d = &km->dests[di];
    d->lo = lo;
    d->hi = hi;

    chain_param_info_t *pinfo = knob_find_param(inst, d->target, d->param);
    if (!pinfo) return;

    float nv = knob_is_multi(km) ? knob_dest_value_at(d, km->position, pinfo)
                                 : knob_dest_clamp(d, d->current_value, pinfo);
    if (nv == d->current_value) return;
    d->current_value = nv;
    knob_format_and_forward(inst, d, pinfo, nv);
}

/* ---- The turn -----------------------------------------------------------
 *
 * One place a chain knob becomes parameter writes, called by all three input
 * paths: the relative CC decode and the absolute CC in chain_midi.c, and
 * knob_N_adjust in chain_host.c (the device's own encoders).
 *
 * THE LINE IS "SEVERAL DESTINATIONS", NOT "HAS A RANGE", and the two branches
 * below are that line:
 *
 *   ONE destination, ranged or not -- the parameter's own step, its own
 *   acceleration, its own enum feel, and a clamp into the destination's
 *   window. The knob's `position` is not consulted and not stored.
 *
 *   SEVERAL destinations -- there is no single parameter to be, so the knob's
 *   own 0..1 position becomes the thing being turned, and each destination
 *   follows it through its own window.
 *
 * The asymmetry is not tidiness. Driving a LONE stepped parameter from a
 * position makes it crawl: an 8-option enum spans 7 units, so one detent of
 * KNOB_STEP_FLOAT moves it 0.0105 -- 4 -> 4.0105 -> (int) 4, the same value,
 * for ~95 detents per option instead of one. A parameter that shares a knob
 * with others pays that price by necessity; one that does not must never be
 * made to.
 *
 * REALTIME: fixed arrays, a stack buffer per write, no allocation, no logging.
 * A multi-destination turn issues up to MAX_KNOB_DESTS plugin writes instead of
 * one -- bounded, and the same shape the LFO tick already runs every block.
 */
void knob_turn(chain_instance_t *inst, int idx, int ticks, float float_fallback_step) {
    if (!inst || idx < 0 || idx >= MAX_KNOB_MAPPINGS) return;
    if (ticks == 0) return;

    knob_mapping_t *km = &inst->knob_mappings[idx];
    if (km->dest_count < 1) return;

    int mag = (ticks < 0) ? -ticks : ticks;
    int dir = (ticks < 0) ? -1 : 1;

    if (!knob_is_multi(km)) {
        knob_dest_t *d = &km->dests[0];
        chain_param_info_t *pinfo = knob_find_param(inst, d->target, d->param);
        if (!pinfo) return;

        int accel = chain_knob_accel_cap(chain_knob_accel(&inst->knob_last_time_ms[idx]),
                                         pinfo->type);
        int mult = relative_cc_multiplier(mag, accel);
        float delta = knob_base_step(pinfo, float_fallback_step) * (float)mult * (float)dir;

        float nv = d->current_value + delta;
        if (nv < pinfo->min_val) nv = pinfo->min_val;
        if (nv > pinfo->max_val) nv = pinfo->max_val;
        if (pinfo->type == KNOB_TYPE_INT || pinfo->type == KNOB_TYPE_ENUM)
            nv = (float)((int)nv);
        nv = knob_dest_clamp(d, nv, pinfo);
        d->current_value = nv;

        knob_format_and_forward(inst, d, pinfo, nv);
        return;
    }

    /* Several destinations: the position is what turns. The float ceiling
     * applies whatever the destinations are -- a stepped destination does not
     * get to slow down a knob it shares with a continuous one. */
    int accel = chain_knob_accel(&inst->knob_last_time_ms[idx]);
    int mult = relative_cc_multiplier(mag, accel);

    float pos = km->position + KNOB_STEP_FLOAT * (float)mult * (float)dir;
    if (pos < 0.0f) pos = 0.0f;
    if (pos > 1.0f) pos = 1.0f;
    km->position = pos;

    for (int i = 0; i < km->dest_count && i < MAX_KNOB_DESTS; i++) {
        knob_dest_t *d = &km->dests[i];
        if (!d->param[0]) continue;
        chain_param_info_t *pinfo = knob_find_param(inst, d->target, d->param);
        if (!pinfo) continue;   /* a destination whose module is gone is skipped,
                                 * not fatal to its siblings */
        float nv = knob_dest_value_at(d, pos, pinfo);
        d->current_value = nv;
        knob_format_and_forward(inst, d, pinfo, nv);
    }
}

/* ---- Chain-knob CC out ---------------------------------------------------
 *
 * The input half has shipped for a long time: CC 102-109 on a slot's receive
 * channel drive that slot's eight chain knobs (see chain_midi.c). Nothing went
 * the other way, so a motorised control surface went stale the moment a value
 * changed anywhere else — the Move's own encoder, or a patch load — and the
 * next touch of a knob jumped the parameter to wherever the stale knob sat.
 *
 * Opt-in per patch via knob_cc_out, off by default: a slot that is not driving
 * a control surface has no business adding eight CC streams to the external
 * port, where they would land on whatever else is plugged in.
 *
 * REALTIME: reached from set_param, on_midi and patch load, all of which ARE
 * the SPI callback. No allocation, no I/O, no locks — midi_send_external is a
 * lock-free ring enqueue that drops on full.
 */

/* Inverse of the inbound scaling in chain_midi.c. Returns -1 when the range is
 * degenerate or dynamic (max_val < 0), where there is no meaningful 0-127. */
/*
 * WHERE THE KNOB SITS ACROSS ITS OWN TRAVEL, as 0-127. One sentence, and
 * therefore one rule, for both kinds of knob:
 *
 *   several destinations -> the knob's own position, which is the only thing
 *                           they have in common;
 *   one destination      -> that parameter's value as a fraction of its own
 *                           WINDOW (not of the parameter's full range).
 *
 * With one destination at the whole-range default the second reduces to
 * (val - min) / (max - min), i.e. exactly knob_value_to_cc -- so nothing
 * changes for any knob that exists today, and test_chain_knob_cc_out's
 * inverse-scaling assertion passes untouched.
 *
 * Measuring a RANGED single destination against its window rather than the
 * full range is what keeps in and out symmetric. The alternative gives an
 * external fader dead zones at both ends, where its travel maps outside the
 * window and clamps, so a third of the throw does nothing.
 */
int knob_position_cc(chain_instance_t *inst, const knob_mapping_t *km) {
    if (!inst || !km || km->dest_count < 1) return -1;

    if (knob_is_multi(km)) {
        int cc_val = (int)(km->position * 127.0f + 0.5f);
        if (cc_val < 0) cc_val = 0;
        if (cc_val > 127) cc_val = 127;
        return cc_val;
    }

    const knob_dest_t *d = &km->dests[0];
    chain_param_info_t *pinfo = knob_find_param(inst, d->target, d->param);
    if (!pinfo) return -1;

    float lo = d->lo, hi = d->hi;
    float span = hi - lo;
    if (span == 0.0f) return -1;              /* a zero-width window has no position */

    float frac = (knob_value_to_frac(d->current_value, pinfo) - lo) / span;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return (int)(frac * 127.0f + 0.5f);
}

/*
 * The inbound half: put the knob AT a position, 0..1, and let every
 * destination follow. The same rule read backwards, so a controller that sends
 * back what it was told lands where it started.
 */
void knob_set_position(chain_instance_t *inst, int idx, float position) {
    if (!inst || idx < 0 || idx >= MAX_KNOB_MAPPINGS) return;
    knob_mapping_t *km = &inst->knob_mappings[idx];
    if (km->dest_count < 1) return;

    if (position < 0.0f) position = 0.0f;
    if (position > 1.0f) position = 1.0f;

    if (knob_is_multi(km)) km->position = position;

    for (int i = 0; i < km->dest_count && i < MAX_KNOB_DESTS; i++) {
        knob_dest_t *d = &km->dests[i];
        if (!d->param[0]) continue;
        chain_param_info_t *pinfo = knob_find_param(inst, d->target, d->param);
        if (!pinfo) continue;
        float nv = knob_dest_value_at(d, position, pinfo);
        d->current_value = nv;
        knob_format_and_forward(inst, d, pinfo, nv);
    }
}

static int knob_value_to_cc(float val, const chain_param_info_t *pinfo) {
    if (!pinfo) return -1;
    float span = pinfo->max_val - pinfo->min_val;
    if (!(span > 0.0f)) return -1;
    int cc_val = (int)(((val - pinfo->min_val) / span) * 127.0f + 0.5f);
    if (cc_val < 0) cc_val = 0;
    if (cc_val > 127) cc_val = 127;
    return cc_val;
}

void knob_emit_cc_out(chain_instance_t *inst, int idx) {
    if (!inst || !inst->knob_cc_out) return;
    if (idx < 0 || idx >= inst->knob_mapping_count) return;
    if (!inst->host || !inst->host->midi_send_external) return;

    knob_mapping_t *km = &inst->knob_mappings[idx];
    int knob = km->cc - KNOB_CC_START;              /* 0-7 */
    if (knob < 0 || knob > (KNOB_CC_END - KNOB_CC_START)) return;

    /* Answer on the channel the slot listens on, so in and out are symmetric.
     * -1 (All) gives us no channel to answer on and -2 means the instance is
     * not slot-registered (master FX); emit nothing rather than guess. */
    if (!inst->host->slot_recv_channel) return;
    int recv_ch = inst->host->slot_recv_channel((void *)inst);
    if (recv_ch < 0 || recv_ch > 15) return;

    int cc_val = knob_position_cc(inst, km);
    if (cc_val < 0) return;
    if (cc_val == km->last_cc_out) return;          /* change detection at CC resolution */

    /* USB-MIDI: cable 2 (external), CIN 0x0B (CC). */
    const uint8_t msg[4] = {
        (uint8_t)((2 << 4) | 0x0B),
        (uint8_t)(0xB0 | (uint8_t)recv_ch),
        (uint8_t)(KNOB_ABS_CC_START + knob),
        (uint8_t)cc_val
    };

    /* Record what the controller knows ONLY if it actually went out. The ring
     * drops-newest when full, and it shares the 20-slot-per-block mailbox
     * budget with LEDs, sequencer notes and JACK MIDI, so a drop under load is
     * expected rather than exceptional. Recording a dropped value would leave
     * last_cc_out claiming the controller is up to date; if the knob then
     * stopped moving, its motor would stay wrong indefinitely — the exact
     * staleness this feature exists to remove. Leaving it unrecorded makes the
     * next emit for this knob retry, so a drop self-heals. */
    if (inst->host->midi_send_external(msg, 4) > 0) {
        km->last_cc_out = cc_val;
    }
}

void knob_emit_cc_out_all(chain_instance_t *inst) {
    if (!inst || !inst->knob_cc_out) return;
    for (int i = 0; i < inst->knob_mapping_count; i++) {
        /* Force a send even when the quantised value is unchanged: after a
         * patch load the controller's motors reflect the previous patch, so
         * "same as last time we spoke" says nothing about where they sit. */
        inst->knob_mappings[i].last_cc_out = -1;
        knob_emit_cc_out(inst, i);
    }
}


float dsp_value_to_float(const char *val_str, chain_param_info_t *pinfo, float fallback) {
    char *endptr;
    float v = strtof(val_str, &endptr);
    if (endptr != val_str) {
        return v;  /* Parsed a number */
    }
    /* Non-numeric — try enum option lookup */
    if (pinfo && pinfo->type == KNOB_TYPE_ENUM) {
        for (int j = 0; j < pinfo->option_count; j++) {
            if (strcmp(val_str, pinfo->options[j]) == 0) {
                return (float)j;
            }
        }
    }
    return fallback;
}

/*
 * Refresh target parameter metadata from runtime plugin chain_params.
 * This allows modulation to resolve dynamic params that are not declared in
 * static module.json metadata.
 */

static int chain_param_key_matches(const char *requested_key, const char *meta_key) {
    if (!requested_key || !meta_key) return 0;
    if (strcmp(requested_key, meta_key) == 0) return 1;

    size_t req_len = strlen(requested_key);
    size_t meta_len = strlen(meta_key);
    if (req_len <= meta_len + 1) return 0;

    const char *suffix = requested_key + (req_len - meta_len);
    if (strcmp(suffix, meta_key) != 0) return 0;
    if (*(suffix - 1) != '_') return 0;

    /* Require strict "..._<index>_<meta_key>" shape for suffix fallback. */
    const char *idx_end = suffix - 1;  /* underscore before suffix */
    const char *idx_start = idx_end;
    while (idx_start > requested_key && *(idx_start - 1) != '_') {
        idx_start--;
    }
    if (idx_start <= requested_key || *(idx_start - 1) != '_') return 0;
    if (idx_start >= idx_end) return 0;

    for (const char *p = idx_start; p < idx_end; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }

    return 1;
}

/*
 * Find parameter metadata by target and key.
 */
chain_param_info_t* find_param_by_key(chain_instance_t *inst, const char *target, const char *key) {
    if (!inst || !target || !key || !key[0]) return NULL;

    if (strcmp(target, "synth") == 0) {
        for (int i = 0; i < inst->synth_param_count; i++) {
            if (chain_param_key_matches(key, inst->synth_params[i].key)) {
                return &inst->synth_params[i];
            }
        }
    } else if (strncmp(target, "fx", 2) == 0) {
        int fx_slot = atoi(target + 2) - 1;
        if (fx_slot >= 0 && fx_slot < MAX_AUDIO_FX) {
            for (int i = 0; i < inst->fx_param_counts[fx_slot]; i++) {
                if (chain_param_key_matches(key, inst->fx_params[fx_slot][i].key)) {
                    return &inst->fx_params[fx_slot][i];
                }
            }
        }
    } else if (strncmp(target, "midi_fx", 7) == 0) {
        int midi_fx_slot = 0;  /* Default to slot 0 */
        if (target[7] != '\0') {
            /* Parse midi_fx1, midi_fx2, etc. */
            midi_fx_slot = atoi(target + 7) - 1;
        }
        if (midi_fx_slot >= 0 && midi_fx_slot < MAX_MIDI_FX) {
            for (int i = 0; i < inst->midi_fx_param_counts[midi_fx_slot]; i++) {
                if (chain_param_key_matches(key, inst->midi_fx_params[midi_fx_slot][i].key)) {
                    return &inst->midi_fx_params[midi_fx_slot][i];
                }
            }
        }
    }

    /* Static metadata missed. Retry with runtime chain_params refresh, throttled
     * to avoid re-parsing dynamic params every tick when a mapping is stale. */
    uint64_t now_ms = get_time_ms();
    uint64_t *last_refresh_ms = NULL;
    if (strcmp(target, "synth") == 0) {
        last_refresh_ms = &inst->mod_param_refresh_ms_synth;
    } else if (strncmp(target, "fx", 2) == 0) {
        int fx_slot = atoi(target + 2) - 1;
        if (fx_slot >= 0 && fx_slot < MAX_AUDIO_FX) {
            last_refresh_ms = &inst->mod_param_refresh_ms_fx[fx_slot];
        }
    } else if (strncmp(target, "midi_fx", 7) == 0) {
        int midi_fx_slot = 0;
        if (target[7] != '\0') {
            midi_fx_slot = atoi(target + 7) - 1;
        }
        if (midi_fx_slot >= 0 && midi_fx_slot < MAX_MIDI_FX) {
            last_refresh_ms = &inst->mod_param_refresh_ms_midi_fx[midi_fx_slot];
        }
    }

    if (!last_refresh_ms) return NULL;
    if (*last_refresh_ms > 0 && (now_ms - *last_refresh_ms) < MOD_PARAM_CACHE_REFRESH_MS) {
        return NULL;
    }
    *last_refresh_ms = now_ms;

    if (chain_mod_refresh_target_param_cache(inst, target) <= 0) {
        return NULL;
    }

    if (strcmp(target, "synth") == 0) {
        for (int i = 0; i < inst->synth_param_count; i++) {
            if (chain_param_key_matches(key, inst->synth_params[i].key)) {
                return &inst->synth_params[i];
            }
        }
    } else if (strncmp(target, "fx", 2) == 0) {
        int fx_slot = atoi(target + 2) - 1;
        if (fx_slot >= 0 && fx_slot < MAX_AUDIO_FX) {
            for (int i = 0; i < inst->fx_param_counts[fx_slot]; i++) {
                if (chain_param_key_matches(key, inst->fx_params[fx_slot][i].key)) {
                    return &inst->fx_params[fx_slot][i];
                }
            }
        }
    } else if (strncmp(target, "midi_fx", 7) == 0) {
        int midi_fx_slot = 0;
        if (target[7] != '\0') {
            midi_fx_slot = atoi(target + 7) - 1;
        }
        if (midi_fx_slot >= 0 && midi_fx_slot < MAX_MIDI_FX) {
            for (int i = 0; i < inst->midi_fx_param_counts[midi_fx_slot]; i++) {
                if (chain_param_key_matches(key, inst->midi_fx_params[midi_fx_slot][i].key)) {
                    return &inst->midi_fx_params[midi_fx_slot][i];
                }
            }
        }
    }

    return NULL;
}

/* V2 get_param handler */

