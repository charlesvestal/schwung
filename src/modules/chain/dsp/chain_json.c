/*
 * Signal Chain — strstr-based JSON helpers.
 * Split from chain_host.c (2026-06 cleanup step 10); pure relocation,
 * no behavior change. Shared types/decls live in chain_internal.h.
 */

#include "chain_internal.h"

/* Simple JSON string extraction - finds "key": "value" and returns value */
int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    /* Find the colon after the key */
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return -1;

    /* Skip whitespace and find opening quote */
    while (*pos && (*pos == ' ' || *pos == '\t' || *pos == ':')) pos++;
    if (*pos != '"') return -1;
    pos++;

    /* Copy until closing quote */
    int i = 0;
    while (*pos && *pos != '"' && i < out_len - 1) {
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return 0;
}

/* Simple JSON integer extraction - finds "key": number */
int json_get_int(const char *json, const char *key, int *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    /* Find the colon after the key */
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return -1;

    /* Skip whitespace */
    while (*pos && (*pos == ' ' || *pos == '\t' || *pos == ':')) pos++;

    /* Parse integer */
    *out = atoi(pos);
    return 0;
}

/* Simple JSON float extraction - finds "key": number */
int json_get_float(const char *json, const char *key, float *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    /* Find the colon after the key */
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return -1;

    /* Skip whitespace */
    while (*pos && (*pos == ' ' || *pos == '\t' || *pos == ':')) pos++;

    char *endptr = NULL;
    float value = strtof(pos, &endptr);
    if (endptr == pos) return -1;

    *out = value;
    return 0;
}

int json_get_section_bounds(const char *json, const char *section_key,
                                   const char **out_start, const char **out_end) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", section_key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    const char *start = strchr(pos, '{');
    if (!start) return -1;

    int depth = 0;
    const char *end = NULL;
    for (const char *p = start; *p; p++) {
        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) {
                end = p;
                break;
            }
        }
    }
    if (!end) return -1;

    *out_start = start;
    *out_end = end;
    return 0;
}

int json_get_string_in_section(const char *json, const char *section_key,
                                      const char *key, char *out, int out_len) {
    const char *start = NULL;
    const char *end = NULL;
    if (json_get_section_bounds(json, section_key, &start, &end) != 0) {
        return -1;
    }

    int len = (int)(end - start + 1);
    char *section = malloc((size_t)len + 1);
    if (!section) return -1;

    memcpy(section, start, (size_t)len);
    section[len] = '\0';

    int ret = json_get_string(section, key, out, out_len);
    free(section);
    return ret;
}

int json_get_int_in_section(const char *json, const char *section_key,
                                   const char *key, int *out) {
    const char *start = NULL;
    const char *end = NULL;
    if (json_get_section_bounds(json, section_key, &start, &end) != 0) {
        return -1;
    }

    int len = (int)(end - start + 1);
    char *section = malloc((size_t)len + 1);
    if (!section) return -1;

    memcpy(section, start, (size_t)len);
    section[len] = '\0';

    int ret = json_get_int(section, key, out);
    free(section);
    return ret;
}

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

/*
 * Parse a single parameter definition object into chain_param_info_t.
 * Returns 0 on success, -1 on error.
 */
/* Helper: bounded strstr - search for needle within [start, end) */
const char *bounded_strstr(const char *start, const char *end, const char *needle) {
    const char *result = strstr(start, needle);
    return (result && result < end) ? result : NULL;
}

