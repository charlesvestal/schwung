/*
 * sampler_source_announce.h — classify a screen-reader announcement as a
 * change of MOVE'S NATIVE SAMPLER INPUT SOURCE.
 *
 * The result gates `native_resample_bridge_source_allows_apply()`: a MIC_IN or
 * USB_C_IN verdict stops the bridge writing Schwung's mix into Move's capture
 * buffer, because the user is sampling something real and overwriting it would
 * destroy the take. So a FALSE POSITIVE silently disables the bridge, and a
 * FALSE NEGATIVE silently overwrites a recording. Both are silent; neither is
 * recoverable after the fact.
 *
 * WHY THIS IS A SEPARATE, PURE FILE. It lived as a static function inside
 * shadow_resample.c and so could not be run on the host at all — which is how
 * it kept a matching rule that metronome_announce.h had already been written
 * to avoid, in the same handler, for the same reason. The two are called from
 * adjacent lines of shadow_dbus_handle_text(); only one of them had a test.
 *
 * THE RULE THIS FILE EXISTS TO KILL: the classifier matched bare substrings
 * ("mic", "line in", "usb-c", "resampl") against EVERY D-Bus text, not only
 * sampling announcements, and latched the result for the session. That is the
 * removed mute auto-correct's shape exactly. Captured on hardware 2026-09-12,
 * from the device's own debug.log:
 *
 *     "USB-C Audio. Submenu. 9 of 12"            -> USB_C_IN
 *     "USB-C Audio Out: Mic. Menu item. 1 of 2"  -> MIC_IN
 *
 * Those are Move's USB-C *OUTPUT* menu. Merely scrolling onto that Settings
 * row reclassified the sampler's INPUT and disabled the bridge until reboot.
 * "mic" is also a substring of "dynamic", "ceramic", "atomic" and "micro", so
 * a preset name was enough on its own. And Schwung's own TTS returns through
 * the same handler — "Analytics, Off, 1 of 5" and "S4: bouba-kiki Bulge: 27%"
 * are in that same capture — so the feedback-gate line "Speaker feedback risk.
 * Speakers and mic active." fired it too.
 *
 * Pure: no allocation, no I/O, no globals.
 */
#ifndef SAMPLER_SOURCE_ANNOUNCE_H
#define SAMPLER_SOURCE_ANNOUNCE_H

#include <stddef.h>

typedef enum {
    NATIVE_SAMPLER_SOURCE_UNKNOWN = 0,
    NATIVE_SAMPLER_SOURCE_RESAMPLING,
    NATIVE_SAMPLER_SOURCE_LINE_IN,
    NATIVE_SAMPLER_SOURCE_MIC_IN,
    NATIVE_SAMPLER_SOURCE_USB_C_IN
} native_sampler_source_t;

/*
 * Lowercase, collapse every whitespace run to one space, trim both ends.
 * Same normalisation as metronome_announce.h, and for the same reason: the
 * wire form of an announcement is not stable across Move's display and
 * screen-reader paths, and widening the MATCH to absorb that variation is
 * what made the old rule unsafe.
 *
 * `truncated_out` reports that the input did not fit. A truncated string must
 * never be compared — noise after a target phrase would normalise to a buffer
 * that could end exactly at the match and be believed.
 */
static inline void sampler_source_normalize(const char *in, char *out,
                                            size_t out_len, int *truncated_out)
{
    if (truncated_out) *truncated_out = 0;
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!in) return;

    size_t o = 0;
    int pending_space = 0;
    int seen_any = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            if (seen_any) pending_space = 1;
            continue;
        }
        if (pending_space) {
            if (o + 1 >= out_len) { if (truncated_out) *truncated_out = 1; break; }
            out[o++] = ' ';
            pending_space = 0;
        }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (o + 1 >= out_len) { if (truncated_out) *truncated_out = 1; break; }
        out[o++] = (char)c;
        seen_any = 1;
    }
    out[o] = '\0';
}

/* ---------------------------------------------------------------------------
 * CURRENT BEHAVIOUR — extracted VERBATIM from shadow_resample.c so that the
 * test below it fails for the reason the field data says it fails, rather than
 * against a rule invented here. The replacement lands in the next commit, once
 * Move's real sampling-source announcement strings have been captured; writing
 * an exact-match whitelist before knowing them would either keep the bug or
 * turn every genuine mic take into an overwrite.
 * ------------------------------------------------------------------------- */
static inline int sampler_source_contains(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return 0;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static inline native_sampler_source_t sampler_source_announce_classify(const char *text)
{
    char norm[256];
    int truncated = 0;
    sampler_source_normalize(text, norm, sizeof(norm), &truncated);
    if (!norm[0]) return NATIVE_SAMPLER_SOURCE_UNKNOWN;

    if (sampler_source_contains(norm, "resampl"))
        return NATIVE_SAMPLER_SOURCE_RESAMPLING;
    if (sampler_source_contains(norm, "line in") ||
        sampler_source_contains(norm, "line-in") ||
        sampler_source_contains(norm, "linein"))
        return NATIVE_SAMPLER_SOURCE_LINE_IN;
    if (sampler_source_contains(norm, "usb-c") ||
        sampler_source_contains(norm, "usb c") ||
        sampler_source_contains(norm, "usbc"))
        return NATIVE_SAMPLER_SOURCE_USB_C_IN;
    if (sampler_source_contains(norm, "mic") ||
        sampler_source_contains(norm, "microphone"))
        return NATIVE_SAMPLER_SOURCE_MIC_IN;

    return NATIVE_SAMPLER_SOURCE_UNKNOWN;
}

#endif /* SAMPLER_SOURCE_ANNOUNCE_H */
