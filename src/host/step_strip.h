/* step_strip.h - read Move's step-editor bar strip off its own OLED frame
 *
 * WHY THIS EXISTS. Make a clip in Move's step editor, press Play, try to
 * record automation: refused. `T1 -`, `loop_len 0.00`, `has_phase false`. The
 * clip is not in `Song.abl` yet -- Move saves about 35 s after an edit -- so we
 * have no length; with no length there is no phase; with no phase, recording
 * correctly refuses rather than guessing. That is the entire hole this reader
 * closes, and it closes it the only way that does not involve modelling Move:
 * Move's own step-editor screen draws the clip's committed segment count, so we
 * read its answer instead of maintaining a parallel one.
 *
 * NOT A MODEL OF MOVE'S SEQUENCER UI. Read what is on the screen; never track
 * its modes, pages or loop points. Every time the lanes work drifted toward a
 * parallel model of Move's editor it produced a bug.
 *
 * WHAT IT CANNOT TELL YOU: where the loop BEGINS in the clip. The strip shows
 * how many bars long the loop is and where the playhead sits inside it, and
 * nothing about the offset into the clip -- confirmed by eye on hardware. That
 * is fine: lane phases are stored LOOP-RELATIVE, so nothing needs it.
 *
 * FRAME FORMAT. 1024 bytes, 8 pages of 128 columns, `buf[(row/8)*128 + col]`
 * with the row's bit at `1 << (row % 8)` -- the same convention the PIN scanner
 * and the volume-bar scanner already read. The whole strip lives in page 7
 * (rows 56-63) plus row 55 in page 6, so a decode is a scan of ~256 bytes.
 *
 * GEOMETRY, MEASURED ON HARDWARE 2026-09-12 (a 5-bar loop):
 *
 *   row 59       (1-23) (26-49) (52-74) (77-100) (103-126)  <- 5 segments
 *   rows 58-60   thicker over one segment                   <- the DISPLAYED bar
 *   playhead     a 1 px INTERRUPTION in the strip, plus a stub at
 *                rows 55-57 and 61-63
 *
 * Segments are 23-24 px separated by 2 px gaps, spanning x=1..126. The 1 px
 * playhead against 2 px gaps is what makes the two separable: a run of unlit
 * columns splits the strip into bars only when it is at least 2 wide, so the
 * playhead does not inflate the segment count.
 *
 * The playhead is PAGE-INDEPENDENT, which is what makes it better than the
 * step LEDs: measured drawn at bars 3 and 4 while bar 5 was the displayed one.
 * The LED playhead is visible only while the displayed page IS the playing
 * page, so on a long clip it is dark for 15 bars in 16.
 *
 * Its resolution is coarse: ~6.25 px/beat on a 20-beat loop, i.e. 0.16
 * beats/px (~80 ms at 120 BPM). Ample as an ANCHOR that re-syncs while the
 * pulse counter interpolates between frames; not a position readout.
 *
 * VALIDATION STATUS. The geometry above is measured; the REJECTION gates below
 * are reasoned, and a false positive here is a wrong loop length, so this is
 * wired to the clip_state diagnostic first and nothing depends on it until a
 * hardware pass has seen it answer on the step editor and refuse on Move's
 * other screens. `reject` names which gate refused, so a refusal on the real
 * editor is diagnosable rather than merely silent.
 *
 * RT: pure, no allocation, no I/O, one pass over 128 columns. Safe on the SPI
 * callback, which is where the frame completes.
 */

#ifndef STEP_STRIP_H
#define STEP_STRIP_H

#include <stdint.h>

/* The strip's span. x=0 and x=127 are not part of it. */
#define STEP_STRIP_X0        1
#define STEP_STRIP_X1        126

/* Rows, as measured. One place to re-measure. */
#define STEP_STRIP_ROW       59   /* the strip itself */
#define STEP_STRIP_ROW_BOLD  58   /* thickening over the displayed bar */
#define STEP_STRIP_ROW_STUB  62   /* playhead stub below the strip */

/* A gap this wide or wider separates two bars; anything narrower is the
 * playhead interrupting one. */
#define STEP_STRIP_GAP_MIN   2

/* Beyond this the segments are too narrow for the uniformity test to mean
 * anything: 16 segments is already only 6 px each. A longer clip is REFUSED
 * rather than guessed at. */
#define STEP_STRIP_MAX_SEGMENTS  16

