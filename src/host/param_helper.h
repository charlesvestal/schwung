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

/*
 * Sentinel for viz_kind meaning "never draw a graphic for this param, whatever
 * a detector thinks" — the `viz: false` of docs/MODULES.md. Not a real kind, so
 * it cannot collide with one.
 */
#define PARAM_VIZ_NONE "false"

/* Parameter definition */
typedef struct {
    const char *key;      /* Parameter key (used in get/set) */
    const char *name;     /* Display name (for UI) */
    param_type_t type;    /* float or int */
    int index;            /* Index into values array */
    float min_val;        /* Minimum value */
    float max_val;        /* Maximum value */

    /*
     * Optional parameter visualisation — see docs/MODULES.md, "Parameter
     * visualisations (viz)". Leave all three NULL (the default for any entry
     * that does not mention them) and the param is simply not declared: the
     * host's detectors get a look, and a plain knob dial is the honest
     * fallback when none of them fire.
     *
     * Declared here, beside the param itself, ON PURPOSE. A module that keeps
     * its viz in a separate key-string lookup has two tables to hold in sync,
     * and renaming a key there silently detaches the graphic — no compile
     * error, the picture just stops appearing.
     */
    const char *viz_group;  /* Group id shared by one graphic's members;
                             * NULL for a single-param kind. Scoped to the
                             * module, never shown on screen. */
    const char *viz_role;   /* This param's part in the group ("attack",
                             * "cutoff", …). Required when viz_group is set. */
    const char *viz_kind;   /* Graphic type. Usually NULL — the host derives it
                             * from the roles present. Set it for a
                             * single-param graphic ("fader", "waveform",
                             * "switch"), or to PARAM_VIZ_NONE to suppress. */
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
 * Headroom the entry loops keep in reserve so a param is never written half
 * way. It has to exceed the longest single entry any module can emit — key,
 * name, type, range, and now a viz object, which alone can run past 80 chars.
 * The old value of 100 predated viz and no longer covers one.
 */
#define PARAM_HELPER_ENTRY_MARGIN 256

/*
 * Emit the optional ",\"viz\":{…}" field for one param definition.
 *
 * Written as its own function because not every module can use the generator
 * below — several hand-assemble chain_params to express something param_def_t
 * has no room for (an enum's options list, a unit, a display format). Those
 * modules should still call this rather than hand-rolling the JSON, so there
 * stays exactly one place that knows the field's shape.
 *
 * The leading comma is included: this always appends to an object that already
 * has at least a "key".
 *
 * Returns: chars written (0 when the param declares no viz), or -1 if the
 * buffer is too small to hold the field.
 */
static inline int param_helper_viz_json(
    const param_def_t *def,
    char *buf,
    int buf_len
) {
    if (!def || !buf || buf_len <= 0) return 0;
    if (!def->viz_group && !def->viz_kind) return 0;   /* nothing declared */

    /* viz: false — suppress any detector guess. */
    if (def->viz_kind && strcmp(def->viz_kind, PARAM_VIZ_NONE) == 0) {
        int n = snprintf(buf, buf_len, ",\"viz\":false");
        return (n < 0 || n >= buf_len) ? -1 : n;
    }

    int offset = snprintf(buf, buf_len, ",\"viz\":{");
    if (offset < 0 || offset >= buf_len) return -1;

    int wrote = 0;
    if (def->viz_group) {
        offset += snprintf(buf + offset, buf_len - offset,
                           "\"group\":\"%s\"", def->viz_group);
        if (offset >= buf_len) return -1;
        wrote = 1;
        if (def->viz_role) {
            offset += snprintf(buf + offset, buf_len - offset,
                               ",\"role\":\"%s\"", def->viz_role);
            if (offset >= buf_len) return -1;
        }
    }
    if (def->viz_kind) {
        offset += snprintf(buf + offset, buf_len - offset,
                           "%s\"kind\":\"%s\"", wrote ? "," : "", def->viz_kind);
        if (offset >= buf_len) return -1;
    }

    offset += snprintf(buf + offset, buf_len - offset, "}");
    if (offset >= buf_len) return -1;
    return offset;
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
    int emitted = 0;
    offset += snprintf(buf + offset, buf_len - offset, "[");

    for (int i = 0; i < def_count && offset < buf_len - PARAM_HELPER_ENTRY_MARGIN; i++, emitted++) {
        if (i > 0) offset += snprintf(buf + offset, buf_len - offset, ",");
        offset += snprintf(buf + offset, buf_len - offset,
            "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g",
            defs[i].key,
            defs[i].name[0] ? defs[i].name : defs[i].key,
            defs[i].type == PARAM_TYPE_INT ? "int" : "float",
            defs[i].min_val,
            defs[i].max_val);
        int vn = param_helper_viz_json(&defs[i], buf + offset, buf_len - offset);
        if (vn < 0) return -1;
        offset += vn;
        offset += snprintf(buf + offset, buf_len - offset, "}");
    }

    offset += snprintf(buf + offset, buf_len - offset, "]");

    if (offset >= buf_len) return -1;
    /*
     * Running out of room used to return a short but well-formed list, which
     * reaches the host as a module that simply has fewer params — no error
     * anywhere, the missing ones just never appear. Say -1 instead and let the
     * caller find out.
     */
    if (emitted < def_count) return -1;
    return offset;
}

/* Convenience macro for array count */
#define PARAM_DEF_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

#endif /* PARAM_HELPER_H */
