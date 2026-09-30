/*
 * The step menu's gesture: hold ONE step, press Menu.
 *
 *   Menu (first)   open, field = Chance
 *   Menu (again)   next field: Chance -> Length -> Velocity -> Chance
 *   Jog            Chance: ours (swallowed; the caller applies the delta)
 *                  Length: Move's own hold-step + jog -- passed through
 *                  Velocity: Move's own hold-step + Volume -- the jog detent
 *                            is REWRITTEN in place as a Volume (CC 79) detent
 *   Step release   close
 *
 * Measured on 2.1.x (docs/plans/2026-09-30-step-menu-design.md): Menu with a
 * step held still toggles Note/Session, so BOTH edges of every Menu press we
 * take are swallowed, latched -- a lone release Move never saw go down is
 * still a button event to Move. The release is owed even if the step was
 * let go first.
 *
 * Only over MOVE's screen (the caller passes `eligible`): with the shadow UI
 * up, a held step is the p-lock gesture and Menu is the UI's own.
 *
 * Pure, so tests/host drives it; RT-safe.
 */
#ifndef STEP_MENU_H
#define STEP_MENU_H

#include <stdint.h>
#include <math.h>
#include "step_plock.h"

#define SM_CC_MENU   50
#define SM_CC_JOG    14
#define SM_CC_VOLUME 79
#define SM_STEP_NOTE0 16

enum { SM_PASS = 0, SM_SWALLOW = 1, SM_REWRITE = 2 };
enum { SM_FIELD_CHANCE = 0, SM_FIELD_LENGTH = 1, SM_FIELD_VELOCITY = 2, SM_FIELDS = 3 };

/* MOVE DECIDES TAP-vs-HOLD ON THE RELEASE, at ~500 ms (measured 2.1.x:
 * 500 ms toggled the note, 520 ms did not). The Menu press is swallowed, so
 * to Move a quick step + Menu + let go IS a tap -- and it toggles the note:
 * deletes the one you opened the menu to edit, or adds one to an empty step.
 * So a release that comes too soon after a press the menu USED is withheld
 * and handed to Move once the press is SM_HOLD_SAFE_MS old. */
#define SM_HOLD_SAFE_MS 700

typedef struct {
    uint8_t open;
    uint8_t field;
    uint8_t step;         /* 0..15, the held step the menu is about */
    uint8_t menu_latch;   /* a Menu press we swallowed; its release is owed */
    uint64_t press_ms[16];/* when each step went down (0 = not seen) */
    uint8_t used[16];     /* the menu opened during this press */
    uint64_t owe_ms[16];  /* a withheld release, due at this time (0 = none) */
} sm_state_t;

/* The one held step in a mask, or -1 for none / more than one. */
static inline int sm_single_step(uint32_t held_mask)
{
    uint32_t m = held_mask & 0xFFFFu;
    if (m == 0 || (m & (m - 1)) != 0) return -1;
    for (int i = 0; i < 16; i++) if (m & (1u << i)) return i;
    return -1;
}

/* Relative encoder detent: 1..63 clockwise, 65..127 counter-clockwise. */
static inline int sm_jog_dir(uint8_t v)
{
    if (v >= 1 && v <= 63) return 1;
    if (v >= 65) return -1;
    return 0;
}

/* One cable-0 MIDI_IN event. Returns SM_PASS / SM_SWALLOW / SM_REWRITE (then
 * `out` holds the three bytes Move gets instead). `*chance_dir` is set to the
 * jog direction when the Chance field takes a detent, else 0. */
static inline int sm_on_input(sm_state_t *s, uint32_t held_mask, int shift_held, int eligible,
                              uint8_t status, uint8_t d1, uint8_t d2,
                              uint8_t out[3], int *chance_dir, uint64_t now_ms)
{
    if (chance_dir) *chance_dir = 0;
    const uint8_t type = status & 0xF0;

    /* Step presses and releases: the tap/hold guard. */
    if ((type == 0x90 || type == 0x80) && d1 >= SM_STEP_NOTE0 && d1 < SM_STEP_NOTE0 + 16) {
        const int i = d1 - SM_STEP_NOTE0;
        const int is_on = (type == 0x90 && d2 > 0);
        if (is_on) {
            if (s->owe_ms[i]) {
                /* Pressed again while Move still holds the first press: to
                 * Move it is one long hold. Swallow, and owe nothing yet. */
                s->owe_ms[i] = 0;
                return SM_SWALLOW;
            }
            s->press_ms[i] = now_ms ? now_ms : 1;
            s->used[i] = 0;
            return SM_PASS;
        }
        if (s->open && i == s->step) s->open = 0;
        if (s->used[i] && s->press_ms[i] &&
            now_ms < s->press_ms[i] + SM_HOLD_SAFE_MS) {
            s->owe_ms[i] = s->press_ms[i] + SM_HOLD_SAFE_MS;
            return SM_SWALLOW;
        }
        s->used[i] = 0;
        s->press_ms[i] = 0;
        return SM_PASS;
    }

    if (type == 0xB0 && d1 == SM_CC_MENU) {
        if (d2 > 0) {
            const int step = sm_single_step(held_mask);
            if (!eligible || shift_held || step < 0) return SM_PASS;
            s->menu_latch = 1;
            s->used[step] = 1;
            if (!s->open || s->step != step) {
                s->open = 1; s->field = SM_FIELD_CHANCE; s->step = (uint8_t)step;
            } else {
                s->field = (uint8_t)((s->field + 1) % SM_FIELDS);
            }
            return SM_SWALLOW;
        }
        if (s->menu_latch) { s->menu_latch = 0; return SM_SWALLOW; }
        return SM_PASS;
    }

    if (!s->open) return SM_PASS;

    if (type == 0xB0 && d1 == SM_CC_JOG) {
        const int dir = sm_jog_dir(d2);
        if (s->field == SM_FIELD_CHANCE) {
            if (chance_dir) *chance_dir = dir;
            return SM_SWALLOW;
        }
        if (s->field == SM_FIELD_VELOCITY) {
            out[0] = status; out[1] = SM_CC_VOLUME; out[2] = d2;
            return SM_REWRITE;
        }
        return SM_PASS;   /* Length: Move's own gesture */
    }
    return SM_PASS;
}

