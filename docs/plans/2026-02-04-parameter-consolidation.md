# Parameter Consolidation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Consolidate parameter metadata into ui_hierarchy as single source of truth, eliminating chain_params duplication and adding proper enum support with units.

**Architecture:** Extend chain_param_info_t struct with unit/format/step fields, add KNOB_TYPE_ENUM, update parser to read from ui_hierarchy (with shared_params support for multi-mode modules), simplify knob_mapping_t to reference-only, centralize value formatting.

**Tech Stack:** C (chain_host.c parser), JSON schema (module.json), cross-repo migration (19 module repos)

---

## Phase 1: Core Infrastructure

### Task 1: Extend Type System

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c:108-139`

**Step 1: Add KNOB_TYPE_ENUM to knob_type_t**

Locate the enum definition around line 108:

```c
typedef enum {
    KNOB_TYPE_FLOAT = 0,
    KNOB_TYPE_INT = 1,
    KNOB_TYPE_ENUM = 2
} knob_type_t;
```

**Step 2: Update chain_param_info_t struct**

Locate the struct around line 128 and modify:

```c
typedef struct {
    char key[32];           /* Parameter key (e.g., "preset", "decay") */
    char name[64];          /* Display name (increased from 32) */
    knob_type_t type;       /* float, int, or enum */
    float min_val;          /* Minimum value */
    float max_val;          /* Maximum value (or -1 if dynamic via max_param) */
    float default_val;      /* Default value */
    char max_param[32];     /* Dynamic max param key (e.g., "preset_count") */
    char unit[16];          /* NEW: Display unit ("Hz", "dB", "ms", "%") */
    char display_format[16]; /* NEW: Printf format ("%.0f", "%.1f", "%.2f") */
    float step;             /* NEW: Edit step size */
    char options[MAX_ENUM_OPTIONS][32];  /* Enum options (if type is "enum") */
    int option_count;       /* Number of enum options */
} chain_param_info_t;
```

**Step 3: Search for type_str references and remove**

Search for `type_str` in chain_host.c and remove all references (it was redundant with the `type` enum field).

**Step 4: Build and verify no compilation errors**

```bash
./scripts/build.sh
```

Expected: Clean build (may have warnings about unused fields, that's ok)

**Step 5: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "Add KNOB_TYPE_ENUM and extend chain_param_info_t

- Add KNOB_TYPE_ENUM to support proper enum parameters
- Add unit, display_format, step fields to chain_param_info_t
- Increase name field from 32 to 64 chars
- Remove redundant type_str field

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 2: Centralized Value Formatting

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c` (add new function after struct definitions)

**Step 1: Add format_param_value function**

Add this function after the chain_param_info_t definition (around line 155):

```c
/*
 * Format a parameter value for display based on its metadata.
 * Returns length of formatted string, or -1 on error.
 */
static int format_param_value(chain_param_info_t *param, float value, char *buf, int buf_len) {
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

    /* Format numeric value */
    char val_str[32];
    if (param->display_format[0]) {
        /* Use custom format */
        snprintf(val_str, sizeof(val_str), param->display_format, value);
    } else {
        /* Use defaults based on type */
        if (param->type == KNOB_TYPE_FLOAT) {
            snprintf(val_str, sizeof(val_str), "%.2f", value);
        } else {
            snprintf(val_str, sizeof(val_str), "%d", (int)value);
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
```

**Step 2: Build to verify syntax**

```bash
./scripts/build.sh
```

Expected: Clean build (function unused warning is ok)

**Step 3: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "Add centralized parameter value formatting

Adds format_param_value() to format parameter values consistently
based on type, display_format, and unit metadata.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 3: Update Parser to Read ui_hierarchy

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c` (parse_chain_params function around line 1404)

**Step 1: Add helper to check if JSON value is object vs string**

Add before parse_chain_params function:

```c
/*
 * Check if a JSON value is an object (starts with '{') vs string/primitive
 */
static int json_value_is_object(const char *val) {
    while (*val == ' ' || *val == '\t' || *val == '\n') val++;
    return *val == '{';
}

/*
 * Check if JSON object has a specific key
 */
static int json_object_has_key(const char *obj, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    return strstr(obj, search) != NULL;
}
```

**Step 2: Rename old function and create new entry point**

Find the existing parse_chain_params function (around line 1404) and rename it to `parse_chain_params_legacy`:

```c
static int parse_chain_params_legacy(const char *json, chain_param_info_t *out_params, int max_params) {
    /* Keep existing implementation unchanged for now */
    /* ... existing code ... */
}
```

**Step 3: Create new parse_chain_params that tries ui_hierarchy first**

Add new function:

```c
/*
 * Parse parameter definitions from module.json.
 * First tries ui_hierarchy (new format), falls back to chain_params (legacy).
 */
static int parse_chain_params(const char *json, chain_param_info_t *out_params, int max_params) {
    /* Try ui_hierarchy first */
    const char *hierarchy = strstr(json, "\"ui_hierarchy\"");
    if (hierarchy) {
        return parse_hierarchy_params(json, out_params, max_params);
    }

    /* Fall back to legacy chain_params */
    return parse_chain_params_legacy(json, out_params, max_params);
}
```

**Step 4: Implement parse_hierarchy_params skeleton**

Add before parse_chain_params:

```c
/*
 * Parse parameters from ui_hierarchy structure.
 * Extracts param definitions from shared_params and all levels.
 */
