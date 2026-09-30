/* edit_gesture + edit_follow: Move's copy/paste/undo gestures become lane
 * commands ONLY where the live model confirms Move did the edit. */
#include <stdio.h>
#include <string.h>
#include "edit_gesture.h"
#include "edit_follow.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static char log_[16][160];
static int nlog;
static void cmd(void *ctx, int slot, const char *key, const char *val)
{
    (void)ctx;
    if (nlog < 16) snprintf(log_[nlog++], sizeof log_[0], "%d %s %s", slot, key, val);
}
static int logged(const char *s)
{
    for (int i = 0; i < nlog; i++) if (strstr(log_[i], s)) return 1;
    return 0;
}

static void gesture_tests(void)
{
    edit_gesture_t g;
    eg_intent_t in;
    edit_gesture_reset(&g);
    CHECK(!edit_gesture_on_event(&g, 0x90, 16, 100, &in), "a plain step press is no intent");
    edit_gesture_on_event(&g, 0xB0, 60, 127, &in);                     /* Copy down */
    CHECK(edit_gesture_on_event(&g, 0x90, 16, 100, &in) && in.kind == EG_SOURCE && in.src == 0, "source");
    CHECK(edit_gesture_on_event(&g, 0x90, 24, 100, &in) && in.kind == EG_PASTE && in.src == 0 && in.dst == 8 && !in.page, "pair");
    CHECK(edit_gesture_on_event(&g, 0x90, 20, 100, &in) && in.kind == EG_SOURCE && in.src == 4, "pairs, not one-source-many");
    edit_gesture_on_event(&g, 0xB0, 60, 0, &in);                       /* Copy up: the source SURVIVES */
    CHECK(edit_gesture_on_event(&g, 0x90, 30, 100, &in) && in.kind == EG_PASTE && in.src == 4 && in.dst == 14, "armed source pastes after release");
    CHECK(!edit_gesture_on_event(&g, 0x90, 30, 100, &in), "and is then spent");
    edit_gesture_on_event(&g, 0xB0, 58, 127, &in);                     /* Loop + Copy: pages */
    edit_gesture_on_event(&g, 0xB0, 60, 127, &in);
    edit_gesture_on_event(&g, 0x90, 16, 100, &in);
    CHECK(edit_gesture_on_event(&g, 0x90, 17, 100, &in) && in.kind == EG_PASTE && in.page == 1, "page pair");
    edit_gesture_reset(&g);
    CHECK(edit_gesture_on_event(&g, 0xB0, 56, 127, &in) && in.kind == EG_UNDO, "undo");
    edit_gesture_on_event(&g, 0xB0, 49, 127, &in);
    CHECK(edit_gesture_on_event(&g, 0xB0, 56, 127, &in) && in.kind == EG_REDO, "shift+undo is redo");
    CHECK(!edit_gesture_on_event(&g, 0x80, 16, 0, &in), "note-offs are ignored");
    CHECK(edit_gesture_on_event(&g, 0x90, 30, 100, &in) && in.kind == EG_DOUBLE, "shift+step15 is Double Loop");
}

/* A synthetic clip: T2 slot 1, id 500. */
static move_model_t model(uint32_t content)
{
    move_model_t m;
    memset(&m, 0, sizeof m);
    m.valid = 1; m.doc_gen = 1;
    mm_clip_t *c = &m.track[1].slot[0];
    c->exists = 1; c->clip_id = 500; c->content_hash = content; c->notes_len = 37; c->notes_hash = content;
    c->region_end = c->loop_end = 4; c->loop_on = 1;
    return m;
}
static ef_notes_t notes(const mm_note_t *n, int count, uint32_t content)
{
    ef_notes_t e = { n, count, { 1, 1, 0, 500, content } };
    return e;
}
static ef_intent_t paste(double src, double dst, double len, uint32_t pre)
{
    ef_intent_t in = { EF_PASTE, 1, 0, 500, src, dst, len, pre, 0 };
    return in;
}
/* The state hash of the synthetic clip with a given content hash. */
static uint32_t st(uint32_t content) { move_model_t m = model(content); return mm_clip_state_hash(&m.track[1].slot[0]); }

