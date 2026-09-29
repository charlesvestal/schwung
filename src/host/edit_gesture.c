/* edit_gesture.c -- see edit_gesture.h. */
#include "edit_gesture.h"

#include <string.h>

void edit_gesture_reset(edit_gesture_t *g)
{
    memset(g, 0, sizeof *g);
    g->src = -1;
}

int edit_gesture_on_event(edit_gesture_t *g, uint8_t status, uint8_t d1, uint8_t d2, eg_intent_t *out)
{
    const uint8_t type = status & 0xF0;
    memset(out, 0, sizeof *out);
    if (type == 0xB0) {
        const int down = d2 > 0;
        if (d1 == EG_CC_COPY) g->copy_held = (uint8_t)down;   /* release keeps the source */
        else if (d1 == EG_CC_LOOP) g->loop_held = (uint8_t)down;
        else if (d1 == EG_CC_SHIFT) g->shift_held = (uint8_t)down;
        else if (d1 == EG_CC_UNDO && down) {
            out->kind = g->shift_held ? EG_REDO : EG_UNDO;
            return 1;
        }
        return 0;
    }
    if (type != 0x90 || d2 == 0 || d1 < EG_STEP_LO || d1 > EG_STEP_HI) return 0;
    const int button = d1 - EG_STEP_LO;
    if (g->shift_held && !g->copy_held && g->src < 0 && button == 14) {
        out->kind = EG_DOUBLE;                       /* Shift + step 15 */
        return 1;
    }
    if (g->src < 0) {
        if (!g->copy_held) return 0;                 /* a plain step press */
        g->src = (int8_t)button;
        g->src_page = g->loop_held;
        out->kind = EG_SOURCE;
        out->page = g->src_page;
        out->src = button;
        return 1;
    }
    /* A latched source pairs with the next press, Copy held or not -- the
     * pair is consumed either way, including a re-tap of the source itself
     * (measured: that pastes onto itself and frees the next press to be a
     * new source). */
    out->kind = EG_PASTE;
    out->page = g->src_page;
    out->src = g->src;
    out->dst = button;
    g->src = -1;
    return 1;
}
