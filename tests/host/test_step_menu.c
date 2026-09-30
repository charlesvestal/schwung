/*
 * The step menu gesture. The failures that matter are the ones Move sees:
 * a Menu edge that leaks flips Note/Session (measured) and the pads start
 * launching clips; a jog that leaks on Chance changes the note's length
 * under the user; a release owed and never swallowed is an orphan button-up.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
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
    /* not eligible (the shadow UI disabled): Move's */
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
    /* ...ALWAYS as a rewrite, even unchanged: over the shadow UI the jog has
     * already been filtered out of Move's copy, and a PASS delivers nothing. */
    assert(cc(S5, 0, 1, 14, 1) == SM_REWRITE && cd == 0);
    assert(out[0] == 0xB0 && out[1] == 14 && out[2] == 1);

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

    /* ---- jog ACCELERATION on Length and Velocity ------------------------- */
    assert(sm_scale_rel(1, 16) == 16 && sm_scale_rel(127, 16) == 112);
    assert(sm_scale_rel(5, 16) == 63 && sm_scale_rel(65, 16) == 65);   /* clamped */
    assert(sm_scale_rel(0, 8) == 0);
    memset(&s, 0, sizeof s);
    now = 60000; step_on(0, 4); now += 100;
    cc(1u << 4, 0, 1, 50, 127); cc(1u << 4, 0, 1, 50, 0);   /* Chance */
    cc(1u << 4, 0, 1, 50, 127); cc(1u << 4, 0, 1, 50, 0);   /* Length */
    /* a slow turn is Move's own detent, delivered as the SAME bytes */
    now += 500; assert(cc(1u << 4, 0, 1, 14, 1) == SM_REWRITE && out[2] == 1);
    now += 200; assert(cc(1u << 4, 0, 1, 14, 1) == SM_REWRITE && out[2] == 1);
    /* a fast spin is rewritten into bigger detents, IN PLACE (still CC 14) */
    now += 60;  assert(cc(1u << 4, 0, 1, 14, 1) == SM_REWRITE && out[1] == 14 && out[2] == 3);
    now += 30;  assert(cc(1u << 4, 0, 1, 14, 1) == SM_REWRITE && out[2] == 8);
    now += 10;  assert(cc(1u << 4, 0, 1, 14, 1) == SM_REWRITE && out[2] == 16);
    /* a reversal starts over at x1, even fast */
    now += 10;  assert(cc(1u << 4, 0, 1, 14, 127) == SM_REWRITE && out[2] == 127);
    now += 10;  assert(cc(1u << 4, 0, 1, 14, 127) == SM_REWRITE && out[2] == 112);
    /* Velocity: rewritten to Volume, accelerated the same way */
    cc(1u << 4, 0, 1, 50, 127); cc(1u << 4, 0, 1, 50, 0);   /* Velocity */
    now += 500; assert(cc(1u << 4, 0, 1, 14, 1) == SM_REWRITE && out[1] == 79 && out[2] == 1);
    now += 20;  assert(cc(1u << 4, 0, 1, 14, 1) == SM_REWRITE && out[1] == 79 && out[2] == 16);
    /* Chance is never accelerated: 57 values, one per detent */
    cc(1u << 4, 0, 1, 50, 127); cc(1u << 4, 0, 1, 50, 0);   /* Chance */
    now += 10;  assert(cc(1u << 4, 0, 1, 14, 1) == SM_SWALLOW && cd == 1);
    now += 10; step_off(0, 4);

    /* ---- NO WIND-UP: detents past the cap are trimmed, then swallowed ----- */
    memset(&s, 0, sizeof s);
    now = 70000; step_on(0, 6); now += 100;
    cc(1u << 6, 0, 1, 50, 127); cc(1u << 6, 0, 1, 50, 0);
    cc(1u << 6, 0, 1, 50, 127); cc(1u << 6, 0, 1, 50, 0);   /* Length */
    assert(!s.bound_known);                                  /* a field change clears it */
    sm_bound_seed(&s, 5, 2);                                 /* 0.5 step of room up, 0.2 down */
    now += 500; assert(cc(1u << 6, 0, 1, 14, 1) == SM_REWRITE && out[2] == 1); /* 1 of 5 */
    now += 10;  assert(cc(1u << 6, 0, 1, 14, 1) == SM_REWRITE && out[2] == 4); /* x16 trimmed to 4 */
    now += 10;  assert(cc(1u << 6, 0, 1, 14, 1) == SM_SWALLOW);          /* at the cap: nothing to Move */
    now += 10;  assert(cc(1u << 6, 0, 1, 14, 1) == SM_SWALLOW);
    /* ...so ONE detent back moves at once -- nothing banked */
    now += 500; assert(cc(1u << 6, 0, 1, 14, 127) == SM_REWRITE && out[2] == 127);
    assert(s.rem_down == 6 && s.rem_up == 1);
    /* the floor is bounded the same way */
    sm_bound_seed(&s, 10, 1);
    now += 500; assert(cc(1u << 6, 0, 1, 14, 127) == SM_REWRITE && out[2] == 127);
    now += 500; assert(cc(1u << 6, 0, 1, 14, 127) == SM_SWALLOW);
    /* unknown bounds pass everything, as before */
    s.bound_known = 0;
    now += 500; assert(cc(1u << 6, 0, 1, 14, 127) == SM_REWRITE && out[2] == 127);
    now += 10; step_off(0, 6);
    /* Move's cap: the next note of the SAME pitch, else the clip end */
    {
        sm_note_t nt[] = { { 1, 0.0, 0.1, 100, 36, 0 }, { 2, 0.55, 0.1, 100, 36, 0 },
                           { 3, 0.25, 0.1, 100, 38, 0 }, { 4, 1.0, 0.1, 100, 60, 0 } };
        assert(fabs(sm_note_cap(nt, 4, 0, 4.0) - 0.55) < 1e-9);   /* the 2.2-step kick */
        assert(fabs(sm_note_cap(nt, 4, 2, 4.0) - 3.75) < 1e-9);   /* no later 38: clip end */
        assert(fabs(sm_note_cap(nt, 4, 3, 4.0) - 3.0) < 1e-9);
    }

    /* ---- condition stepping clamps -------------------------------------- */
    assert(sm_step_cond(0, -1, 57) == 0);
    assert(sm_step_cond(56, 1, 57) == 56);
    assert(sm_step_cond(3, 1, 57) == 4);

    /* ---- notes on a button ---------------------------------------------- */
    {
        /* 1/16 grid, page at scroll 4.0: button 0 = [4.0, 4.25), button 1 =
         * [4.25, 4.5). A note belongs to the step it STARTS in (Move's rule,
         * measured on a live-played clip). */
        sm_note_t nt[] = {
            { 1, 4.0,  0.25, 100, 36, 0 },   /* step 0 */
            { 2, 4.0,  0.25, 100, 42, 0 },   /* step 0, another voice */
            { 3, 4.23, 0.25, 100, 38, 0 },   /* 4.23: still inside step 0 */
            { 4, 4.36, 0.25, 100, 38, 0 },   /* step 1 */
            { 5, 3.99, 0.25, 100, 50, 0 },   /* before the page: not on it */
            { 6, 0.0,  0.25, 100, 36, 0 },   /* another page */
            { 7, 4.2499999999, 0.25, 100, 60, 0 },   /* float error at the edge: step 1 */
        };
        int idx[8], k;
        k = sm_button_notes(nt, 7, 4.0, 0.25, 0, 16.0, 0, idx, 8);
        assert(k == 3);
        k = sm_button_notes(nt, 7, 4.0, 0.25, 0, 16.0, 1, idx, 8);
        assert(k == 2 && nt[idx[0]].id == 4 && nt[idx[1]].id == 7);
        /* THE LIVE-PLAYED CLIP: a chord at 17.425/17.428/17.434 is on the step
         * spanning 17.25-17.5 (button 5 of the page at 16), and a chord that
         * straddles the half-step (18.871/18.872/18.892) is NOT split. */
        {
            sm_note_t live[] = { { 1, 17.425, 0.2, 90, 73, 0 }, { 2, 17.428, 0.2, 90, 64, 0 },
                                 { 3, 17.434, 0.2, 90, 71, 0 }, { 4, 18.871, 0.2, 90, 71, 0 },
                                 { 5, 18.872, 0.2, 90, 74, 0 }, { 6, 18.892, 0.2, 90, 64, 0 } };
            assert(sm_button_notes(live, 6, 16.0, 0.25, 0, 32.0, 5, idx, 8) == 3);
            assert(sm_button_notes(live, 6, 16.0, 0.25, 0, 32.0, 6, idx, 8) == 0);
            assert(sm_button_notes(live, 6, 16.0, 0.25, 0, 32.0, 11, idx, 8) == 3);
            assert(sm_button_notes(live, 6, 16.0, 0.25, 0, 32.0, 12, idx, 8) == 0);
        }
        /* past the clip: nothing */
        assert(sm_button_notes(nt, 7, 16.0, 0.25, 0, 16.0, 0, idx, 8) == 0);
        /* triplet: button 3 is dead, button 4 is step 3 = scroll + 3/6 */
        sm_note_t tr[] = { { 7, 0.5, 0.1, 100, 60, 0 } };
        assert(sm_button_notes(tr, 1, 0.0, 1.0 / 6.0, 1, 4.0, 3, idx, 8) == 0);
        assert(sm_button_notes(tr, 1, 0.0, 1.0 / 6.0, 1, 4.0, 4, idx, 8) == 1);

        /* ---- voice scoping ---- */
        k = sm_button_notes(nt, 7, 4.0, 0.25, 0, 16.0, 0, idx, 8);
        assert(sm_scope_voice(nt, idx, k, 42) == 1 && nt[idx[0]].id == 2);
        k = sm_button_notes(nt, 7, 4.0, 0.25, 0, 16.0, 0, idx, 8);
        assert(sm_scope_voice(nt, idx, k, 37) == 3);   /* voice not on the step: all */
        assert(sm_scope_voice(nt, idx, k, -1) == 3);   /* unknown: all */
    }
    assert(sm_drum_cell_pitch(68) == 36 && sm_drum_cell_pitch(69) == 37);
    assert(sm_drum_cell_pitch(76) == 40 && sm_drum_cell_pitch(95) == 51);
    assert(sm_drum_cell_pitch(72) == -1 && sm_drum_cell_pitch(67) == -1);

    /* ---- the shim's HAND-OFF release rides the same owed queue ---------- *
     * A step withheld from Move and handed to it when the menu opened: the
     * shim takes the release and owes it here, due SM_HOLD_SAFE_MS after the
     * hand-off. Never before -- sooner is a tap to Move, a toggled note. */
    memset(&s, 0, sizeof s);
    sm_owe_release(&s, 3, 90000);
    assert(sm_due_releases(&s, 89999) == 0);
    assert(sm_due_releases(&s, 90000) == (1u << 3));
    assert(sm_due_releases(&s, 99999) == 0);          /* handed over ONCE */
    /* a due time of 0 still owes (the next frame), never "nothing owed" */
    sm_owe_release(&s, 4, 0);
    assert(sm_due_releases(&s, 1) == (1u << 4));
    /* out of range is ignored, not a write past the arrays */
    sm_owe_release(&s, 16, 5); sm_owe_release(&s, -1, 5);
    assert(sm_due_releases(&s, 1u << 30) == 0);

    printf("test_step_menu: PASS\n");
    return 0;
}
