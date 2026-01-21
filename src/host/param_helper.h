/*
 * param_helper.h - Parameter definition and access helpers for plugins
 *
 * This helper allows plugins to define parameters once in a table and get
 * automatic string-based get/set handling, plus auto-generated chain_params JSON.
 *
 * Usage:
 *   1. Define your params: static const param_def_t my_params[] = { ... };
 *   2. In get_param: return param_helper_get(my_params, COUNT, values, key, buf, len);
 *   3. In set_param: return param_helper_set(my_params, COUNT, values, key, val);
 */

#ifndef PARAM_HELPER_H
#define PARAM_HELPER_H

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Parameter types */
typedef enum {
    PARAM_TYPE_FLOAT = 0,
    PARAM_TYPE_INT = 1
} param_type_t;

/* Parameter definition */
typedef struct {
    const char *key;      /* Parameter key (used in get/set) */
    const char *name;     /* Display name (for UI) */
    param_type_t type;    /* float or int */
    int index;            /* Index into values array */
    float min_val;        /* Minimum value */
    float max_val;        /* Maximum value */
} param_def_t;

/*
 * Get a parameter value by key.
 * Returns: length written to buf, or -1 if key not found
 */
static inline int param_helper_get(
    const param_def_t *defs,
    int def_count,
    const float *values,
    const char *key,
    char *buf,
    int buf_len
) {
    for (int i = 0; i < def_count; i++) {
        if (strcmp(key, defs[i].key) == 0) {
            if (defs[i].type == PARAM_TYPE_INT) {
                return snprintf(buf, buf_len, "%d", (int)values[defs[i].index]);
            } else {
                return snprintf(buf, buf_len, "%.3f", values[defs[i].index]);
            }
        }
    }
    return -1;  /* Key not found */
}

/*
 * Set a parameter value by key.
 * Returns: 0 on success, -1 if key not found
 */
static inline int param_helper_set(
    const param_def_t *defs,
    int def_count,
    float *values,
    const char *key,
    const char *val
) {
    for (int i = 0; i < def_count; i++) {
        if (strcmp(key, defs[i].key) == 0) {
            float v = (float)atof(val);
            /* Clamp to min/max */
            if (v < defs[i].min_val) v = defs[i].min_val;
            if (v > defs[i].max_val) v = defs[i].max_val;
            values[defs[i].index] = v;
            return 0;
        }
    }
    return -1;  /* Key not found */
}

/*
 * Generate chain_params JSON from parameter definitions.
 * Returns: length written to buf, or -1 if buffer too small
 */
static inline int param_helper_chain_params_json(
    const param_def_t *defs,
    int def_count,
    char *buf,
    int buf_len
) {
    int offset = 0;
    offset += snprintf(buf + offset, buf_len - offset, "[");

    for (int i = 0; i < def_count && offset < buf_len - 100; i++) {
        if (i > 0) offset += snprintf(buf + offset, buf_len - offset, ",");
        offset += snprintf(buf + offset, buf_len - offset,
            "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g}",
            defs[i].key,
            defs[i].name[0] ? defs[i].name : defs[i].key,
            defs[i].type == PARAM_TYPE_INT ? "int" : "float",
            defs[i].min_val,
            defs[i].max_val);
    }

    offset += snprintf(buf + offset, buf_len - offset, "]");

    if (offset >= buf_len) return -1;
    return offset;
}

/* Convenience macro for array count */
#define PARAM_DEF_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

#endif /* PARAM_HELPER_H */
