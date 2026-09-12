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
 * WHERE THE PAGE COMES FROM: nowhere, and that is a REFUSAL rather than a
 * guess. A bar spans one page only while `steps_per_bar <= STEPS_PER_PAGE` --
 * true for 4/4 at 1/16 (the default, and Move's own "the entire bar can be
 * accessed at once") and false for 11/8 at 1/16, which pages 16 + 6. Move puts
 * the page number on the display as you move between pages; we do not read it
 * yet. So a multi-page bar answers "cannot tell", and the caller must not
 * write a breakpoint at a phase it cannot place: a p-lock one page out is a
 * value on the wrong sixteenth, silently.
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

#endif /* STEP_PLOCK_H */