static void follow_tests(void)
{
    /* Melodic: step 1 (60) pasted onto step 5 which holds 29 and 31 -- Move ADDS 60 there. */
    mm_note_t pre[] = { {60, 0.0, 0.25, 100, 1, 0, -1, 0}, {29, 1.0, 0.25, 100, 2, 0, -1, 0}, {31, 1.0, 0.25, 100, 3, 0, -1, 0} };
    mm_note_t post[] = { {60, 0.0, 0.25, 100, 1, 0, -1, 0}, {29, 1.0, 0.25, 100, 2, 0, -1, 0}, {31, 1.0, 0.25, 100, 3, 0, -1, 0}, {60, 1.0, 0.25, 100, 4, 0, -1, 0} };
    ef_notes_t P = notes(pre, 3, 0xA), Q = notes(post, 4, 0xB);
    CHECK(edit_follow_is_paste(&P, &Q, 0.0, 1.0, 0.25) == 1, "a real paste is confirmed");
    CHECK(edit_follow_is_paste(&P, &P, 0.0, 1.0, 0.25) == -1, "nothing new yet");
    CHECK(edit_follow_is_paste(&P, &Q, 0.0, 2.0, 0.25) == 0, "new note outside the destination: not this paste");
    mm_note_t odd[] = { {60, 0.0, 0.25, 100, 1, 0, -1, 0}, {29, 1.0, 0.25, 100, 2, 0, -1, 0}, {31, 1.0, 0.25, 100, 3, 0, -1, 0}, {61, 1.0, 0.25, 100, 4, 0, -1, 0} };
    ef_notes_t O = notes(odd, 4, 0xB);
    CHECK(edit_follow_is_paste(&P, &O, 0.0, 1.0, 0.25) == 0, "a different pitch is not a copy (the user played it)");
    /* Paste onto an occupied step REPLACES a same-pitch note: old id gone, new id -- still confirmed. */
    mm_note_t pre2[] = { {36, 0.0, 0.25, 100, 1, 0, -1, 0}, {36, 1.0, 0.25, 100, 2, 0, -1, 0} };
    mm_note_t post2[] = { {36, 0.0, 0.25, 100, 1, 0, -1, 0}, {36, 1.0, 0.25, 100, 4, 0, -1, 0} };
    ef_notes_t P2 = notes(pre2, 2, 0xA), Q2 = notes(post2, 2, 0xB);
    CHECK(edit_follow_is_paste(&P2, &Q2, 0.0, 1.0, 0.25) == 1, "replace-by-pitch is a paste");

    /* The full path: intent, Move's change, the command. */
    edit_follow_reset(); nlog = 0;
    move_model_t m0 = model(0xA), m1 = model(0xB);
    ef_intent_t in = paste(0.0, 1.0, 0.25, st(0xA));
    in.t_ms = 1000;
    edit_follow_intent(&in);
    edit_follow_on_change(&m1, &m0, &Q, &P, 1010, cmd, NULL);
    CHECK(nlog == 1 && logged("1 lanes:paste_span 1 0 0 1 0.25 1 v=60"), "paste mirrored, naming the pasted voice: %s", nlog ? log_[0] : "(none)");

    /* Move's Undo: the clip returns EXACTLY to 0xA -> journal undo. */
    nlog = 0;
    ef_intent_t u = { EF_UNDO, 1, 0, 500, 0, 0, 0, st(0xB), 2000 };
    edit_follow_intent(&u);
    edit_follow_on_change(&m0, &m1, &P, &Q, 2010, cmd, NULL);
    CHECK(nlog == 1 && logged("lanes:journal undo 1"), "undo mirrored");
    /* ...and Redo returns it to 0xB. */
    nlog = 0;
    ef_intent_t r = { EF_REDO, 1, 0, 500, 0, 0, 0, st(0xA), 3000 };
    edit_follow_intent(&r);
    edit_follow_on_change(&m1, &m0, &Q, &P, 3010, cmd, NULL);
    CHECK(nlog == 1 && logged("lanes:journal redo 1"), "redo mirrored");

    /* An Undo that lands somewhere else (it undid an unrelated note edit)
     * issues nothing -- and a partial state keeps the intent waiting. */
    nlog = 0;
    move_model_t mX = model(0xC);
    ef_intent_t u2 = { EF_UNDO, 1, 0, 500, 0, 0, 0, st(0xB), 4000 };
    edit_follow_intent(&u2);
    edit_follow_on_change(&mX, &m1, NULL, NULL, 4010, cmd, NULL);
    CHECK(nlog == 0, "an unrelated undo is ignored");
    edit_follow_on_change(&m0, &mX, NULL, NULL, 4030, cmd, NULL);
    CHECK(nlog == 1 && logged("undo 1"), "...but reaching the pre-state in a second tick still lands");

    /* MOVE'S UNDO IS THE DOCUMENT'S: the paste was on T2's clip, the user has
     * since selected T1 (the intent carries T1's clip, or none at all), and
     * Move's Undo reverts T2's clip -- the lane paste must follow it. */
    edit_follow_reset(); nlog = 0;
    {
        ef_intent_t pin = paste(0.0, 1.0, 0.25, st(0xA));
        pin.t_ms = 20000;
        edit_follow_intent(&pin);
        edit_follow_on_change(&m1, &m0, &Q, &P, 20010, cmd, NULL);
        CHECK(nlog == 1 && logged("paste_span"), "pasted");
        nlog = 0;
        ef_intent_t elsewhere = { EF_UNDO, 0, 0, 600, 0, 0, 0, 0, 21000 };   /* T1's clip, or nothing */
        edit_follow_intent(&elsewhere);
        edit_follow_on_change(&m0, &m1, NULL, NULL, 21010, cmd, NULL);
        CHECK(nlog == 1 && logged("1 lanes:journal undo"), "undo followed the clip Move reverted, not the one selected: %s",
              nlog ? log_[0] : "(none)");
        /* The paste's "after" settles for a moment: Move landing its own
         * automation a tick later must not strand the Redo. */
        edit_follow_reset(); nlog = 0;
        edit_follow_intent(&pin);
        edit_follow_on_change(&m1, &m0, &Q, &P, 22010, cmd, NULL);
        move_model_t m1b = model(0xD);                      /* Move's envelopes, a tick later */
        edit_follow_tick(&m1b, &Q, &Q, 22030, cmd, NULL);
        edit_follow_tick(&m1b, &Q, &Q, 22600, cmd, NULL);   /* settled */
        nlog = 0;
        ef_intent_t un = { EF_UNDO, 0, 0, 0, 0, 0, 0, 0, 23000 };
        edit_follow_intent(&un);
        edit_follow_on_change(&m0, &m1b, NULL, NULL, 23010, cmd, NULL);
        ef_intent_t re = { EF_REDO, 0, 0, 0, 0, 0, 0, 0, 23100 };
        edit_follow_intent(&re);
        edit_follow_on_change(&m1b, &m0, NULL, NULL, 23110, cmd, NULL);
        CHECK(nlog == 2 && logged("lanes:journal undo") && logged("lanes:journal redo"), "redo matches the SETTLED after-state (%d)", nlog);
    }

    /* A declined paste (an "empty" source: Move changed nothing new) never
     * issues a command, and times out. */
    edit_follow_reset(); nlog = 0;
    ef_intent_t e = paste(2.0, 3.0, 0.25, st(0xA));
    e.t_ms = 5000;
    edit_follow_intent(&e);
    edit_follow_tick(&m0, &P, &P, 5100, cmd, NULL);
    edit_follow_tick(&m0, &P, &P, 5700, cmd, NULL);
    CHECK(nlog == 0 && edit_follow_stats().expired == 1, "declined paste expired silently");

    /* The race: the change was published BEFORE the intent was read. */
    edit_follow_reset(); nlog = 0;
    ef_intent_t late = paste(0.0, 1.0, 0.25, st(0xA));
    late.t_ms = 6000;
    edit_follow_intent(&late);
    edit_follow_tick(&m1, &Q, &P, 6010, cmd, NULL);
    CHECK(nlog == 1 && logged("paste_span"), "late intent still confirmed on the tick");

    /* Double Loop on an EMPTY clip: confirmed by geometry, not notes. */
    edit_follow_reset(); nlog = 0;
    move_model_t d0 = model(0xA), d1 = model(0xA);
    d1.track[1].slot[0].loop_end = d1.track[1].slot[0].region_end = 8;
    ef_intent_t dbl = { EF_DOUBLE, 1, 0, 500, 0.0, 4.0, 4.0, st(0xA), 9000 };
    edit_follow_intent(&dbl);
    edit_follow_on_change(&d1, &d0, NULL, NULL, 9010, cmd, NULL);
    CHECK(nlog == 1 && logged("lanes:paste_span 1 0 0 4 4"), "double mirrored as a paste: %s", nlog ? log_[0] : "");
    /* ...and its Undo (geometry back) is matched on the STATE hash. */
    nlog = 0;
    ef_intent_t du = { EF_UNDO, 1, 0, 500, 0, 0, 0, mm_clip_state_hash(&d1.track[1].slot[0]), 9100 };
    edit_follow_intent(&du);
    edit_follow_on_change(&d0, &d1, NULL, NULL, 9110, cmd, NULL);
    CHECK(nlog == 1 && logged("lanes:journal undo"), "double undone");
    /* A Shift+step-15 that Move did not act on (loop unchanged) never lands. */
    edit_follow_reset(); nlog = 0;
    edit_follow_intent(&dbl);
    move_model_t d2 = model(0xB);
    edit_follow_on_change(&d2, &d0, NULL, NULL, 9210, cmd, NULL);
    CHECK(nlog == 0, "no doubling, no paste");

    /* Clips: delete -> stash; the SAME id back (Move's Undo) -> unstash, even
     * in another slot; a new id with the same content -> copy. */
    edit_follow_reset(); nlog = 0;
    move_model_t with = model(0xA), without = model(0xA);
    memset(&without.track[1].slot[0], 0, sizeof(mm_clip_t));
    edit_follow_on_change(&without, &with, NULL, NULL, 7000, cmd, NULL);
    CHECK(nlog == 1 && logged("1 lanes:stash 1 0 1"), "stash: %s", nlog ? log_[0] : "");
    nlog = 0;
    edit_follow_on_change(&with, &without, NULL, NULL, 7100, cmd, NULL);
    CHECK(nlog == 1 && logged("lanes:unstash 1 1 0"), "unstash on the same id");
    nlog = 0;
    move_model_t dup = with;
    dup.track[1].slot[3] = with.track[1].slot[0];
    dup.track[1].slot[3].clip_id = 777;
    edit_follow_on_change(&dup, &with, NULL, NULL, 7200, cmd, NULL);
    CHECK(nlog == 1 && logged("lanes:copy_clip 0 3"), "copy");
    nlog = 0;
    move_model_t remade = with;
    remade.track[1].slot[0].clip_id = 901;          /* deleted and remade: a stranger */
    edit_follow_on_change(&remade, &with, NULL, NULL, 7300, cmd, NULL);
    CHECK(logged("lanes:stash 1 0") && !logged("unstash"), "a new clip in the slot starts clean");

    /* A set load clears everything and issues nothing. */
    nlog = 0;
    move_model_t other = model(0xA);
    other.doc_gen = 2;
    other.track[1].slot[0].clip_id = 42;
    edit_follow_on_change(&other, &with, NULL, NULL, 8000, cmd, NULL);
    CHECK(nlog == 0, "a set load is not a mass deletion");
}

int main(void)
{
    gesture_tests();
    follow_tests();
    if (fails) { printf("test_edit_follow: %d FAILED\n", fails); return 1; }
    printf("test_edit_follow: PASS\n");
    return 0;
}
