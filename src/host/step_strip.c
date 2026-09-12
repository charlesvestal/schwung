/* step_strip.c - see step_strip.h for the geometry and why this exists. */

#include <string.h>
#include "step_strip.h"

#define ROWBIT(frame, row, col) \
    (!!((frame)[((row) / 8) * 128 + (col)] & (1u << ((row) % 8))))

void step_strip_decode(const uint8_t *frame, step_strip_t *out)
{
    memset(out, 0, sizeof(*out));
    out->playhead_col = -1;
    out->phase_frac = -1.0;
    if (!frame) { out->reject = STEP_STRIP_NO_STRIP; return; }

    /* 1. The strip row, over its span only. */
    int first = -1, last = -1;
    for (int x = STEP_STRIP_X0; x <= STEP_STRIP_X1; x++) {
        if (!ROWBIT(frame, STEP_STRIP_ROW, x)) continue;
        if (first < 0) first = x;
        last = x;
    }
    if (first < 0) { out->reject = STEP_STRIP_NO_STRIP; return; }
    /* A strip that does not reach both ends is some other screen's line. The
     * editor's strip is the clip, so it always spans the display. */
    if (first > STEP_STRIP_X0 + 1 || last < STEP_STRIP_X1 - 1) {
        out->reject = STEP_STRIP_NOT_FULL_WIDTH;
        return;
    }

    /* 2. Walk the span. A hole of GAP_MIN or more is a bar boundary; a
     * narrower one is the playhead interrupting a bar, and must NOT split it
     * -- counting it would make a 5-bar loop read as 6.
     *
     * A hole of GAP_MIN + 1 is the playhead sitting ON a boundary. It is one
     * boundary, and the playhead's column inside it is not recoverable from
     * the strip alone, so the gap evidence is withheld (the stub below can
     * still name it). */
    int seg_start[STEP_STRIP_MAX_BARS + 1];
    int seg_end[STEP_STRIP_MAX_BARS + 1];
    int nseg = 0;
    int gap_ph_col = -1;
    int gap_ph_n = 0;
    int run_start = first;
    int x = first;
    while (x <= last) {
        if (ROWBIT(frame, STEP_STRIP_ROW, x)) { x++; continue; }
        int hole = x;
        while (hole <= last && !ROWBIT(frame, STEP_STRIP_ROW, hole)) hole++;
        int width = hole - x;
        if (width < STEP_STRIP_GAP_MIN) {
            /* The playhead, inside a bar. */
            gap_ph_col = x;
            gap_ph_n++;
        } else if (width <= STEP_STRIP_GAP_MIN + 1) {
            if (nseg > STEP_STRIP_MAX_BARS) { out->reject = STEP_STRIP_TOO_MANY_BARS; return; }
            seg_start[nseg] = run_start;
            seg_end[nseg] = x - 1;
            nseg++;
            run_start = hole;
        } else {
            /* Wider than any boundary: not the strip. */
            out->reject = STEP_STRIP_GAP_TOO_WIDE;
            return;
        }
        x = hole;
    }
    if (nseg > STEP_STRIP_MAX_BARS) { out->reject = STEP_STRIP_TOO_MANY_BARS; return; }
    seg_start[nseg] = run_start;
    seg_end[nseg] = last;
    nseg++;
    if (nseg > STEP_STRIP_MAX_BARS) { out->reject = STEP_STRIP_TOO_MANY_BARS; return; }

    /* 3. The segments must divide the span evenly. This is the gate that
     * separates the editor's strip from any other full-width line: a ruler, a
     * progress bar or a separator does not come in equal pieces with 2 px
     * gaps. */
    const int span = last - first + 1;
    const double expect = (double)(span - STEP_STRIP_GAP_MIN * (nseg - 1)) / (double)nseg;
    for (int i = 0; i < nseg; i++) {
        double w = (double)(seg_end[i] - seg_start[i] + 1);
        double d = w - expect;
        if (d < 0) d = -d;
        if (d > (double)STEP_STRIP_SEG_TOL) { out->reject = STEP_STRIP_NONUNIFORM; return; }
    }

    /* 4. The displayed bar is thickened, and its absence is how a single-bar
     * clip is told from an unrelated full-width line -- a 1-bar loop is one
     * solid 126 px run with no gaps to measure, so without this gate the
     * uniformity test above has nothing to say about it. */
    int bold = 0;
    for (int i = 0; i < nseg && !bold; i++) {
        int lit = 0;
        for (int c = seg_start[i]; c <= seg_end[i]; c++)
            if (ROWBIT(frame, STEP_STRIP_ROW_BOLD, c)) lit++;
        /* Most of the segment, not a stray pixel or a glyph's descender. */
        if ((double)lit >= expect - (double)STEP_STRIP_SEG_TOL) bold = i + 1;
    }
    if (!bold) { out->reject = STEP_STRIP_NO_BOLD; return; }

    /* 5. The playhead. Two independent signatures; the stub is the definite
     * one (nothing else is drawn below the strip), the interruption
     * corroborates it. A second interruption means we are reading something
     * we do not understand, so the gap evidence is dropped rather than
     * guessed between. */
    int stub_col = -1, stub_n = 0;
    for (int c = STEP_STRIP_X0; c <= STEP_STRIP_X1; c++) {
        if (!ROWBIT(frame, STEP_STRIP_ROW_STUB, c)) continue;
        stub_col = c;
        stub_n++;
    }
    int col = -1, ev = 0;
    if (stub_n == 1) { col = stub_col; ev |= STEP_STRIP_PH_STUB; }
    if (gap_ph_n == 1) {
        if (col < 0) { col = gap_ph_col; ev |= STEP_STRIP_PH_GAP; }
        else if (gap_ph_col >= col - 1 && gap_ph_col <= col + 1) ev |= STEP_STRIP_PH_GAP;
    }

    out->valid = 1;
    out->reject = STEP_STRIP_OK;
    out->bars = nseg;
    out->bold_bar = bold;
    out->playhead_col = col;
    out->playhead_evidence = ev;
    if (col >= 0) {
        double f = (double)(col - STEP_STRIP_X0) /
                   (double)(STEP_STRIP_X1 - STEP_STRIP_X0);
        if (f < 0.0) f = 0.0;
        if (f > 1.0) f = 1.0;
        out->phase_frac = f;
    }
}

