/*
 * resample_bridge_mode.h — parse the `resample_bridge_mode` config value.
 *
 * ONE FACT WITH TWO CONSUMERS. This key is read by C (here, from shim init via
 * native_resample_bridge_load_mode_from_shadow_config) and by JS
 * (parseResampleBridgeMode in shadow/shadow_ui.js, on config load). They
 * disagreed about the legacy value `1` for months:
 *
 *   JS:  if (text === "1" || text === "mix") return 2;   // Backward compatibility
 *   C:   -> NATIVE_RESAMPLE_BRIDGE_MIX                   // no migration
 *
 * and the C one runs FIRST. The Feb 2026 setting was `Resample Src` with three
 * values (Off / Mix / Replace), so `1` is genuinely on disk on old devices —
 * which came up in the retired additive mode at every boot until shadow_ui got
 * around to loading its config and wrote a 2 over it. A window, not a steady
 * state, but a window in which a retired code path was live.
 *
 * Mode 1 is now retired in both, and its value is left as a HOLE in the enum
 * rather than renumbered, because it is on disk and renumbering would silently
 * reinterpret those files.
 *
 * Pure: no allocation, no I/O, no globals. tests/host runs this and the JS
 * function over the same table and requires the two to agree —
 * test_resample_bridge_mode.sh.
 */
#ifndef RESAMPLE_BRIDGE_MODE_H
#define RESAMPLE_BRIDGE_MODE_H

#include <stddef.h>

typedef enum {
    NATIVE_RESAMPLE_BRIDGE_OFF = 0,
    /* 1 = retired additive mode. Never returned; accepted on input and
     * migrated, exactly as shadow_ui.js has always done. */
    NATIVE_RESAMPLE_BRIDGE_OVERWRITE = 2
} native_resample_bridge_mode_t;

static inline void resample_bridge_mode_lower(const char *in, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!in) return;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        /* Trim ASCII whitespace at both ends the way a JSON token might carry
         * it; the JS side does String(raw).trim(). */
        if (o == 0 && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) continue;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (o + 1 >= out_len) break;
        out[o++] = (char)c;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t' ||
                     out[o - 1] == '\n' || out[o - 1] == '\r')) o--;
    out[o] = '\0';
}

static inline int resample_bridge_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == '\0' && *b == '\0';
}

static inline native_resample_bridge_mode_t
resample_bridge_mode_from_text_pure(const char *text)
{
    if (!text || !text[0]) return NATIVE_RESAMPLE_BRIDGE_OFF;

    char lower[64];
    resample_bridge_mode_lower(text, lower, sizeof(lower));

    if (resample_bridge_streq(lower, "0") || resample_bridge_streq(lower, "off"))
        return NATIVE_RESAMPLE_BRIDGE_OFF;
    if (resample_bridge_streq(lower, "2") ||
        resample_bridge_streq(lower, "overwrite") ||
        resample_bridge_streq(lower, "replace"))
        return NATIVE_RESAMPLE_BRIDGE_OVERWRITE;
    /* LEGACY: the retired mode 1, migrated rather than honoured. */
    if (resample_bridge_streq(lower, "1") || resample_bridge_streq(lower, "mix"))
        return NATIVE_RESAMPLE_BRIDGE_OVERWRITE;

    return NATIVE_RESAMPLE_BRIDGE_OFF;
}

#endif /* RESAMPLE_BRIDGE_MODE_H */
