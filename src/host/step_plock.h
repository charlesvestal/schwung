/* step_plock.h - a held STEP BUTTON's position in clip time.
 *
 * The p-lock gesture is hold a step, turn a knob: set a value ON that step.
 * This is the arithmetic half -- which phase that step is -- and it is the
 * inverse of the mapping the phase check verified on hardware:
 *
 *     idx = (step_in_clip mod steps_per_bar) mod STEPS_PER_PAGE
 *
 * so, given the displayed bar and page:
 *
 *     step_in_clip = (bar - 1) * steps_per_bar + page * STEPS_PER_PAGE + idx
 *     phase        = step_in_clip * step_resolution        [quarters]
 *
 * MEASURED, not assumed. 16 playhead sightings on an 11/8 set at 1/16 fixed
 * every term: Move's lit index ran bar-relative and page-wrapped, six per pass
 * with a ~43-step gap, exactly as `32 mod 22 = 10` predicts for a loop
 * starting at quarter 8.0. The forward model then scored 26/26 on device. The
 * design doc called the page oracle "the least certain part of Project 1";
 * this is what settles it.
 *
 * WHERE THE BAR COMES FROM: the step editor's own bar strip
 * (`step_strip.h`'s `bold_segment` -- the thickened segment IS the displayed
 * bar), read off the screen. NOT Move's "Bar N" announcement, which needs the
 * screen reader running: measured 2026-09-13 with it off, `shadow_editor_bar`
 * stayed 0 through repeated arrow presses, so an oracle built on it is absent
 * exactly when nobody has turned that on.
 *
 * WHERE THE PAGE COMES FROM: `stepEditorScrollPosition`, and it makes the
 * arithmetic above unnecessary rather than merely feasible.
 *
 * MOVE RECORDS THE PAGE ORIGIN ITSELF, per clip, in quarters -- so the
 * displayed page does not have to be reconstructed from a bar, a page index
 * and a steps-per-bar at all. The held button is simply
 *
 *     phase = scroll_beats + idx * step_resolution
 *
 * which is `step_plock_phase_from_scroll()`. It carries no signature and no
 * bar, so 11/8 and 4/4 are the same code, and the MULTI_PAGE refusal below
 * does not arise: a bar that spans two pages has each page named directly.
 *
 * Measured 2026-09-13 on a one-bar 11/8 clip (loop 0..5.5, 22 steps at 1/16):
 * one right-arrow moved the file's scroll from 0 to exactly 4.0 -- SIXTEEN
 * STEPS, not a bar -- and a second to 5.5, the "+" that adds another bar. So
 * a page is 16 steps, as Move 2.1.0's notes say ("when bars exceed 16 steps,
 * they display across multiple pages using arrow buttons"), and steps 16..21
 * of that bar live on page 2 and are perfectly reachable.
 *
 * (An earlier revision of this comment said a press moved a whole BAR. That
 * read a two-press sequence as one, and it is the kind of error this form is
 * immune to anyway: the scroll is taken as given rather than reconstructed,
 * so what the arrows step by never enters the arithmetic.)
 *
 * The strip's segment count does NOT change as you page, which is what had
 * made the page look unreadable: the strip counts BARS, and the page moves
 * within one.
 *
 * ITS ONE WEAKNESS IS AGE. It comes from Song.abl, which Move writes lazily,
 * so paging and immediately p-locking can read the previous page. The bar
 * strip is live and is the cross-check: where it names a bar, the scroll must
 * fall inside it, and where they disagree the LIVE reading wins. The
 * bar-and-page form below is kept for that path and for a clip the file has
 * never seen.
 *
 * The other refusals are the same kind: an unusable bar or index, a grid we
 * could not parse (step_resolution <= 0), and a clip whose length is unknown.
 *
 * RT: pure arithmetic, no state, no I/O.
 */

#ifndef STEP_PLOCK_H
#define STEP_PLOCK_H

#include <math.h>
#include "step_strip.h"   /* STEP_STRIP_STEPS_PER_PAGE */

/* Why an answer was refused, so a gesture that does nothing can say which
 * fact was missing rather than just failing. */