/* ---- published reading ---------------------------------------------------
 * Written on the SPI callback where the frame completes, read by the 200 ms
 * worker. One writer, and every field is published together under a sequence
 * number the reader can see move; a reader that catches a half-written struct
 * sees a bar count that is still a bar count, and the next frame corrects it.
 * No lock, because a lock held by a non-RT thread is the one thing the
 * callback may not wait on. */
static step_strip_t  g_last;
static int           g_last_track = -1;
static unsigned      g_seq;
static int           g_bars[4];   /* CLIP_TRACKS, kept local to avoid the dep */

void step_strip_observe(const uint8_t *frame, int selected_track)
{
    step_strip_t r;
    step_strip_decode(frame, &r);
    g_last = r;
    g_last_track = selected_track;
    g_seq++;
    /* Only a VALID reading updates the cache, and only for a named track. An
     * invalid frame is Move showing something else, which says nothing about
     * the clip -- clearing on it would make the length flicker away every
     * time the user left the editor, which is most of the time. */
    if (r.valid && selected_track >= 0 && selected_track < 4)
        g_bars[selected_track] = r.bars;
}

unsigned step_strip_latest(step_strip_t *out, int *track)
{
    if (out) *out = g_last;
    if (track) *track = g_last_track;
    return g_seq;
}

int step_strip_bars_for_track(int track)
{
    if (track < 0 || track >= 4) return 0;
    return g_bars[track];
}

void step_strip_reset(void)
{
    memset(&g_last, 0, sizeof(g_last));
    g_last.playhead_col = -1;
    g_last.phase_frac = -1.0;
    g_last_track = -1;
    memset(g_bars, 0, sizeof(g_bars));
    /* g_seq is NOT reset: it is "has anything been observed", and rewinding it
     * would let a reader mistake a fresh reading for the one it already saw. */
}
