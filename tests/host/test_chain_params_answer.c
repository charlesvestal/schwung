/*
 * Does an empty array count as a plugin ANSWERING the chain_params query?
 *
 * All three get_param routes (synth, audio FX, MIDI FX) ask the sub-plugin
 * first and render the module.json fallback only if it declines. "Declined"
 * used to be `result <= 0` — a length test — so a plugin returning the two
 * characters "[]" suppressed the correct metadata the host had already parsed.
 * That is what impressive-chords v0.1.24 does when the chain_params.json it
 * reads at runtime is not installed, and it is why its knobs drove every
 * parameter as a float 0..1: an int wrote a fraction its atoi read as 0, an
 * enum took option 0. Reported from the device 2026-08-21.
 *
 * The predicate is the whole fix, so it is what is pinned. It is `static
 * inline` in chain_internal.h precisely so it can be RUN here rather than
 * grepped for.
 */
#include <stdio.h>
#include <string.h>

#include "chain_internal.h"

static int failures = 0;

static void check(const char *what, const char *buf, int result, int want) {
    int got = chain_params_answer_is_useful(buf, result);
    if (got != want) {
        fprintf(stderr, "FAIL: %s -> %d, wanted %d\n", what, got, want);
        failures++;
    }
}

int main(void) {
    /* ---- the reported case, and its neighbours ---- */
    check("\"[]\" (impressive-chords with no chain_params.json)", "[]", 2, 0);
    check("\"[ ]\"", "[ ]", 3, 0);
    check("\"[\\n]\"", "[\n]", 3, 0);
    check("empty string with length 0", "", 0, 0);
    check("declined (-1)", "", -1, 0);
    check("NULL buffer", NULL, 4, 0);

    /* ---- a real table is still an answer ---- */
    const char *real = "[{\"key\":\"cutoff\",\"type\":\"float\"}]";
    check("a one-param table", real, (int)strlen(real), 1);
    check("a table with leading whitespace", "  [{\"key\":\"a\"}]", 15, 1);

    /* ---- result is the LENGTH, and it bounds the scan ---- */
    /* A plugin reports how much it wrote; anything past that is not ours to
     * read. A scan that ran to the NUL instead would read a stale tail out of
     * a 64KB buffer the caller reuses. */
    {
        char buf[64];
        memcpy(buf, "[]", 3);
        memcpy(buf + 3, "{\"key\":\"stale\"}", 16);   /* left over, past result */
        check("stale bytes past result are not read", buf, 2, 0);
    }

    if (failures) return 1;
    printf("PASS: an empty chain_params array is not an answer, a real table is\n");
    return 0;
}