enum {
    STEP_PLOCK_OK = 0,
    STEP_PLOCK_NO_BAR,        /* the strip did not name a displayed bar */
    STEP_PLOCK_NO_GRID,       /* step_resolution unknown or nonsensical */
    STEP_PLOCK_BAD_INDEX,     /* not one of the 16 step buttons */
    STEP_PLOCK_MULTI_PAGE,    /* the bar spans several pages and we cannot
                               * tell which one is displayed */
    STEP_PLOCK_OUTSIDE_CLIP,  /* the step is past the clip's own length */
    STEP_PLOCK_CLIP_PENDING   /* Move is step-editing a clip on this track and
                               * we cannot NAME it yet -- see below */
};

/*
 * STEP_PLOCK_CLIP_PENDING, and why "no clip" was the wrong answer.
 *
 * A lane is keyed by (track, clip slot), and there are exactly two ways to
 * learn the slot: `identity_valid`, set by a ch-9 ON in Move's LED stream, and
 * `Song.abl`. A clip you have just made has neither -- it has never played, so
 * no ch-9, and Move writes the file ~10 s late. MEASURED 2026-09-14: delete a
 * clip, add a note in Note view, and the slot is unknown for 8-12 s, arriving
 * in the same second the file is written.
 *
 * Both sources, one shared blind spot, and it lands exactly on "make a clip
 * and lock its steps" -- the flow the feature is for. The refusal that reached
 * the user was "no clip on this track" while they were plainly looking at one,
 * which describes a PERMANENT state and reads as the feature being broken.
 * The honest answer is "not yet".
 *
 * The two are distinguishable: if Move's bar strip names THIS track, Move is
 * step-editing a clip here, so one exists and we simply cannot name it. If the
 * strip names nothing, the track really is empty.
 */

/* All inputs in the units the rest of the lane code uses: quarters, and a
 * 1-based bar as the strip reports it.
 *
 * `clip_len_quarters` <= 0 means "unknown", which skips the bounds check
 * rather than failing it -- a clip Move has not saved yet has no length, and
 * refusing a p-lock on it would take the gesture away exactly where the rest
 * of this feature works hardest to keep it.
 *
 * Returns STEP_PLOCK_OK and writes *out_phase, else a reason. */
static inline int step_plock_phase(int bar_1based, int step_index,
                                   double quarters_per_bar,
                                   double step_resolution,
                                   double clip_len_quarters,
                                   double *out_phase)
{
    if (out_phase) *out_phase = NAN;
    if (bar_1based < 1) return STEP_PLOCK_NO_BAR;
    if (step_index < 0 || step_index >= STEP_STRIP_STEPS_PER_PAGE)
        return STEP_PLOCK_BAD_INDEX;
    if (!isfinite(step_resolution) || step_resolution <= 0.0)
        return STEP_PLOCK_NO_GRID;
    if (!isfinite(quarters_per_bar) || quarters_per_bar <= 0.0)
        return STEP_PLOCK_NO_GRID;

    const double steps_per_bar_f = quarters_per_bar / step_resolution;
    if (!isfinite(steps_per_bar_f) || steps_per_bar_f < 1.0)
        return STEP_PLOCK_NO_GRID;
    const int steps_per_bar = (int)(steps_per_bar_f + 0.5);
    /* A bar that does not fit the step buttons is displayed across pages, and
     * which page is on screen is a fact we do not have. */
    if (steps_per_bar > STEP_STRIP_STEPS_PER_PAGE) return STEP_PLOCK_MULTI_PAGE;
    /* A step index past the bar's own length is not on the buttons for this
     * bar at all -- at 1/8 in 4/4 a bar is 8 steps, so buttons 9..16 belong to
     * no step of it. */
    if (step_index >= steps_per_bar) return STEP_PLOCK_BAD_INDEX;

    const double phase = ((double)(bar_1based - 1) * (double)steps_per_bar
                          + (double)step_index) * step_resolution;
    if (!isfinite(phase) || phase < 0.0) return STEP_PLOCK_NO_GRID;
    if (clip_len_quarters > 0.0 && phase >= clip_len_quarters)
        return STEP_PLOCK_OUTSIDE_CLIP;
    if (out_phase) *out_phase = phase;
    return STEP_PLOCK_OK;
}

