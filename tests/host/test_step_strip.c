/* test_step_strip.c - the step-editor bar strip decoder.
 *
 * WHAT THESE FIXTURES ARE. Case 1 is the geometry MEASURED on hardware on
 * 2026-09-12 (a 5-bar loop: segments 1-23, 26-49, 52-74, 77-100, 103-126,
 * playhead at x=79), written out column by column rather than produced by the
 * same renderer the other cases use -- so the positive control is the real
 * screen's numbers and not a restatement of my model of it.
 *
 * Everything else is synthetic, and synthetic fixtures can only prove the
 * ALGORITHM. Whether the rejection gates refuse Move's other screens is a
 * hardware question, which is why the decoder is wired to the clip_state
 * diagnostic first and nothing depends on it yet. The negative cases here are
 * the ones a synthetic frame CAN answer: a blank screen, a full-width line
 * that is not the editor's, a partial line, uneven pieces.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "step_strip.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); fails++; } } while (0)

static uint8_t fb[1024];

static void px(int row, int col, int on)
{
    if (row < 0 || row > 63 || col < 0 || col > 127) return;
    uint8_t m = (uint8_t)(1u << (row % 8));
    if (on) fb[(row / 8) * 128 + col] |= m;
    else    fb[(row / 8) * 128 + col] &= (uint8_t)~m;
}

static void hline(int row, int x0, int x1) { for (int x = x0; x <= x1; x++) px(row, x, 1); }

/* Draw a strip of `bars` equal segments over 1..126 with 2 px gaps, the
 * displayed bar thickened, and the playhead at `ph_col` (-1 for none). */
static void draw_strip(int bars, int bold, int ph_col)
{
    memset(fb, 0, sizeof(fb));
    /* Integer layout, so the gaps come out EXACTLY 2 px. Computing the
     * segment edges in floating point put a 1 px gap between two of them at
     * 3 and 5 bars, which the decoder correctly read as a playhead and a
     * merged bar -- the renderer was wrong, not the decoder, and a fixture
     * that draws the wrong picture tests nothing it claims to. */
    const int span = STEP_STRIP_X1 - STEP_STRIP_X0 + 1;
    const int usable = span - 2 * (bars - 1);
    const int base = usable / bars;
    int rem = usable % bars;
    int s = STEP_STRIP_X0;
    for (int i = 0; i < bars; i++) {
        int w = base + (i < rem ? 1 : 0);
        int e = s + w - 1;
        hline(STEP_STRIP_ROW, s, e);
        if (i + 1 == bold) { hline(58, s, e); hline(60, s, e); }
        s = e + 3;
    }
    if (ph_col >= 0) {
        px(STEP_STRIP_ROW, ph_col, 0);
        px(STEP_STRIP_ROW_STUB, ph_col, 1);
        px(55, ph_col, 1);
    }
}