static int parse_hierarchy_params(const char *json, chain_param_info_t *out_params, int max_params) {
    int param_count = 0;

    /* Find ui_hierarchy section */
    const char *hierarchy = strstr(json, "\"ui_hierarchy\"");
    if (!hierarchy) return 0;

    /* TODO: Parse shared_params if present */
    /* TODO: Parse params from all levels */
    /* TODO: Validate no duplicate keys */

    return param_count;
}
```

**Step 5: Build to verify structure compiles**

```bash
./scripts/build.sh
```

Expected: Clean build

**Step 6: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "Refactor parser to support ui_hierarchy

- Rename parse_chain_params to parse_chain_params_legacy
- Add parse_hierarchy_params skeleton
- Update parse_chain_params to try hierarchy first, fall back to legacy
- Add JSON helper functions

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 4: Implement Hierarchy Parser - shared_params

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c:parse_hierarchy_params`

**Step 1: Add helper to parse single param object**

Add before parse_hierarchy_params:

```c
/*
 * Parse a single parameter definition object into chain_param_info_t.
 * Returns 0 on success, -1 on error.
 */
static int parse_param_object(const char *param_json, chain_param_info_t *param) {
    memset(param, 0, sizeof(chain_param_info_t));

    /* Extract key (required) */
    const char *key_start = strstr(param_json, "\"key\"");
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
    const char *label_start = strstr(param_json, "\"label\"");
    if (!label_start) label_start = strstr(param_json, "\"name\"");
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
    const char *type_start = strstr(param_json, "\"type\"");
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
    const char *min_start = strstr(param_json, "\"min\"");
    if (min_start) {
        min_start = strchr(min_start, ':');
        if (min_start) {
            param->min_val = atof(min_start + 1);
        }
    }

    /* Extract max (optional for enum) */
    const char *max_start = strstr(param_json, "\"max\"");
    if (max_start) {
        max_start = strchr(max_start, ':');
        if (max_start) {
            param->max_val = atof(max_start + 1);
        }
    }

    /* Extract default (optional) */
    const char *default_start = strstr(param_json, "\"default\"");
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
    const char *step_start = strstr(param_json, "\"step\"");
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
    const char *unit_start = strstr(param_json, "\"unit\"");
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
    const char *format_start = strstr(param_json, "\"display_format\"");
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
        const char *options_start = strstr(param_json, "\"options\"");
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
    }

    /* Extract max_param (dynamic max reference) */
    const char *max_param_start = strstr(param_json, "\"max_param\"");
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
```

**Step 2: Implement shared_params parsing in parse_hierarchy_params**

Update parse_hierarchy_params:

```c
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

    /* TODO: Parse params from levels */

    return param_count;
}
```

**Step 3: Build to verify**

```bash
./scripts/build.sh
```

Expected: Clean build

**Step 4: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "Implement shared_params parsing

- Add parse_param_object helper to extract param metadata from JSON
- Parse shared_params array in ui_hierarchy
- Extract key, label, type, min, max, default, step, unit, display_format
- Parse options array for enums

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 5: Implement Hierarchy Parser - levels params

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c:parse_hierarchy_params`

**Step 1: Add helper to recursively parse level params**

Add before parse_hierarchy_params:

```c
/*
 * Parse params array from a single level.
 * Recursively processes nested levels if needed.
 */
static int parse_level_params(const char *level_json, chain_param_info_t *out_params, int *param_count, int max_params) {
    /* Find params array in this level */
    const char *params = strstr(level_json, "\"params\"");
    if (!params) return 0;

    params = strchr(params, '[');
    if (!params) return 0;
    params++;

    /* Iterate through params array */
    const char *param_start = params;
    while (*param_count < max_params) {
        /* Skip whitespace */
        while (*param_start == ' ' || *param_start == '\t' || *param_start == '\n') param_start++;

        /* Check for end of array */
        if (*param_start == ']') break;

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

            /* Check if this is a navigation item (has "level" key) or param definition (has "type" key) */
            if (json_object_has_key(param_start, "type")) {
                /* This is a param definition - parse it */
                if (parse_param_object(param_start, &out_params[*param_count]) == 0) {
                    (*param_count)++;
                }
            }
            /* Skip navigation items (they don't define params) */

            param_start = param_end;
        } else if (*param_start == '"') {
            /* String reference - skip (already defined elsewhere) */
            param_start = strchr(param_start + 1, '"');
            if (param_start) param_start++;
        }

        /* Skip comma */
        param_start = strchr(param_start, ',');
        if (!param_start) break;
        param_start++;
    }

    return 0;
}
```

**Step 2: Update parse_hierarchy_params to parse all levels**

Add after shared_params parsing:

```c
    /* Parse params from all levels */
    const char *levels = strstr(hierarchy, "\"levels\"");
    if (levels) {
        levels = strchr(levels, '{');
        if (levels) {
            levels++;

            /* Iterate through each level object */
            const char *level_start = levels;
            while (param_count < max_params) {
                /* Skip to next level definition */
                level_start = strchr(level_start, '{');
                if (!level_start) break;

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

                /* Check if we've reached end of levels object */
                level_start = level_end;
                if (*level_start == '}') break;
            }
        }
    }

    return param_count;
