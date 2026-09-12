#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lane_store.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); fails++; } } while (0)

static lane_t *mk(lane_store_t *st) {
    lane_fingerprint_t fp = { 0.0, 8.0, 14, 41 };
    lane_store_reset(st);
    return lane_alloc(st, "synth", "cutoff", 2, 3, &fp);
}

int main(void) {
    lane_store_t st;
    float v;

    /* 1. An empty lane says nothing -- it does not say 0.0. */
    lane_t *ln = mk(&st);
    CHECK(ln != NULL, "alloc returned NULL");
    CHECK(lane_eval(ln, 0.0, 8.0, 0, &v) == 0, "empty lane produced a value");

    /* 2. One point is that value everywhere (hold at both ends). */
    lane_write(ln, 2.0, 0.5f);
    CHECK(lane_eval(ln, 0.0, 8.0, 0, &v) == 1 && fabsf(v - 0.5f) < 1e-6f,
          "single point before: %f", v);
    CHECK(lane_eval(ln, 7.9, 8.0, 0, &v) == 1 && fabsf(v - 0.5f) < 1e-6f,
          "single point after: %f", v);

    /* 3. Linear between two float points. */
    lane_write(ln, 6.0, 1.0f);
    CHECK(lane_eval(ln, 4.0, 8.0, 0, &v) == 1 && fabsf(v - 0.75f) < 1e-6f,
          "midpoint interp: %f", v);

    /* 4. Stepped (int/enum) holds the previous value instead. */
    CHECK(lane_eval(ln, 4.0, 8.0, 1, &v) == 1 && fabsf(v - 0.5f) < 1e-6f,
          "stepped should hold 0.5, got %f", v);

    /* 5. A point past the CURRENT loop end is ignored -- and retained. */
    ln = mk(&st);
    lane_write(ln, 1.0, 0.2f);
    lane_write(ln, 12.0, 0.9f);       /* beyond an 8-beat loop */
    CHECK(lane_eval(ln, 7.0, 8.0, 0, &v) == 1 && fabsf(v - 0.2f) < 1e-6f,
          "dormant point leaked into the curve: %f", v);
    CHECK(ln->n == 2, "dormant point was dropped (n=%d)", ln->n);

    /* 6. Extending the clip reveals it, with no rewrite. */
    CHECK(lane_eval(ln, 12.0, 16.0, 0, &v) == 1 && fabsf(v - 0.9f) < 1e-6f,
          "extended loop did not reveal the point: %f", v);

    /* 7. Thinning: a second write inside the window replaces, not appends. */
    ln = mk(&st);
    lane_write(ln, 1.000f, 0.1f);
    lane_write(ln, 1.005f, 0.4f);     /* < LANE_MIN_POINT_BEATS away */
    CHECK(ln->n == 1, "thinning failed (n=%d)", ln->n);
    CHECK(fabsf(ln->pts[0].value - 0.4f) < 1e-6f,
          "thinning kept the OLD value: %f", ln->pts[0].value);

    /* 8. Out-of-order writes leave the list sorted. */
    ln = mk(&st);
    lane_write(ln, 4.0, 0.4f);
    lane_write(ln, 1.0, 0.1f);
    lane_write(ln, 2.0, 0.2f);
    CHECK(ln->n == 3 && ln->pts[0].phase < ln->pts[1].phase &&
          ln->pts[1].phase < ln->pts[2].phase, "points are not sorted");

    /* 9. A full lane degrades resolution; it never drops the gesture. */
    ln = mk(&st);
    for (int i = 0; i < LANE_POINTS_MAX + 8; i++)
        lane_write(ln, 0.1 + i * 0.5, (float)i / 100.0f);
    CHECK(ln->n == LANE_POINTS_MAX, "overflowed (n=%d)", ln->n);
    CHECK(ln->full_hits == 8, "full_hits=%d, want 8", ln->full_hits);

    /* 10. The fingerprint refuses a same-geometry clip with other notes. */
    ln = mk(&st);
    lane_fingerprint_t same = { 0.0, 8.0, 14, 41 };
    lane_fingerprint_t copy = { 0.0, 8.0, 9,  41 };
    CHECK(lane_fingerprint_matches(ln, &same) == 1, "identical fp rejected");
    CHECK(lane_fingerprint_matches(ln, &copy) == 0,
          "different note count accepted");

    /* 10b. THE PLACEHOLDER FINGERPRINT IS ABSENT, NOT MATCHING.
     *
     * {note_count 0, first_note -1} is what a lane carries when nothing ever
     * told it about the clip's content -- every lane recorded before the
     * parser learned to count notes, and every lane recorded while the clip
     * was unknown. loop_len is deliberately not compared (a grown clip is the
     * same clip), so with the content half constant the match degenerates to a
     * single test: loop_start within 1e-6. EVERY clip whose loop starts at 0.0
     * then fingerprints identically, so such a lane would bind to the wrong
     * clip and PLAY -- confidently wrong, which is the one outcome this whole
     * design exists to refuse. It must go stale until it is re-recorded.
     *
     * The cost is a lane recorded against a genuinely empty clip: it also
     * carries 0/-1 and is also stale. Accepted deliberately -- silent and
     * re-recordable beats wrong and audible. */
    {
        lane_store_t ps;
        lane_store_reset(&ps);
        lane_fingerprint_t placeholder = { 0.0, 8.0, 0, -1 };
        lane_t *pl = lane_alloc(&ps, "synth", "cutoff", 0, 0, &placeholder);
        CHECK(pl != NULL, "placeholder lane alloc");
        if (pl) {
            /* Against the identical placeholder -- the exact byte pattern a
             * pre-Task-6 push produced for EVERY clip. */
            CHECK(lane_fingerprint_matches(pl, &placeholder) == 0,
                  "a placeholder fingerprint matched itself -- it would bind "
                  "to any clip whose loop starts at 0.0");
            /* And against a real clip that merely shares loop_start 0.0. */
            lane_fingerprint_t real = { 0.0, 8.0, 14, 41 };
            CHECK(lane_fingerprint_matches(pl, &real) == 0,
                  "a placeholder fingerprint matched a real clip");
        }
    }

    /* 11. An over-length key must be REFUSED, never silently truncated --
     * truncating would let two different long keys collide onto one lane
     * (a lane bound to the wrong parameter) and would orphan the point
     * already written under the earlier truncation. */
    lane_store_reset(&st);
    const char *long_target = "synth_component_AAAA_definitely_over_16_bytes";
    lane_t *a1 = lane_alloc(&st, long_target, "cutoff", 0, 0, NULL);
    CHECK(a1 == NULL, "over-length target should be refused by lane_alloc, got %p",
          (void *)a1);
    CHECK(lane_find(&st, long_target, "cutoff", 0, 0) == NULL,
          "over-length target should never be findable");
    const char *long_param =
        "some_really_long_param_name_that_is_definitely_over_thirty_two_bytes";
    lane_t *a2 = lane_alloc(&st, "synth", long_param, 0, 0, NULL);
    CHECK(a2 == NULL, "over-length param should be refused by lane_alloc, got %p",
          (void *)a2);
    CHECK(lane_find(&st, "synth", long_param, 0, 0) == NULL,
          "over-length param should never be findable");

    /* 12. A non-finite phase must never be stored, and eval must not trust
     * one even if it somehow ends up in pts[] -- writer and reader are
     * different tasks' code, so each must defend for itself. */
    ln = mk(&st);
    lane_write(ln, 1.0, 0.1f);
    lane_write(ln, 2.0, 0.2f);
    lane_write(ln, 3.0, 0.3f);
    lane_write(ln, NAN, 0.9f);
    CHECK(ln->n == 3, "NaN phase was stored by lane_write (n=%d)", ln->n);
    lane_write(ln, INFINITY, 0.9f);
    CHECK(ln->n == 3, "infinite phase was stored by lane_write (n=%d)", ln->n);
    CHECK(lane_eval(ln, 3.5, 8.0, 0, &v) == 1 && fabsf(v - 0.3f) < 1e-6f,
          "a rejected NaN write still corrupted eval near the boundary: %f", v);

    /* eval's own defence: a NaN poked directly into pts[] (not through
     * lane_write) must not be treated as in-range or averaged into a span. */
    ln = mk(&st);
    lane_write(ln, 1.0, 0.1f);
    lane_write(ln, 2.0, 0.2f);
    ln->pts[ln->n].phase = NAN;
    ln->pts[ln->n].value = 0.9f;
    ln->n++;
    CHECK(lane_eval(ln, 2.5, 8.0, 0, &v) == 1 && fabsf(v - 0.2f) < 1e-6f,
          "lane_eval trusted a NaN point planted directly in pts[]: %f", v);


    /* ---- 13. A SECOND PASS ERASES THE SPAN IT SWEEPS -----------------
     *
     * Found on hardware: recording a second automation pass over an existing
     * lane produced "super jumpiness". LANE_MIN_POINT_BEATS is ~5 ms at 120
     * BPM, and a second pass's writes land TENS of milliseconds from the
     * first pass's -- so they never fell inside the replace window and the
     * two curves INTERLEAVED:
     *
     *   pass 1:  (1.00, 20)        (1.50, 40)        (2.00, 60) ...
     *   pass 2:        (1.04, 90)        (1.56, 91)        ...
     *   result:  20 -> 90 -> 40 -> 91 -> 60            <-- audible jumping
     *
     * The spacings below are a real knob sweep's: half a beat between
     * detents, and a second pass offset by 0.04-0.10 beats -- far outside
     * the thinning window and far inside the detent spacing. Asserting a
     * point COUNT would pass while the lane still jumps, so this asserts on
     * the VALUES. */
    {
        lane_store_t ps;
        lane_t *l = mk(&ps);
        const double loop = 8.0;

        /* Pass 1: a rising sweep, 20 -> 100. */
        const double p1[5] = { 1.00, 1.50, 2.00, 2.50, 3.00 };
        const float  v1[5] = { 20.0f, 40.0f, 60.0f, 80.0f, 100.0f };
        for (int i = 0; i < 5; i++) lane_record_point(l, p1[i], v1[i], loop);
        lane_record_end(l);
        CHECK(l->n == 5, "pass 1 did not lay down 5 points (n=%d)", l->n);

        /* Pass 2 over the middle of it, deliberately offset. Every value is
         * above every pass-1 value in the region, so a surviving old point
         * shows up as a DIP -- the jumpiness the user heard. */
        const double p2[4] = { 1.04, 1.56, 2.08, 2.60 };
        const float  v2[4] = { 190.0f, 191.0f, 192.0f, 193.0f };
        for (int i = 0; i < 4; i++) lane_record_point(l, p2[i], v2[i], loop);
        lane_record_end(l);

        /* Nothing from pass 1 may survive strictly inside the swept span. */
        for (int i = 0; i < l->n; i++) {
            if (l->pts[i].phase <= p2[0] || l->pts[i].phase >= p2[3]) continue;
            CHECK(l->pts[i].value >= 189.0f,
                  "an old value survived between two new ones at phase %.4f: "
                  "%.1f -- the second pass INTERLEAVED instead of erasing, "
                  "which is the jumpiness",
                  l->pts[i].phase, (double)l->pts[i].value);
        }

        /* And the curve across the overwritten region must not jump about:
         * pass 2 is monotonically rising, so any dip is a leftover. */
        for (int i = 0; i + 1 < l->n; i++) {
            if (l->pts[i].phase < p2[0] || l->pts[i + 1].phase > p2[3]) continue;
            CHECK(l->pts[i + 1].value >= l->pts[i].value,
                  "the overwritten region is not monotonic: %.1f at %.4f then "
                  "%.1f at %.4f -- the lane JUMPS backwards",
                  (double)l->pts[i].value, l->pts[i].phase,
                  (double)l->pts[i + 1].value, l->pts[i + 1].phase);
        }

        /* The region either side of the pass is the user's and was not swept. */
        CHECK(fabsf(l->pts[0].value - 20.0f) < 1e-3f,
              "the pass erased backwards past its own first write: %.1f",
              (double)l->pts[0].value);
        CHECK(fabsf(l->pts[l->n - 1].value - 100.0f) < 1e-3f,
              "the pass erased forwards past its own last write: %.1f",
              (double)l->pts[l->n - 1].value);
    }

    /* ---- 14. A PAUSE IS NOT A SWEEP ---------------------------------
     *
     * The gap threshold is the whole reason the erase cannot eat the lane:
     * stopping mid-take and picking the knob up somewhere else must leave
     * the part the user did not touch exactly as it was. */
    {
        lane_store_t ps;
        lane_t *l = mk(&ps);
        const double loop = 8.0;
        lane_write(l, 2.0, 50.0f);
        lane_write(l, 3.0, 60.0f);
        lane_write(l, 4.0, 70.0f);

        lane_record_point(l, 1.0, 10.0f, loop);     /* first write: no span */
        lane_record_point(l, 1.2, 11.0f, loop);     /* sweeping */
        /* ... hand off the knob for three beats, then turn it again. */
        lane_record_point(l, 5.0, 12.0f, loop);
        lane_record_end(l);

        int kept = 0;
        for (int i = 0; i < l->n; i++) {
            float x = l->pts[i].value;
            if (fabsf(x - 50.0f) < 1e-3f || fabsf(x - 60.0f) < 1e-3f ||
                fabsf(x - 70.0f) < 1e-3f) kept++;
        }
        CHECK(kept == 3,
              "a pause erased the part of the lane the user left alone: "
              "%d of 3 points survived", kept);
    }

    /* ---- 15. A WRAPPED SWEEP ERASES ONLY WHAT IT SWEPT --------------
     *
     * Phase wraps at loop_len, so a pass crossing the loop boundary has
     * cur < prev. Erasing "between prev and cur" as one naive interval
     * erases the whole MIDDLE of the lane -- the exact opposite of what the
     * pass touched. The swept span is (prev, loop_len) plus [0, cur). */
    {
        lane_store_t ps;
        lane_t *l = mk(&ps);
        const double loop = 8.0;
        const double lay[7] = { 0.1, 0.5, 2.0, 4.0, 7.0, 7.5, 7.8 };
        const float  lv[7]  = { 11.0f, 15.0f, 20.0f, 40.0f, 70.0f,
                                75.0f, 78.0f };
        for (int i = 0; i < 7; i++) lane_write(l, lay[i], lv[i]);

        lane_record_point(l, 7.6, 200.0f, loop);   /* first write: no span */
        lane_record_point(l, 0.3, 201.0f, loop);   /* wrapped: gap 0.7 beats */
        lane_record_end(l);

        /* Swept: 7.8 (past prev) and 0.1 (before cur). */
        for (int i = 0; i < l->n; i++) {
            CHECK(fabsf(l->pts[i].value - 78.0f) > 1e-3f,
                  "a wrapped sweep left the point it passed over at 7.8");
            CHECK(fabsf(l->pts[i].value - 11.0f) > 1e-3f,
                  "a wrapped sweep left the point it passed over at 0.1");
        }
        /* Untouched: everything in the middle, which a naive (min,max)
         * interval would have eaten instead. */
        const float want[5] = { 15.0f, 20.0f, 40.0f, 70.0f, 75.0f };
        for (int k = 0; k < 5; k++) {
            int found = 0;
            for (int i = 0; i < l->n; i++)
                if (fabsf(l->pts[i].value - want[k]) < 1e-3f) found = 1;
            CHECK(found, "a wrapped sweep erased the untouched middle of the "
                         "lane: %.1f is gone", (double)want[k]);
        }
    }

    /* ---- 16. THE FIRST WRITE OF A PASS ERASES NOTHING ---------------
     * There is no swept span yet. Without this, the first write of a take
     * erases back to wherever the previous one happened to stop. */
    {
        lane_store_t ps;
        lane_t *l = mk(&ps);
        lane_write(l, 1.0, 10.0f);
        lane_write(l, 2.0, 20.0f);
        lane_write(l, 3.0, 30.0f);
        lane_record_point(l, 2.5, 99.0f, 8.0);
        CHECK(l->n == 4, "the first write of a pass erased something (n=%d)",
              l->n);
        lane_record_end(l);
    }

    /* ---- 17. A NEW TAKE DOES NOT ERASE BACK INTO THE OLD ONE --------
     * The pass ENDS when recording stops. The next armed write is the first
     * write of a fresh pass, so it erases nothing -- even though it lands
     * well within the gap threshold of where the last take stopped. */
    {
        lane_store_t ps;
        lane_t *l = mk(&ps);
        const double loop = 8.0;
        lane_write(l, 1.4, 44.0f);          /* the user's, between the takes */
        lane_record_point(l, 1.0, 10.0f, loop);
        lane_record_point(l, 1.3, 11.0f, loop);
        lane_record_end(l);                  /* Record goes out */
        lane_record_point(l, 1.5, 12.0f, loop);   /* a fresh take */
        lane_record_end(l);
        int found = 0;
        for (int i = 0; i < l->n; i++)
            if (fabsf(l->pts[i].value - 44.0f) < 1e-3f) found = 1;
        CHECK(found, "the first write of a new take erased back into the "
                     "previous one");
    }


    /* ---- 18. lane_write ERASES NOTHING, EVER -------------------------
     *
     * This is the rule that keeps the second-pass erase out of the shared
     * writer: lane_serial's deserializer and any future lane editor write
     * through lane_write, and neither has swept anything.
     *
     * THE LOAD-BEARING ASSERTION HERE IS THE LAST ONE, and that was measured
     * rather than assumed. Moving the erase into lane_write was tried: the
     * verbatim checks below stayed GREEN, because an ascending run of writes
     * erases only the empty spans between consecutive new points. What went
     * red was the pass state -- lane_write advancing rec_last_phase means a
     * later lane_record_point sweeps from a phase an editor or a load put
     * there, and erases across everything in between. The round-trip test's
     * document case stays green under the same mutation for a second reason:
     * today's parser fills pts[] directly and never calls lane_write at all.
     *
     * The verbatim checks are kept because they are the contract a reader
     * needs stated, not because they are what catches the regression. */
    {
        lane_store_t ps;
        lane_t *l = mk(&ps);
        const double wp[6] = { 1.00, 1.04, 1.50, 1.56, 2.00, 2.08 };
        const float  wv[6] = { 20.0f, 190.0f, 40.0f, 191.0f, 60.0f, 192.0f };
        for (int i = 0; i < 6; i++) lane_write(l, wp[i], wv[i]);
        CHECK(l->n == 6,
              "lane_write erased as it went (n=%d, want 6) -- the swept-span "
              "erase has leaked out of lane_record_point", l->n);
        for (int i = 0; i < l->n && i < 6; i++) {
            CHECK(fabs(l->pts[i].phase - wp[i]) < 1e-9,
                  "point %d is at phase %.6f, want %.6f",
                  i, l->pts[i].phase, wp[i]);
            CHECK(fabsf(l->pts[i].value - wv[i]) < 1e-3f,
                  "point %d is %.1f, want %.1f",
                  i, (double)l->pts[i].value, (double)wv[i]);
        }
        /* And it opens no pass, or the next lane_record_point would sweep
         * from a phase an editor or a load put there. */
        CHECK(l->rec_active == 0, "lane_write opened a recording pass");
    }


    /* ---- 19. A LANE BELONGS TO ITS CLIP POSITION --------------------
     *
     * Exactly the sequence the user ran on hardware: record `synth/cutoff`
     * on clip slot 0, switch to clip slot 1, record the same parameter
     * again. lane_find compared only (target, param), so lane_alloc handed
     * back clip 0's lane and the second take landed in it -- overflowing it
     * to LANE_POINTS_MAX and interleaving its values into the first take's
     * curve. The device's own lanes file showed both lanes stamped
     * `track 0 slot 0`, one at n=64 with spikes through a smooth sweep.
     *
     * Playback then refused, correctly: lane_tick's position gate saw a lane
     * bound to slot 0 while clip 1 played. So the user's symptom was "it
     * didn't record", which points at the recorder rather than at the key.
     *
     * This is also why it had to land with the swept-span erase: on a
     * mis-keyed lane that erase deletes the OTHER clip's points rather than
     * merely interleaving with them. */
    {
        lane_store_t ps;
        lane_store_reset(&ps);
        /* Two clips at the same geometry with different content -- which is
         * what the fingerprint is for, and what must not be shared. */
        lane_fingerprint_t f0 = { 0.0, 4.0, 7, 50 };
        lane_fingerprint_t f1 = { 0.0, 4.0, 3, 62 };

        lane_t *c0 = lane_alloc(&ps, "synth", "cutoff", 0, 0, &f0);
        CHECK(c0 != NULL, "clip 0 lane alloc");
        lane_record_point(c0, 0.0, 10.0f, 4.0);
        lane_record_point(c0, 1.0, 11.0f, 4.0);
        lane_record_end(c0);

        lane_t *c1 = lane_alloc(&ps, "synth", "cutoff", 0, 1, &f1);
        CHECK(c1 != NULL, "clip 1 lane alloc");
        CHECK(c1 != c0,
              "recording the same parameter on a SECOND CLIP took over the "
              "first clip's lane -- one lane per param across all 8 clip "
              "slots, which is the hardware defect");
        lane_record_point(c1, 0.0, 90.0f, 4.0);
        lane_record_point(c1, 1.0, 91.0f, 4.0);
        lane_record_end(c1);

        int used = 0;
        for (int i = 0; i < LANE_MAX; i++) if (ps.lanes[i].used) used++;
        CHECK(used == 2, "two clips' automation collapsed into %d lane(s)",
              used);

        /* Each lane keeps its OWN points -- no value from one may appear in
         * the other, which is what a shared lane looks like from the audio. */
        if (c1 != c0) {
            CHECK(c0->n == 2, "clip 0's lane holds %d points, want 2", c0->n);
            CHECK(c1->n == 2, "clip 1's lane holds %d points, want 2", c1->n);
            for (int i = 0; i < c0->n; i++)
                CHECK(c0->pts[i].value < 50.0f,
                      "clip 1's value %.1f is in CLIP 0's lane",
                      (double)c0->pts[i].value);
            for (int i = 0; i < c1->n; i++)
                CHECK(c1->pts[i].value > 50.0f,
                      "clip 0's value %.1f is in CLIP 1's lane",
                      (double)c1->pts[i].value);
            /* And its own fingerprint, or one clip's lane goes stale on the
             * other clip's content. */
            CHECK(c0->slot == 0 && c1->slot == 1,
                  "the lanes are not bound to their own slots (%d, %d)",
                  c0->slot, c1->slot);
            CHECK(c0->fp.first_note == 50 && c1->fp.first_note == 62,
                  "the second alloc overwrote the first clip's fingerprint "
                  "(%d, %d)", c0->fp.first_note, c1->fp.first_note);
        }

        /* And lane_find must answer each position separately -- lane_alloc
         * routes through it, so this IS the defect's mechanism. */
        CHECK(lane_find(&ps, "synth", "cutoff", 0, 0) == c0,
              "lane_find did not answer clip 0's lane for slot 0");
        CHECK(lane_find(&ps, "synth", "cutoff", 0, 1) == c1,
              "lane_find did not answer clip 1's lane for slot 1");
        CHECK(lane_find(&ps, "synth", "cutoff", 0, 4) == NULL,
              "lane_find answered a lane for a clip slot that has none");
        CHECK(lane_find(&ps, "synth", "cutoff", 1, 0) == NULL,
              "lane_find ignored the TRACK half of the key");
    }

    /* ---- 20. AN ERASE IS CONFINED TO ITS OWN LANE -------------------
     * The two defects had to land together: a swept-span erase on a
     * mis-keyed lane deletes another clip's automation outright, which is
     * strictly worse than interleaving with it. */
    {
        lane_store_t ps;
        lane_store_reset(&ps);
        lane_fingerprint_t f0 = { 0.0, 8.0, 7, 50 };
        lane_fingerprint_t f1 = { 0.0, 8.0, 3, 62 };
        lane_t *c0 = lane_alloc(&ps, "synth", "cutoff", 0, 0, &f0);
        lane_t *c1 = lane_alloc(&ps, "synth", "cutoff", 0, 1, &f1);
        CHECK(c0 && c1 && c0 != c1, "two lanes for the erase-isolation case");
        if (c0 && c1 && c0 != c1) {
            /* Clip 0 carries a take the user wants to keep. */
            const double p0[5] = { 1.0, 1.5, 2.0, 2.5, 3.0 };
            for (int i = 0; i < 5; i++)
                lane_write(c0, p0[i], (float)(20 + i * 10));
            /* Clip 1 gets a second pass right across the same phases. */
            for (int i = 0; i < 5; i++)
                lane_write(c1, p0[i], (float)(120 + i * 10));
            const double p2[4] = { 1.04, 1.56, 2.08, 2.60 };
            for (int i = 0; i < 4; i++)
                lane_record_point(c1, p2[i], 199.0f, 8.0);
            lane_record_end(c1);

            CHECK(c0->n == 5,
                  "an erase on clip 1's lane deleted clip 0's automation "
                  "(clip 0 now holds %d of 5 points)", c0->n);
            for (int i = 0; i < c0->n && i < 5; i++) {
                CHECK(fabs(c0->pts[i].phase - p0[i]) < 1e-9 &&
                      fabsf(c0->pts[i].value - (float)(20 + i * 10)) < 1e-3f,
                      "clip 0's point %d was disturbed: %.4f -> %.1f",
                      i, c0->pts[i].phase, (double)c0->pts[i].value);
            }
            /* ...and the erase did do its job on its own lane. */
            for (int i = 0; i < c1->n; i++) {
                if (c1->pts[i].phase <= p2[0] || c1->pts[i].phase >= p2[3])
                    continue;
                CHECK(c1->pts[i].value >= 199.0f - 1e-3f,
                      "clip 1's own pass did not erase: %.1f survived at %.4f",
                      (double)c1->pts[i].value, c1->pts[i].phase);
            }
        }
    }

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: lane_store\n");
    return 0;
}
