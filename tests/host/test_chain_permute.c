/*
 * Renumbering a chain section without unloading anything.
 *
 * chain_permute.h is the operation that replaced "rewrite every <id>:module".
 * The thing it must never do is lose track of WHICH ARRAY ENTRY belongs to
 * which module: every per-position array has to move as one, and every routing
 * that names a position by string has to follow. A permutation that gets one of
 * those wrong leaves a module driving another module's parameters, which is
 * worse than the reload it replaced.
 *
 * The fixtures are deliberately PARALLEL arrays with distinguishable values, so
 * a shift that lands one place off, or that moves one array and not another, is
 * visible rather than plausible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain_permute.h"

static int failures;
static void fail(const char *m) { printf("FAIL: %s\n", m); failures++; }

#define CAP 8

/*
 * Arrays of different strides, standing in for the families a real chain
 * position is made of -- and CRUCIALLY including an OWNED-BUFFER one.
 *
 * The fixture used to be value arrays only. That is precisely why 28 mutations
 * all passed while a null dereference was live on hardware: nothing here had
 * the shape of `chain_param_info_t *fx_params[]`, so no test could observe that
 * vacating a position zeroed its buffer POINTER instead of its contents.
 */
#define OWNED_BYTES 16
typedef struct {
    void *ptr[CAP];          /* like fx_instances -- a value, legitimately null */
    int   flag[CAP];         /* like fx_bypassed    */
    char  name[CAP][8];      /* like current_fx_modules */
    char *owned[CAP];        /* like fx_params -- allocated once, never null */
} world_t;

/* Value arrays ONLY. Some cases need a descriptor with no owned buffers in it:
   the owned-buffer pre-check reads one entry PAST the live range (the spare an
   insert rotates in), so at a full section it is looking at a position that
   does not exist -- and a refusal that happens to come from there would mask
   whether the cap is being enforced at all. Isolate the cap with these. */
static int collect_values(world_t *w, chain_perm_array_t *a) {
    a[0].base = w->ptr;   a[0].elem = sizeof(w->ptr[0]);   a[0].pointee = 0;
    a[1].base = w->flag;  a[1].elem = sizeof(w->flag[0]);  a[1].pointee = 0;
    a[2].base = w->name;  a[2].elem = sizeof(w->name[0]);  a[2].pointee = 0;
    return 3;
}

static int collect(world_t *w, chain_perm_array_t *a) {
    a[0].base = w->ptr;   a[0].elem = sizeof(w->ptr[0]);   a[0].pointee = 0;
    a[1].base = w->flag;  a[1].elem = sizeof(w->flag[0]);  a[1].pointee = 0;
    a[2].base = w->name;  a[2].elem = sizeof(w->name[0]);  a[2].pointee = 0;
    a[3].base = w->owned; a[3].elem = sizeof(w->owned[0]); a[3].pointee = OWNED_BYTES;
    return 4;
}

/*
 * THE INVARIANT THE CRASH BROKE, in one assertion.
 *
 * Every owned buffer is still there and still distinct: no index is null (the
 * segfault) and the multiset of pointers is unchanged (the leak -- a shift that
 * overwrites the pointer already at the destination loses that allocation, and
 * nothing else holds it). It deliberately does not care WHICH index each buffer
 * ended up at: a permutation is free to move them, it is not free to invent or
 * drop one.
 */
static void snapshot_owned(world_t *w, void **out) {
    for (int i = 0; i < CAP; i++) out[i] = w->owned[i];
}

static void check_owned_conserved(world_t *w, void **before, const char *what) {
    char msg[192];
    for (int i = 0; i < CAP; i++) {
        if (!w->owned[i]) {
            snprintf(msg, sizeof msg,
                     "%s: owned buffer at position %d is NULL -- the next module "
                     "load writes its param table straight through it", what, i);
            fail(msg);
            return;
        }
    }
    for (int i = 0; i < CAP; i++) {
        int seen = 0;
        for (int k = 0; k < CAP; k++) if ((void *)w->owned[k] == before[i]) seen++;
        if (seen != 1) {
            snprintf(msg, sizeof msg,
                     "%s: buffer %d appears %d times afterwards -- a permutation "
                     "must not allocate or LEAK one", what, i, seen);
            fail(msg);
            return;
        }
    }
}

/* Positions 0..n-1 hold "a","b","c"... with matching ptr and flag, so every
 * array can be checked to have moved the SAME way. */