```

**Step 3: Build to verify**

```bash
./scripts/build.sh
```

Expected: Clean build

**Step 4: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "Implement levels params parsing

- Add parse_level_params to extract params from hierarchy levels
- Recursively process all levels in ui_hierarchy
- Skip navigation items (objects with 'level' key)
- Skip string references (already defined elsewhere)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 6: Add Duplicate Key Validation

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c:parse_hierarchy_params`

**Step 1: Add validation at end of parse_hierarchy_params**

Add before the final return statement:

```c
    /* Validate no duplicate keys */
    for (int i = 0; i < param_count; i++) {
        for (int j = i + 1; j < param_count; j++) {
            if (strcmp(out_params[i].key, out_params[j].key) == 0) {
                plugin_log("ERROR: Duplicate parameter key '%s' in ui_hierarchy", out_params[i].key);
                return -1; /* Signal error */
            }
        }
    }

    return param_count;
```

**Step 2: Update callers to check for negative return value**

Find all calls to parse_chain_params and add error handling. Search for `parse_chain_params(` in chain_host.c and add checks like:

```c
int count = parse_chain_params(json, params_array, max_count);
if (count < 0) {
    plugin_log("ERROR: Failed to parse parameters");
    /* Handle error appropriately (skip module load, etc.) */
}
```

**Step 3: Build to verify**

```bash
./scripts/build.sh
```

Expected: Clean build

**Step 4: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "Add duplicate key validation to hierarchy parser

- Validate param keys are unique across all levels
- Return -1 on duplicate key error
- Add error logging for duplicate keys

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 7: Simplify knob_mapping_t

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c` (knob_mapping_t struct around line 115)

**Step 1: Update knob_mapping_t struct**

Find and modify:

```c
typedef struct {
    int cc;              /* CC number (71-78 for knobs 1-8) */
    char target[16];     /* Component: "synth", "fx1", "fx2", "midi_fx" */
    char param[32];      /* Parameter key (lookup metadata in chain_params) */
    float current_value; /* Current value only */
} knob_mapping_t;
```

**Step 2: Update knob mapping load/save code**

Search for code that reads/writes knob_mappings from JSON patches. Look for patterns like:

```c
/* OLD CODE that reads type/min/max: */
sscanf(json, "\"type\":\"%15[^\"]\"", mapping->type_str);
sscanf(json, "\"min\":%f", &mapping->min_val);
sscanf(json, "\"max\":%f", &mapping->max_val);
```

Remove these lines - they're no longer needed.

**Step 3: Update knob CC processing code**

Find the code that handles knob CC input (search for `CC 71` or `handle_knob_cc`). Update to look up param metadata:

```c
/* When knob CC arrives */
knob_mapping_t *mapping = &inst->knob_mappings[knob_index];
if (!mapping->param[0]) return; /* No mapping */

/* Look up param metadata */
chain_param_info_t *param = NULL;
if (strcmp(mapping->target, "synth") == 0) {
    /* Search synth params */
    for (int i = 0; i < inst->synth_param_count; i++) {
        if (strcmp(inst->synth_params[i].key, mapping->param) == 0) {
            param = &inst->synth_params[i];
            break;
        }
    }
} else if (strncmp(mapping->target, "fx", 2) == 0) {
    /* Parse fx slot number */
    int fx_slot = atoi(mapping->target + 2) - 1;
    if (fx_slot >= 0 && fx_slot < MAX_FX_SLOTS) {
        /* Search fx params */
        for (int i = 0; i < inst->fx_param_count[fx_slot]; i++) {
            if (strcmp(inst->fx_params[fx_slot][i].key, mapping->param) == 0) {
                param = &inst->fx_params[fx_slot][i];
                break;
            }
        }
    }
}

if (!param) return; /* Param not found */

/* Use param->step, param->min_val, param->max_val for CC processing */
```

**Step 4: Build to verify**

```bash
./scripts/build.sh
```

Expected: Build may have errors in knob processing code - note them for next fix

**Step 5: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "Simplify knob_mapping_t to reference-only

- Remove type, min_val, max_val from knob_mapping_t
- Keep only cc, target, param, current_value
- Update knob processing to look up param metadata dynamically

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Phase 2: Use Formatting in Display Code

### Task 8: Update Knob Overlay Display

**Files:**
- Modify: `src/move_anything_shim.c` (knob overlay code around line 1354-1747)

**Step 1: Find knob overlay value formatting code**

Search for `knob_N_value` queries in move_anything_shim.c. You'll find code that queries the DSP for formatted values.

**Step 2: Update chain_host.c knob value response**

In chain_host.c, find the get_param handler for "knob_N_value" (around line 5461-5496). Update to use format_param_value:

```c
/* Handle knob_N_value queries */
if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_value")) {
    int knob_idx = atoi(key + 5) - 1;
    if (knob_idx >= 0 && knob_idx < MAX_KNOB_MAPPINGS) {
        knob_mapping_t *mapping = &inst->knob_mappings[knob_idx];
        if (mapping->param[0]) {
            /* Look up param metadata */
            chain_param_info_t *param = find_param_by_key(inst, mapping->target, mapping->param);
            if (param) {
                /* Use centralized formatting */
                return format_param_value(param, mapping->current_value, buf, buf_len);
            }
        }
    }
    return 0;
}
```

**Step 3: Add find_param_by_key helper**

Add before the get_param handler:

```c
/*
 * Find parameter metadata by target and key.
 */