/* A SEGMENT IS A BAR, ROUNDED UP -- and it took two wrong answers to get here.
 *
 * Move's manual, on this exact strip: "Each line represents a bar... A thick
 * line specifies that the bar is selected and part of the loop... A thin line
 * indicates that the bar is part of the loop but not selected. A plus icon
 * signifies that the bar is outside of the loop."
 *
 * MEASURED against the file on an 11/8 set (quarters per bar = 5.5):
 *
 *     T2  16 quarters   ceil(16/5.5) = 3 bars   4 pages   strip: 3
 *     T1  12 quarters   ceil(12/5.5) = 3 bars   3 pages   strip: 3
 *     T4   4 quarters   ceil(4/5.5)  = 1 bar    1 page    strip: refused
 *
 * T2 is the only one that separates the two models, and it says BARS. The
 * first wrong answer was `bars * 4` (a 4/4 assumption); the second was "a
 * segment is a 16-step page", which came from T1 agreeing exactly through
 * pages -- 12 quarters is 3 pages AND 3 bars-rounded-up, so it never
 * disambiguated anything. One coincidence, believed twice.
 *
 *     quarters ~= segments * quarters_per_bar        (clip_regions.h)
 *
 * and it is a CEILING, so the answer is a RANGE, INCLUSIVE AT BOTH ENDS:
 * n segments under 11/8 means [(n-1) * 5.5, n * 5.5].
 *
 * THE LOWER END IS INCLUSIVE BECAUSE MOVE CAN SHOW ONE BAR MORE THAN THE LOOP
 * HOLDS -- measured 2026-09-13 by lengthening a loop on the device with
 * Loop + jog. The file settled at `region == loop == 8.00..24.50`, which is
 * 16.5 quarters and EXACTLY 3 bars of 11/8, while the strip drew FOUR plain
 * identical segments, persistently (switching to another track and back gave
 * 3 for its clip and 4 again for this one). That fourth is the next bar Move
 * offers you to extend into -- the manual's "plus icon signifies that the bar
 * is outside of the loop" -- and at the pixel level it is NOT distinguishable
 * from a bar that is in the loop, so the count cannot be trusted to a bar.
 *
 * So: exact only for a settled clip whose loop is not bar-aligned, an upper
 * bound otherwise, and the FILE WINS whenever it has the clip. The strip's job
 * is the ~10 s before that. An earlier version of this comment claimed
 * "segments = bars, rounded up" full stop; that was a third coincidence, and
 * the loop-lengthening measurement is what broke it. */

/* Move has 16 step buttons, and its playhead index is page-relative to them.
 * Not used for the length -- see above -- but it is the modulus the phase
 * check needs, and deriving that from the bar instead made it worse. */
#define STEP_STRIP_STEPS_PER_PAGE  16

/* How many CONSECUTIVE agreeing readings the per-track cache requires.
 *
 * A frame is assembled from six slices, so it can straddle two of Move's
 * screen updates: the accumulator hands us a TORN picture, part old strip and
 * part new. Measured on hardware 2026-09-12 -- paging and switching tracks in
 * the editor produced one-off refusals on three different gates (not full
 * width, non-uniform, no displayed bar), each for a single reading, which is
 * exactly what a half-drawn strip looks like and is the safe direction to
 * fail in. But a torn frame could in principle come out uniform and WRONG,
 * and a wrong segment count is a wrong loop length, so the cache waits for a
 * second reading that says the same thing. At ~30 frames/s that costs ~33 ms
 * and removes the whole class.
 */
#define STEP_STRIP_CONFIRM   2

/* Slack on a segment's width against the uniform expectation, in pixels.
 * Measured widths were 23 and 24 for an expected 23.6. */
#define STEP_STRIP_SEG_TOL   3

/* Why a frame was refused. Reported, because "we saw no strip" and "we saw a
 * strip we did not believe" are different findings and the second is the one
 * that needs a re-measure. */
enum {
    STEP_STRIP_OK = 0,
    STEP_STRIP_NO_STRIP,      /* nothing lit on the strip row */
    STEP_STRIP_NOT_FULL_WIDTH,/* lit, but not spanning 1..126 */
    STEP_STRIP_GAP_TOO_WIDE,  /* a hole no bar boundary explains */
    STEP_STRIP_TOO_MANY_SEGMENTS,
    STEP_STRIP_NONUNIFORM,    /* segments do not divide the span evenly */
    STEP_STRIP_NO_BOLD        /* no displayed-bar thickening, and more than
                               * one segment: not the editor */
};

/* Playhead evidence, as a bitmask: the two signatures are independent, and
 * which of them fired is the difference between a reading and a coincidence. */
#define STEP_STRIP_PH_STUB   0x1   /* a column lit below the strip */
#define STEP_STRIP_PH_GAP    0x2   /* a 1 px interruption in the strip */

