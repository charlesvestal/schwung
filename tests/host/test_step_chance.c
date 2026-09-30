/*
 * The step menu's Chance conditions: Elektron's list, and the rule each one
 * plays by.
 *
 * Two ways this goes wrong silently. A:B off by one pass plays the right
 * NUMBER of times on the wrong passes -- 1:4 on the second bar instead of the
 * first -- which sounds like a groove, not a bug. And a percentage that rolls
 * against the wrong scale (rnd % 128, a byte) still thins a pattern, just by
 * the wrong amount, and nobody counts hits by ear.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "step_chance.h"

static const char *name(int idx) {
    static char buf[16];
    sc_name(idx, buf, sizeof buf);
    return buf;
}

int main(void) {
    /* ---- the list, in jog order ------------------------------------------ */
    assert(SC_ALWAYS == 0);
    assert(strcmp(name(0), "100%") == 0);
    assert(strcmp(name(1), "99%") == 0);
    assert(strcmp(name(21), "1%") == 0);
    /* then the ratios, B-major: 1:2 2:2 1:3 2:3 3:3 1:4 ... */
    assert(strcmp(name(22), "1:2") == 0);
    assert(strcmp(name(23), "2:2") == 0);
    assert(strcmp(name(24), "1:3") == 0);
    assert(strcmp(name(26), "3:3") == 0);
    assert(strcmp(name(27), "1:4") == 0);
    assert(strcmp(name(sc_count() - 1), "8:8") == 0);
    assert(sc_count() == 1 + 21 + (2 + 3 + 4 + 5 + 6 + 7 + 8));

    /* Percentages strictly DESCEND: the jog walks from "always" to "rarely",
     * and a repeat would be a detent that does nothing. */
    for (int i = 1; i <= 21; i++)
        assert(sc_percent(i) < sc_percent(i - 1) && sc_percent(i) > 0);
    assert(sc_percent(22) == -1);

    /* Out of range is ALWAYS, never "never": a stored index from a future
     * build with a longer list must not silence a note. */
    assert(sc_should_play(-1, 0, 0xFFFFFFFFu) == 1);
    assert(sc_should_play(sc_count(), 0, 0xFFFFFFFFu) == 1);
    assert(strcmp(name(sc_count()), "100%") == 0);

    /* ---- 100% never rolls ------------------------------------------------ */
    for (uint32_t r = 0; r < 1000; r++) assert(sc_should_play(SC_ALWAYS, 0, r * 4294967u) == 1);
    assert(sc_should_play(SC_ALWAYS, 0, 0xFFFFFFFFu) == 1);

    /* ---- a percentage rolls against the FULL 32-bit range ----------------
     * 50% plays exactly the lower half of rnd; 1% the lowest hundredth. */
    int p50 = -1, p1 = -1;
    for (int i = 0; i < sc_count(); i++) {
        if (sc_percent(i) == 50) p50 = i;
        if (sc_percent(i) == 1)  p1 = i;
    }
    assert(p50 > 0 && p1 > 0);
    assert(sc_should_play(p50, 0, 0x7FFFFFFFu) == 1);
    assert(sc_should_play(p50, 0, 0x80000000u) == 0);
    assert(sc_should_play(p1, 0, 0) == 1);
    assert(sc_should_play(p1, 0, 0x03000000u) == 0);   /* ~1.2% of the range */
    {
        /* A sweep of rnd lands within a hair of the nominal rate. */
        long hits = 0; const long N = 100000;
        for (long k = 0; k < N; k++)
            hits += sc_should_play(p50, 0, (uint32_t)((uint64_t)k * 0xFFFFFFFFu / N));
        assert(hits > N * 49 / 100 && hits < N * 51 / 100);
    }

    /* ---- A:B plays pass A of every B, counting passes from 0 -------------
     * The FIRST pass after launch is pass 0 and is "1" of 1:B. */
    int r14 = -1, r34 = -1, r22 = -1;
    for (int i = 0; i < sc_count(); i++) {
        int a, b;
        if (!sc_ratio(i, &a, &b)) continue;
        if (a == 1 && b == 4) r14 = i;
        if (a == 3 && b == 4) r34 = i;
        if (a == 2 && b == 2) r22 = i;
    }
    assert(r14 > 0 && r34 > 0 && r22 > 0);
    int got[8];
    for (int p = 0; p < 8; p++) got[p] = sc_should_play(r14, p, 0);
    assert(got[0] == 1 && got[1] == 0 && got[2] == 0 && got[3] == 0 && got[4] == 1 && got[5] == 0);
    assert(sc_should_play(r34, 2, 0) == 1 && sc_should_play(r34, 6, 0) == 1);
    assert(sc_should_play(r34, 3, 0) == 0);
    assert(sc_should_play(r22, 0, 0) == 0 && sc_should_play(r22, 1, 0) == 1);
    /* A ratio ignores the random number entirely -- it is deterministic. */
    assert(sc_should_play(r14, 0, 0xFFFFFFFFu) == 1);
    /* An unknown pass (negative) plays: a note must never be dropped for want
     * of a clock, the same "unknown is not zero" rule as the lane phase. */
    assert(sc_should_play(r14, -1, 0) == 1);

    /* ---- the pass index from the model's clock ---------------------------
     * beats since the clip launched / loop length, floored. */
    assert(sc_pass_index(0.0, 4.0) == 0);
    assert(sc_pass_index(3.999, 4.0) == 0);
    assert(sc_pass_index(4.0, 4.0) == 1);
    assert(sc_pass_index(17.0, 4.0) == 4);
    /* Unknown geometry answers unknown, never pass 0. */
    assert(sc_pass_index(4.0, 0.0) == -1);
    assert(sc_pass_index(-0.5, 4.0) == -1);
    /* A note ON the loop boundary may be seen a hair early by the clock: the
     * roll must land on the pass the note belongs to, not the one ending. */
    assert(sc_pass_index(3.9999999, 4.0) == 1);

    /* ---- the pass from Move's launch beat -------------------------------
     * A 4-beat loop launched at beat 10: passes turn at 14, 18, ... */
    assert(sc_clip_pass(0, 0, 4, 1, 10.0, 10.0) == 0);
    assert(sc_clip_pass(0, 0, 4, 1, 10.0, 13.9) == 0);
    assert(sc_clip_pass(0, 0, 4, 1, 10.0, 14.0) == 1);
    assert(sc_clip_pass(0, 0, 4, 1, 10.0, 17.99999999) == 2);   /* a hair early */
    assert(sc_clip_pass(0, 0, 4, 1, 10.0, 26.5) == 4);
    /* region 0..16 with the loop 8..12: the FIRST pass is 0..12, 12 beats */
    assert(sc_clip_pass(0, 8, 12, 1, 0.0, 11.9) == 0);
    assert(sc_clip_pass(0, 8, 12, 1, 0.0, 12.0) == 1);
    assert(sc_clip_pass(0, 8, 12, 1, 0.0, 16.0) == 2);
    /* before the launch beat (a queued launch), and a dead loop: unknown */
    assert(sc_clip_pass(0, 0, 4, 1, 10.0, 9.0) == -1);
    assert(sc_clip_pass(0, 4, 4, 1, 0.0, 9.0) == -1);
    /* unlooped plays once: always the first pass */
    assert(sc_clip_pass(0, 0, 4, 0, 0.0, 3.0) == 0);

    printf("test_step_chance: PASS\n");
    return 0;
}
