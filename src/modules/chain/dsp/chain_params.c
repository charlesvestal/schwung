/*
 * Signal Chain — parameter metadata, parsing, smoothing, knob mapping.
 * Split from chain_host.c (2026-06 cleanup step 10); pure relocation,
 * no behavior change. Shared types/decls live in chain_internal.h.
 */

#include "chain_internal.h"

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

    /* Extract max_param (dynamic max reference) */
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
                    param->max_val = -1; /* Marker for dynamic max */
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

        /* Parse max_param (dynamic max) */
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
                    p->max_val = -1.0f;  /* Marker for dynamic max */
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

/* Look up param metadata from a knob mapping's target string (synth/fx1-3/midi_fx1-2). */
chain_param_info_t *knob_find_param(chain_instance_t *inst, const char *target, const char *param) {
    if (strcmp(target, "synth") == 0)
        return find_param_info(inst->synth_params, inst->synth_param_count, param);
    if (strcmp(target, "fx1") == 0 && inst->fx_count > 0)
        return find_param_info(inst->fx_params[0], inst->fx_param_counts[0], param);
    if (strcmp(target, "fx2") == 0 && inst->fx_count > 1)
        return find_param_info(inst->fx_params[1], inst->fx_param_counts[1], param);
    if (strcmp(target, "fx3") == 0 && inst->fx_count > 2)
        return find_param_info(inst->fx_params[2], inst->fx_param_counts[2], param);
    if (strcmp(target, "midi_fx1") == 0 && inst->midi_fx_count > 0)
        return find_param_info(inst->midi_fx_params[0], inst->midi_fx_param_counts[0], param);
    if (strcmp(target, "midi_fx2") == 0 && inst->midi_fx_count > 1)
        return find_param_info(inst->midi_fx_params[1], inst->midi_fx_param_counts[1], param);
    return NULL;
}

/* Forward a formatted value string to the plugin identified by target. */
void knob_forward_value(chain_instance_t *inst, const char *target, const char *param, const char *val_str) {
    if (strcmp(target, "synth") == 0) {
        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->set_param)
            inst->synth_plugin_v2->set_param(inst->synth_instance, param, val_str);
    } else if (strcmp(target, "fx1") == 0 && inst->fx_count > 0) {
        if (inst->fx_is_v2[0] && inst->fx_plugins_v2[0] && inst->fx_instances[0])
            inst->fx_plugins_v2[0]->set_param(inst->fx_instances[0], param, val_str);
    } else if (strcmp(target, "fx2") == 0 && inst->fx_count > 1) {
        if (inst->fx_is_v2[1] && inst->fx_plugins_v2[1] && inst->fx_instances[1])
            inst->fx_plugins_v2[1]->set_param(inst->fx_instances[1], param, val_str);
    } else if (strcmp(target, "fx3") == 0 && inst->fx_count > 2) {
        if (inst->fx_is_v2[2] && inst->fx_plugins_v2[2] && inst->fx_instances[2])
            inst->fx_plugins_v2[2]->set_param(inst->fx_instances[2], param, val_str);
    } else if (strcmp(target, "midi_fx1") == 0 && inst->midi_fx_count > 0) {
        if (inst->midi_fx_plugins[0] && inst->midi_fx_instances[0] && inst->midi_fx_plugins[0]->set_param)
            inst->midi_fx_plugins[0]->set_param(inst->midi_fx_instances[0], param, val_str);
    } else if (strcmp(target, "midi_fx2") == 0 && inst->midi_fx_count > 1) {
        if (inst->midi_fx_plugins[1] && inst->midi_fx_instances[1] && inst->midi_fx_plugins[1]->set_param)
            inst->midi_fx_plugins[1]->set_param(inst->midi_fx_instances[1], param, val_str);
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

