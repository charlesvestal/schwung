/*
 * metronome_announce.h — classify a Move screen-reader announcement.
 *
 * Move raises "Metronome\nOn" / "Metronome\nOff" (strings at 0x169474 and
 * 0x1909d8 in MoveOriginal, sitting among "Clip\ncreated" and "Notes\ndeleted")
 * and pushes them out as com.ableton.move.ScreenReader.text, which
 * shadow_dbus.c already receives through its catch-all type='signal' match.
 *
 * WHY THIS IS NOT THE MUTE BUG. The removed mute auto-correct matched any text
 * ENDING IN " muted"/" soloed", so Move's own "Lay Down Kit muted" and
 * Schwung's TTS looping back through the same handler both hit it — and it
 * PERSISTED the result, so a spurious match silenced slots across projects.
 * Here the match is exact equality on a whole normalised string, Schwung never
 * utters either string, and the result is runtime-only.
 *
 * Normalisation, not a family of literals: the binary holds the DISPLAY form
 * (with a newline) and the announcement may normalise it differently. Lowering
 * case and collapsing whitespace covers every plausible shape without widening
 * the match to a substring — which is precisely what made the mute rule unsafe.
 *
 * Pure: no allocation, no I/O, no globals.
 */
#ifndef METRONOME_ANNOUNCE_H
#define METRONOME_ANNOUNCE_H

#include <stddef.h>

typedef enum {
    METRONOME_ANNOUNCE_NONE = 0,  /* not about the metronome — change nothing */
    METRONOME_ANNOUNCE_ON   = 1,
    METRONOME_ANNOUNCE_OFF  = 2,
} metronome_announce_t;

/*
 * Lowercase, collapse every whitespace run to one space, trim both ends.
 *
 * `truncated_out` reports whether the input did not fit. A truncated string
 * must never be compared: "Metronome On" followed by noise would normalise to
 * a buffer that could end exactly at the match and be believed. The caller
 * treats truncation as NONE.
 */
static inline void metronome_announce_normalize(const char *in, char *out,
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

static inline int metronome_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == '\0' && *b == '\0';
}

/*
 * EXACT equality on the whole normalised string. Not a prefix, not a substring:
 * "Metronome On Track" must NOT be read as the metronome coming on.
 */
static inline metronome_announce_t metronome_announce_classify(const char *text)
{
    char norm[64];
    int truncated = 0;
    metronome_announce_normalize(text, norm, sizeof(norm), &truncated);
    /* DEFENCE IN DEPTH, and deliberately unreachable at this buffer size: 64
     * bytes cannot truncate down to a 12-character target, so no input can
     * currently reach this line with truncated set AND then match. It is here
     * so that shrinking `norm`, or adding a longer phrase to the list below,
     * cannot silently turn a truncated string into a match. The flag itself is
     * tested directly against a small buffer in test_metronome_announce.c —
     * mutating this line alone does not fail the suite, and pretending
     * otherwise would be a probe that cannot fail. */
    if (truncated) return METRONOME_ANNOUNCE_NONE;
    if (metronome_streq(norm, "metronome on"))  return METRONOME_ANNOUNCE_ON;
    if (metronome_streq(norm, "metronome off")) return METRONOME_ANNOUNCE_OFF;
    return METRONOME_ANNOUNCE_NONE;
}

#endif /* METRONOME_ANNOUNCE_H */
