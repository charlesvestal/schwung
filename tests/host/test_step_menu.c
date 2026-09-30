/*
 * The step menu gesture. The failures that matter are the ones Move sees:
 * a Menu edge that leaks flips Note/Session (measured) and the pads start
 * launching clips; a jog that leaks on Chance changes the note's length
 * under the user; a release owed and never swallowed is an orphan button-up.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "step_menu.h"

static sm_state_t s;
static uint8_t out[3];
static int cd;

static uint64_t now = 10000;   /* ms; tests advance it by hand */
static int cc(uint32_t held, int shift, int elig, int n, int v) {
    return sm_on_input(&s, held, shift, elig, 0xB0, (uint8_t)n, (uint8_t)v, out, &cd, now);
}
static int step_off(uint32_t held, int step) {
    return sm_on_input(&s, held, 0, 1, 0x80, (uint8_t)(16 + step), 0, out, &cd, now);
}
static int step_on(uint32_t held, int step) {
    return sm_on_input(&s, held, 0, 1, 0x90, (uint8_t)(16 + step), 100, out, &cd, now);
}

int main(void) {
    const uint32_t S5 = 1u << 5;

    /* ---- Menu alone is Move's -------------------------------------------- */
    memset(&s, 0, sizeof s);
    assert(cc(0, 0, 1, 50, 127) == SM_PASS && !s.open);
    assert(cc(0, 0, 1, 50, 0) == SM_PASS);
    /* two steps held names no step: Move's */
    assert(cc(S5 | 1, 0, 1, 50, 127) == SM_PASS && !s.open);
    assert(cc(S5 | 1, 0, 1, 50, 0) == SM_PASS);
    /* Shift+Menu is the screen reader / Master FX: never ours */
    assert(cc(S5, 1, 1, 50, 127) == SM_PASS && !s.open);
    cc(S5, 1, 1, 50, 0);
    /* shadow UI up: not eligible */
    assert(cc(S5, 0, 0, 50, 127) == SM_PASS && !s.open);
    cc(S5, 0, 0, 50, 0);

    /* ---- step + Menu opens on Chance, both edges swallowed --------------- */
    assert(cc(S5, 0, 1, 50, 127) == SM_SWALLOW);
    assert(s.open && s.field == SM_FIELD_CHANCE && s.step == 5);
    assert(cc(S5, 0, 1, 50, 0) == SM_SWALLOW);

    /* jog on Chance: swallowed, direction reported */
    assert(cc(S5, 0, 1, 14, 1) == SM_SWALLOW && cd == 1);
    assert(cc(S5, 0, 1, 14, 127) == SM_SWALLOW && cd == -1);
    /* other controls pass (knobs are Move's per-step automation) */
    assert(cc(S5, 0, 1, 71, 1) == SM_PASS);

    /* Menu again: Length. The jog is Move's own length edit. */
    assert(cc(S5, 0, 1, 50, 127) == SM_SWALLOW && s.field == SM_FIELD_LENGTH);
    assert(cc(S5, 0, 1, 50, 0) == SM_SWALLOW);
    assert(cc(S5, 0, 1, 14, 1) == SM_PASS && cd == 0);

    /* Menu again: Velocity. The jog becomes a Volume detent, same value. */
    cc(S5, 0, 1, 50, 127); cc(S5, 0, 1, 50, 0);
    assert(s.field == SM_FIELD_VELOCITY);
    assert(cc(S5, 0, 1, 14, 126) == SM_REWRITE);
    assert(out[0] == 0xB0 && out[1] == 79 && out[2] == 126);

    /* and wraps back to Chance */
    cc(S5, 0, 1, 50, 127); cc(S5, 0, 1, 50, 0);
    assert(s.field == SM_FIELD_CHANCE);

    /* ---- releasing the step closes; the release is Move's ---------------- */
    assert(step_off(0, 5) == SM_PASS && !s.open);
    /* ...and the jog is Move's again */
    assert(cc(0, 0, 1, 14, 1) == SM_PASS);
    /* another step's release does not close it */
    cc(S5, 0, 1, 50, 127); cc(S5, 0, 1, 50, 0);
    assert(step_off(S5, 6) == SM_PASS && s.open);

    /* ---- the owed release survives the step going first ------------------ */
    memset(&s, 0, sizeof s);
    assert(cc(S5, 0, 1, 50, 127) == SM_SWALLOW);
    step_off(0, 5);                                   /* step let go first */
    assert(!s.open);
    assert(cc(0, 0, 1, 50, 0) == SM_SWALLOW);         /* still swallowed */
    assert(cc(0, 0, 1, 50, 0) == SM_PASS);            /* once */

    /* ---- a different step + Menu reopens on THAT step, on Chance ---------- */
    memset(&s, 0, sizeof s);
    cc(S5, 0, 1, 50, 127); cc(S5, 0, 1, 50, 0);
    cc(S5, 0, 1, 50, 127); cc(S5, 0, 1, 50, 0);       /* Length */
    sm_validate(&s, 1u << 9, 1);                     /* step 5 gone, 9 held */
    assert(!s.open);
    cc(1u << 9, 0, 1, 50, 127);
    assert(s.open && s.step == 9 && s.field == SM_FIELD_CHANCE);
    cc(1u << 9, 0, 1, 50, 0);

    /* ---- validate: the display coming up closes it ---------------------- */
    sm_validate(&s, 1u << 9, 0);
    assert(!s.open);

    /* ---- THE TAP GUARD: Move toggles a note on a release < ~500 ms -------- */
    memset(&s, 0, sizeof s);
    /* a plain quick tap with no menu is Move's, untouched */
    now = 20000; assert(step_on(0, 3) == SM_PASS);
    now += 100;  assert(step_off(1u << 3, 3) == SM_PASS);
    assert(sm_due_releases(&s, now + 5000) == 0);
    /* step + Menu + quick release: the release is WITHHELD... */
    now = 30000; step_on(0, 3);
    now += 150; cc(1u << 3, 0, 1, 50, 127); cc(1u << 3, 0, 1, 50, 0);
    now += 100; assert(step_off(1u << 3, 3) == SM_SWALLOW);
    assert(!s.open);                                     /* the menu closes */
    assert(sm_due_releases(&s, 30000 + SM_HOLD_SAFE_MS - 1) == 0);
    /* ...and handed to Move once the press is old enough, exactly once */
    assert(sm_due_releases(&s, 30000 + SM_HOLD_SAFE_MS) == (1u << 3));
    assert(sm_due_releases(&s, 30000 + SM_HOLD_SAFE_MS + 50) == 0);
    /* a slow release after the menu is Move's own, straight through */
    now = 40000; step_on(0, 3);
    now += 100; cc(1u << 3, 0, 1, 50, 127); cc(1u << 3, 0, 1, 50, 0);
    now += 900; assert(step_off(1u << 3, 3) == SM_PASS);
    /* re-pressed while the release is still owed: one long hold to Move */
    now = 50000; step_on(0, 3);
    now += 100; cc(1u << 3, 0, 1, 50, 127); cc(1u << 3, 0, 1, 50, 0);
    now += 100; assert(step_off(1u << 3, 3) == SM_SWALLOW);
    now += 100; assert(step_on(0, 3) == SM_SWALLOW);     /* Move never saw it go up */
    assert(sm_due_releases(&s, now + 5000) == 0);        /* nothing owed while held */
    now += 50;  assert(step_off(1u << 3, 3) == SM_SWALLOW); /* still < 700 from the FIRST press */
    assert(sm_due_releases(&s, 50000 + SM_HOLD_SAFE_MS) == (1u << 3));

    /* ---- condition stepping clamps -------------------------------------- */
    assert(sm_step_cond(0, -1, 57) == 0);
    assert(sm_step_cond(56, 1, 57) == 56);
    assert(sm_step_cond(3, 1, 57) == 4);

    /* ---- notes on a button ---------------------------------------------- */
    {
        /* 1/16 grid, page at scroll 4.0: button 0 = 4.0, button 1 = 4.25 */
        sm_note_t nt[] = {
            { 1, 4.0,  0.25, 100, 36 },   /* step 0 */
            { 2, 4.0,  0.25, 100, 42 },   /* step 0, another voice */
            { 3, 4.23, 0.25, 100, 38 },   /* step 1, nudged 8% early */
            { 4, 4.36, 0.25, 100, 38 },   /* 44% late: still step 1 */
            { 5, 3.99, 0.25, 100, 50 },   /* step 0, nudged early -- off the page start */
            { 6, 0.0,  0.25, 100, 36 },   /* another page */
        };
        int idx[8], k;
        k = sm_button_notes(nt, 6, 4.0, 0.25, 0, 16.0, 0, idx, 8);
        assert(k == 3);
        k = sm_button_notes(nt, 6, 4.0, 0.25, 0, 16.0, 1, idx, 8);
        assert(k == 2 && nt[idx[0]].id == 3 && nt[idx[1]].id == 4);
        /* past the clip: nothing */
        assert(sm_button_notes(nt, 6, 16.0, 0.25, 0, 16.0, 0, idx, 8) == 0);
        /* triplet: button 3 is dead, button 4 is step 3 = scroll + 3/6 */
        sm_note_t tr[] = { { 7, 0.5, 0.1, 100, 60 } };
        assert(sm_button_notes(tr, 1, 0.0, 1.0 / 6.0, 1, 4.0, 3, idx, 8) == 0);
        assert(sm_button_notes(tr, 1, 0.0, 1.0 / 6.0, 1, 4.0, 4, idx, 8) == 1);

        /* ---- voice scoping ---- */
        k = sm_button_notes(nt, 6, 4.0, 0.25, 0, 16.0, 0, idx, 8);
        assert(sm_scope_voice(nt, idx, k, 42) == 1 && nt[idx[0]].id == 2);
        k = sm_button_notes(nt, 6, 4.0, 0.25, 0, 16.0, 0, idx, 8);
        assert(sm_scope_voice(nt, idx, k, 37) == 3);   /* voice not on the step: all */
        assert(sm_scope_voice(nt, idx, k, -1) == 3);   /* unknown: all */
    }
    assert(sm_drum_cell_pitch(68) == 36 && sm_drum_cell_pitch(69) == 37);
    assert(sm_drum_cell_pitch(76) == 40 && sm_drum_cell_pitch(95) == 51);
    assert(sm_drum_cell_pitch(72) == -1 && sm_drum_cell_pitch(67) == -1);

    printf("test_step_menu: PASS\n");
    return 0;
}
