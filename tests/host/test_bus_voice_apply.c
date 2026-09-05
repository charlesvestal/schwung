/*
 * bus_voice_apply: stored voice IDS resolve to render indices, and an id that
 * no longer resolves is COUNTED and left alone.
 *
 * The failure this pins is silent by construction: if an unresolved id were
 * dropped from the bus's list, or re-pointed to whatever now sits at that
 * index, a restored kit would come back sounding wrong with nothing anywhere
 * reporting that anything was lost.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "bus_voice_apply.h"

static int failures = 0;

static void check(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); failures++; }
}

static void reset(int8_t *m, int n) { for (int i = 0; i < n; i++) m[i] = BUS_MIX_MAIN; }

int main(void) {
    const char *ids[] = { "kick", "snare", "chh", "ohh" };
    int8_t map[4];

    /* --- Ids resolve by NAME, not by position. --- */
    {
        const char *stored[] = { "chh", "ohh" };
        reset(map, 4);
        int orphans = bus_voice_apply(ids, 4, stored, 2, 1, map, 4);
        check(orphans == 0, "no orphans when every id resolves");
        check(map[0] == BUS_MIX_MAIN, "kick untouched");
        check(map[1] == BUS_MIX_MAIN, "snare untouched");
        check(map[2] == 1, "chh -> bus 1");
        check(map[3] == 1, "ohh -> bus 1");
    }

    /* --- A module that INSERTS a voice must not re-point existing buses. ---
     * This is the whole reason ids are stored rather than indices: "ohh" was
     * index 3 and is now index 4. An index-based config would have pointed the
     * bus at "clap".
     */
    {
        const char *ids_v2[] = { "kick", "snare", "chh", "clap", "ohh" };
        const char *stored[] = { "ohh" };
        int8_t m5[5];
        reset(m5, 5);
        int orphans = bus_voice_apply(ids_v2, 5, stored, 1, 2, m5, 5);
        check(orphans == 0, "no orphan after an inserted voice");
        check(m5[4] == 2, "ohh followed its id to the new index");
        check(m5[3] == BUS_MIX_MAIN, "clap did NOT inherit the bus");
    }

    /* --- An id that no longer exists is counted, and nothing is re-pointed. --- */
    {
        const char *stored[] = { "kick", "rimshot" };
        reset(map, 4);
        int orphans = bus_voice_apply(ids, 4, stored, 2, 0, map, 4);
        check(orphans == 1, "the missing id is counted");
        check(map[0] == 0, "the id that DID resolve still applied");
        check(map[1] == BUS_MIX_MAIN && map[2] == BUS_MIX_MAIN &&
              map[3] == BUS_MIX_MAIN, "no voice absorbed the orphan");
    }

    /* --- A slot with no module at all orphans everything, and crashes on
     * nothing. This is the state between a set load and the synth's own
     * split_voices answer arriving. --- */
    {
        const char *stored[] = { "kick", "chh" };
        reset(map, 4);
        int orphans = bus_voice_apply(NULL, 0, stored, 2, 0, map, 4);
        check(orphans == 2, "every id orphans with no voice list");
        check(map[0] == BUS_MIX_MAIN, "map untouched with no voice list");
    }

    /* --- An empty entry in the stored list is a HOLE, not a lost voice. --- */
    {
        const char *stored[] = { "", "snare", NULL };
        reset(map, 4);
        int orphans = bus_voice_apply(ids, 4, stored, 3, 3, map, 4);
        check(orphans == 0, "holes are not orphans");
        check(map[1] == 3, "snare still resolved past the holes");
    }

    /* --- It only ever WRITES; the caller owns the clear. Applying bus 1 must
     * not undo an assignment bus 0 already made. --- */
    {
        const char *stored_a[] = { "kick" };
        const char *stored_b[] = { "snare" };
        reset(map, 4);
        bus_voice_apply(ids, 4, stored_a, 1, 0, map, 4);
        bus_voice_apply(ids, 4, stored_b, 1, 1, map, 4);
        check(map[0] == 0 && map[1] == 1, "two buses coexist in one map");
    }

    /* --- A map shorter than the voice list is a miss, never a write past the
     * end of it on the audio thread. --- */
    {
        const char *stored[] = { "ohh" };
        int8_t m2[2];
        reset(m2, 2);
        int orphans = bus_voice_apply(ids, 4, stored, 1, 0, m2, 2);
        check(orphans == 1, "an out-of-range index counts as a miss");
        check(m2[0] == BUS_MIX_MAIN && m2[1] == BUS_MIX_MAIN, "short map untouched");
    }

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("PASS: bus_voice_apply\n");
    return 0;
}