static void seed(world_t *w, int n) {
    /* The owned buffers persist across seeds, like chain_alloc_position_storage
       allocating them once per instance: a test that reallocated them each time
       could not tell a rotation from a fresh allocation. */
    static char *pool[CAP];
    static int pooled;
    if (!pooled) { for (int i = 0; i < CAP; i++) pool[i] = calloc(1, OWNED_BYTES); pooled = 1; }
    memset(w, 0, sizeof(*w));
    for (int i = 0; i < CAP; i++) {
        w->owned[i] = pool[i];
        /* Distinguishable CONTENTS, so a buffer that travelled with its module
           can be told from one that merely stayed at the same index. */
        snprintf(pool[i], OWNED_BYTES, "buf%d", i);
    }
    for (int i = 0; i < n; i++) {
        w->ptr[i] = (void *)(long)(0x100 + i);
        w->flag[i] = 10 + i;
        w->name[i][0] = (char)('a' + i);
        w->name[i][1] = '\0';
    }
}

/* "a,b,c" for the first n positions; "-" for an empty one. */
static const char *shape(world_t *w, int n, char *buf) {
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i) strcat(buf, ",");
        strcat(buf, w->name[i][0] ? w->name[i] : "-");
    }
    return buf;
}

/* Every array agrees about what is at each position: the ptr, the flag and the
 * name at position i all belong to the same original module. This is the check
 * that catches "moved one array and not another". */
static void check_parallel(world_t *w, int n, const char *what) {
    for (int i = 0; i < n; i++) {
        if (!w->name[i][0]) {
            if (w->ptr[i] || w->flag[i])
                fail("an emptied position kept a stale pointer or flag");
            continue;
        }
        int orig = w->name[i][0] - 'a';
        if (w->ptr[i] != (void *)(long)(0x100 + orig) || w->flag[i] != 10 + orig) {
            char msg[160];
            snprintf(msg, sizeof msg,
                     "%s: position %d holds module %s but the pointer/flag of another",
                     what, i, w->name[i]);
            fail(msg);
        }
    }
}

