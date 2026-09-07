/*
 * lfo_process_midi must loop the COUNT IT IS GIVEN, not the global LFO_COUNT.
 *
 * The function takes a pointer to an array but bounded its loop by the macro.
 * That was harmless only because the two collections it serves happened to be
 * the same size: a slot's LFOs (LFO_COUNT) and Master FX's
 * (MASTER_FX_LFO_COUNT), both 2. Take the slot to 8 while Master FX stays at 2
 * and the same call walks six elements past shadow_master_fx_lfos, writing to
 * them, on the SPI callback.
 *
 * HOW THIS TELLS THE TWO IMPLEMENTATIONS APART, and why it does not use -D.
 *
 * The obvious rig is to compile with -DLFO_COUNT=8 so the macro is wrong for a
 * 2-element array. It does not work: lfo_common.h defines LFO_COUNT
 * unconditionally, so the header's 2 REDEFINES the command line's 8 and the
 * test goes green against either implementation -- a probe measuring the wrong
 * thing and reporting success.
 *
 * So the array is SHORTER than LFO_COUNT instead: one real route, then a guard.
 * Called with count = 1. A loop bounded by the macro runs twice and lands on the
 * guard; a loop bounded by the parameter runs once and does not. No macro games,
 * and the discrimination is structural.
 */
#include <stdio.h>
#include <string.h>
#include "lfo_common.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } \
    else printf("  ok  %s\n", (msg)); \
} while (0)

/* The rig only discriminates while the array is shorter than the macro. If
 * LFO_COUNT ever becomes 1, this test silently stops testing anything. */
#if LFO_COUNT < 2
#error "test_lfo_process_midi_count needs LFO_COUNT >= 2 to discriminate"
#endif

int main(void) {
    /* One real route, then a guard. Laid out as one struct so the guard is
     * genuinely the next object in memory rather than wherever the allocator
     * happened to put a separate variable. */
    struct { lfo_state_t routes[1]; lfo_state_t guard; } arena;
    memset(&arena, 0, sizeof(arena));
    arena.routes[0].retrigger = 1;
    arena.guard.retrigger = 1;
    arena.guard.phase = 0.75;

    const uint8_t note_on[3]    = { 0x90, 60, 100 };
    const uint8_t note_on2[3]   = { 0x90, 64, 100 };
    const uint8_t note_off[3]   = { 0x80, 60, 0 };
    const uint8_t note_on_v0[3] = { 0x90, 60, 0 };

    /* ---- the discriminating case: count < LFO_COUNT -------------------- */
    lfo_process_midi(arena.routes, 1, note_on, 3);
    CHECK(arena.routes[0].held_count == 1, "the one route counted the note on");
    CHECK(arena.guard.held_count == 0,
          "the element past the array was NOT touched (count, not LFO_COUNT)");
    CHECK(arena.guard.phase == 0.75, "the guard's phase was NOT reset");

    /* ---- behaviour is otherwise unchanged ------------------------------ */
    /* Retrigger fires on the FIRST note of a phrase, not on every note. */
    arena.routes[0].phase = 0.5;
    lfo_process_midi(arena.routes, 1, note_on2, 3);
    CHECK(arena.routes[0].phase == 0.5, "a second held note does not retrigger");
    CHECK(arena.routes[0].held_count == 2, "held_count reached 2");

    lfo_process_midi(arena.routes, 1, note_off, 3);
    lfo_process_midi(arena.routes, 1, note_off, 3);
    CHECK(arena.routes[0].held_count == 0, "held_count returned to 0");

    arena.routes[0].phase = 0.5;
    lfo_process_midi(arena.routes, 1, note_on, 3);
    CHECK(arena.routes[0].phase == 0.0, "the first note of a NEW phrase retriggers");

    /* A note-on with velocity 0 is a note OFF (running status): it must
     * decrement rather than count as a new note. */
    lfo_process_midi(arena.routes, 1, note_on_v0, 3);
    CHECK(arena.routes[0].held_count == 0, "note-on velocity 0 is a note off");

    /* Note-off below zero must not wrap the counter -- held_count is unsigned
     * in spirit and a stuck negative would disable retrigger forever. */
    lfo_process_midi(arena.routes, 1, note_off, 3);
    CHECK(arena.routes[0].held_count == 0, "held_count does not go below zero");

    /* ---- degenerate inputs are no-ops, not wild reads ------------------ */
    lfo_process_midi(arena.routes, 0, note_on, 3);
    CHECK(arena.routes[0].held_count == 0, "count=0 touched nothing");
    lfo_process_midi(arena.routes, -1, note_on, 3);
    CHECK(arena.routes[0].held_count == 0, "a negative count touched nothing");
    lfo_process_midi(NULL, 1, note_on, 3);
    CHECK(1, "a NULL array did not crash");
    lfo_process_midi(arena.routes, 1, note_on, 2);
    CHECK(arena.routes[0].held_count == 0, "a short message is ignored");

    /* ---- and the guard is untouched after every call above ------------- */
    CHECK(arena.guard.held_count == 0, "the guard survived every call");
    CHECK(arena.guard.phase == 0.75, "the guard's phase survived every call");

    printf(failures ? "FAIL\n" : "ALL PASS\n");
    return failures ? 1 : 0;
}
