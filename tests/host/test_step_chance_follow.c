/*
 * Chance follows Move's edits: paste (and Double Loop), its Undo/Redo, clip
 * copy, clip delete + Undo, note delete + Undo, and the shim's adoption of a
 * copied condition by Move's real note id.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "step_chance_follow.h"

static sc_store_t st;
static sc_journal_t j;
static sc_pruned_t ring;

int main(void) {
    /* ---- step paste: step 1 (0.0) onto step 5 (1.0), 1/16 = 0.25 -------- */
    memset(&st, 0, sizeof st); memset(&j, 0, sizeof j);
    sc_store_set(&st, 0, 11, 36, 0.0, 22);        /* kick 1:2 on step 1 */
    sc_store_set(&st, 0, 12, 36, 1.0, 5);         /* the dst's own old condition */
    sc_store_set(&st, 0, 13, 42, 1.0, 9);         /* another voice on the dst step */
    j.jid = 7;
    assert(sc_store_paste(&st, 0, 0.0, 1.0, 0.25, NULL, &j) == 1);
    assert(sc_store_match(&st, 0, 36, 1.0, 0.0, 4.0) == 22);   /* the copy plays at once */
    assert(sc_store_match(&st, 0, 36, 0.0, 0.0, 4.0) == 22);   /* source untouched */
    assert(sc_store_match(&st, 0, 42, 1.0, 0.0, 4.0) == SC_ALWAYS); /* unscoped: whole step replaced */
    assert(j.nrem == 2 && j.nadd == 1 && !j.overflow);
    /* the copy's id is synthetic until adopted */
    int64_t synth = 0;
    for (int i = 0; i < SC_STORE_MAX; i++) if (st.e[i].used && st.e[i].start == 1.0) synth = st.e[i].id;
    assert(synth < 0);

    /* ---- adoption by Move's real id; then a nudge follows it ------------- */
    assert(sc_store_adopt(&st, 0, 900, 36, 1.0) == 1);
    assert(sc_store_get(&st, 0, 900) == 22);
    assert(sc_store_adopt(&st, 0, 900, 36, 1.0) == 0);          /* already its own */
    assert(sc_store_adopt(&st, 0, 901, 36, 3.0) == 0);          /* nothing synthetic there */
    sc_store_relocate(&st, 0, 900, 36, 1.05);
    assert(sc_store_match(&st, 0, 36, 1.05, 0.0, 4.0) == 22);

    /* ---- Undo of the paste: the copy goes, the replaced ones return ------ */
    assert(sc_journal_apply(&st, &j, 0) == 1);
    assert(sc_store_match(&st, 0, 36, 1.0, 0.0, 4.0) == 5);
    assert(sc_store_get(&st, 0, 12) == 5 && sc_store_get(&st, 0, 13) == 9);
    assert(sc_store_get(&st, 0, 11) == 22);
    /* ...and Redo puts the paste back */
    assert(sc_journal_apply(&st, &j, 1) == 1);
    assert(sc_store_match(&st, 0, 36, 1.0, 0.0, 4.0) == 22);
    assert(sc_store_get(&st, 0, 12) == SC_ALWAYS);

    /* ---- a DRUM paste is scoped to the pasted pad ------------------------ */
    memset(&st, 0, sizeof st); memset(&j, 0, sizeof j);
    sc_store_set(&st, 0, 1, 36, 0.0, 22);
    sc_store_set(&st, 0, 2, 42, 0.0, 9);
    sc_store_set(&st, 0, 3, 42, 1.0, 4);          /* the hat on the dst step stays */
    uint8_t only36[128] = { 0 }; only36[36] = 1;
    assert(sc_store_paste(&st, 0, 0.0, 1.0, 0.25, only36, &j) == 1);
    assert(sc_store_match(&st, 0, 36, 1.0, 0.0, 4.0) == 22);
    assert(sc_store_match(&st, 0, 42, 1.0, 0.0, 4.0) == 4);

    /* ---- DOUBLE LOOP: the loop [0,4) onto [4,8) -------------------------- */
    memset(&st, 0, sizeof st);
    sc_store_set(&st, 0, 1, 36, 0.0, 22);
    sc_store_set(&st, 0, 2, 38, 2.5, 11);
    assert(sc_store_paste(&st, 0, 0.0, 4.0, 4.0, NULL, NULL) == 2);
    assert(sc_store_match(&st, 0, 36, 4.0, 0.0, 8.0) == 22);
    assert(sc_store_match(&st, 0, 38, 6.5, 0.0, 8.0) == 11);

    /* ---- clip COPY to another row; replaces what the row held ------------ */
    memset(&st, 0, sizeof st);
    sc_store_set(&st, 0, 1, 36, 0.0, 22);
    sc_store_set(&st, 3, 5, 60, 0.0, 4);          /* stale, on the destination row */
    assert(sc_store_copy_row(&st, 0, 3) == 1);
    assert(sc_store_match(&st, 3, 36, 0.0, 0.0, 4.0) == 22);
    assert(sc_store_match(&st, 3, 60, 0.0, 0.0, 4.0) == SC_ALWAYS);
    assert(sc_store_get(&st, 0, 1) == 22);        /* source kept */

    /* ---- clip DELETE parks the row; Move's Undo brings it back ----------- */
    assert(sc_store_move_row(&st, 0, SC_ROW_STASH + 1) == 1);
    assert(sc_store_match(&st, 0, 36, 0.0, 0.0, 4.0) == SC_ALWAYS);   /* nothing plays */
    {   /* parked rows are never persisted */
        char doc[512]; sc_store_serialize(&st, doc, sizeof doc);
        assert(strstr(doc, "201 ") == NULL);
    }
    assert(sc_store_move_row(&st, SC_ROW_STASH + 1, 0) == 1);
    assert(sc_store_get(&st, 0, 1) == 22);

    /* ---- a single NOTE deleted, then Move's Undo -------------------------- */
    memset(&st, 0, sizeof st); memset(&ring, 0, sizeof ring);
    sc_store_set(&st, 0, 7, 36, 0.5, 11);
    assert(sc_store_prune_keep(&st, &ring, 0, 0.0, 4.0, NULL, 0) == 1);
    assert(sc_store_get(&st, 0, 7) == SC_ALWAYS);
    /* a DIFFERENT note at the same place (a re-tap) does not inherit it */
    assert(sc_store_revive(&st, &ring, 0, 8, 36, 0.5) == 0);
    /* the same note, id and all, does */
    assert(sc_store_revive(&st, &ring, 0, 7, 36, 0.5) == 1);
    assert(sc_store_get(&st, 0, 7) == 11);
    assert(sc_store_revive(&st, &ring, 0, 7, 36, 0.5) == 0);   /* once */
    /* the ring is bounded: the oldest falls out */
    memset(&st, 0, sizeof st); memset(&ring, 0, sizeof ring);
    for (int k = 0; k < SC_PRUNED + 4; k++) sc_store_set(&st, 0, 100 + k, 60, k * 0.1, 3);
    sc_store_prune_keep(&st, &ring, 0, 0.0, 100.0, NULL, 0);
    assert(sc_store_revive(&st, &ring, 0, 100, 60, 0.0) == 0);   /* evicted */
    assert(sc_store_revive(&st, &ring, 0, 100 + SC_PRUNED + 3, 60, (SC_PRUNED + 3) * 0.1) == 1);

    /* ---- journal overflow refuses Undo rather than half-undoing ----------- */
    memset(&st, 0, sizeof st); memset(&j, 0, sizeof j);
    for (int k = 0; k < SC_JREC + 2; k++) sc_store_set(&st, 0, 200 + k, 60, k * 0.1, 3);
    j.jid = 9;
    sc_store_paste(&st, 0, 0.0, 8.0, 8.0, NULL, &j);
    assert(j.overflow);
    assert(sc_journal_apply(&st, &j, 0) == 0);

    /* ---- a clip copied to ANOTHER TRACK: one row out of one store, into
     * another store's row, as text ------------------------------------------ */
    {
        static sc_store_t a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        sc_store_set(&a, 1, 11, 36, 0.0, 22);
        sc_store_set(&a, 1, 12, 36, 4.25, 14);
        sc_store_set(&a, 0, 13, 38, 0.0, 5);          /* another clip: stays home */
        sc_store_set(&b, 2, 99, 60, 1.0, 3);          /* the destination's stale row */
        char doc[2048];
        assert(sc_store_serialize_row(&a, 1, doc, sizeof doc) > 0);
        assert(strstr(doc, " 38 ") == NULL);          /* only row 1 travels */
        assert(sc_store_import_row(&b, 2, doc) == 2);
        assert(sc_store_match(&b, 2, 36, 0.0, 0.0, 8.0) == 22);
        assert(sc_store_match(&b, 2, 36, 4.25, 0.0, 8.0) == 14);
        assert(sc_store_match(&b, 2, 60, 1.0, 0.0, 8.0) == SC_ALWAYS);   /* replaced */
        for (int i = 0; i < SC_STORE_MAX; i++)
            if (b.e[i].used) assert(b.e[i].id < 0);   /* new notes: synthetic until adopted */
        assert(sc_store_count(&a) == 3);              /* the source is untouched */
        /* an empty source row clears the destination (the copy has none) */
        sc_store_serialize_row(&a, 5, doc, sizeof doc);
        assert(sc_store_import_row(&b, 2, doc) == 0);
        assert(sc_store_count(&b) == 0);
        /* garbage is refused and changes nothing */
        sc_store_set(&b, 2, 7, 60, 1.0, 3);
        assert(sc_store_import_row(&b, 2, "junk") == -1);
        assert(sc_store_count(&b) == 1);
        assert(sc_store_import_row(&b, SC_ROW_PARK, "SC 1\n") == -1);   /* never a parking row */
    }

    printf("test_step_chance_follow: PASS\n");
    return 0;
}