static chain_param_info_t* find_param_by_key(chain_instance_t *inst, const char *target, const char *key) {
    if (strcmp(target, "synth") == 0) {
        for (int i = 0; i < inst->synth_param_count; i++) {
            if (strcmp(inst->synth_params[i].key, key) == 0) {
                return &inst->synth_params[i];
            }
        }
    } else if (strncmp(target, "fx", 2) == 0) {
        int fx_slot = atoi(target + 2) - 1;
        if (fx_slot >= 0 && fx_slot < MAX_FX_SLOTS) {
            for (int i = 0; i < inst->fx_param_count[fx_slot]; i++) {
                if (strcmp(inst->fx_params[fx_slot][i].key, key) == 0) {
                    return &inst->fx_params[fx_slot][i];
                }
            }
        }
    } else if (strcmp(target, "midi_fx") == 0) {
        for (int i = 0; i < inst->midi_fx_param_count; i++) {
            if (strcmp(inst->midi_fx_params[i].key, key) == 0) {
                return &inst->midi_fx_params[i];
            }
        }
    }
    return NULL;
}
```

**Step 4: Build and test**

```bash
./scripts/build.sh
./scripts/install.sh local
```

Test: Load a module with knobs, press Shift+Knob in Move mode, verify units appear

Expected: Knob overlay shows values with units (once modules are updated)

**Step 5: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "Use centralized formatting in knob overlay

- Add find_param_by_key helper to look up param metadata
- Update knob_N_value handler to use format_param_value
- Display knob values with units and proper formatting

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Phase 3: Module Migration

### Task 9: Migrate Built-in Freeverb Module

**Files:**
- Modify: `src/modules/audio_fx/freeverb/module.json`
- Modify: `src/modules/audio_fx/freeverb/freeverb.c` (remove chain_params return)

**Step 1: Read current module.json**

```bash
cat src/modules/audio_fx/freeverb/module.json
```

Note the current chain_params and ui_hierarchy structure.

**Step 2: Update module.json**

Replace chain_params with extended ui_hierarchy:

```json
{
  "id": "freeverb",
  "name": "Freeverb",
  "description": "Classic Freeverb reverb algorithm",
  "dsp": "freeverb.so",
  "api_version": 1,
  "capabilities": {
    "chainable": true,
    "component_type": "audio_fx",
    "ui_hierarchy": {
      "levels": {
        "root": {
          "label": "Freeverb",
          "params": [
            {
              "key": "room_size",
              "label": "Room Size",
              "type": "float",
              "min": 0,
              "max": 1,
              "default": 0.5,
              "step": 0.01,
              "unit": "%",
              "display_format": "%.0f"
            },
            {
              "key": "damping",
              "label": "Damping",
              "type": "float",
              "min": 0,
              "max": 1,
              "default": 0.5,
              "step": 0.01,
              "unit": "%",
              "display_format": "%.0f"
            },
            {
              "key": "wet",
              "label": "Wet",
              "type": "float",
              "min": 0,
              "max": 1,
              "default": 0.33,
              "step": 0.01,
              "unit": "%",
              "display_format": "%.0f"
            },
            {
              "key": "dry",
              "label": "Dry",
              "type": "float",
              "min": 0,
              "max": 1,
              "default": 1,
              "step": 0.01,
              "unit": "%",
              "display_format": "%.0f"
            },
            {
              "key": "width",
              "label": "Width",
              "type": "float",
              "min": 0,
              "max": 1,
              "default": 1,
              "step": 0.01,
              "unit": "%",
              "display_format": "%.0f"
            }
          ],
          "knobs": ["room_size", "damping", "wet", "dry", "width"]
        }
      }
    }
  }
}
```

**Step 3: Remove chain_params from freeverb.c**

Open `src/modules/audio_fx/freeverb/freeverb.c` and find the get_param handler (around line 315). Remove the chain_params case:

```c
/* DELETE THIS BLOCK: */
if (strcmp(key, "chain_params") == 0) {
    const char *params = "["
        "{\"key\":\"room_size\",\"name\":\"Room Size\",\"type\":\"float\",\"min\":0,\"max\":1},"
        /* ... rest of chain_params ... */
    "]";
    /* ... */
}
```

**Step 4: Build and test**

```bash
./scripts/build.sh
./scripts/install.sh local
```

Test: Load freeverb in Signal Chain, verify params appear correctly, check knob overlay shows units

**Step 5: Commit**

```bash
git add src/modules/audio_fx/freeverb/module.json src/modules/audio_fx/freeverb/freeverb.c
git commit -m "Migrate freeverb to ui_hierarchy params

- Move chain_params into ui_hierarchy with full metadata
- Add units (%) and display_format to all params
- Remove duplicate chain_params from freeverb.c DSP code

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 10: Migrate Built-in linein Module

**Files:**
- Modify: `src/modules/sound_generators/linein/module.json`

**Step 1: Update module.json**