/* HOW LONG THE CLIP IS *NOW*, from the two sources that disagree.
 *
 * `file_len` is the clip's end in quarters as Song.abl last recorded it, and
 * `strip_len` is what Move's own bar strip is DRAWING -- segments x
 * quarters-per-bar, live off the screen.
 *
 * THE FILE LAGS BY TENS OF SECONDS. Move writes a clip about 10 s after it is
 * made and ~35 s after an edit, so extending a clip and immediately locking a
 * step in the new bars was refused as OUTSIDE_CLIP: the step WAS inside the
 * clip, and the only thing that disagreed was a file not yet written.
 * Reported from the device in exactly those terms -- "at that point it wasn't
 * [past the end], it just hadn't synced with the file yet".
 *
 * THE LONGER OF THE TWO WINS, and the direction is the point rather than a
 * tie-break. A step the user can SEE on the strip is a step that exists; the
 * cost of erring long is accepting a lock on a bar that is about to exist
 * anyway (the lane store already holds points beyond the current window
 * dormant until the window grows -- the same rule a doubled loop relies on),
 * while the cost of erring short is refusing an edit the user just made, with
 * a message telling them their clip is shorter than it visibly is.
 *
 * This was already the rule for a clip with NO file entry at all, argued in
 * the same direction ("erring long refuses nothing legitimate"); it was
 * simply gated on the file being absent rather than on it being STALE, which
 * is the same condition observed a few seconds later. */
static inline double step_plock_clip_len(double file_len,
                                         int strip_valid, double strip_len)
{
    double best = (file_len > 0.0 && isfinite(file_len)) ? file_len : 0.0;
    if (strip_valid && isfinite(strip_len) && strip_len > best) best = strip_len;
    return best;
}

/* THE SCROLL FORM: the displayed page's origin, straight from Move.
 *
 * `scroll_beats` is where the 16 buttons START, in quarters from the clip's
 * own zero, so button `step_index` is that many steps further on. No bar, no
 * signature, no page count -- which is why this has no MULTI_PAGE refusal to
 * make: a bar spanning two pages simply has two scroll positions, and Move
 * has already told us which one is up.
 *
 * A negative scroll is refused rather than clamped: it is not a page, and
 * clamping it to 0 would place a p-lock on the first bar of a clip the user
 * is not looking at. */
/* ON A TRIPLET GRID, A QUARTER OF THE BUTTONS ARE NOT STEPS.
 *
 * Move lays triplets out three to a group and DEACTIVATES every fourth
 * button, so a page carries 12 steps across 16 buttons. Measured 2026-09-13:
 * one right-arrow at 1/16t moved the scroll 0 -> 2.0, which is 12 steps of
 * 1/6 quarter exactly, and Move 2.1.0's notes say the same ("24 steps across
 * two pages, with every fourth step deactivated").
 *
 * So the button index is NOT the step index, and treating it as one puts
 * every value from the fourth button onward progressively early -- button 4
 * is step 3, button 8 is step 6. Returns -1 for a button that is not a step.
 *
 * The triplet-ness has to be carried separately from the resolution because
 * the duration cannot reveal it: 1/16t is 1/6 of a quarter, and so would a
 * straight 1/24 be. */
static inline int step_plock_button_to_step(int button, int triplet)
{
    if (button < 0 || button >= STEP_STRIP_STEPS_PER_PAGE) return -1;
    if (!triplet) return button;
    if ((button % 4) == 3) return -1;      /* the deactivated one */
    return button - (button / 4);
}

static inline int step_plock_phase_from_scroll(double scroll_beats,
                                               int step_index,
                                               double step_resolution,
                                               int triplet_grid,
                                               double clip_len_quarters,
                                               double *out_phase)
{
    if (out_phase) *out_phase = NAN;
    if (!isfinite(step_resolution) || step_resolution <= 0.0)
        return STEP_PLOCK_NO_GRID;
    if (!isfinite(scroll_beats) || scroll_beats < 0.0)
        return STEP_PLOCK_NO_BAR;
    const int step_in_page = step_plock_button_to_step(step_index,
                                                       triplet_grid);
    if (step_in_page < 0)
        return STEP_PLOCK_BAD_INDEX;

    const double phase = scroll_beats + (double)step_in_page * step_resolution;
    if (!isfinite(phase) || phase < 0.0) return STEP_PLOCK_NO_GRID;
    /* Past the end is refused for the same reason as the bar form: Move lets
     * you page onto the "+" beyond a clip, and a scroll sitting exactly at the
     * loop end is that -- a bar that does not exist yet, which a lane has no
     * time for. */
    if (clip_len_quarters > 0.0 && phase >= clip_len_quarters)
        return STEP_PLOCK_OUTSIDE_CLIP;
    if (out_phase) *out_phase = phase;
    return STEP_PLOCK_OK;
}

#endif /* STEP_PLOCK_H */
