/*
 * song_abl_mix.h — read each track's mute and solo out of Move's Song.abl.
 *
 *   tracks[i].mixer.speakerOn   false = muted
 *   tracks[i].mixer.solo-cue    true  = soloed
 *
 * Measured on firmware 2.1.x sets (2026-09-26). `speakerOn` CHANGES SHAPE when
 * Move stores a preset value beside it: a bare `false` becomes
 * `{"value": false, "presetValue": true}` (seen on drum cells; handled here for
 * tracks too). A reader that only looks for the literal on the key's own line
 * reads the object form as UNMUTED.
 *
 * Why a path walk and not a key search: the same two keys appear on every drum
 * cell's mixer, deeper in the same track. Only the track's OWN mixer counts.
 *
 * Song.abl is the LAST SAVE, not Move's live state. It equals the live state
 * at exactly two moments — boot and set load, when Move has just read it —
 * which are the only places this is called.
 *
 * Pure: operates on a NUL-terminated buffer, no allocation, no I/O.
 */
#ifndef SONG_ABL_MIX_H
#define SONG_ABL_MIX_H

#include <string.h>

#define SONG_ABL_MIX_TRACKS 4
#define SONG_ABL_MIX_MAXDEPTH 64

/* Returns how many tracks carried a mixer.speakerOn (0 = nothing usable).
 * muted_out / soloed_out are zeroed first; entries past the count stay 0. */
static inline int song_abl_mix_parse(const char *s, int muted_out[SONG_ABL_MIX_TRACKS],
                                     int soloed_out[SONG_ABL_MIX_TRACKS])
{
    for (int i = 0; i < SONG_ABL_MIX_TRACKS; i++) { muted_out[i] = 0; soloed_out[i] = 0; }
    if (!s) return 0;

    /* What each open container is, by the key that opened it. */
    enum { C_OTHER, C_ROOT, C_TRACKS, C_TRACK, C_MIXER, C_SPEAKER, C_SOLO };
    int kind[SONG_ABL_MIX_MAXDEPTH];
    int depth = 0;
    int track = -1;
    int found = 0;
    int pending = C_OTHER;   /* what the next container opened will be */
    int lit_key = C_OTHER;   /* which mixer field a literal about to arrive is */

    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '"') {
            const char *k = ++p;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
            if (!*p) break;
            size_t n = (size_t)(p - k);
            const char *q = p + 1;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q != ':') continue;               /* a string value, not a key */
            int parent = depth > 0 ? kind[depth - 1] : C_OTHER;
            pending = C_OTHER;
            lit_key = C_OTHER;
#define SONG_ABL_KEY(lit) (n == sizeof(lit) - 1 && memcmp(k, lit, n) == 0)
            if (parent == C_ROOT && SONG_ABL_KEY("tracks")) pending = C_TRACKS;
            else if (parent == C_TRACK && SONG_ABL_KEY("mixer")) pending = C_MIXER;
            else if (parent == C_MIXER && SONG_ABL_KEY("speakerOn")) { pending = C_SPEAKER; lit_key = C_SPEAKER; }
            else if (parent == C_MIXER && SONG_ABL_KEY("solo-cue")) { pending = C_SOLO; lit_key = C_SOLO; }
            else if ((parent == C_SPEAKER || parent == C_SOLO) && SONG_ABL_KEY("value")) lit_key = parent;
#undef SONG_ABL_KEY
            p = q;                                   /* at ':' */
            continue;
        }
        if (c == '{' || c == '[') {
            if (depth >= SONG_ABL_MIX_MAXDEPTH) return 0;
            int parent = depth > 0 ? kind[depth - 1] : -1;
            int k;
            if (depth == 0) k = C_ROOT;
            else if (parent == C_TRACKS && c == '{') { k = C_TRACK; track++; }
            else if (c == '{' && (pending == C_MIXER || pending == C_SPEAKER || pending == C_SOLO)) k = pending;
            else if (c == '[' && pending == C_TRACKS) k = C_TRACKS;
            else k = C_OTHER;
            kind[depth++] = k;
            pending = C_OTHER;
            lit_key = C_OTHER;
            continue;
        }
        if (c == '}' || c == ']') {
            if (depth > 0) depth--;
            pending = C_OTHER;
            lit_key = C_OTHER;
            continue;
        }
        if ((c == 't' && strncmp(p, "true", 4) == 0) || (c == 'f' && strncmp(p, "false", 5) == 0)) {
            int v = (c == 't');
            if (lit_key != C_OTHER && track >= 0 && track < SONG_ABL_MIX_TRACKS) {
                if (lit_key == C_SPEAKER) {
                    muted_out[track] = !v;
                    if (track + 1 > found) found = track + 1;
                } else if (lit_key == C_SOLO) {
                    soloed_out[track] = v;
                }
            }
            p += v ? 3 : 4;
            lit_key = C_OTHER;
            pending = C_OTHER;
            continue;
        }
        if (c == ',') { lit_key = C_OTHER; pending = C_OTHER; }
    }
    return found;
}

#endif /* SONG_ABL_MIX_H */
