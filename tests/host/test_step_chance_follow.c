/*
 * Chance follows Move's edits: paste (and Double Loop), its Undo/Redo, clip
 * copy on the same track, clip delete + Undo, and the shim's adoption of a
 * copied condition by Move's real note id.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "step_chance_follow.h"

static sc_store_t st;
static sc_journal_t j;

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

    /* ---- journal overflow refuses Undo rather than half-undoing ----------- */
    memset(&st, 0, sizeof st); memset(&j, 0, sizeof j);
    for (int k = 0; k < SC_JREC + 2; k++) sc_store_set(&st, 0, 200 + k, 60, k * 0.1, 3);
    j.jid = 9;
    sc_store_paste(&st, 0, 0.0, 8.0, 8.0, NULL, &j);
    assert(j.overflow);
    assert(sc_journal_apply(&st, &j, 0) == 0);

    printf("test_step_chance_follow: PASS\n");
    return 0;
}