typedef struct {
    int    valid;            /* the bar strip was recognised */
    /* ONE SEGMENT, NO THICKENING -- and it is a real reading, not a refusal.
     * The manual: "if a loop contains only one bar, a thin line is displayed
     * instead." So the displayed-bar gate cannot apply to a one-bar loop, and
     * a one-bar loop is what a NEW clip is, which is the case this reader
     * exists for. It is flagged rather than merged, because it is the one
     * shape indistinguishable from an unrelated full-width line: a consumer
     * that would rather refuse than be wrong can.
     *
     * Surveyed for false positives 2026-09-12 by driving Move through Menu,
     * Loop Mode and the screen Back lands on: row 59 was EMPTY on all three,
     * and the editor's own strip is unmistakable beside them (three 41 px
     * segments, a 41 px bold). Not exhaustive -- it is three screens. */
    int    single_thin;
    int    reject;           /* STEP_STRIP_* -- why not, when !valid */
    int    segments;         /* BARS, rounded up. 1..STEP_STRIP_MAX_SEGMENTS */
    int    bold_segment;     /* the displayed page, 1..segments (0 = unknown) */
    int    playhead_col;     /* x of the playhead, or -1 */
    int    playhead_evidence;/* STEP_STRIP_PH_* bits */
    double phase_frac;       /* playhead as 0..1 of the loop; <0 if unknown */
} step_strip_t;

/* Decode one complete 1024-byte frame. Always writes *out. */
void step_strip_decode(const uint8_t *frame, step_strip_t *out);

/* ---- the published latest reading -----------------------------------------
 *
 * OPPORTUNISTIC, NOT A CLOCK. The segment count does not change while you record,
 * so a reading from ten seconds ago is as good as a live one -- which is what
 * keeps this to one cached fact rather than a second position pipeline. The
 * phase side stays with the existing anchor machinery.
 *
 * `observe` runs where the frame completes (the SPI callback) and publishes a
 * decoded result plus the track that was selected AT THAT MOMENT: the step
 * editor shows one track, and pairing the reading with the selection a worker
 * poll later would attribute it to whatever track is selected by then.
 */
void step_strip_observe(const uint8_t *frame, int selected_track);

/* The last reading, whatever it said. Returns its sequence number, which is 0
 * until a frame has ever been observed; `*track` is the selection it was
 * paired with. Both out params may be NULL. */
unsigned step_strip_latest(step_strip_t *out, int *track);

/* The last reading that was VALID, for the per-track loop-length cache:
 * SEGMENTS for `track`, or 0 if no valid frame has named that track.
 *
 * MULTIPLY BY THE BAR -- `clip_regions_quarters_per_bar()` -- not by
 * STEP_STRIP_STEPS_PER_PAGE * step_resolution. This said the latter, which is
 * right only in 4/4 on a 1/16 grid, where a bar and a 16-step page are the
 * same 4 quarters; it is the coincidence this file warns about elsewhere and
 * then fell for. On the 11/8 set it was measured on, the two differ by 27%
 * (4.0 against 5.5) and the wrong one is silently short.
 *
 * Move's manual says each line is a BAR, and the measurement agrees: the one
 * clip that separates the two (16 quarters = 2.91 bars but exactly 4 pages)
 * drew THREE segments. So this answers a RANGE, ((n-1) * qpb, n * qpb], and
 * is exact only when the loop is a whole number of bars -- which a clip Move
 * created in the current signature is. */
int step_strip_segments_for_track(int track);

/* THE DISPLAYED BAR the strip names for `track`, 1-based, or 0 for "it cannot
 * say" -- which is what step_plock_phase() wants, and it refuses 0 rather than
 * guessing the first bar.
 *
 * THE ONE-BAR CASE IS THE WHOLE REASON THIS IS A FUNCTION. A one-bar loop
 * draws a thin line and no thickening, so `bold_segment` is 0 -- identical, at
 * that field, to "no reading". Every consumer that reached past `valid` for
 * the bold segment therefore refused every single-bar clip, which is the
 * common short clip AND exactly what Move's Shift+Step 14 creates. The reader
 * flagged the shape as `single_thin` and defended the pure function against a
 * bare 0; what was missing was the step BETWEEN them, so it now exists once
 * instead of at each call site.
 *
 * The selection is checked here too: the strip shows ONE track, and a reading
 * paired with a different one would place a p-lock on a bar the user is not
 * looking at. */
static inline int step_strip_displayed_bar(const step_strip_t *ss,
                                           int strip_track, int track)
{
    if (!ss || !ss->valid || strip_track != track) return 0;
    /* Mutually exclusive by construction (`single_thin` is set only when there
     * is no bold and exactly one segment), so the order does not matter --
     * stated because a later reader will wonder. */
    if (ss->single_thin) return 1;
    return ss->bold_segment;
}

/* Forget everything. A set change invalidates every cached length. */
void step_strip_reset(void);

#endif /* STEP_STRIP_H */