int main(void)
{
    step_strip_t r;

    /* 1. THE MEASURED FRAME. 5 bars, bar 5 displayed, playhead at x=79. */
    memset(fb, 0, sizeof(fb));
    hline(59, 1, 23); hline(59, 26, 49); hline(59, 52, 74);
    hline(59, 77, 100); hline(59, 103, 126);
    hline(58, 103, 126); hline(60, 103, 126);     /* bar 5 is the bold one */
    px(59, 79, 0);                                 /* the 1 px interruption */
    px(62, 79, 1); px(55, 79, 1);                  /* and its stubs */
    step_strip_decode(fb, &r);
    CHECK(r.valid, "the measured frame was refused (reject=%d)", r.reject);
    CHECK(r.bars == 5, "bars=%d, want 5 -- the playhead must not split a bar", r.bars);
    CHECK(r.bold_bar == 5, "bold_bar=%d, want 5", r.bold_bar);
    CHECK(r.playhead_col == 79, "playhead_col=%d, want 79", r.playhead_col);
    CHECK(r.playhead_evidence == (STEP_STRIP_PH_STUB | STEP_STRIP_PH_GAP),
          "evidence=0x%x, want both signatures", r.playhead_evidence);
    CHECK(fabs(r.phase_frac - 78.0 / 125.0) < 1e-9,
          "phase_frac=%f, want %f", r.phase_frac, 78.0 / 125.0);

    /* 1b. The other measured sighting: the playhead WRAPPED to x=17, which is
     * inside bar 1 while bar 5 is still the displayed one. Page-independence
     * is the whole reason this beats the step LEDs, so it is a test. */
    memset(fb, 0, sizeof(fb));
    hline(59, 1, 23); hline(59, 26, 49); hline(59, 52, 74);
    hline(59, 77, 100); hline(59, 103, 126);
    hline(58, 103, 126); hline(60, 103, 126);
    px(59, 17, 0); px(62, 17, 1);
    step_strip_decode(fb, &r);
    CHECK(r.valid && r.bars == 5 && r.playhead_col == 17 && r.bold_bar == 5,
          "wrapped playhead on a non-displayed bar: valid=%d bars=%d col=%d bold=%d",
          r.valid, r.bars, r.playhead_col, r.bold_bar);

    /* 2. The playhead sitting ON a bar boundary widens that gap to 3 px. It is
     * still ONE boundary -- counting it twice, or refusing the frame, both
     * lose the bar count for a whole beat every bar. Its column is not
     * recoverable from the strip there, so only the stub names it. */
    memset(fb, 0, sizeof(fb));
    hline(59, 1, 23); hline(59, 26, 49); hline(59, 52, 74);
    hline(59, 77, 100); hline(59, 103, 126);
    hline(58, 1, 23); hline(60, 1, 23);
    px(59, 26, 0);                 /* gap 24-26 = 3 px */
    px(62, 26, 1);
    step_strip_decode(fb, &r);
    CHECK(r.valid, "playhead on a boundary refused the frame (reject=%d)", r.reject);
    CHECK(r.bars == 5, "bars=%d, want 5 with the playhead on a boundary", r.bars);
    CHECK(r.playhead_col == 26 && r.playhead_evidence == STEP_STRIP_PH_STUB,
          "col=%d evidence=0x%x -- the stub alone should name it",
          r.playhead_col, r.playhead_evidence);

    /* 3. A blank screen says nothing. */
    memset(fb, 0, sizeof(fb));
    step_strip_decode(fb, &r);
    CHECK(!r.valid && r.reject == STEP_STRIP_NO_STRIP,
          "blank frame: valid=%d reject=%d", r.valid, r.reject);

    /* 4. FALSE-POSITIVE CONTROL: a full-width line with no displayed-bar
     * thickening is not the editor. Without this gate a 1-bar clip -- one
     * solid 126 px run with no gaps for the uniformity test to measure --
     * is indistinguishable from any rule or progress bar drawn on row 59. */
    memset(fb, 0, sizeof(fb));
    hline(59, 1, 126);
    step_strip_decode(fb, &r);
    CHECK(!r.valid && r.reject == STEP_STRIP_NO_BOLD,
          "a bare full-width line was accepted: valid=%d reject=%d bars=%d",
          r.valid, r.reject, r.bars);

    /* 4b. ...and the same line WITH the thickening is a one-bar loop. */
    memset(fb, 0, sizeof(fb));
    hline(59, 1, 126); hline(58, 1, 126); hline(60, 1, 126);
    step_strip_decode(fb, &r);
    CHECK(r.valid && r.bars == 1 && r.bold_bar == 1,
          "a 1-bar loop: valid=%d bars=%d bold=%d", r.valid, r.bars, r.bold_bar);

    /* 5. A line that does not span the display belongs to another screen. */
    memset(fb, 0, sizeof(fb));
    hline(59, 30, 100); hline(58, 30, 100);
    step_strip_decode(fb, &r);
    CHECK(!r.valid && r.reject == STEP_STRIP_NOT_FULL_WIDTH,
          "a partial line was accepted: valid=%d reject=%d", r.valid, r.reject);

    /* 6. Uneven pieces are not bars. */
    memset(fb, 0, sizeof(fb));
    hline(59, 1, 10); hline(59, 13, 126); hline(58, 13, 126);
    step_strip_decode(fb, &r);
    CHECK(!r.valid && r.reject == STEP_STRIP_NONUNIFORM,
          "uneven segments accepted: valid=%d reject=%d bars=%d",
          r.valid, r.reject, r.bars);

    /* 7. A hole wider than any boundary is not a strip at all. */
    memset(fb, 0, sizeof(fb));
    hline(59, 1, 40); hline(59, 60, 126); hline(58, 1, 40);
    step_strip_decode(fb, &r);
    CHECK(!r.valid && r.reject == STEP_STRIP_GAP_TOO_WIDE,
          "a 19 px hole accepted: valid=%d reject=%d", r.valid, r.reject);

    /* 8. Every bar count the tolerance can carry, counted right. */
    for (int bars = 1; bars <= 8; bars++) {
        draw_strip(bars, 1, -1);
        step_strip_decode(fb, &r);
        CHECK(r.valid && r.bars == bars,
              "%d bars read as valid=%d bars=%d reject=%d",
              bars, r.valid, r.bars, r.reject);
    }

    /* 9. Beyond the cap the segments are too narrow for the uniformity test to
     * mean anything, so the frame is REFUSED rather than answered. */
    draw_strip(STEP_STRIP_MAX_BARS + 4, 1, -1);
    step_strip_decode(fb, &r);
    CHECK(!r.valid, "%d bars produced a bar count anyway (bars=%d)",
          STEP_STRIP_MAX_BARS + 4, r.bars);

    /* 10. phase_frac is linear over the span's ENDS, not over 0..127. */
    draw_strip(4, 1, -1);
    px(59, STEP_STRIP_X0, 0); px(62, STEP_STRIP_X0, 1);
    step_strip_decode(fb, &r);
    CHECK(r.valid && r.phase_frac == 0.0, "playhead at x=1: frac=%f", r.phase_frac);
    draw_strip(4, 1, -1);
    px(59, STEP_STRIP_X1, 0); px(62, STEP_STRIP_X1, 1);
    step_strip_decode(fb, &r);
    CHECK(r.valid && fabs(r.phase_frac - 1.0) < 1e-12,
          "playhead at x=126: frac=%f", r.phase_frac);

    /* 11. Two interruptions is a frame we do not understand. The bar count
     * survives (neither hole splits a bar), but nothing is chosen between
     * them -- picking one would be a phase anchor invented from a
     * coincidence. */
    draw_strip(5, 2, -1);
    px(59, 10, 0); px(59, 60, 0);
    step_strip_decode(fb, &r);
    CHECK(r.valid && r.bars == 5, "two interruptions: valid=%d bars=%d",
          r.valid, r.bars);
    CHECK(r.playhead_col < 0 && r.playhead_evidence == 0,
          "two interruptions named a playhead: col=%d ev=0x%x",
          r.playhead_col, r.playhead_evidence);

    /* 12. The published reading: paired with the track selected AT DECODE
     * TIME, and an INVALID frame must not clear a cached length -- Move shows
     * something other than the editor most of the time, and a length that
     * flickers away is worse than one that is merely old. */
    step_strip_reset();
    CHECK(step_strip_bars_for_track(1) == 0, "reset left a cached length");
    draw_strip(3, 1, -1);
    step_strip_observe(fb, 1);
    unsigned seq1 = step_strip_latest(&r, NULL);
    CHECK(seq1 != 0 && r.valid && r.bars == 3, "observe published nothing usable");
    CHECK(step_strip_bars_for_track(1) == 3, "cache=%d, want 3",
          step_strip_bars_for_track(1));
    memset(fb, 0, sizeof(fb));            /* Move left the editor */
    step_strip_observe(fb, 1);
    int trk = -9;
    unsigned seq2 = step_strip_latest(&r, &trk);
    CHECK(seq2 != seq1, "the sequence number did not move on a new frame");
    CHECK(!r.valid && trk == 1, "latest: valid=%d track=%d", r.valid, trk);
    CHECK(step_strip_bars_for_track(1) == 3,
          "an invalid frame cleared the cached length (now %d)",
          step_strip_bars_for_track(1));
    /* A valid frame for another track does not touch this one's. */
    draw_strip(7, 1, -1);
    step_strip_observe(fb, 2);
    CHECK(step_strip_bars_for_track(2) == 7 && step_strip_bars_for_track(1) == 3,
          "cross-track leak: t1=%d t2=%d",
          step_strip_bars_for_track(1), step_strip_bars_for_track(2));
    /* No selected track: published, but cached against nothing. */
    draw_strip(2, 1, -1);
    step_strip_observe(fb, -1);
    CHECK(step_strip_latest(&r, &trk) && r.valid && trk == -1,
          "a reading with no selected track was dropped");

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: step_strip\n");
    return 0;
}
