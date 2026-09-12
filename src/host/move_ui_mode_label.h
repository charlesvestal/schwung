/*
 * move_ui_mode_label.h — may a Track button press relabel Move's UI mode?
 *
 * shadow_control_t.move_ui_mode is our belief about what MOVE is showing.
 * D-Bus announcements are the authority for Session and Set Overview; the one
 * thing no announcement reports is a track selection, so the shim infers NOTE
 * from a Track button press (CC 40-43). That inference was unconditional, and
 * it was wrong for the one press that never reaches Move at all.
 *
 * THE FAILURE MODE, found on hardware: Shift+Vol+Track is the gesture that
 * OPENS the shadow UI, and its Track CC is swallowed (midi_in_swallow) so Move
 * never sees it. Move's view therefore does not change — but the label said
 * NOTE. Nothing sets it back except the exact "Session Mode" announcement,
 * which never arrives because the user was ALREADY in Session. clip_state_on_led
 * then rejects every pad event (its gate is Session-only, deliberately), so
 * clip identity froze on whatever was playing when the UI was opened: the user
 * switched clips, Schwung kept the old ones, and lanes played against a clip
 * that was no longer running. Stale, not invalid — which is the outcome the
 * clip-awareness design explicitly refuses.
 *
 * THE RULE: a press may relabel only if it was DELIVERED to Move. That is the
 * real predicate, not a list of combos — a press withheld from Move cannot have
 * changed Move's view, and every press Move does receive selects a track and
 * puts its instrument under the pads (which is what NOTE means). Tying it to
 * the swallow means a future shortcut that withholds a Track press is covered
 * the day it is written, rather than re-introducing this bug.
 *
 * Deliberately NOT excluded (both were considered; both are delivered to Move):
 *   - a Track tap while the shadow UI is up ("Keep Schwung" slot switch). Its
 *     own comment in schwung_shim.c says the CC is NOT blocked, precisely so
 *     Move's selected track follows the slot.
 *   - the 500 ms Track hold. The real press is not swallowed, and when the
 *     long-press fires it INJECTS a track tap for Move ("the pads play the
 *     SELECTED track's rack"), so Move does end up on that track's instrument.
 * Suppressing either would claim SESSION while Move was in NOTE, which opens
 * the clip gate over a keyboard and invents clip launches — strictly worse than
 * the bug above.
 */
#ifndef MOVE_UI_MODE_LABEL_H
#define MOVE_UI_MODE_LABEL_H

/* Mirrors shadow_control_t.move_ui_mode. CLIP_UI_MODE_SESSION (clip_state.h)
 * is the same numbering from the consumer's side. */
#define MOVE_UI_MODE_UNKNOWN      0
#define MOVE_UI_MODE_SESSION      1
#define MOVE_UI_MODE_NOTE         2
#define MOVE_UI_MODE_SET_OVERVIEW 3

/* `pressed`             — d2 > 0 (a release changes no view; Move's own
 *                         selection happened on the press it already saw).
 * `withheld_from_move`  — this press was swallowed from Move's MIDI_IN.
 *
 * Returns non-zero if the label may be set to MOVE_UI_MODE_NOTE. */
static inline int move_ui_mode_track_press_relabels(int pressed,
                                                    int withheld_from_move)
{
    return pressed && !withheld_from_move;
}

#endif /* MOVE_UI_MODE_LABEL_H */
