/*
 * The per-slot Chance store: which of Move's notes carry a condition, and the
 * lookup the SPI callback does for every note-on.
 *
 * The lookup is the dangerous half. It runs on a note that arrives as bare
 * MIDI -- no id, just a pitch and the moment it landed -- so it matches by
 * pitch and clip phase. Too loose and a condition leaks onto the neighbouring
 * step or another pitch; too strict and it silently never applies. And a note
 * on the loop's first beat arrives while the phase reads either the window's
 * start or a hair under its END, depending on which side of the wrap the
 * frame fell -- both must match.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "step_chance_store.h"

static sc_store_t st;

int main(void) {
    memset(&st, 0, sizeof st);

    /* ---- set / get / remove -------------------------------------------- */
    assert(sc_store_get(&st, 0, 1001) == SC_ALWAYS);
    assert(sc_store_set(&st, 0, 1001, 36, 1.0, 5) == 1);
    assert(sc_store_get(&st, 0, 1001) == 5);
    /* the same id on another ROW is another note */
    assert(sc_store_get(&st, 1, 1001) == SC_ALWAYS);
    /* re-set replaces in place, never duplicates */
    assert(sc_store_set(&st, 0, 1001, 36, 1.0, 7) == 1);
    assert(sc_store_count(&st) == 1);
    assert(sc_store_get(&st, 0, 1001) == 7);
    /* 100% is the absence of an entry -- setting it REMOVES */
    assert(sc_store_set(&st, 0, 1001, 36, 1.0, SC_ALWAYS) == 1);
    assert(sc_store_count(&st) == 0);
    /* an invalid condition is refused, not stored */
    assert(sc_store_set(&st, 0, 1001, 36, 1.0, sc_count()) == 0);
    assert(sc_store_count(&st) == 0);
    /* rev bumps on every change, so persistence can skip unchanged saves */
    uint32_t r0 = st.rev;
    sc_store_set(&st, 0, 1002, 38, 2.0, 3);
    assert(st.rev != r0);

    /* ---- match: pitch + phase ----------------------------------------- */
    memset(&st, 0, sizeof st);
    sc_store_set(&st, 2, 11, 36, 0.0, 4);    /* kick on beat 1 */
    sc_store_set(&st, 2, 12, 42, 0.5, 9);    /* hat on the and */
    sc_store_set(&st, 2, 13, 36, 2.0, 22);   /* kick on beat 3, 1:2 */
    const double LS = 0.0, LL = 4.0;
    assert(sc_store_match(&st, 2, 36, 0.0, LS, LL) == 4);
    assert(sc_store_match(&st, 2, 42, 0.5, LS, LL) == 9);
    assert(sc_store_match(&st, 2, 36, 2.0, LS, LL) == 22);
    /* wrong pitch at the right time: nothing */
    assert(sc_store_match(&st, 2, 38, 0.0, LS, LL) == SC_ALWAYS);
    /* right pitch, neighbouring 1/16 step (0.25 away): nothing */
    assert(sc_store_match(&st, 2, 36, 0.25, LS, LL) == SC_ALWAYS);
    /* another row: nothing */
    assert(sc_store_match(&st, 1, 36, 0.0, LS, LL) == SC_ALWAYS);
    /* a frame's worth late still matches (2.9 ms at 300 BPM ~ 0.015 q) */
    assert(sc_store_match(&st, 2, 36, 2.012, LS, LL) == 22);
    /* THE WRAP: beat 1 seen a hair before the window's end */
    assert(sc_store_match(&st, 2, 36, 3.995, LS, LL) == 4);
    /* ...in a window that does not start at 0 */
    memset(&st, 0, sizeof st);
    sc_store_set(&st, 0, 21, 60, 8.0, 6);
    assert(sc_store_match(&st, 0, 60, 8.0, 8.0, 12.0) == 6);
    assert(sc_store_match(&st, 0, 60, 19.996, 8.0, 12.0) == 6);
    /* unknown geometry: no wrap reasoning, direct distance only */
    assert(sc_store_match(&st, 0, 60, 8.0, 0.0, 0.0) == 6);
    assert(sc_store_match(&st, 0, 60, 19.996, 0.0, 0.0) == SC_ALWAYS);

    /* ---- capacity: full refuses, never overwrites ---------------------- */
    memset(&st, 0, sizeof st);
    for (int i = 0; i < SC_STORE_MAX; i++)
        assert(sc_store_set(&st, 0, 5000 + i, 60, i * 0.25, 1) == 1);
    assert(sc_store_set(&st, 0, 99999, 60, 0.0, 1) == 0);
    assert(sc_store_get(&st, 0, 5000) == 1);
    /* ...but an existing entry can still be changed or freed when full */
    assert(sc_store_set(&st, 0, 5000, 60, 0.0, 2) == 1);
    assert(sc_store_set(&st, 0, 5001, 60, 0.25, SC_ALWAYS) == 1);
    assert(sc_store_set(&st, 0, 99999, 60, 0.0, 1) == 1);

    /* ---- relocate: Move moved a note (nudge) -- it keeps its condition -- */
    memset(&st, 0, sizeof st);
    sc_store_set(&st, 0, 7, 48, 1.0, 10);
    sc_store_relocate(&st, 0, 7, 50, 1.1);
    assert(sc_store_match(&st, 0, 50, 1.1, 0.0, 4.0) == 10);
    assert(sc_store_match(&st, 0, 48, 1.0, 0.0, 4.0) == SC_ALWAYS);
    /* relocating an unknown id is a no-op */
    uint32_t r1 = st.rev;
    sc_store_relocate(&st, 0, 8, 50, 1.1);
    assert(st.rev == r1);

    /* ---- prune: deleted notes go, but only inside the window looked at ---- */
    memset(&st, 0, sizeof st);
    sc_store_set(&st, 0, 1, 36, 0.0, 4);    /* kept: still live */
    sc_store_set(&st, 0, 2, 38, 1.0, 4);    /* deleted */
    sc_store_set(&st, 0, 3, 38, 9.0, 4);    /* outside the window: untouched */
    sc_store_set(&st, 1, 2, 38, 1.0, 4);    /* another row: untouched */
    {
        const int64_t live[] = { 1, 77 };
        uint32_t r2 = st.rev;
        assert(sc_store_prune_window(&st, 0, 0.0, 4.0, live, 2) == 1);
        assert(st.rev != r2);
        assert(sc_store_get(&st, 0, 1) == 4);
        assert(sc_store_get(&st, 0, 2) == SC_ALWAYS);
        assert(sc_store_get(&st, 0, 3) == 4);
        assert(sc_store_get(&st, 1, 2) == 4);
        /* nothing to prune: rev unchanged (the autosave must not rewrite) */
        r2 = st.rev;
        assert(sc_store_prune_window(&st, 0, 0.0, 4.0, live, 2) == 0);
        assert(st.rev == r2);
        /* an empty page window prunes everything in it */
        assert(sc_store_prune_window(&st, 0, 0.0, 4.0, live, 0) == 1);
    }

    /* ---- the document round-trips, and a bad one changes NOTHING ------ */
    memset(&st, 0, sizeof st);
    sc_store_set(&st, 0, 1, 36, 0.0, 4);
    sc_store_set(&st, 3, -9223372036854775807LL, 42, 1.3333333333333333, 40);
    char doc[1024];
    int n = sc_store_serialize(&st, doc, sizeof doc);
    assert(n > 0 && n < (int)sizeof doc);
    static sc_store_t back;
    memset(&back, 0, sizeof back);
    assert(sc_store_parse(&back, doc) == 2);
    assert(sc_store_get(&back, 0, 1) == 4);
    assert(sc_store_get(&back, 3, -9223372036854775807LL) == 40);
    /* start survives BIT-EXACT: a triplet position must match after reload */
    assert(sc_store_match(&back, 3, 42, 1.3333333333333333, 0.0, 4.0) == 40);
    /* too small a buffer is -1, never a truncated document */
    assert(sc_store_serialize(&st, doc, 10) == -1);
    /* garbage is refused whole and leaves the store alone */
    assert(sc_store_parse(&back, "SC 1\n0 1 36 zero 4\n") == -1);
    assert(sc_store_get(&back, 0, 1) == 4);
    assert(sc_store_parse(&back, "SC 2\n") == -1);          /* unknown version */
    assert(sc_store_parse(&back, "0 1 36 0 4\n") == -1);    /* no header */
    assert(sc_store_parse(&back, "SC 1\n0 1 36 0 999\n") == -1); /* bad cond */
    assert(sc_store_count(&back) == 2);
    /* an empty document is a valid EMPTY store */
    assert(sc_store_parse(&back, "SC 1\n") == 0);
    assert(sc_store_count(&back) == 0);
    /* and "" is the same (a slot that never had chance) */
    sc_store_set(&back, 0, 1, 36, 0.0, 4);
    assert(sc_store_parse(&back, "") == 0);
    assert(sc_store_count(&back) == 0);

    printf("test_step_chance_store: PASS\n");
    return 0;
}
