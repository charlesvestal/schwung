/* The announcement matcher.
 *
 * The near-miss cases are the point. A suffix or substring match here would be
 * the drum-slot mute bug again: that rule matched any text ending in " muted",
 * so Move's own "Lay Down Kit muted" and Schwung's TTS looping back through
 * the same handler both fired it — and it PERSISTED the result.
 */
#include <stdio.h>
#include <string.h>
#include "metronome_announce.h"

static int failures = 0;

static void expect(const char *in, metronome_announce_t want, const char *what)
{
    metronome_announce_t got = metronome_announce_classify(in);
    if (got != want) {
        printf("FAIL: %s: classify(\"%s\") = %d, want %d\n",
               what, in ? in : "(null)", (int)got, (int)want);
        failures++;
    }
}

int main(void)
{
    /* ---- every plausible shape of the wire text ---- */
    expect("Metronome On",       METRONOME_ANNOUNCE_ON,  "space form");
    expect("Metronome\nOn",      METRONOME_ANNOUNCE_ON,  "display form, newline");
    expect("metronome on",       METRONOME_ANNOUNCE_ON,  "lowercase");
    expect("METRONOME ON",       METRONOME_ANNOUNCE_ON,  "uppercase");
    expect("  Metronome   On  ", METRONOME_ANNOUNCE_ON,  "padded and doubled space");
    expect("Metronome\r\nOn",    METRONOME_ANNOUNCE_ON,  "crlf");

    expect("Metronome Off",      METRONOME_ANNOUNCE_OFF, "space form");
    expect("Metronome\nOff",     METRONOME_ANNOUNCE_OFF, "display form, newline");
    expect("metronome off",      METRONOME_ANNOUNCE_OFF, "lowercase");
    expect("  Metronome\tOff",   METRONOME_ANNOUNCE_OFF, "tab");

    /* ---- near misses. Each of these WOULD match a substring rule. ---- */
    expect("Metronome",          METRONOME_ANNOUNCE_NONE, "bare noun");
    expect("Metronome On Track", METRONOME_ANNOUNCE_NONE, "longer sentence starting the same");
    expect("Turn Metronome On",  METRONOME_ANNOUNCE_NONE, "prefixed sentence");
    expect("Onmetronome",        METRONOME_ANNOUNCE_NONE, "no separator");
    expect("Metronome Onn",      METRONOME_ANNOUNCE_NONE, "trailing char");
    expect("Metronome Offset",   METRONOME_ANNOUNCE_NONE, "off is a prefix of a real word");
    expect("Lay Down Kit muted", METRONOME_ANNOUNCE_NONE, "the mute-bug shape");
    expect("unmuted",            METRONOME_ANNOUNCE_NONE, "unrelated");
    expect("",                   METRONOME_ANNOUNCE_NONE, "empty");
    expect("   ",                METRONOME_ANNOUNCE_NONE, "whitespace only");
    expect(NULL,                 METRONOME_ANNOUNCE_NONE, "null");

    /* ---- a very long string must not overrun the normalise buffer ---- */
    {
        char big[512];
        for (int i = 0; i < 511; i++) big[i] = 'x';
        big[511] = '\0';
        expect(big, METRONOME_ANNOUNCE_NONE, "overlong input");
    }
    /* ---- and one that is a valid match followed by a lot of noise: the
     * truncation must not turn it INTO a match. ---- */
    {
        char big[512];
        int n = snprintf(big, sizeof(big), "Metronome On");
        for (int i = n; i < 511; i++) big[i] = 'z';
        big[511] = '\0';
        expect(big, METRONOME_ANNOUNCE_NONE, "match prefix then noise must not truncate into a match");
    }

    /* ---- the truncation flag, tested where it is REACHABLE ----
     *
     * classify() uses a 64-byte buffer, which cannot truncate down to a
     * 12-character target — so its `if (truncated)` guard is unreachable and
     * mutating it away does not fail anything. The flag is what protects a
     * future caller with a smaller buffer or a longer phrase, so it is tested
     * here directly, at a size where truncation lands exactly on a match.
     */
    {
        char small[13];   /* holds 12 chars + NUL == exactly "metronome on" */
        int trunc = -1;
        metronome_announce_normalize("Metronome Online", small, sizeof(small), &trunc);
        if (strcmp(small, "metronome on") != 0) {
            printf("FAIL: truncation demo: normalised to \"%s\", want \"metronome on\"\n", small);
            failures++;
        }
        if (trunc != 1) {
            printf("FAIL: truncation must be REPORTED — a caller that believed \"%s\" "
                   "would read \"Metronome Online\" as the metronome coming on\n", small);
            failures++;
        }
        /* And a string that fits must NOT be flagged. */
        char room[64];
        trunc = -1;
        metronome_announce_normalize("Metronome On", room, sizeof(room), &trunc);
        if (trunc != 0) {
            printf("FAIL: a string that fits must not be flagged truncated\n");
            failures++;
        }
    }

    if (failures) { printf("test_metronome_announce: FAIL (%d)\n", failures); return 1; }
    printf("test_metronome_announce: PASS\n");
    return 0;
}
