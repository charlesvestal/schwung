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
    STEP_PLOCK_OUTSIDE_CLIP   /* the step is past the clip's own length */
};

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
static inline int step_plock_phase_from_scroll(double scroll_beats,
                                               int step_index,
                                               double step_resolution,
                                               double clip_len_quarters,
                                               double *out_phase)
{
    if (out_phase) *out_phase = NAN;
    if (!isfinite(step_resolution) || step_resolution <= 0.0)
        return STEP_PLOCK_NO_GRID;
    if (!isfinite(scroll_beats) || scroll_beats < 0.0)
        return STEP_PLOCK_NO_BAR;
    if (step_index < 0 || step_index >= STEP_STRIP_STEPS_PER_PAGE)
        return STEP_PLOCK_BAD_INDEX;

    const double phase = scroll_beats + (double)step_index * step_resolution;
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