```json
{
  "id": "linein",
  "name": "Line In",
  "description": "Pass through audio from line input",
  "api_version": 1,
  "capabilities": {
    "audio_in": true,
    "audio_out": true,
    "chainable": true,
    "component_type": "sound_generator",
    "ui_hierarchy": {
      "levels": {
        "root": {
          "label": "Line In",
          "params": [
            {
              "key": "gain",
              "label": "Gain",
              "type": "float",
              "min": 0,
              "max": 2,
              "default": 1.0,
              "step": 0.01
            }
          ],
          "knobs": ["gain"]
        }
      }
    }
  }
}
```

**Step 2: Build and test**

```bash
./scripts/build.sh
./scripts/install.sh local
```

**Step 3: Commit**

```bash
git add src/modules/sound_generators/linein/module.json
git commit -m "Migrate linein to ui_hierarchy params

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 11: Migrate Built-in MIDI FX Modules

**Files:**
- Modify: `src/modules/midi_fx/arp/module.json`
- Modify: `src/modules/midi_fx/chord/module.json`

**Step 1: Update arp module.json**

Replace chain_params with ui_hierarchy including proper enum with options:

```json
{
  "id": "arp",
  "name": "Arpeggiator",
  "description": "MIDI arpeggiator",
  "dsp": "arp.so",
  "api_version": 1,
  "capabilities": {
    "chainable": true,
    "component_type": "midi_fx",
    "ui_hierarchy": {
      "levels": {
        "root": {
          "label": "Arpeggiator",
          "params": [
            {
              "key": "mode",
              "label": "Mode",
              "type": "enum",
              "options": ["Up", "Down", "Up/Down", "Random"],
              "default": 0
            },
            {
              "key": "rate",
              "label": "Rate",
              "type": "int",
              "min": 1,
              "max": 16,
              "default": 8,
              "step": 1,
              "unit": "steps"
            },
            {
              "key": "octaves",
              "label": "Octaves",
              "type": "int",
              "min": 1,
              "max": 4,
              "default": 1,
              "step": 1
            },
            {
              "key": "gate",
              "label": "Gate",
              "type": "float",
              "min": 0,
              "max": 1,
              "default": 0.8,
              "step": 0.01,
              "unit": "%",
              "display_format": "%.0f"
            }
          ],
          "knobs": ["mode", "rate", "octaves", "gate"]
        }
      }
    }
  }
}
```

**Step 2: Update chord module.json**

```json
{
  "id": "chord",
  "name": "Chord",
  "description": "MIDI chord generator",
  "dsp": "chord.so",
  "api_version": 1,
  "capabilities": {
    "chainable": true,
    "component_type": "midi_fx",
    "ui_hierarchy": {
      "levels": {
        "root": {
          "label": "Chord",
          "params": [
            {
              "key": "chord_type",
              "label": "Type",
              "type": "enum",
              "options": ["Major", "Minor", "Dim", "Aug", "Sus2", "Sus4", "Maj7", "Min7", "Dom7"],
              "default": 0
            },
            {
              "key": "inversion",
              "label": "Inversion",
              "type": "int",
              "min": 0,
              "max": 3,
              "default": 0,
              "step": 1
            },
            {
              "key": "voicing",
              "label": "Voicing",
              "type": "enum",
              "options": ["Close", "Open", "Drop2", "Drop3"],
              "default": 0
            },
            {
              "key": "velocity_scale",
              "label": "Velocity",
              "type": "float",
              "min": 0,
              "max": 1,
              "default": 1.0,
              "step": 0.01,
              "unit": "%",
              "display_format": "%.0f"
            },
            {
              "key": "octave_spread",
              "label": "Spread",
              "type": "int",
              "min": 0,
              "max": 2,
              "default": 0,
              "step": 1,
              "unit": "oct"
            }
          ],
          "knobs": ["chord_type", "inversion", "voicing", "velocity_scale"]
        }
      }
    }
  }
}
```

**Step 3: Build and test**

```bash
./scripts/build.sh
./scripts/install.sh local
```

Test both arp and chord in Signal Chain

**Step 4: Commit**

```bash
git add src/modules/midi_fx/arp/module.json src/modules/midi_fx/chord/module.json
git commit -m "Migrate MIDI FX modules to ui_hierarchy params

- Migrate arp and chord modules
- Add proper enum support with options arrays
- Add units and display formatting

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 12: Create Migration Script for External Modules

**Files:**
- Create: `scripts/migrate-module-params.py`

**Step 1: Create Python migration script**