/* Once per frame: a menu whose step is no longer the one held (a release we
 * did not see, the display coming up) closes. The latch is NOT dropped --
 * a Menu release is still owed to nobody but us. */
static inline void sm_validate(sm_state_t *s, uint32_t held_mask, int eligible)
{
    if (!s->open) return;
    if (!eligible || sm_single_step(held_mask) != s->step) s->open = 0;
}

/* Withheld releases now due: a mask of steps whose note-off the caller must
 * hand to Move this frame. Each is returned once. */
static inline uint32_t sm_due_releases(sm_state_t *s, uint64_t now_ms)
{
    uint32_t m = 0;
    for (int i = 0; i < 16; i++) {
        if (s->owe_ms[i] && now_ms >= s->owe_ms[i]) {
            s->owe_ms[i] = 0; s->used[i] = 0; s->press_ms[i] = 0;
            m |= 1u << i;
        }
    }
    return m;
}

/* The next condition index for a jog detent, clamped to the list. */
static inline int sm_step_cond(int cur, int dir, int count)
{
    int n = cur + dir;
    if (n < 0) n = 0;
    if (n > count - 1) n = count - 1;
    return n;
}

/* ---- the displayed page: which of Move's notes sit on which button ---- */

typedef struct {
    int64_t id;       /* Move's note id */
    double  start;    /* clip time, quarters */
    double  dur;      /* quarters */
    float   vel;      /* 0..127 as Move stores it */
    uint8_t pitch;
} sm_note_t;

/* Notes on `button` of the page at `scroll`: every note whose start is
 * NEAREST that button's step -- a note Move nudged early still belongs to its
 * step, which is where Move draws it and where hold-step + jog edits it.
 * A triplet grid's dead button (every 4th) holds nothing. Returns the count
 * written to idx[] (indices into notes[]). */
static inline int sm_button_notes(const sm_note_t *notes, int n, double scroll,
                                  double step_beats, int triplet, double clip_len,
                                  int button, int *idx, int max)
{
    double ph = 0.0;
    if (step_plock_phase_from_scroll(scroll, button, step_beats, triplet,
                                     clip_len, &ph) != STEP_PLOCK_OK) return 0;
    const double lo = ph - step_beats * 0.5, hi = ph + step_beats * 0.5;
    int k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (notes[i].start >= lo && notes[i].start < hi) idx[k++] = i;
    return k;
}

/* A drum rack's selected CELL, from Move's own pad LED: the left 4x4 of the
 * pad grid is notes 36..51, counting up from the bottom-left (68 = 36,
 * 69 = 37, 76 = 40 -- measured). -1 for a pad outside it. */
static inline int sm_drum_cell_pitch(int pad)
{
    if (pad < 68 || pad > 99) return -1;
    const int row = (pad - 68) / 8, col = (pad - 68) % 8;
    if (col > 3) return -1;
    return 36 + row * 4 + col;
}

/* Scope a step's notes to the selected drum voice -- Move's step shows and
 * edits only that voice's note on a drum track. Only when the voice is known
 * AND present on the step; otherwise the whole step (a chord on a melodic
 * track keeps every note). Returns the new count. */
static inline int sm_scope_voice(const sm_note_t *notes, int *idx, int k, int voice_pitch)
{
    if (voice_pitch < 0) return k;
    int m = 0;
    for (int i = 0; i < k; i++) if (notes[idx[i]].pitch == voice_pitch) m++;
    if (m == 0) return k;
    int w = 0;
    for (int i = 0; i < k; i++) if (notes[idx[i]].pitch == voice_pitch) idx[w++] = idx[i];
    return w;
}

#endif /* STEP_MENU_H */
