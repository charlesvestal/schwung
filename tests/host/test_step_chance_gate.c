/*
 * The Chance gate: drop a losing note-on AND its note-off; one roll per step
 * per pass so a chord drops whole; unknown phase always delivers.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "step_chance_gate.h"

static sc_store_t st;

static int on(sc_gate_t *g, int ch, int n, double ph, long pass) {
    uint8_t m[3] = { (uint8_t)(0x90 | ch), (uint8_t)n, 100 };
    return sc_gate(g, &st, m, 3, 1, ph, 0.0, 4.0, 0, pass);
}
static int off(sc_gate_t *g, int ch, int n) {
    uint8_t m[3] = { (uint8_t)(0x80 | ch), (uint8_t)n, 0 };
    return sc_gate(g, &st, m, 3, 1, 0.0, 0.0, 4.0, 0, 0);
}

int main(void) {
    int r12 = -1, p50 = -1;
    for (int i = 0; i < sc_count(); i++) {
        int a, b;
        if (sc_ratio(i, &a, &b) && a == 1 && b == 2) r12 = i;
        if (sc_percent(i) == 50) p50 = i;
    }
    assert(r12 > 0 && p50 > 0);

    /* ---- no entry: everything passes ----------------------------------- */
    sc_gate_t g; memset(&g, 0, sizeof g); memset(&st, 0, sizeof st);
    assert(on(&g, 0, 60, 0.0, 0) == 1);
    assert(off(&g, 0, 60) == 1);

    /* ---- 1:2 on a kick: plays pass 0, drops pass 1 with its off -------- */
    sc_store_set(&st, 0, 1, 36, 0.0, r12);
    assert(on(&g, 9, 36, 0.0, 0) == 1);
    assert(off(&g, 9, 36) == 1);
    assert(on(&g, 9, 36, 0.0, 1) == 0);
    assert(off(&g, 9, 36) == 0);          /* the paired off is dropped */
    assert(off(&g, 9, 36) == 1);          /* ...once: a stray second off passes */
    /* the same pitch on ANOTHER channel is unrelated */
    assert(on(&g, 9, 36, 0.0, 1) == 0);
    assert(off(&g, 8, 36) == 1);
    assert(off(&g, 9, 36) == 0);

    /* ---- note-on with velocity 0 is an off ----------------------------- */
    assert(on(&g, 9, 36, 0.0, 3) == 0);
    { uint8_t m[3] = { 0x99, 36, 0 };
      assert(sc_gate(&g, &st, m, 3, 1, 0.0, 0.0, 4.0, 0, 3) == 0); }

    /* ---- THE LOOP BOUNDARY: a note on the first beat, seen a hair early ----
     * phase 3.995 of a 4-beat loop is the NEXT pass's beat 1. Counted as the
     * ending pass, 1:2 flipped parity (13 drops in 30 passes on hardware). */
    memset(&st, 0, sizeof st); memset(&g, 0, sizeof g);
    sc_store_set(&st, 0, 1, 36, 0.0, r12);
    for (long pass = 0; pass < 40; pass++) {
        /* odd passes: the frame lands just before the wrap (still pass-1) */
        const double ph = (pass % 3 == 1) ? 3.995 : 0.0;
        const long counted = (ph > 3.0) ? pass - 1 : pass;
        const int played = on(&g, 9, 36, ph, counted);
        off(&g, 9, 36);
        assert(played == (pass % 2 == 0));
    }

    /* ---- a CHORD drops as a unit: one roll per step per pass ----------- */
    memset(&st, 0, sizeof st); memset(&g, 0, sizeof g);
    sc_store_set(&st, 0, 10, 60, 1.0, p50);
    sc_store_set(&st, 0, 11, 64, 1.0, p50);
    sc_store_set(&st, 0, 12, 67, 1.0, p50);
    int split = 0, played = 0;
    for (long pass = 0; pass < 400; pass++) {
        int a = on(&g, 0, 60, 1.0, pass), b = on(&g, 0, 64, 1.0, pass), c = on(&g, 0, 67, 1.0, pass);
        off(&g, 0, 60); off(&g, 0, 64); off(&g, 0, 67);
        if (!(a == b && b == c)) split++;
        played += a;
    }
    assert(split == 0);
    /* the counters: 1200 matched note-ons, and dropped = 3 x the passes lost */
    assert(g.matched == 1200);
    assert(g.dropped_n == (uint32_t)(1200 - 3 * played));
    /* and it really is ~50%, not stuck on one answer */
    assert(played > 150 && played < 250);

    /* ---- a chord PLAYED IN LIVE: starts a few ms apart, one trig ---------
     * Measured on a real clip: 17.425 / 17.428 / 17.434. Set together on one
     * step (group 17.25), they must still roll as one. */
    memset(&st, 0, sizeof st); memset(&g, 0, sizeof g);
    sc_store_set_grp(&st, 0, 21, 73, 1.425, p50, 1.25);
    sc_store_set_grp(&st, 0, 22, 64, 1.428, p50, 1.25);
    sc_store_set_grp(&st, 0, 23, 71, 1.434, p50, 1.25);
    split = 0; played = 0;
    for (long pass = 0; pass < 400; pass++) {
        int a = on(&g, 0, 73, 1.425, pass), b = on(&g, 0, 64, 1.428, pass), c = on(&g, 0, 71, 1.434, pass);
        off(&g, 0, 73); off(&g, 0, 64); off(&g, 0, 71);
        if (!(a == b && b == c)) split++;
        played += a;
    }
    assert(split == 0);
    assert(played > 150 && played < 250);

    /* ---- unknown phase / no row: deliver, never roll -------------------- */
    memset(&st, 0, sizeof st); memset(&g, 0, sizeof g);
    sc_store_set(&st, 0, 1, 36, 0.0, r12);
    { uint8_t m[3] = { 0x99, 36, 100 };
      assert(sc_gate(&g, &st, m, 3, 0, 0.0, 0.0, 4.0, 0, 1) == 1);
      assert(sc_gate(&g, &st, m, 3, 1, 0.0, 0.0, 4.0, -1, 1) == 1); }
    /* unknown PASS on an A:B plays (step_chance.h) */
    assert(on(&g, 9, 36, 0.0, -1) == 1);

    /* ---- SLOT TRANSPOSE: the gate sees the note AFTER the shim moved it ----
     * The store holds Move's pitch (64); a slot at +12 delivers 76. Matched
     * on the delivered pitch, nothing on a transposed slot ever rolled
     * (chance:stats "0 0" on hardware, 2026-10-01). The off arrives
     * transposed too, so the dropped set stays keyed on the DELIVERED pitch. */
    memset(&st, 0, sizeof st); memset(&g, 0, sizeof g);
    sc_store_set(&st, 0, 1, 64, 0.0, r12);
    g.transpose = 12;
    assert(on(&g, 1, 76, 0.0, 0) == 1);
    assert(off(&g, 1, 76) == 1);
    assert(on(&g, 1, 76, 0.0, 1) == 0);   /* the losing pass drops... */
    assert(off(&g, 1, 76) == 0);          /* ...with its off, at the delivered pitch */
    assert(g.matched == 2 && g.dropped_n == 1);
    /* the untransposed pitch is now a DIFFERENT note: it must not match */
    assert(on(&g, 1, 64, 0.0, 1) == 1);
    /* a transpose pushing the match pitch out of range matches nothing */
    g.transpose = 100;
    assert(on(&g, 1, 76, 0.0, 1) == 1);
    g.transpose = -60;
    assert(on(&g, 1, 76, 0.0, 1) == 1);
    g.transpose = 0;

    /* ---- non-note messages pass untouched ------------------------------- */
    { uint8_t cc[3] = { 0xB0, 74, 10 }, pb[3] = { 0xE0, 0, 64 };
      assert(sc_gate(&g, &st, cc, 3, 1, 0.0, 0.0, 4.0, 0, 1) == 1);
      assert(sc_gate(&g, &st, pb, 3, 1, 0.0, 0.0, 4.0, 0, 1) == 1); }

    printf("test_step_chance_gate: PASS\n");
    return 0;
}