```python
#!/usr/bin/env python3
"""
Migrate module.json from chain_params to ui_hierarchy format.

Usage: ./scripts/migrate-module-params.py path/to/module.json
"""

import json
import sys
from pathlib import Path

def migrate_module(module_path):
    with open(module_path, 'r') as f:
        module = json.load(f)

    caps = module.get('capabilities', {})
    chain_params = caps.get('chain_params', [])
    ui_hierarchy = caps.get('ui_hierarchy', {})

    if not chain_params:
        print(f"No chain_params found in {module_path}")
        return False

    # Get existing hierarchy or create new
    levels = ui_hierarchy.get('levels', {})
    root = levels.get('root', {})

    # Convert chain_params to full param objects
    new_params = []
    for param in chain_params:
        new_param = {
            'key': param['key'],
            'label': param.get('name', param['key']),
            'type': param['type']
        }

        if 'min' in param:
            new_param['min'] = param['min']
        if 'max' in param:
            new_param['max'] = param['max']
        if 'max_param' in param:
            new_param['max_param'] = param['max_param']
        if 'default' in param:
            new_param['default'] = param['default']
        if 'step' in param:
            new_param['step'] = param['step']
        if 'options' in param:
            new_param['options'] = param['options']

        new_params.append(new_param)

    # Update or create root level
    root['label'] = root.get('label', module['name'])
    root['params'] = new_params

    # Keep existing knobs or default to first 8 params
    if 'knobs' not in root:
        root['knobs'] = [p['key'] for p in new_params[:8]]

    # Update hierarchy
    levels['root'] = root
    ui_hierarchy['levels'] = levels
    caps['ui_hierarchy'] = ui_hierarchy

    # Remove chain_params
    del caps['chain_params']

    module['capabilities'] = caps

    # Write back
    with open(module_path, 'w') as f:
        json.dump(module, f, indent=2)
        f.write('\n')

    print(f"Migrated {module_path}")
    return True

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print("Usage: ./scripts/migrate-module-params.py path/to/module.json")
        sys.exit(1)

    module_path = Path(sys.argv[1])
    if not module_path.exists():
        print(f"File not found: {module_path}")
        sys.exit(1)

    if migrate_module(module_path):
        print("Success!")
    else:
        print("Nothing to migrate")
```

**Step 2: Make executable**

```bash
chmod +x scripts/migrate-module-params.py
```

**Step 3: Test on a copy**

```bash
cp src/modules/audio_fx/freeverb/module.json /tmp/test-module.json
./scripts/migrate-module-params.py /tmp/test-module.json
cat /tmp/test-module.json
```

Expected: See ui_hierarchy with params, no chain_params

**Step 4: Commit**

```bash
git add scripts/migrate-module-params.py
git commit -m "Add module parameter migration script

Python script to automatically migrate chain_params to ui_hierarchy format.
Preserves existing hierarchy structure and adds param definitions.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 13: Migrate Simple External Modules

**Files:**
- Multiple external module repos (braids, cloudseed, psxverb, tapescam, space-delay, obxd, clap)

**Step 1: Migrate CloudSeed (already has ui_hierarchy)**

```bash
cd ../move-anything-cloudseed
git checkout -b parameter-consolidation
```

CloudSeed already has ui_hierarchy with string references. Need to convert to full objects:

Edit `src/module.json` manually - replace params array in root level with full param objects from chain_params, add units where appropriate (mix, decay, size get "%" unit).

**Step 2: Build and test**

```bash
./scripts/build.sh
./scripts/install.sh
cd ../move-anything
./scripts/build.sh
./scripts/install.sh local
```

Test CloudSeed in Signal Chain

**Step 3: Commit in cloudseed repo**

```bash
cd ../move-anything-cloudseed
git add src/module.json
git commit -m "Migrate to ui_hierarchy params with units

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
git push origin parameter-consolidation
```

**Step 4: Repeat for other simple modules**

Apply same process to:
- move-anything-psxverb
- move-anything-tapescam
- move-anything-space-delay
- move-anything-obxd
- move-anything-braids (no ui_hierarchy yet - create one)
- move-anything-clap (no ui_hierarchy yet - create one)

For modules without ui_hierarchy, use the migration script as starting point.

**Step 5: Document migrations**

Keep notes of which modules are complete for final verification.

---

### Task 14: Migrate SF2 Module (Medium Complexity)

**Files:**
- Modify: `../move-anything-sf2/src/module.json`
- Modify: `../move-anything-sf2/src/dsp/sf2_plugin.c`

**Step 1: Update module.json**

SF2 has ui_hierarchy generated in C code. Move it to module.json:

```json
{
  "id": "sf2",
  "name": "SF2 Player",
  "capabilities": {
    "chainable": true,
    "component_type": "sound_generator",
    "ui_hierarchy": {
      "levels": {
        "root": {
          "label": "SF2",
          "list_param": "preset",
          "count_param": "preset_count",
          "name_param": "preset_name",
          "params": [
            {
              "key": "preset",
              "label": "Preset",
              "type": "int",
              "min": 0,
              "max_param": "preset_count",
              "default": 0
            },
            {
              "key": "octave_transpose",
              "label": "Octave",
              "type": "int",
              "min": -4,
              "max": 4,
              "default": 0,
              "step": 1
            },
            {
              "key": "gain",
              "label": "Gain",
              "type": "float",
              "min": 0,
              "max": 2,
              "default": 1.0,
              "step": 0.01
            },
            {"level": "soundfont", "label": "Choose Soundfont"}
          ],
          "knobs": ["octave_transpose", "gain"]
        },
        "soundfont": {
          "label": "Soundfont",
          "items_param": "soundfont_list",
          "select_param": "soundfont_index",
          "params": [],
          "knobs": []
        }
      }
    }
  }
}
```

**Step 2: Remove ui_hierarchy from sf2_plugin.c**

Find and remove the get_param("ui_hierarchy") case - it's now in module.json.

**Step 3: Build and test**

```bash
cd ../move-anything-sf2
git checkout -b parameter-consolidation
./scripts/build.sh
./scripts/install.sh
```

Test SF2 module, verify preset browser and navigation work

**Step 4: Commit**

```bash
git add src/module.json src/dsp/sf2_plugin.c
git commit -m "Migrate SF2 to module.json ui_hierarchy

