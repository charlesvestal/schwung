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
    CHECK(r.segments == 5, "segments=%d, want 5 -- the playhead must not split a bar", r.segments);
    CHECK(r.bold_segment == 5, "bold_bar=%d, want 5", r.bold_segment);
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
    CHECK(r.valid && r.segments == 5 && r.playhead_col == 17 && r.bold_segment == 5,
          "wrapped playhead on a non-displayed bar: valid=%d segments=%d col=%d bold=%d",
          r.valid, r.segments, r.playhead_col, r.bold_segment);

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
    CHECK(r.segments == 5, "segments=%d, want 5 with the playhead on a boundary", r.segments);
    CHECK(r.playhead_col == 26 && r.playhead_evidence == STEP_STRIP_PH_STUB,
          "col=%d evidence=0x%x -- the stub alone should name it",
          r.playhead_col, r.playhead_evidence);

    /* 3. A blank screen says nothing. */
    memset(fb, 0, sizeof(fb));
    step_strip_decode(fb, &r);
    CHECK(!r.valid && r.reject == STEP_STRIP_NO_STRIP,
          "blank frame: valid=%d reject=%d", r.valid, r.reject);

    /* 4. A ONE-BAR LOOP IS A BARE THIN LINE, and Move's manual says so:
     * "if a loop contains only one bar, a thin line is displayed instead."
     *
     * THIS TEST ASSERTED THE OPPOSITE and was wrong on hardware: the 1-bar
     * clip on T4 came back refused (gate 6) twice before the manual explained
     * it, and a one-bar loop is exactly what a NEW clip is -- the case this
     * whole reader exists for. It is the one shape indistinguishable from an
     * unrelated full-width line, so it is FLAGGED rather than merged:
     * `single_thin` lets a consumer refuse it.
     *
     * Surveyed for false positives by driving Move through Menu, Loop Mode and
     * the screen Back lands on -- row 59 empty on all three. Three screens is
     * not proof, which is what the flag is for. */
    memset(fb, 0, sizeof(fb));
    hline(59, 1, 126);
    step_strip_decode(fb, &r);
    CHECK(r.valid && r.segments == 1 && r.single_thin == 1,
          "a bare full-width line: valid=%d segments=%d single_thin=%d "
          "(Move draws a one-bar loop exactly like this)",
          r.valid, r.segments, r.single_thin);

    /* 4b. ...and the same line WITH thickening is a one-bar loop that is NOT
     * ambiguous -- so the flag is off. (Move draws the thin line for a 1-bar
     * loop, but a clip whose single bar is selected can carry it.) */
    memset(fb, 0, sizeof(fb));
    hline(59, 1, 126); hline(58, 1, 126); hline(60, 1, 126);
    step_strip_decode(fb, &r);
    CHECK(r.valid && r.segments == 1 && r.bold_segment == 1 && !r.single_thin,
          "a thickened single bar: valid=%d segments=%d bold=%d thin=%d",
          r.valid, r.segments, r.bold_segment, r.single_thin);

    /* 4c. THE GATE THAT REMAINS: two or more segments with no thickening is
     * refused. That is what still separates the editor from a ruler, a
     * progress bar or any other divided line -- the concession above is
     * scoped to the one shape the manual documents. */
    draw_strip(4, 0, -1);          /* four segments, nothing bold */
    step_strip_decode(fb, &r);
    CHECK(!r.valid && r.reject == STEP_STRIP_NO_BOLD,
          "four segments with no bold were accepted: valid=%d reject=%d",
          r.valid, r.reject);

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
          "uneven segments accepted: valid=%d reject=%d segments=%d",
          r.valid, r.reject, r.segments);

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
        CHECK(r.valid && r.segments == bars,
              "%d bars read as valid=%d segments=%d reject=%d",
              bars, r.valid, r.segments, r.reject);
    }

    /* 9. Beyond the cap the segments are too narrow for the uniformity test to
     * mean anything, so the frame is REFUSED rather than answered. */
    draw_strip(STEP_STRIP_MAX_SEGMENTS + 4, 1, -1);
    step_strip_decode(fb, &r);
    CHECK(!r.valid, "%d bars produced a bar count anyway (segments=%d)",
          STEP_STRIP_MAX_SEGMENTS + 4, r.segments);

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
    CHECK(r.valid && r.segments == 5, "two interruptions: valid=%d segments=%d",
          r.valid, r.segments);
    CHECK(r.playhead_col < 0 && r.playhead_evidence == 0,
          "two interruptions named a playhead: col=%d ev=0x%x",
          r.playhead_col, r.playhead_evidence);

    /* 11b. TWO REAL FRAMES, captured off the device 2026-09-12 (the dump
     * trigger in shadow_pin_scanner.c writes the accumulated buffer). These
     * are the positive control the synthetic cases cannot be: Move's own
     * pixels, including two things I had not drawn -- the playhead's hole
     * punches the BOLD rows as well as the strip, and the 4-bar screen has
     * the playhead in bar 2 while bar 3 is the displayed one, which is the
     * page-independence claim in actual pixels rather than in a fixture I
     * built to agree with me.
     *
     * A missing fixture FAILS rather than skips: a test that quietly stops
     * measuring is worse than one that is absent. */
    {
        struct { const char *path; int segments, bold, ph; } real[] = {
            { "tests/fixtures/oled_step_editor_5bar.bin",        5, 4, 86 },
            { "tests/fixtures/oled_step_editor_4bar_offpage.bin", 4, 3, 36 },
        };
        for (unsigned i = 0; i < sizeof(real) / sizeof(real[0]); i++) {
            uint8_t buf[1024];
            FILE *f = fopen(real[i].path, "rb");
            size_t got = f ? fread(buf, 1, sizeof(buf), f) : 0;
            if (f) fclose(f);
            CHECK(got == sizeof(buf), "%s: read %zu of 1024 bytes",
                  real[i].path, got);
            if (got != sizeof(buf)) continue;
            step_strip_decode(buf, &r);
            CHECK(r.valid, "%s refused (reject=%d)", real[i].path, r.reject);
            CHECK(r.segments == real[i].segments, "%s: segments=%d, want %d",
                  real[i].path, r.segments, real[i].segments);
            CHECK(r.bold_segment == real[i].bold, "%s: bold_bar=%d, want %d",
                  real[i].path, r.bold_segment, real[i].bold);
            CHECK(r.playhead_col == real[i].ph, "%s: playhead_col=%d, want %d",
                  real[i].path, r.playhead_col, real[i].ph);
            CHECK(r.playhead_evidence ==
                      (STEP_STRIP_PH_STUB | STEP_STRIP_PH_GAP),
                  "%s: evidence=0x%x, want both signatures",
                  real[i].path, r.playhead_evidence);
        }
    }

    /* 12. The published reading: paired with the track selected AT DECODE
     * TIME, and an INVALID frame must not clear a cached length -- Move shows
     * something other than the editor most of the time, and a length that
     * flickers away is worse than one that is merely old. */
    step_strip_reset();
    CHECK(step_strip_segments_for_track(1) == 0, "reset left a cached length");
    draw_strip(3, 1, -1);
    step_strip_observe(fb, 1);
    unsigned seq1 = step_strip_latest(&r, NULL);
    CHECK(seq1 != 0 && r.valid && r.segments == 3, "observe published nothing usable");
    /* ONE reading is not enough: a frame is six slices and can straddle two
     * of Move's screen updates, so a single torn picture must not become a
     * loop length. */
    CHECK(step_strip_segments_for_track(1) == 0,
          "one reading cached a length (%d) -- a torn frame would too",
          step_strip_segments_for_track(1));
    step_strip_observe(fb, 1);
    CHECK(step_strip_segments_for_track(1) == 3, "cache=%d, want 3 after %d agreeing",
          step_strip_segments_for_track(1), STEP_STRIP_CONFIRM);
    memset(fb, 0, sizeof(fb));            /* Move left the editor */
    step_strip_observe(fb, 1);
    int trk = -9;
    unsigned seq2 = step_strip_latest(&r, &trk);
    CHECK(seq2 != seq1, "the sequence number did not move on a new frame");
    CHECK(!r.valid && trk == 1, "latest: valid=%d track=%d", r.valid, trk);
    CHECK(step_strip_segments_for_track(1) == 3,
          "an invalid frame cleared the cached length (now %d)",
          step_strip_segments_for_track(1));
    /* A valid frame for another track does not touch this one's. */
    draw_strip(7, 1, -1);
    step_strip_observe(fb, 2);
    step_strip_observe(fb, 2);
    CHECK(step_strip_segments_for_track(2) == 7 && step_strip_segments_for_track(1) == 3,
          "cross-track leak: t1=%d t2=%d",
          step_strip_segments_for_track(1), step_strip_segments_for_track(2));

    /* 12b. DISAGREEING readings commit nothing, and an invalid frame BREAKS
     * the run -- the rule is that consecutive frames agreed, so a torn
     * picture in the middle of two good ones starts the count over instead of
     * completing it. The already-cached length survives all of it. */
    step_strip_reset();
    draw_strip(3, 1, -1); step_strip_observe(fb, 0);
    draw_strip(6, 1, -1); step_strip_observe(fb, 0);
    CHECK(step_strip_segments_for_track(0) == 0,
          "two DIFFERENT readings cached %d", step_strip_segments_for_track(0));
    draw_strip(6, 1, -1); step_strip_observe(fb, 0);
    CHECK(step_strip_segments_for_track(0) == 6,
          "the agreeing pair did not commit (cache=%d)",
          step_strip_segments_for_track(0));
    draw_strip(2, 1, -1); step_strip_observe(fb, 0);
    memset(fb, 0, sizeof(fb)); step_strip_observe(fb, 0);   /* a torn frame */
    draw_strip(2, 1, -1); step_strip_observe(fb, 0);
    CHECK(step_strip_segments_for_track(0) == 6,
          "an interrupted run committed anyway (cache=%d, want the old 6)",
          step_strip_segments_for_track(0));
    draw_strip(2, 1, -1); step_strip_observe(fb, 0);
    CHECK(step_strip_segments_for_track(0) == 2,
          "the restarted run did not commit (cache=%d)",
          step_strip_segments_for_track(0));
    /* No selected track: published, but cached against nothing. */
    draw_strip(2, 1, -1);
    step_strip_observe(fb, -1);
    CHECK(step_strip_latest(&r, &trk) && r.valid && trk == -1,
          "a reading with no selected track was dropped");

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: step_strip\n");
    return 0;
}
