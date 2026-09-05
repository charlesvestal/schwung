/*
 * Copy a JSON object MINIFIED: whitespace outside strings stripped, string
 * contents preserved byte-for-byte (escape-aware).
 *
 * Why this exists: slot_N.json and master_fx_N.json are written by
 * JSON.stringify(wrapper, null, 2), and the loaders do not re-serialize —
 * they used to brace-match the module's "state" object in the FILE TEXT and
 * hand the raw pretty-printed slice back to set_param("state"). A module
 * parses what it emitted, which is compact, so every hand-rolled parser that
 * matched `"key":"` (no space) silently missed the stored `"key": "` form.
 * Field-confirmed in five modules (minijv lost every working-patch edit on
 * set reload; mono ignored its entire state; work lost its patterns; smack
 * its locks; noisemaker its bank) while whitespace-tolerant number parsers
 * in the same modules kept restoring the headline fields — the right patch
 * loaded, the modifications vanished. JP-8000 hit it, fixed it locally, and
 * wrote "never match a JSON field with a whitespace-exact pattern" in a
 * comment; this header is that lesson applied at the one altitude that fixes
 * every module at once, including ones that will never update.
 *
 * Compacting at the LOAD boundary keeps the on-disk files pretty (they are
 * hand-debugged often) while modules receive the same encoding they emitted.
 *
 * Header-only and dependency-free so tests/host can exercise it directly.
 */
#ifndef JSON_COMPACT_H
#define JSON_COMPACT_H

#include <stddef.h>

/*
 * src must point at the object's opening '{'. Copies through the matching
 * closing brace (string-aware, like json_object_end). Returns the compacted
 * length (excluding NUL), or -1 when src is not an object, the object never
 * closes, or dst cannot hold the COMPACT form — the caller keeps its
 * existing "state absent" behavior on -1.
 */
static inline int json_object_compact_copy(char *dst, size_t dst_len,
                                           const char *src)
{
    if (!dst || dst_len == 0 || !src || *src != '{') return -1;
    size_t w = 0;
    int depth = 0, in_string = 0, escaped = 0;
    for (const char *p = src; *p; p++) {
        char c = *p;
        if (in_string) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') in_string = 0;
        } else {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            if (c == '"') in_string = 1;
            else if (c == '{') depth++;
            else if (c == '}') {
                if (w + 1 >= dst_len) return -1;
                dst[w++] = c;
                if (--depth == 0) { dst[w] = '\0'; return (int)w; }
                continue;
            }
        }
        if (w + 1 >= dst_len) return -1;
        dst[w++] = c;
    }
    return -1; /* unterminated object */
}

#endif /* JSON_COMPACT_H */