- Move ui_hierarchy from C code to module.json
- Add full param definitions with metadata
- Remove runtime hierarchy generation

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
git push origin parameter-consolidation
```

---

### Task 15: Migrate DX7 Module (Complex)

**Files:**
- Modify: `../move-anything-dx7/src/module.json`
- Modify: `../move-anything-dx7/src/dsp/dx7_plugin.cpp`

**Step 1: Analyze existing DX7 hierarchy**

```bash
cd ../move-anything-dx7
grep -A 500 '"ui_hierarchy"' src/dsp/dx7_plugin.cpp > /tmp/dx7-hierarchy.txt
cat /tmp/dx7-hierarchy.txt
```

Note the 22-level structure with operators.

**Step 2: Convert C string to proper JSON in module.json**

This is complex - the hierarchy has:
- root → main → global, lfo, pitch_eg, operators
- operators → op1, op2, op3, op4, op5, op6
- Each operator → opN_eg, opN_kbd

Manually convert the C string to proper JSON in module.json, merging chain_params into param definitions.

**Step 3: Add shared_params for cross-level params**

If any params appear in multiple operator levels, move to shared_params.

**Step 4: Remove hierarchy from C code**

Delete the get_param("ui_hierarchy") case from dx7_plugin.cpp.

**Step 5: Build and test thoroughly**

```bash
./scripts/build.sh
./scripts/install.sh
```

Test all navigation levels, operator editing, preset browser.

**Step 6: Commit**

```bash
cd ../move-anything-dx7
git checkout -b parameter-consolidation
git add src/module.json src/dsp/dx7_plugin.cpp
git commit -m "Migrate DX7 to module.json ui_hierarchy

- Move 22-level hierarchy from C to module.json
- Merge chain_params into param definitions
- Add metadata for all operator parameters

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
git push origin parameter-consolidation
```

---

### Task 16: Migrate JV880 Module (Most Complex)

**Files:**
- Modify: `../move-anything-jv880/src/module.json`
- Modify: `../move-anything-jv880/src/dsp/jv880_plugin.cpp`

**Step 1: Analyze JV880 hierarchy and modes**

```bash
cd ../move-anything-jv880
grep -A 1000 '"ui_hierarchy"' src/dsp/jv880_plugin.cpp > /tmp/jv880-hierarchy.txt
cat /tmp/jv880-hierarchy.txt
```

Note:
- Mode switching (patch vs performance)
- Child prefixing (tone_1, tone_2, etc.)
- 11 levels

**Step 2: Create shared_params section**

Extract params that appear in both modes:

```json
{
  "ui_hierarchy": {
    "shared_params": [
      {
        "key": "mode",
        "label": "Mode",
        "type": "enum",
        "options": ["Patch", "Performance"],
        "default": 0
      },
      {
        "key": "octave_transpose",
        "label": "Octave",
        "type": "int",
        "min": -4,
        "max": 4,
        "default": 0,
        "step": 1
      }
    ],
    "modes": ["patch", "performance"],
    "mode_param": "mode",
    "levels": {
      ...
    }
  }
}
```

**Step 3: Convert hierarchy to JSON**

Carefully convert the C string to proper JSON structure, preserving:
- child_prefix and child_count
- All navigation structure
- Param definitions with full metadata

**Step 4: Merge chain_params**

Take the 4 params from chain_params array and merge into hierarchy param definitions.

**Step 5: Remove hierarchy from C code**

Delete get_param("ui_hierarchy") from jv880_plugin.cpp.

**Step 6: Build and test exhaustively**

```bash
./scripts/build.sh
./scripts/install.sh
```

Test:
- Mode switching between patch and performance
- Tone editing (child prefixing)
- Part editing in performance mode
- All navigation levels
- Preset browser

**Step 7: Commit**

```bash
cd ../move-anything-jv880
git checkout -b parameter-consolidation
git add src/module.json src/dsp/jv880_plugin.cpp
git commit -m "Migrate JV880 to module.json ui_hierarchy

- Add shared_params for mode and octave_transpose
- Move 11-level hierarchy with modes from C to module.json
- Preserve child_prefix pattern for tones and parts
- Merge chain_params into param definitions

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
git push origin parameter-consolidation
```

---

## Phase 4: Verification and Cleanup

### Task 17: Comprehensive Testing

**Files:**
- None (testing only)

**Step 1: Test each built-in module**

```bash
cd /path/to/move-anything
./scripts/build.sh
./scripts/install.sh local
```

On device, test:
- [ ] freeverb - knobs, overlay with units
- [ ] linein - gain knob
- [ ] arp - mode enum, rate with steps
- [ ] chord - chord_type enum

**Step 2: Test each external module**

For each migrated external module:
- [ ] CloudSeed
- [ ] PSXVerb
- [ ] Tapescam
- [ ] Space Delay
- [ ] OB-Xd
- [ ] Braids
- [ ] CLAP
- [ ] SF2
- [ ] DX7
- [ ] JV880

Test:
1. Module loads in Signal Chain
2. Params appear in hierarchy menu
3. Knob assignments work
4. Knob overlay shows proper units
5. Values format correctly (enums show labels, numbers show units)
6. Patches save/load correctly

**Step 3: Document any issues**

Create issue list for any problems found.

**Step 4: Create test report**

Write test results to `docs/parameter-consolidation-test-report.md`.

---

### Task 18: Remove Legacy chain_params Support

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c`

