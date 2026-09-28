/* edit_gesture.h -- what the user ASKED Move to do, read off the buttons.
 *
 * Move's copy/paste pairs step presses (docs/MOVE_COPY_GESTURES.md): Copy
 * down, press A (the source, latched), press B (paste A -> B), press C (a new
 * source)... With Loop held too, the buttons are PAGES. The source SURVIVES
 * releasing Copy, and the next step press anywhere pastes. Undo is CC 56;
 * with Shift it is Redo.
 *
 * This only reports INTENT. Whether Move actually did it is the live model's
 * to say (move_model_sync.c confirms every paste against the notes Move
 * wrote), so the parts of this gesture that were never measured -- a range
 * selection, an armed source Move silently cleared -- can at worst produce an
 * intent that the model does not confirm, which is a no-op. They can never
 * produce an automation edit Move did not make.
 *
 * Pure, and RT-safe: a few bytes of state, no I/O. */
#pragma once
#include <stdint.h>

enum { EG_NONE = 0, EG_SOURCE, EG_PASTE, EG_UNDO, EG_REDO, EG_DOUBLE };

typedef struct {
    uint8_t copy_held, loop_held, shift_held;
    int8_t  src;          /* latched source button, -1 = none */
    uint8_t src_page;     /* the source was taken with Loop held */
} edit_gesture_t;

typedef struct {
    int kind;             /* EG_* */
    int page;             /* page mode (Loop held at the source) */
    int src, dst;         /* buttons 0..15 */
} eg_intent_t;

#define EG_CC_COPY  60
#define EG_CC_LOOP  58
#define EG_CC_SHIFT 49
#define EG_CC_UNDO  56
#define EG_STEP_LO  16
#define EG_STEP_HI  31

void edit_gesture_reset(edit_gesture_t *g);

/* Feed one cable-0 MIDI_IN event (status, d1, d2). Returns 1 and fills *out
 * when it is an intent (EG_SOURCE included, so the caller can capture the
 * source's position at the moment it was pressed -- the page may change
 * before the destination is). Shift + step 15 is Move's Double Loop. */
int edit_gesture_on_event(edit_gesture_t *g, uint8_t status, uint8_t d1, uint8_t d2, eg_intent_t *out);
