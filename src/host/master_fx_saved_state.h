/*
 * Extract the `state` value from a per-set Master FX JSON file.
 *
 * Master FX modules may return either structured JSON or an opaque string from
 * get_param("state"). The JS save path preserves both, but the original boot
 * loader only looked for a `{` after the key. An opaque state such as PALETTE's
 * CSV was therefore saved correctly and silently ignored at the next boot.
 *
 * Header-only and dependency-free so the native host test exercises the exact
 * parser used by shadow_chain_mgmt.c without linking that translation unit's
 * device dependencies.
 */
#ifndef MASTER_FX_SAVED_STATE_H
#define MASTER_FX_SAVED_STATE_H

#include <stddef.h>
#include <string.h>

#include "json_compact.h"

static inline const char *master_fx_state_skip_ws(const char *p)
{
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Returns bytes written (excluding NUL), or -1 for absent/malformed/overflow. */
static inline int master_fx_saved_state_copy(const char *json, char *out, size_t out_len)
{
    if (!json || !out || out_len == 0) return -1;

    const char *key = strstr(json, "\"state\"");
    if (!key) return -1;
    const char *value = strchr(key + sizeof("\"state\"") - 1, ':');
    if (!value) return -1;
    value = master_fx_state_skip_ws(value + 1);
    if (!value) return -1;

    if (*value == '{') {
        /* Compacted, never the raw pretty file slice: the file is written by
         * JSON.stringify(w, null, 2) and modules parse what they emitted
         * (compact) — see json_compact.h. */
        return json_object_compact_copy(out, out_len, value);
    }

    if (*value != '"') return -1;
    value++;
    size_t written = 0;
    while (*value && *value != '"') {
        unsigned char c = (unsigned char)*value++;
        if (c == '\\') {
            c = (unsigned char)*value++;
            if (!c) return -1;
            switch (c) {
                case '"': case '\\': case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: return -1;
            }
        }
        if (written + 1 >= out_len) return -1;
        out[written++] = (char)c;
    }
    if (*value != '"') return -1;
    out[written] = '\0';
    return (int)written;
}

#endif /* MASTER_FX_SAVED_STATE_H */