**Step 1: Remove parse_chain_params_legacy function**

Delete the entire function (was renamed from parse_chain_params earlier).

**Step 2: Remove fallback in parse_chain_params**

Update parse_chain_params to only support ui_hierarchy:

```c
static int parse_chain_params(const char *json, chain_param_info_t *out_params, int max_params) {
    /* Only support ui_hierarchy format now */
    const char *hierarchy = strstr(json, "\"ui_hierarchy\"");
    if (!hierarchy) {
        plugin_log("ERROR: Module missing ui_hierarchy - all modules must be updated");
        return -1;
    }

    return parse_hierarchy_params(json, out_params, max_params);
}
```

**Step 3: Build and verify**

```bash
./scripts/build.sh
```

Expected: Clean build, all modules still work (since they're all migrated)

**Step 4: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "Remove legacy chain_params support

All modules now use ui_hierarchy exclusively.
Remove backward compatibility code.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 19: Update Documentation

**Files:**
- Modify: `move-anything/CLAUDE.md`
- Create: `move-anything/docs/parameter-schema.md`

**Step 1: Create parameter schema documentation**

Write comprehensive docs about the new parameter schema:

```markdown
# Parameter Schema

## Overview

Parameters are defined in `ui_hierarchy` within module.json. This is the single
source of truth for all parameter metadata.

## Schema

### Simple Module

```json
{
  "ui_hierarchy": {
    "levels": {
      "root": {
        "label": "Module Name",
        "params": [
          {
            "key": "param_key",
            "label": "Display Name",
            "type": "float|int|enum",
            "min": 0,
            "max": 100,
            "default": 50,
            "step": 1,
            "unit": "Hz",
            "display_format": "%.1f"
          }
        ],
        "knobs": ["param_key"]
      }
    }
  }
}
```

### Multi-Mode Module

```json
{
  "ui_hierarchy": {
    "shared_params": [
      {"key": "mode", "type": "enum", ...}
    ],
    "modes": ["mode1", "mode2"],
    "mode_param": "mode",
    "levels": {...}
  }
}
```

## Field Reference

### Required Fields
- `key` - Unique parameter identifier
- `label` - Display name
- `type` - "float", "int", or "enum"

### Optional Fields
- `min` - Minimum value (required for numeric types)
- `max` - Maximum value (required for numeric types)
- `max_param` - Dynamic max from another param
- `default` - Default value (defaults to min for numeric, 0 for enum)
- `step` - Edit step size (defaults: 0.0015 float, 1 int/enum)
- `unit` - Display unit ("Hz", "dB", "ms", "%")
- `display_format` - Printf format ("%.0f", "%.1f", "%.2f")
- `options` - Array of strings (required for enums)

## Usage

Parameters are referenced everywhere by key:
- Knob mappings: `{"target": "synth", "param": "cutoff"}`
- Patches: Store key + value only
- Display: Look up metadata for formatting

## Migration

See docs/plans/2026-02-04-parameter-consolidation.md for migration guide.
```

**Step 2: Update CLAUDE.md**

Add section about parameter definitions:

```markdown
## Parameter Definitions

Parameters are defined in ui_hierarchy within module.json. See docs/parameter-schema.md
for full documentation.

Key points:
- Single source of truth for all parameter metadata
- Includes type, range, units, display formatting
- Proper enum support with options arrays
- Referenced everywhere by key only
```

**Step 3: Commit**

```bash
git add CLAUDE.md docs/parameter-schema.md
git commit -m "Add parameter schema documentation

Document ui_hierarchy parameter format and usage.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 20: Final Integration and Merge

**Files:**
- Multiple repos

**Step 1: Push main repo branch**

```bash
cd /path/to/move-anything
git push origin parameter-consolidation
```

**Step 2: Create PRs for all external modules**

For each external module repo:
```bash
cd ../move-anything-<module>
git push origin parameter-consolidation
# Create PR on GitHub
```

**Step 3: Merge main repo**

```bash
cd /path/to/move-anything
git checkout main
git merge parameter-consolidation
git push origin main
```

**Step 4: Merge external modules**

Merge each external module PR after review.

**Step 5: Tag releases**

For each external module:
```bash
cd ../move-anything-<module>
# Update version in module.json
git commit -am "Bump version to X.Y.Z for parameter consolidation"
git tag vX.Y.Z
git push --tags
```

**Step 6: Verify releases build**

Wait for GitHub Actions to build and upload tarballs for each module.

**Step 7: Test end-to-end on device**

Fresh install:
```bash
./scripts/install.sh  # Install from releases
```

Test all modules work correctly with new parameter system.

**Step 8: Celebrate!**

Parameter consolidation is complete! 🎉

---

## Summary

This plan consolidates parameter definitions from duplicated chain_params into ui_hierarchy
as single source of truth. Key improvements:

1. ✅ Proper enum support with KNOB_TYPE_ENUM
2. ✅ Units and display formatting (Hz, dB, ms, %)
3. ✅ Single source of truth (no duplication)
4. ✅ Simplified knob mappings (reference-only)
5. ✅ Centralized value formatting
6. ✅ Support for multi-mode modules (JV880)
7. ✅ Validated unique parameter keys

All 19 modules migrated to new format.
