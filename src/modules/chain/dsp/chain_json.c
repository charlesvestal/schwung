/*
 * Signal Chain — strstr-based JSON helpers.
 * Split from chain_host.c (2026-06 cleanup step 10); pure relocation,
 * no behavior change. Shared types/decls live in chain_internal.h.
 */

#include "chain_internal.h"

static int json_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int json_read_hex4(const char *p, const char *limit, uint32_t *value) {
    uint32_t result = 0;
    for (int i = 0; i < 4; i++) {
        if ((limit && p + i >= limit) || !p[i]) return 0;
        int digit = json_hex_digit(p[i]);
        if (digit < 0) return 0;
        result = (result << 4) | (uint32_t)digit;
    }
    *value = result;
    return 1;
}

static int json_append_utf8(uint32_t codepoint, char *out, int out_len, int *used) {
    unsigned char bytes[4];
    int count;
    if (codepoint == 0 || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) return 0;
    if (codepoint <= 0x7f) {
        bytes[0] = (unsigned char)codepoint;
        count = 1;
    } else if (codepoint <= 0x7ff) {
        bytes[0] = (unsigned char)(0xc0 | (codepoint >> 6));
        bytes[1] = (unsigned char)(0x80 | (codepoint & 0x3f));
        count = 2;
    } else if (codepoint <= 0xffff) {
        bytes[0] = (unsigned char)(0xe0 | (codepoint >> 12));
        bytes[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | (codepoint & 0x3f));
        count = 3;
    } else {
        bytes[0] = (unsigned char)(0xf0 | (codepoint >> 18));
        bytes[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[3] = (unsigned char)(0x80 | (codepoint & 0x3f));
        count = 4;
    }
    if (*used + count >= out_len) return 0;
    for (int i = 0; i < count; i++) out[(*used)++] = (char)bytes[i];
    return 1;
}

/* Decode one JSON string value. `limit` is the first byte outside the parent
 * object, or NULL for a NUL-terminated top-level scan. State is later handed
 * to plugins as a C string, so an escaped NUL is rejected instead of silently
 * truncating the snapshot. */
int json_decode_quoted_string(const char *quoted, const char *limit,
                              char *out, int out_len) {
    if (!out || out_len < 1) return -1;
    out[0] = '\0';
    if (!quoted || *quoted != '"') return -1;
    const char *p = quoted + 1;
    int used = 0;
    while (*p && (!limit || p < limit)) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') {
            out[used] = '\0';
            return used;
        }
        if (c == '\\') {
            if (!*p || (limit && p >= limit)) goto fail;
            c = (unsigned char)*p++;
            switch (c) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    uint32_t codepoint;
                    if (!json_read_hex4(p, limit, &codepoint)) goto fail;
                    p += 4;
                    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                        uint32_t low;
                        if ((limit && p + 6 > limit) || p[0] != '\\' || p[1] != 'u' ||
                            !json_read_hex4(p + 2, limit, &low) ||
                            low < 0xdc00 || low > 0xdfff) goto fail;
                        codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                        p += 6;
                    }
                    if (!json_append_utf8(codepoint, out, out_len, &used)) goto fail;
                    continue;
                }
                default: goto fail;
            }
        } else if (c < 0x20) {
            goto fail;
        }
        if (used + 1 >= out_len) goto fail;
        out[used++] = (char)c;
    }
fail:
    out[0] = '\0';
    return -1;
}

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

/* Simple JSON boolean extraction - finds "key": true|false.
 * Returns 0 if the key is found (value written to *out as 1/0), -1 otherwise.
 * NOTE: json_get_int cannot be used for booleans — atoi("true") yields 0. */
int json_get_bool(const char *json, const char *key, int *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    /* Find the colon after the key */
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return -1;

    /* Skip whitespace */
    while (*pos && (*pos == ':' || *pos == ' ' || *pos == '\t' || *pos == '\n')) pos++;

    *out = (strncmp(pos, "true", 4) == 0) ? 1 : 0;
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

    /* Only treat this key as a section if its VALUE is an object. Skipping
     * straight to the next '{' would, for a null-valued key (e.g. an empty FX
     * slot: "fx2":null), grab a LATER slot's object — corrupting saved presets
     * with gaps (filled/empty/filled). Anchor on the key's colon + value.
     * (Carried from PR #115/#117; also fixes a latent Master-preset bug.) */
    const char *colon = strchr(pos + strlen(search), ':');
    if (!colon) return -1;
    const char *start = colon + 1;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') start++;
    if (*start != '{') return -1;   /* value is null / not an object */

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

int json_get_bool_in_section(const char *json, const char *section_key,
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

    int ret = json_get_bool(section, key, out);
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
