/* step_strip.h - read Move's step-editor bar strip off its own OLED frame
 *
 * WHY THIS EXISTS. Make a clip in Move's step editor, press Play, try to
 * record automation: refused. `T1 -`, `loop_len 0.00`, `has_phase false`. The
 * clip is not in `Song.abl` yet -- Move saves about 35 s after an edit -- so we
 * have no length; with no length there is no phase; with no phase, recording
 * correctly refuses rather than guessing. That is the entire hole this reader
 * closes, and it closes it the only way that does not involve modelling Move:
 * Move's own step-editor screen draws the clip's committed bar count, so we
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
 * playhead does not inflate the bar count.
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
 * anything: 16 bars is already only 6 px each. A longer clip is REFUSED
 * rather than guessed at. */
#define STEP_STRIP_MAX_BARS  16

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
    STEP_STRIP_TOO_MANY_BARS,
    STEP_STRIP_NONUNIFORM,    /* segments do not divide the span evenly */
    STEP_STRIP_NO_BOLD        /* no displayed-bar thickening: not the editor */
};

/* Playhead evidence, as a bitmask: the two signatures are independent, and
 * which of them fired is the difference between a reading and a coincidence. */
#define STEP_STRIP_PH_STUB   0x1   /* a column lit below the strip */
#define STEP_STRIP_PH_GAP    0x2   /* a 1 px interruption in the strip */

typedef struct {
    int    valid;            /* the bar strip was recognised */
    int    reject;           /* STEP_STRIP_* -- why not, when !valid */
    int    bars;             /* loop length in bars, 1..STEP_STRIP_MAX_BARS */
    int    bold_bar;         /* the displayed bar, 1..bars (0 = unknown) */
    int    playhead_col;     /* x of the playhead, or -1 */
    int    playhead_evidence;/* STEP_STRIP_PH_* bits */
    double phase_frac;       /* playhead as 0..1 of the loop; <0 if unknown */
} step_strip_t;

/* Decode one complete 1024-byte frame. Always writes *out. */
void step_strip_decode(const uint8_t *frame, step_strip_t *out);

/* ---- the published latest reading -----------------------------------------
 *
 * OPPORTUNISTIC, NOT A CLOCK. The bar count does not change while you record,
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

/* The last reading that was VALID, for the per-track loop-length cache: bars
 * for `track`, or 0 if no valid frame has named that track. */
int step_strip_bars_for_track(int track);

/* Forget everything. A set change invalidates every cached length. */
void step_strip_reset(void);

#endif /* STEP_STRIP_H */