int main(void) {
    char buf[64];
    world_t w;
    chain_perm_array_t a[8];
    int map[CHAIN_PERM_MAX_POS];

    /* ---- INSERT ---------------------------------------------------------- */

    /* 1. At the head: everything shifts along and position 0 is a clean hole. */
    seed(&w, 3);
    if (chain_perm_insert(a, collect(&w, a), 3, CAP, 0, map) != 4)
        fail("inserting at the head did not grow the section");
    if (strcmp(shape(&w, 4, buf), "-,a,b,c") != 0)
        fail("insert at the head produced the wrong order");
    check_parallel(&w, 4, "insert head");
    if (map[0] != 1 || map[1] != 2 || map[2] != 3)
        fail("the insert map does not say where each module went");

    /* 2. A hole is ZEROED, not a copy of its neighbour. A chain walk decides
          per position whether a plugin is loaded; a duplicated pointer there
          would run one module twice. */
    if (w.ptr[0] != NULL || w.flag[0] != 0 || w.name[0][0] != '\0')
        fail("the inserted hole was not cleared");

    /* 3. In the middle, and at the end (which is an append). */
    seed(&w, 3);
    chain_perm_insert(a, collect(&w, a), 3, CAP, 1, map);
    if (strcmp(shape(&w, 4, buf), "a,-,b,c") != 0) fail("insert at 1 misplaced");
    seed(&w, 3);
    if (chain_perm_insert(a, collect(&w, a), 3, CAP, 3, map) != 4)
        fail("insert at the end was refused");
    if (strcmp(shape(&w, 4, buf), "a,b,c,-") != 0) fail("insert at the end misplaced");

    /* 4. Refused where it would corrupt: past the end, negative, or full. */
    seed(&w, 3);
    if (chain_perm_insert(a, collect(&w, a), 3, CAP, 4, map) != -1)
        fail("insert past the end was accepted");
    if (chain_perm_insert(a, collect(&w, a), 3, CAP, -1, map) != -1)
        fail("insert at a negative position was accepted");
    seed(&w, CAP);
    if (chain_perm_insert(a, collect_values(&w, a), CAP, CAP, 0, map) != -1)
        fail("insert into a FULL section was accepted, which would shift past the array");
    if (strcmp(shape(&w, CAP, buf), "a,b,c,d,e,f,g,h") != 0)
        fail("a refused insert still modified the section");
    /* ...and with the owned arrays in play too, which is the real descriptor. */
    seed(&w, CAP);
    if (chain_perm_insert(a, collect(&w, a), CAP, CAP, 0, map) != -1)
        fail("insert into a FULL section with owned buffers was accepted");
    if (strcmp(shape(&w, CAP, buf), "a,b,c,d,e,f,g,h") != 0)
        fail("a refused full-section insert still modified the section");

    /* ---- REMOVE ---------------------------------------------------------- */

    /* 5. The gap closes and the vacated tail is cleared. Leaving it would keep
          a second reachable copy of the module that used to be last. */
    seed(&w, 3);
    if (chain_perm_remove(a, collect(&w, a), 3, 0, map) != 2)
        fail("removing did not shrink the section");
    if (strcmp(shape(&w, 3, buf), "b,c,-") != 0)
        fail("remove did not close the gap and clear the tail");
    check_parallel(&w, 3, "remove head");
    if (map[0] != -1 || map[1] != 0 || map[2] != 1)
        fail("the remove map does not mark the removed position as gone");

    /* 6. From the middle and from the end. */
    seed(&w, 3);
    chain_perm_remove(a, collect(&w, a), 3, 1, map);
    if (strcmp(shape(&w, 3, buf), "a,c,-") != 0) fail("remove from the middle misplaced");
    seed(&w, 3);
    chain_perm_remove(a, collect(&w, a), 3, 2, map);
    if (strcmp(shape(&w, 3, buf), "a,b,-") != 0) fail("remove from the end misplaced");

    /* 7. Refused out of range, and an empty section cannot be removed from. */
    seed(&w, 2);
    if (chain_perm_remove(a, collect(&w, a), 2, 2, map) != -1)
        fail("remove past the end was accepted");
    if (chain_perm_remove(a, collect(&w, a), 0, 0, map) != -1)
        fail("remove from an empty section was accepted");

    /* ---- MOVE ------------------------------------------------------------ */

    /* 8. Rightwards and leftwards rotate the span between, they do not swap.
          A swap would be a different edit: moving fx1 to fx3 must leave fx2 and
          fx3 in their original relative order, one place earlier. */
    seed(&w, 4);
    if (chain_perm_move(a, collect(&w, a), 4, 0, 2, map) != 4)
        fail("a move changed the section length");
    if (strcmp(shape(&w, 4, buf), "b,c,a,d") != 0)
        fail("moving right swapped instead of rotating");
    check_parallel(&w, 4, "move right");
    if (map[0] != 2 || map[1] != 0 || map[2] != 1 || map[3] != 3)
        fail("the move map does not describe the rotation");

    seed(&w, 4);
    chain_perm_move(a, collect(&w, a), 4, 3, 1, map);
    if (strcmp(shape(&w, 4, buf), "a,d,b,c") != 0)
        fail("moving left swapped instead of rotating");
    check_parallel(&w, 4, "move left");

    /* 9. Adjacent move, the Shift+jog gesture. */
    seed(&w, 3);
    chain_perm_move(a, collect(&w, a), 3, 0, 1, map);
    if (strcmp(shape(&w, 3, buf), "b,a,c") != 0) fail("an adjacent move misplaced");

    /* 10. Refused: out of range either end, and a move to itself (so a caller
           cannot announce a move that did not happen). */
    seed(&w, 3);
    if (chain_perm_move(a, collect(&w, a), 3, 1, 1, map) != -1)
        fail("a move onto itself was accepted");
    if (chain_perm_move(a, collect(&w, a), 3, 0, 3, map) != -1)
        fail("a move past the end was accepted");
    if (chain_perm_move(a, collect(&w, a), 3, -1, 0, map) != -1)
        fail("a move from a negative position was accepted");
    if (strcmp(shape(&w, 3, buf), "a,b,c") != 0)
        fail("a refused move still modified the section");

    /* 11. An element larger than the scratch is REFUSED, not truncated. The
           scratch is a stack buffer in the audio callback; silently copying
           part of an element would corrupt a live plugin pointer. */
    {
        static char huge[CAP][CHAIN_PERM_MAX_ELEM + 1];
        chain_perm_array_t big = { huge, sizeof(huge[0]), 0 };
        if (chain_perm_move(&big, 1, 3, 0, 1, map) != -1)
            fail("an oversized per-position element was permuted through a smaller scratch");
        if (chain_perm_insert(&big, 1, 3, CAP, 0, map) != -1)
            fail("insert accepted an oversized element");
    }

    /* ---- OWNED BUFFERS ARE ROTATED, NEVER ZEROED ------------------------- */

    /*
     * THE HARDWARE CRASH. Loading a MIDI FX in front of an existing one is an
     * insert at position 0, and the vacated position had its param-table
     * POINTER zeroed rather than its contents. v2_load_midi_fx_slot then parsed
     * the new module's params straight through that null: SIGSEGV, si_addr=0,
     * on the SCHED_FIFO SPI callback. The same memmove also overwrote the
     * pointer already at the destination, leaking that allocation.
     */
    {
        void *before[CAP];
        seed(&w, 3);
        snapshot_owned(&w, before);
        if (chain_perm_insert(a, collect(&w, a), 3, CAP, 0, map) != 4)
            fail("insert with an owned array was refused");
        check_owned_conserved(&w, before, "insert at the head");

        /* The new position gets a buffer whose CONTENTS are cleared -- which is
           what "empty" has to mean when the emptiness is a heap block. */
        if (w.owned[0][0] != '\0')
            fail("the inserted position buffer contents were not cleared -- it is "
                 "holding the metadata of whichever module used to be there");
        /* ...and the module that was pushed along KEEPS its buffer contents. */
        if (strcmp(w.owned[1], "buf0") != 0)
            fail("the module pushed to position 1 lost its metadata");
    }

    /* And in the middle, and at the end, and for a remove and a move -- the
       same one assertion, because it does not care where anything went. */
    {
        void *before[CAP];
        seed(&w, 3); snapshot_owned(&w, before);
        chain_perm_insert(a, collect(&w, a), 3, CAP, 1, map);
        check_owned_conserved(&w, before, "insert in the middle");

        seed(&w, 3); snapshot_owned(&w, before);
        chain_perm_insert(a, collect(&w, a), 3, CAP, 3, map);
        check_owned_conserved(&w, before, "insert at the end");

        seed(&w, 3); snapshot_owned(&w, before);
        chain_perm_remove(a, collect(&w, a), 3, 0, map);
        check_owned_conserved(&w, before, "remove");
        if (w.owned[2][0] != '\0')
            fail("the tail a removal vacated kept the departed module metadata");

        seed(&w, 4); snapshot_owned(&w, before);
        chain_perm_move(a, collect(&w, a), 4, 0, 2, map);
        check_owned_conserved(&w, before, "move");
        /* A move vacates NOTHING, so nothing may be cleared: EVERY module keeps
           the metadata it came with, not just the one that was dragged. */
        {
            const char *want[4] = { "buf1", "buf2", "buf0", "buf3" };
            for (int i = 0; i < 4; i++) {
                if (strcmp(w.owned[i], want[i]) != 0) {
                    char msg[160];
                    snprintf(msg, sizeof msg,
                             "a move left position %d holding \"%s\" instead of \"%s\" "
                             "-- a move must rotate metadata, never clear it",
                             i, w.owned[i], want[i]);
                    fail(msg);
                }
            }
        }
    }

    /* A descriptor that claims owned buffers but has a null one is REFUSED
       rather than followed -- there is no buffer to invent, and a fixture that
       skipped the allocation must not get a segfault two calls later. */
    {
        seed(&w, 3);
        w.owned[1] = NULL;
        if (chain_perm_insert(a, collect(&w, a), 3, CAP, 0, map) != -1)
            fail("insert accepted an owned array with a NULL buffer in it");
        if (chain_perm_remove(a, collect(&w, a), 3, 0, map) != -1)
            fail("remove accepted an owned array with a NULL buffer in it");
        if (chain_perm_move(a, collect(&w, a), 3, 0, 1, map) != -1)
            fail("move accepted an owned array with a NULL buffer in it");
        seed(&w, 3);  /* restore the pool entry for later cases */
    }

    /* A mis-declared descriptor -- owned buffers whose stride is not a pointer
       -- is refused, because the rotation would reinterpret arbitrary bytes as
       an address and then memset through it.

       Every entry is non-zero on purpose: with holes in it the null pre-check
       would refuse this for the WRONG reason and the stride guard would never
       be exercised at all. */
    {
        seed(&w, 3);
        for (int i = 0; i < CAP; i++) w.flag[i] = 0x2A;
        chain_perm_array_t bad = { w.flag, sizeof(w.flag[0]), OWNED_BYTES };
        if (chain_perm_insert(&bad, 1, 3, CAP, 0, map) != -1)
            fail("a value array declared as owned-buffer was accepted -- the rotation "
                 "would treat its bytes as an address");
        if (chain_perm_remove(&bad, 1, 3, 0, map) != -1)
            fail("remove accepted a value array declared as owned-buffer");
        if (chain_perm_move(&bad, 1, 3, 0, 1, map) != -1)
            fail("move accepted a value array declared as owned-buffer");
    }

    /* ---- RETARGETING THE STRING KEYS ------------------------------------- */

    /* An LFO, a modulation entry and a knob mapping all name a position as
       "fx2". Moving the arrays and not these silently re-aims every routing at
       whatever slid into the position it named. */

    /* 12. Follows the module through an insert at the head. */
    {
        char id[16] = "fx1";
        seed(&w, 2);
        chain_perm_insert(a, collect(&w, a), 2, CAP, 0, map);
        if (chain_perm_retarget(id, sizeof id, "fx", CAP, map, 2) != 1 ||
            strcmp(id, "fx2") != 0)
            fail("a routing aimed at fx1 did not follow it to fx2 after a head insert");
    }

    /* 13. ONE pass, never a sequence of renames. Renaming 1->2 and then 2->3
           would move the same id twice and land it two places away. */
    {
        char ids[3][16] = { "fx1", "fx2", "fx3" };
        seed(&w, 3);
        chain_perm_insert(a, collect(&w, a), 3, CAP, 0, map);
        for (int i = 0; i < 3; i++) chain_perm_retarget(ids[i], 16, "fx", CAP, map, 3);
        if (strcmp(ids[0], "fx2") || strcmp(ids[1], "fx3") || strcmp(ids[2], "fx4"))
            fail("retargeting a whole table double-applied the shift");
    }

    /* 14. A routing whose module LEFT is cleared, and says so. */
    {
        char id[16] = "fx2";
        seed(&w, 3);
        chain_perm_remove(a, collect(&w, a), 3, 1, map);
        if (chain_perm_retarget(id, sizeof id, "fx", CAP, map, 3) != -1 || id[0] != '\0')
            fail("a routing aimed at a REMOVED module was not cleared");
    }

    /* 15. ...but one aimed at a module that only MOVED UP keeps pointing at it,
           rather than at whatever took its old index. */
    {
        char id[16] = "fx3";
        seed(&w, 3);
        chain_perm_remove(a, collect(&w, a), 3, 0, map);
        if (chain_perm_retarget(id, sizeof id, "fx", CAP, map, 3) != 1 ||
            strcmp(id, "fx2") != 0)
            fail("a routing aimed at fx3 did not follow it down to fx2 after a removal");
    }

    /* 16. The OTHER section, the synth and an LFO are none of this
           permutation's business. "midi_fx1" must not match prefix "fx". */
    for (int i = 0; i < 3; i++) {
        const char *src[3] = { "midi_fx1", "synth", "lfo1" };
        char id[16];
        snprintf(id, sizeof id, "%s", src[i]);
        seed(&w, 2);
        chain_perm_insert(a, collect(&w, a), 2, CAP, 0, map);
        if (chain_perm_retarget(id, sizeof id, "fx", CAP, map, 2) != 0 ||
            strcmp(id, src[i]) != 0) {
            char msg[96];
            snprintf(msg, sizeof msg, "an fx permutation rewrote %s to %s", src[i], id);
            fail(msg);
        }
    }

    /* 17. And it works the same way for the midi_fx prefix, which is the
           section the reported bug was about. */
    {
        char id[16] = "midi_fx1";
        seed(&w, 2);
        chain_perm_insert(a, collect(&w, a), 2, CAP, 0, map);
        if (chain_perm_retarget(id, sizeof id, "midi_fx", CAP, map, 2) != 1 ||
            strcmp(id, "midi_fx2") != 0)
            fail("a MIDI FX routing did not follow its module across a head insert");
    }

    /* 18. An id naming a position beyond what moved is left alone rather than
           read off the end of the map. */
    {
        char id[16] = "fx5";
        seed(&w, 2);
        chain_perm_insert(a, collect(&w, a), 2, CAP, 0, map);
        if (chain_perm_retarget(id, sizeof id, "fx", CAP, map, 2) != 0 ||
            strcmp(id, "fx5") != 0)
            fail("an id past the permuted range was rewritten");
    }

    if (failures) return 1;
    printf("PASS: chain permute - insert/remove/move shift every parallel array "
           "together, clear what they vacate, refuse what would corrupt, and "
           "carry each position string routing to where its module actually went\n");
    return 0;
}
