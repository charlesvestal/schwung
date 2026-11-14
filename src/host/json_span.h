/*
 * json_span.h -- where a JSON object or array ends, the way JSON says.
 *
 * shadow_save_state() preserves two values written by shadow_ui.js -- the
 * "patches" array and the "master_fx_chain" object -- by copying them out of
 * shadow_config.json from their opening bracket to the matching close. That
 * match counted { } and [ ] blindly, so a bracket inside a STRING counted too:
 * a user-typed name like a preset "Pad }" ended the copy early, and the file
 * was rewritten as invalid JSON. Brackets inside a string are text, and a
 * backslash escapes the next character, so neither may end a string or a span.
 */
#ifndef JSON_SPAN_H
#define JSON_SPAN_H

#include <stddef.h>

/* p points at an opening '{' or '['. Returns a pointer one past its matching
 * close, or NULL if the input ends first (including inside a string). */
static inline const char *json_span_end(const char *p)
{
    int depth = 0;
    int in_string = 0;
    for (; *p; p++) {
        const char c = *p;
        if (in_string) {
            if (c == '\\') {
                if (!p[1]) return NULL;
                p++;                      /* the escaped character is text */
            } else if (c == '"') {
                in_string = 0;
            }
            continue;
        }
        if (c == '"') in_string = 1;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') {
            if (--depth == 0) return p + 1;
        }
    }
    return NULL;
}

#endif /* JSON_SPAN_H */
