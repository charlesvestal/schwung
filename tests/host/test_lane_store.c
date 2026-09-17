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
    CHECK(lane_eval(ln, 0.0, 0.0, 8.0, 0, &v) == 0, "empty lane produced a value");

    /* 2. One point is that value everywhere (hold at both ends). */
    lane_write(ln, 2.0, 0.5f, 0);
    CHECK(lane_eval(ln, 0.0, 0.0, 8.0, 0, &v) == 1 && fabsf(v - 0.5f) < 1e-6f,
          "single point before: %f", v);
    CHECK(lane_eval(ln, 7.9, 0.0, 8.0, 0, &v) == 1 && fabsf(v - 0.5f) < 1e-6f,
          "single point after: %f", v);

    /* 3. Linear between two float points. */
    lane_write(ln, 6.0, 1.0f, 0);
    CHECK(lane_eval(ln, 4.0, 0.0, 8.0, 0, &v) == 1 && fabsf(v - 0.75f) < 1e-6f,
          "midpoint interp: %f", v);

    /* 4. Stepped (int/enum) holds the previous value instead. */
    CHECK(lane_eval(ln, 4.0, 0.0, 8.0, 1, &v) == 1 && fabsf(v - 0.5f) < 1e-6f,
          "stepped should hold 0.5, got %f", v);

    /* 5. A point past the CURRENT loop end is ignored -- and retained. */
    ln = mk(&st);
    lane_write(ln, 1.0, 0.2f, 0);
    lane_write(ln, 12.0, 0.9f, 0);       /* beyond an 8-beat loop */
    CHECK(lane_eval(ln, 7.0, 0.0, 8.0, 0, &v) == 1 && fabsf(v - 0.2f) < 1e-6f,
          "dormant point leaked into the curve: %f", v);
    CHECK(ln->n == 2, "dormant point was dropped (n=%d)", ln->n);

    /* 6. Extending the clip reveals it, with no rewrite. */
    CHECK(lane_eval(ln, 12.0, 0.0, 16.0, 0, &v) == 1 && fabsf(v - 0.9f) < 1e-6f,
          "extended loop did not reveal the point: %f", v);

    /* 7. Thinning: a second write inside the window replaces, not appends. */
    ln = mk(&st);
    lane_write(ln, 1.000f, 0.1f, 0);
    lane_write(ln, 1.005f, 0.4f, 0);     /* < LANE_MIN_POINT_BEATS away */
    CHECK(ln->n == 1, "thinning failed (n=%d)", ln->n);
    CHECK(fabsf(ln->pts[0].value - 0.4f) < 1e-6f,
          "thinning kept the OLD value: %f", ln->pts[0].value);

    /* 8. Out-of-order writes leave the list sorted. */
    ln = mk(&st);
    lane_write(ln, 4.0, 0.4f, 0);
    lane_write(ln, 1.0, 0.1f, 0);
    lane_write(ln, 2.0, 0.2f, 0);
    CHECK(ln->n == 3 && ln->pts[0].phase < ln->pts[1].phase &&
          ln->pts[1].phase < ln->pts[2].phase, "points are not sorted");

    /* 9. A full lane degrades resolution; it never drops the gesture. */
    ln = mk(&st);
    for (int i = 0; i < LANE_POINTS_MAX + 8; i++)
        lane_write(ln, 0.1 + i * 0.5, (float)i / 100.0f, 0);
    CHECK(ln->n == LANE_POINTS_MAX, "overflowed (n=%d)", ln->n);
    CHECK(ln->full_hits == 8, "full_hits=%d, want 8", ln->full_hits);

    /* 10. The fingerprint refuses a same-geometry clip with other notes. */
    ln = mk(&st);
    lane_fingerprint_t same = { 0.0, 8.0, 14, 41 };
    lane_fingerprint_t copy = { 0.0, 8.0, 9,  41 };
    CHECK(lane_fingerprint_matches(ln, &same) == 1, "identical fp rejected");
    CHECK(lane_fingerprint_matches(ln, &copy) == 0,
          "different note count accepted");

    /* 10a. MOVING A CLIP'S LOOP IS NOT A DIFFERENT CLIP.
     *
     * Nothing on Move says "the loop area moved" -- so a lane that goes stale
     * on it goes stale SILENTLY, and the user's automation disappears with no
     * gesture that brings it back but re-recording. loop_len was already
     * excluded for the same reason (a clip that grew is the same clip); a clip
     * whose loop you dragged is too. Both loop fields live in the fingerprint
     * for diagnostics only, and the content half is what discriminates.
     *
     * Phases are stored LOOP-RELATIVE (shadow_slot_clip_phase subtracts
     * loop_start), so a moved loop replays the same gesture from the new
     * start rather than needing a re-origin nobody can observe. */
    ln = mk(&st);
    {
        lane_fingerprint_t moved  = { 4.0, 8.0, 14, 41 };
        lane_fingerprint_t grown  = { 4.0, 16.0, 14, 41 };
        lane_fingerprint_t recut  = { 4.0, 8.0, 14, 48 };
        CHECK(lane_fingerprint_matches(ln, &moved) == 1,
              "a moved loop_start went stale -- silently loses automation");
        CHECK(lane_fingerprint_matches(ln, &grown) == 1,
              "a moved AND grown loop went stale");
        CHECK(lane_fingerprint_matches(ln, &recut) == 0,
              "a different first note was accepted");
    }

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
    lane_write(ln, 1.0, 0.1f, 0);
    lane_write(ln, 2.0, 0.2f, 0);
    lane_write(ln, 3.0, 0.3f, 0);
    lane_write(ln, NAN, 0.9f, 0);
    CHECK(ln->n == 3, "NaN phase was stored by lane_write (n=%d)", ln->n);
    lane_write(ln, INFINITY, 0.9f, 0);
    CHECK(ln->n == 3, "infinite phase was stored by lane_write (n=%d)", ln->n);
    CHECK(lane_eval(ln, 3.5, 0.0, 8.0, 0, &v) == 1 && fabsf(v - 0.3f) < 1e-6f,
          "a rejected NaN write still corrupted eval near the boundary: %f", v);

    /* eval's own defence: a NaN poked directly into pts[] (not through
     * lane_write) must not be treated as in-range or averaged into a span. */
    ln = mk(&st);
    lane_write(ln, 1.0, 0.1f, 0);
    lane_write(ln, 2.0, 0.2f, 0);
    ln->pts[ln->n].phase = NAN;
    ln->pts[ln->n].value = 0.9f;
    ln->n++;
    CHECK(lane_eval(ln, 2.5, 0.0, 8.0, 0, &v) == 1 && fabsf(v - 0.2f) < 1e-6f,
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
        for (int i = 0; i < 5; i++) lane_record_point(l, p1[i], v1[i], 0.0, loop, 0);
        lane_record_end(l);
        CHECK(l->n == 5, "pass 1 did not lay down 5 points (n=%d)", l->n);

        /* Pass 2 over the middle of it, deliberately offset. Every value is
         * above every pass-1 value in the region, so a surviving old point
         * shows up as a DIP -- the jumpiness the user heard. */
        const double p2[4] = { 1.04, 1.56, 2.08, 2.60 };
        const float  v2[4] = { 190.0f, 191.0f, 192.0f, 193.0f };
        for (int i = 0; i < 4; i++) lane_record_point(l, p2[i], v2[i], 0.0, loop, 0);
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
        lane_write(l, 2.0, 50.0f, 0);
        lane_write(l, 3.0, 60.0f, 0);
        lane_write(l, 4.0, 70.0f, 0);

        lane_record_point(l, 1.0, 10.0f, 0.0, loop, 0);     /* first write: no span */
        lane_record_point(l, 1.2, 11.0f, 0.0, loop, 0);     /* sweeping */
        /* ... hand off the knob for three beats, then turn it again. */
        lane_record_point(l, 5.0, 12.0f, 0.0, loop, 0);
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
        for (int i = 0; i < 7; i++) lane_write(l, lay[i], lv[i], 0);

        lane_record_point(l, 7.6, 200.0f, 0.0, loop, 0);   /* first write: no span */
        lane_record_point(l, 0.3, 201.0f, 0.0, loop, 0);   /* wrapped: gap 0.7 beats */
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
        lane_write(l, 1.0, 10.0f, 0);
        lane_write(l, 2.0, 20.0f, 0);
        lane_write(l, 3.0, 30.0f, 0);
        lane_record_point(l, 2.5, 99.0f, 0.0, 8.0, 0);
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
        lane_write(l, 1.4, 44.0f, 0);          /* the user's, between the takes */
        lane_record_point(l, 1.0, 10.0f, 0.0, loop, 0);
        lane_record_point(l, 1.3, 11.0f, 0.0, loop, 0);
        lane_record_end(l);                  /* Record goes out */
        lane_record_point(l, 1.5, 12.0f, 0.0, loop, 0);   /* a fresh take */
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
        for (int i = 0; i < 6; i++) lane_write(l, wp[i], wv[i], 0);
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
        lane_record_point(c0, 0.0, 10.0f, 0.0, 4.0, 0);
        lane_record_point(c0, 1.0, 11.0f, 0.0, 4.0, 0);
        lane_record_end(c0);

        lane_t *c1 = lane_alloc(&ps, "synth", "cutoff", 0, 1, &f1);
        CHECK(c1 != NULL, "clip 1 lane alloc");
        CHECK(c1 != c0,
              "recording the same parameter on a SECOND CLIP took over the "
              "first clip's lane -- one lane per param across all 8 clip "
              "slots, which is the hardware defect");
        lane_record_point(c1, 0.0, 90.0f, 0.0, 4.0, 0);
        lane_record_point(c1, 1.0, 91.0f, 0.0, 4.0, 0);
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
                lane_write(c0, p0[i], (float)(20 + i * 10), 0);
            /* Clip 1 gets a second pass right across the same phases. */
            for (int i = 0; i < 5; i++)
                lane_write(c1, p0[i], (float)(120 + i * 10), 0);
            const double p2[4] = { 1.04, 1.56, 2.08, 2.60 };
            for (int i = 0; i < 4; i++)
                lane_record_point(c1, p2[i], 199.0f, 0.0, 8.0, 0);
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

    /* ---- THE PASS'S EXTENT, computed ONCE ------------------------------
     *
     * lane_record_point erases the span a pass sweeps; lane_tick must go
     * silent over exactly that span. Two readings of "the pass is here" from
     * one function, because a second copy of this arithmetic is free to
     * disagree with the first -- and a disagreement erases points the lane is
     * still playing, which is unrecoverable and invisible. */
    {
        /* Forward is the plain difference. */
        CHECK(fabs(lane_pass_travel(2.0, 2.5, 0.0, 8.0) - 0.5) < 1e-9,
              "forward travel 2.0 -> 2.5 read %.6f",
              lane_pass_travel(2.0, 2.5, 0.0, 8.0));
        CHECK(lane_pass_travel(2.0, 2.0, 0.0, 8.0) == 0.0,
              "no travel at all read %.6f", lane_pass_travel(2.0, 2.0, 0.0, 8.0));

        /* BACKWARDS IS A WRAP, and the whole point of this function: the
         * travelled distance is (loop_len - prev) + phase. The tempting
         * phase - prev is NEGATIVE, so it reads as "cannot tell" and hands
         * the old curve back in the middle of a sweep across the loop
         * boundary -- and it describes the untouched MIDDLE of the lane
         * rather than the two swept ends. */
        CHECK(fabs(lane_pass_travel(7.5, 0.2, 0.0, 8.0) - 0.7) < 1e-9,
              "wrapped travel 7.5 -> 0.2 in an 8-beat loop read %.6f, "
              "expected 0.7", lane_pass_travel(7.5, 0.2, 0.0, 8.0));
        CHECK(lane_pass_travel(7.5, 0.2, 0.0, 8.0) > 0.0,
              "wrapped travel came out negative -- that is phase - prev");

        /* Cannot tell, and -1.0 says so once for every caller. A real
         * distance is never negative, so the sentinel cannot be an answer. */
        CHECK(lane_pass_travel(7.5, 0.2, 0.0, 0.0) < 0.0,
              "a backwards phase with no loop length answered %.6f",
              lane_pass_travel(7.5, 0.2, 0.0, 0.0));
        CHECK(lane_pass_travel(9.0, 0.2, 0.0, 8.0) < 0.0,
              "a prev past the loop end answered %.6f",
              lane_pass_travel(9.0, 0.2, 0.0, 8.0));
        CHECK(lane_pass_travel(NAN, 1.0, 0.0, 8.0) < 0.0, "a NaN prev answered a distance");
        CHECK(lane_pass_travel(1.0, NAN, 0.0, 8.0) < 0.0, "a NaN phase answered a distance");

        lane_store_t lps;
        lane_store_reset(&lps);
        lane_fingerprint_t lfp = { 0.0, 8.0, 3, 60 };
        lane_t *lp = lane_alloc(&lps, "synth", "cutoff", 0, 0, &lfp);
        CHECK(lp != NULL, "a lane for the pass-window cases");
        if (lp) {
            /* NO PASS IS NOT A LIVE PASS. `rec_active` is 0 until the first
             * write of a take, and the lane must play normally there. */
            CHECK(lane_pass_live_at(lp, 2.0, 0.0, 8.0) == 0,
                  "a lane with no recording pass reads as live");

            lane_record_point(lp, 2.0, 55.0f, 0.0, 8.0, 0);
            CHECK(lp->rec_active == 1, "premise: the pass started");

            /* Inside the window, forward: live. */
            CHECK(lane_pass_live_at(lp, 2.0, 0.0, 8.0) == 1,
                  "the pass is not live at its own last write");
            CHECK(lane_pass_live_at(lp, 2.0 + LANE_PASS_GAP_BEATS * 0.5, 0.0, 8.0) == 1,
                  "the pass is not live half a threshold ahead of itself");

            /* PAST THE THRESHOLD IS NOT LIVE -- that bound is the whole
             * difference between a punch and a recording MODE that silences
             * the rest of the bar. */
            CHECK(lane_pass_live_at(lp, 2.0 + LANE_PASS_GAP_BEATS * 1.5, 0.0, 8.0) == 0,
                  "a phase past the threshold still reads as the live pass");

            /* AND IT AGREES WITH THE ERASE. A write at a phase the predicate
             * calls live erases back to the previous write; one it calls dead
             * erases nothing. Asserted as the AGREEMENT rather than as two
             * separate expected numbers, because what matters is that the two
             * readings cannot drift apart. */
            lane_write(lp, 2.4, 99.0f, 0);
            const int live_near = lane_pass_live_at(lp, 2.5, 0.0, 8.0);
            lane_record_point(lp, 2.5, 56.0f, 0.0, 8.0, 0);
            int survived = 0;
            for (int i = 0; i < lp->n; i++)
                if (fabs(lp->pts[i].phase - 2.4) < 1e-9) survived = 1;
            CHECK(live_near == 1 && survived == 0,
                  "live=%d but the swept point at 2.4 survived=%d -- the "
                  "suppression and the erase disagree about the pass",
                  live_near, survived);

            lane_write(lp, 6.0, 98.0f, 0);
            const int live_far = lane_pass_live_at(lp, 7.0, 0.0, 8.0);
            lane_record_point(lp, 7.0, 57.0f, 0.0, 8.0, 0);
            survived = 0;
            for (int i = 0; i < lp->n; i++)
                if (fabs(lp->pts[i].phase - 6.0) < 1e-9) survived = 1;
            CHECK(live_far == 0 && survived == 1,
                  "live=%d but the point at 6.0 survived=%d -- a write beyond "
                  "the threshold erased a span nothing was suppressing",
                  live_far, survived);

            /* WRAPPED, THROUGH BOTH READINGS. The pass sits at 7.0; 0.3 is
             * 1.3 beats of travel away round the loop, which is past the
             * threshold, while 7.5 is 0.5 ahead and inside it. */
            CHECK(lane_pass_live_at(lp, 7.5, 0.0, 8.0) == 1,
                  "the pass is not live 0.5 beats ahead of 7.0");
            lane_record_point(lp, 7.8, 58.0f, 0.0, 8.0, 0);
            CHECK(lane_pass_live_at(lp, 0.1, 0.0, 8.0) == 1,
                  "a pass at 7.8 is not live 0.3 beats later at phase 0.1 -- "
                  "the wrap was measured as phase - prev");
            CHECK(lane_pass_live_at(lp, 4.0, 0.0, 8.0) == 0,
                  "a pass at 7.8 reads as live in the untouched middle of the "
                  "loop at phase 4.0");

            /* The end of the pass is the end of the window, everywhere. */
            lane_record_end(lp);
            CHECK(lane_pass_live_at(lp, 7.8, 0.0, 8.0) == 0,
                  "an ended pass still reads as live at its last write");
        }
    }

    /* ================= THE LOOP IS A WINDOW IN CLIP TIME =================
     *
     * The case the user named, and the reason V1's loop-relative storage was
     * wrong. Measured against Move's own file (2026-09-12): a clip whose
     * region/loop is 8..20 quarters carries notes at startTime 0.0, 9.5 and
     * 16.5 -- so notes are absolute from the clip's start and the loop is a
     * window over them. A lane stored in that same coordinate cannot slide.
     *
     * Play a bars-3-to-5 loop (8..20), record over the notes at 9.5, then open
     * the loop out to the whole clip (0..32). The point must still be at 9.5,
     * on the same notes. Stored loop-relative it would have been 1.5, and
     * would now replay two bars early.
     */
    {
        lane_store_t w;
        lane_fingerprint_t fp = { 8.0, 12.0, 3, 60 };
        lane_store_reset(&w);
        lane_t *wl = lane_alloc(&w, "synth", "cutoff", 0, 2, &fp);
        CHECK(wl != NULL, "window lane alloc");
        if (wl) {
            lane_write(wl, 9.5, 0.75f, 0);      /* over the note at 9.5 */
            lane_write(wl, 16.5, 0.25f, 0);     /* and the one at 16.5 */

            /* Inside the bars-3-to-5 window, where it was recorded. */
            CHECK(lane_eval(wl, 9.5, 8.0, 12.0, 0, &v) == 1 &&
                  fabsf(v - 0.75f) < 1e-6f,
                  "in its own window: %f", v);

            /* The loop opened out to the whole clip. SAME clip position, same
             * value -- that is the entire requirement. */
            CHECK(lane_eval(wl, 9.5, 0.0, 32.0, 0, &v) == 1 &&
                  fabsf(v - 0.75f) < 1e-6f,
                  "the loop grew and the point moved: %f at 9.5", v);
            CHECK(lane_eval(wl, 16.5, 0.0, 32.0, 0, &v) == 1 &&
                  fabsf(v - 0.25f) < 1e-6f,
                  "the loop grew and the second point moved: %f", v);

            /* The loop MOVED to bars 1-3 (0..12): the recorded material is
             * outside it now, so the lane is DORMANT there rather than
             * playing its nearest point. A prefix test ([0, loop_len)) would
             * have played 9.5's value at beat 2 -- material from a part of the
             * clip the loop no longer reaches. */
            CHECK(lane_eval(wl, 2.0, 0.0, 8.0, 0, &v) == 0,
                  "a window that excludes every point still produced %f", v);

            /* And a point below the window is dormant even when others are
             * inside it: window 12..20 leaves 16.5 audible and 9.5 not, so
             * the value at 13.0 must be 16.5's, never 9.5's. */
            CHECK(lane_eval(wl, 13.0, 12.0, 8.0, 0, &v) == 1 &&
                  fabsf(v - 0.25f) < 1e-6f,
                  "a point BELOW the window leaked in: %f", v);

            /* An unknown window start is not a window. NaN must refuse, the
             * same way an unknown phase does -- 0.0 is a legal loop start, so
             * a missed gate must not find a usable number. */
            CHECK(lane_eval(wl, 9.5, NAN, 12.0, 0, &v) == 0,
                  "a NaN window start produced %f", v);
        }
    }

    /* A recording pass WRAPS AT THE WINDOW, not at zero.
     *
     * On a bars-3-to-5 loop the sweep goes ... 19.5, 8.2 ... and the span it
     * erases is (prev, 20) then [8, 8.2). Erasing from 0 instead would delete
     * the automation on the clip's first two bars -- material this pass never
     * touched, that the user cannot see from inside the loop, and that becomes
     * audible the moment they open the loop out. */
    {
        lane_store_t w;
        lane_fingerprint_t fp = { 8.0, 12.0, 3, 60 };
        lane_store_reset(&w);
        lane_t *wl = lane_alloc(&w, "synth", "cutoff", 0, 2, &fp);
        CHECK(wl != NULL, "wrap lane alloc");
        if (wl) {
            lane_write(wl, 1.0, 0.10f, 0);       /* before the loop: untouchable */
            lane_write(wl, 4.0, 0.20f, 0);       /* also before it */
            lane_write(wl, 19.0, 0.30f, 0);      /* inside, and in the swept span */

            /* travel across the window's own wrap: 19.5 -> 8.2 is 0.7 beats,
             * not a jump backwards of 11.3 and not "cannot tell". */
            double tr = lane_pass_travel(19.5, 8.2, 8.0, 12.0);
            CHECK(fabs(tr - 0.7) < 1e-9, "wrapped travel = %f, want 0.7", tr);

            /* A prev outside the window cannot have been swept from inside it. */
            CHECK(lane_pass_travel(2.0, 8.2, 8.0, 12.0) < 0.0,
                  "a prev below the window was turned into a wrap");

            lane_record_point(wl, 19.5, 0.40f, 8.0, 12.0, 0);
            lane_record_point(wl, 8.2, 0.50f, 8.0, 12.0, 0);
            CHECK(lane_eval(wl, 1.0, 0.0, 32.0, 0, &v) == 1 &&
                  fabsf(v - 0.10f) < 1e-6f,
                  "the wrap erased the clip's first bar: %f at beat 1", v);
            CHECK(lane_eval(wl, 4.0, 0.0, 32.0, 0, &v) == 1 &&
                  fabsf(v - 0.20f) < 1e-6f,
                  "the wrap erased material before the loop: %f at beat 4", v);
        }
    }

    /* ============ A TAKE RECORDED BLIND, THEN IDENTIFIED ==============
     *
     * Move writes a new clip to Song.abl about 10 s after it is made
     * (measured; the figure long quoted was 35 s). Inside that window there
     * are no notes to fingerprint and no `loop.start` to anchor to, so a take
     * is recorded against an ASSUMED origin of 0 -- and the absent
     * fingerprint is what marks it as still needing the real one.
     *
     * When the clip appears, both unknowns are answered in the same parse, so
     * adoption re-origins and identifies in one step, with the number that
     * just arrived rather than a guess.
     */
    {
        lane_store_t b;
        lane_store_reset(&b);
        lane_fingerprint_t absent = { 0.0, 8.0, 0, -1 };
        lane_t *bl = lane_alloc(&b, "synth", "cutoff", 1, 4, &absent);
        CHECK(bl != NULL, "blind lane alloc");
        if (bl) {
            bl->origin_pending = 1;   /* what the recording path sets */
            CHECK(lane_fp_absent(&bl->fp),
                  "a lane recorded blind must carry the absent fingerprint");
            /* A take over the first two beats of what the user sees as the
             * clip's start. */
            lane_write(bl, 0.5, 0.20f, 0);
            lane_write(bl, 1.5, 0.80f, 0);
            /* AND IT PLAYS, against the assumed origin, for as long as the
             * clip cannot be identified. That is not a gap in the staleness
             * rule -- the lane is at the position that is playing and nothing
             * else can be there, so refusing would make a take go silent the
             * instant it was recorded. Staleness is for a position holding a
             * DIFFERENT clip, which needs a fingerprint to establish. */
            CHECK(lane_eval(bl, 0.5, 0.0, 8.0, 0, &v) == 1 &&
                  fabsf(v - 0.20f) < 1e-6f,
                  "a blind take did not play at its assumed origin: %f", v);

            /* THE CLIP ARRIVES: 3 notes, first note 60, loop starting at beat
             * 8 (bar 3). Adoption shifts the take there. */
            lane_fingerprint_t real = { 8.0, 12.0, 3, 60 };
            CHECK(lane_adopt_fingerprint(bl, &real) == 1, "adoption refused");
            CHECK(bl->adopted == 1, "adoption was not counted (%d)", bl->adopted);
            CHECK(!lane_fp_absent(&bl->fp) && bl->fp.first_note == 60,
                  "the lane was not identified: first_note=%d", bl->fp.first_note);
            CHECK(fabs(bl->pts[0].phase - 8.5) < 1e-9 &&
                  fabs(bl->pts[1].phase - 9.5) < 1e-9,
                  "the take was not re-origined: %f, %f (want 8.5, 9.5)",
                  bl->pts[0].phase, bl->pts[1].phase);
            /* And now it plays, in the window it belongs to. */
            CHECK(lane_eval(bl, 8.5, 8.0, 12.0, 0, &v) == 1 &&
                  fabsf(v - 0.20f) < 1e-6f,
                  "an adopted lane did not play at 8.5: %f", v);
            CHECK(lane_fingerprint_matches(bl, &real) == 1,
                  "an adopted lane does not match the clip it adopted");

            /* IDEMPOTENT, and it cannot be hijacked. A second, DIFFERENT clip
             * must not overwrite an identity -- that is the whole hazard of
             * adopting anything, and the guard is structural: the fingerprint
             * is no longer absent. */
            lane_fingerprint_t other = { 0.0, 4.0, 9, 41 };
            CHECK(lane_adopt_fingerprint(bl, &other) == 0,
                  "an identified lane accepted ANOTHER clip's fingerprint");
            CHECK(bl->fp.first_note == 60 &&
                  fabs(bl->pts[0].phase - 8.5) < 1e-9,
                  "the refused adoption still changed the lane (%d, %f)",
                  bl->fp.first_note, bl->pts[0].phase);
        }
    }
    {
        /* WHAT ADOPTION REFUSES. Each of these leaves the lane exactly as it
         * was, still adoptable: a bad answer now must not cost the chance of
         * a good one later. */
        lane_store_t b;
        lane_store_reset(&b);
        lane_fingerprint_t absent = { 0.0, 8.0, 0, -1 };
        lane_t *bl = lane_alloc(&b, "synth", "cutoff", 1, 4, &absent);
        CHECK(bl != NULL, "refusal lane alloc");
        if (bl) {
            bl->origin_pending = 1;
            lane_write(bl, 1.0, 0.5f, 0);
            /* An absent fingerprint: a no-op that would still have cleared
             * the pending state, stranding the take at origin 0. */
            CHECK(lane_adopt_fingerprint(bl, &absent) == 0,
                  "an ABSENT fingerprint was adopted");
            /* A non-finite or negative origin puts every point somewhere
             * unnameable. */
            lane_fingerprint_t nan_start = { NAN, 12.0, 3, 60 };
            lane_fingerprint_t neg_start = { -4.0, 12.0, 3, 60 };
            CHECK(lane_adopt_fingerprint(bl, &nan_start) == 0,
                  "a NaN loop_start was adopted");
            CHECK(lane_adopt_fingerprint(bl, &neg_start) == 0,
                  "a negative loop_start was adopted");
            CHECK(lane_fp_absent(&bl->fp) && bl->adopted == 0 &&
                  fabs(bl->pts[0].phase - 1.0) < 1e-9,
                  "a refused adoption modified the lane (fp absent=%d "
                  "adopted=%d phase=%f)", lane_fp_absent(&bl->fp),
                  bl->adopted, bl->pts[0].phase);
            /* A loop that really does start at 0 adopts and shifts NOTHING --
             * the common case for a clip made at bar 1. */
            lane_fingerprint_t at_zero = { 0.0, 16.0, 5, 48 };
            CHECK(lane_adopt_fingerprint(bl, &at_zero) == 1,
                  "a loop_start of 0 was refused");
            CHECK(fabs(bl->pts[0].phase - 1.0) < 1e-9,
                  "adopting at origin 0 moved the take to %f", bl->pts[0].phase);
        }
    }
    {
        /* A TAKE STILL IN PROGRESS moves with its points. `rec_last_phase` is
         * where the next write erases from, so leaving it behind would erase a
         * span the gesture never swept -- 8 beats away, in this case. */
        lane_store_t b;
        lane_store_reset(&b);
        lane_fingerprint_t absent = { 0.0, 8.0, 0, -1 };
        lane_t *bl = lane_alloc(&b, "synth", "cutoff", 1, 4, &absent);
        CHECK(bl != NULL, "in-progress lane alloc");
        if (bl) {
            bl->origin_pending = 1;
            lane_record_point(bl, 1.0, 0.3f, 0.0, 8.0, 0);
            CHECK(bl->rec_active && fabs(bl->rec_last_phase - 1.0) < 1e-9,
                  "premise: a pass is live at phase 1.0");
            lane_fingerprint_t real = { 8.0, 12.0, 3, 60 };
            CHECK(lane_adopt_fingerprint(bl, &real) == 1, "adoption refused");
            CHECK(fabs(bl->rec_last_phase - 9.0) < 1e-9,
                  "the live pass's mark stayed at %f while its points moved "
                  "to clip time", bl->rec_last_phase);
        }
    }

    {
        /* A PLACEHOLDER LANE LOADED FROM DISK IS NOT ADOPTABLE. Same bytes as
         * a blind take, different meaning: nothing says the clip now at that
         * position is the one it was recorded against, so labelling it would
         * bind the lane to a stranger and play it. `origin_pending` is what
         * separates them, and it is never serialized. */
        lane_store_t b;
        lane_store_reset(&b);
        lane_fingerprint_t absent = { 0.0, 8.0, 0, -1 };
        lane_t *bl = lane_alloc(&b, "synth", "cutoff", 1, 4, &absent);
        CHECK(bl != NULL, "loaded lane alloc");
        if (bl) {
            lane_write(bl, 1.0, 0.5f, 0);
            lane_fingerprint_t real = { 8.0, 12.0, 3, 60 };
            CHECK(lane_adopt_fingerprint(bl, &real) == 0,
                  "a lane with no origin_pending was adopted -- a placeholder "
                  "from disk must never take a stranger's identity");
            CHECK(lane_fp_absent(&bl->fp) && fabs(bl->pts[0].phase - 1.0) < 1e-9,
                  "the refused adoption changed the lane");
        }
    }

    /* ================= A P-LOCK IS A RECTANGLE ========================
     *
     * Setting a value ON a step is not a slope towards the next one. Under
     * plain interpolation two neighbouring p-locks glide into each other,
     * which sounds like automation rather than a sequencer -- so a point
     * carries its own `hold`, independent of whether the PARAMETER is stepped.
     */
    {
        lane_store_t h;
        lane_store_reset(&h);
        lane_fingerprint_t fp2 = { 0.0, 4.0, 3, 60 };
        lane_t *hl = lane_alloc(&h, "synth", "cutoff", 0, 0, &fp2);
        CHECK(hl != NULL, "hold lane alloc");
        if (hl) {
            lane_write(hl, 0.0, 0.0f, 1);      /* p-lock on step 1 */
            lane_write(hl, 2.0, 1.0f, 1);      /* p-lock on step 9  */
            /* Between two held points the FIRST one stands: a step, not a
             * ramp. A float parameter, so `stepped` is 0 -- the point's own
             * flag is doing the work. */
            CHECK(lane_eval(hl, 1.0, 0.0, 4.0, 0, &v) == 1 && v == 0.0f,
                  "a held point ramped: %f at the midpoint, want 0.0", v);
            CHECK(lane_eval(hl, 1.999, 0.0, 4.0, 0, &v) == 1 && v == 0.0f,
                  "a held point ramped near its end: %f", v);
            CHECK(lane_eval(hl, 2.0, 0.0, 4.0, 0, &v) == 1 && v == 1.0f,
                  "the second p-lock did not take effect at its own phase: %f", v);

            /* And an UNHELD point between two p-locks still ramps -- the flag
             * is per point, so a recorded sweep and a p-lock coexist in one
             * lane. */
            lane_write(hl, 2.0, 1.0f, 0);      /* replace it with a ramp point */
            CHECK(lane_eval(hl, 1.0, 0.0, 4.0, 0, &v) == 1 && v == 0.0f,
                  "the LEFT point's flag decides the segment, not the right's "
                  "(%f)", v);
            lane_write(hl, 0.0, 0.0f, 0);
            CHECK(lane_eval(hl, 1.0, 0.0, 4.0, 0, &v) == 1 &&
                  fabsf(v - 0.5f) < 1e-6f,
                  "two unheld points did not interpolate: %f, want 0.5", v);
        }
    }

    /* ============== DOUBLE LOOP TAKES THE AUTOMATION WITH IT ==========
     *
     * Move's own Double Loop (Shift+Step 15) is documented as doubling a loop
     * "including its notes and automation". A lane that did not follow would
     * leave the second half silent while the notes played -- the automation
     * and the music disagreeing from that bar on.
     */
    {
        lane_store_t db;
        lane_store_reset(&db);
        lane_fingerprint_t fp = { 0.0, 8.0, 3, 60 };
        lane_t *dl = lane_alloc(&db, "synth", "cutoff", 0, 0, &fp);
        CHECK(dl != NULL, "double lane alloc");
        if (dl) {
            lane_write(dl, 1.0, 0.20f, 0);
            lane_write(dl, 5.0, 0.80f, 1);      /* a p-lock, shape and all */
            const int copied = lane_double(dl, 0.0, 8.0);
            CHECK(copied == 2, "copied %d point(s), want 2", copied);
            CHECK(dl->n == 4, "n=%d, want 4", dl->n);
            CHECK(fabs(dl->pts[2].phase - 9.0) < 1e-9 &&
                  fabsf(dl->pts[2].value - 0.20f) < 1e-6f,
                  "the first copy is at %f = %f, want 9.0 = 0.20",
                  dl->pts[2].phase, dl->pts[2].value);
            CHECK(fabs(dl->pts[3].phase - 13.0) < 1e-9 &&
                  dl->pts[3].hold == 1,
                  "the second copy is at %f (hold=%d) -- SHAPE travels with the "
                  "value, or a doubled p-lock becomes a ramp",
                  dl->pts[3].phase, dl->pts[3].hold);

            /* THE COPIES ARE DORMANT until the clip's new length arrives.
             * Move writes the doubled loop to Song.abl ~10 s later, and a
             * point past the current window does not play -- the same rule
             * that governs any lengthened clip. */
            CHECK(lane_eval(dl, 5.0, 0.0, 8.0, 0, &v) == 1 &&
                  fabsf(v - 0.80f) < 1e-6f,
                  "the original half stopped playing: %f", v);
            /* ...and once the window grows, the second half plays the same. */
            CHECK(lane_eval(dl, 13.0, 0.0, 16.0, 0, &v) == 1 &&
                  fabsf(v - 0.80f) < 1e-6f,
                  "the copied half does not play at 13.0 in a 16-beat window: "
                  "%f", v);
            CHECK(lane_eval(dl, 9.0, 0.0, 16.0, 0, &v) == 1 &&
                  fabsf(v - 0.20f) < 1e-6f,
                  "the copied half is wrong at 9.0: %f", v);
        }
    }
    {
        /* POINTS OUTSIDE THE WINDOW ARE NOT COPIED -- they belong to material
         * this gesture did not touch, and copying them would scatter values
         * into bars nobody doubled. */
        lane_store_t db;
        lane_store_reset(&db);
        lane_fingerprint_t fp = { 8.0, 4.0, 3, 60 };
        lane_t *dl = lane_alloc(&db, "synth", "cutoff", 0, 0, &fp);
        CHECK(dl != NULL, "window lane alloc");
        if (dl) {
            lane_write(dl, 2.0, 0.10f, 0);      /* before the window */
            lane_write(dl, 9.0, 0.50f, 0);      /* inside  [8, 12)   */
            lane_write(dl, 20.0, 0.90f, 0);     /* past it            */
            const int copied = lane_double(dl, 8.0, 4.0);
            CHECK(copied == 1, "copied %d, want 1 (only the point inside)", copied);
            CHECK(lane_eval(dl, 13.0, 8.0, 12.0, 0, &v) == 1 &&
                  fabsf(v - 0.50f) < 1e-6f,
                  "the copy did not land at 13.0 (9.0 + 4): %f", v);
        }
    }
    {
        /* A REFUSAL LEAVES THE LANE ALONE: an unusable window is not a reason
         * to scatter points at NaN, and a lane with nothing inside the window
         * copies nothing rather than reporting success. */
        lane_store_t db;
        lane_store_reset(&db);
        lane_fingerprint_t fp = { 0.0, 8.0, 3, 60 };
        lane_t *dl = lane_alloc(&db, "synth", "cutoff", 0, 0, &fp);
        CHECK(dl != NULL, "refusal lane alloc");
        if (dl) {
            lane_write(dl, 1.0, 0.2f, 0);
            CHECK(lane_double(dl, 0.0, 0.0) == 0 && dl->n == 1,
                  "a zero-length window copied something (n=%d)", dl->n);
            CHECK(lane_double(dl, 0.0, NAN) == 0 && dl->n == 1,
                  "a NaN length copied something (n=%d)", dl->n);
            CHECK(lane_double(dl, 40.0, 8.0) == 0 && dl->n == 1,
                  "a window with no points in it copied something (n=%d)",
                  dl->n);
        }
    }

    /* CLEARING AT THE RIGHT GRAIN, and undo that is its own redo.
     *
     * `lanes:clear` empties the whole SLOT -- every clip, every parameter --
     * which was the only granularity there was, and far blunter than what is
     * actually wanted: "that clip" or "that knob". The key is
     * (track, slot, target, param), so each grain is just which fields match.
     *
     * The store cannot do the clearing itself: a DRIVING lane holds a
     * modulation override on the chain, and dropping it without handing that
     * back leaves the parameter pinned wherever the automation last wrote it,
     * with nothing left to move it. Hence predicates here, release in the
     * chain. */
    {
        lane_store_t cst;
        lane_store_reset(&cst);
        lane_fingerprint_t cfp;
        memset(&cfp, 0, sizeof cfp);

        lane_t *la = lane_alloc(&cst, "synth", "pinch", 0, 1, &cfp);
        lane_t *lb = lane_alloc(&cst, "synth", "morph", 0, 1, &cfp);
        lane_t *lc = lane_alloc(&cst, "synth", "pinch", 0, 2, &cfp);
        lane_t *ld = lane_alloc(&cst, "synth", "pinch", 1, 1, &cfp);
        CHECK(la && lb && lc && ld, "four distinct lanes must be allocatable");

        CHECK(lane_is_for_clip(la, 0, 1) && lane_is_for_clip(lb, 0, 1),
              "both parameters of clip (0,1) belong to it");
        CHECK(!lane_is_for_clip(lc, 0, 1),
              "a different clip SLOT is a different clip");
        CHECK(!lane_is_for_clip(ld, 0, 1),
              "the same slot on another TRACK is a different clip");

        CHECK(lane_is_for_param(la, 0, 1, "synth", "pinch"),
              "the parameter grain matches target AND param");
        CHECK(!lane_is_for_param(lb, 0, 1, "synth", "pinch"),
              "a sibling parameter of the same clip must NOT match");
        CHECK(!lane_is_for_param(la, 0, 1, "fx1", "pinch"),
              "the same param name on another target must not match");

        lane_clear_one(la);
        CHECK(!la->used, "a cleared lane is unused");
        CHECK(lb->used && lc->used && ld->used,
              "clearing one lane must not disturb its neighbours");
    }

    /* UNDO IS A SWAP, so the same verb is redo -- which matters more for
     * automation than for text: the mistake is HEARD, not seen, and the real
     * gesture is "put it back; no, the other one". */
    {
        lane_store_t cur, undo;
        lane_store_reset(&cur);
        lane_store_reset(&undo);
        lane_fingerprint_t ufp;
        memset(&ufp, 0, sizeof ufp);
        lane_alloc(&cur, "synth", "pinch", 0, 1, &ufp);

        int n_cur = 0, n_undo = 0;
        for (int i = 0; i < LANE_MAX; i++) {
            if (cur.lanes[i].used) n_cur++;
            if (undo.lanes[i].used) n_undo++;
        }
        CHECK(n_cur == 1 && n_undo == 0, "one lane, empty undo buffer");

        lane_store_swap(&cur, &undo);
        n_cur = n_undo = 0;
        for (int i = 0; i < LANE_MAX; i++) {
            if (cur.lanes[i].used) n_cur++;
            if (undo.lanes[i].used) n_undo++;
        }
        CHECK(n_cur == 0 && n_undo == 1, "the swap moved the lane across");

        lane_store_swap(&cur, &undo);
        n_cur = 0;
        for (int i = 0; i < LANE_MAX; i++) if (cur.lanes[i].used) n_cur++;
        CHECK(n_cur == 1, "swapping twice is the identity -- undo, then redo");
    }

    /* A FULL STORE MUST SPEND AN ORPHAN BEFORE IT REFUSES.
     *
     * An orphaned lane belongs to a clip that was DELETED, and it is kept on
     * purpose so that undoing the delete brings the automation back with it.
     * But it is kept invisibly and still occupies one of LANE_MAX, so without
     * this a user who deletes clips for a while reaches a slot that silently
     * refuses new automation -- no message, nothing on screen to clear.
     *
     * The guard that must survive: an orphan STILL DRIVING is not a victim.
     * Orphaning does not release the modulation override, and lane_alloc is
     * pure -- it cannot hand one back -- so dropping that lane would pin its
     * parameter wherever automation last wrote it. */
    {
        lane_store_t fs;
        lane_store_reset(&fs);
        lane_fingerprint_t ffp;
        memset(&ffp, 0, sizeof ffp);

        char pname[24];
        for (int i = 0; i < LANE_MAX; i++) {
            snprintf(pname, sizeof pname, "p%d", i);
            CHECK(lane_alloc(&fs, "synth", pname, 0, 0, &ffp) != NULL,
                  "the store should hold LANE_MAX lanes, failed at %d", i);
        }
        CHECK(lane_alloc(&fs, "synth", "one_more", 0, 0, &ffp) == NULL,
              "a store full of LIVE lanes must refuse -- evicting one would "
              "throw away automation for a clip that still exists");

        /* Orphan one, and the next allocation takes its place. */
        fs.lanes[7].orphaned = 1;
        lane_t *taken = lane_alloc(&fs, "synth", "one_more", 0, 0, &ffp);
        CHECK(taken != NULL, "a full store must spend an ORPHAN rather than refuse");
        CHECK(taken == &fs.lanes[7], "the orphan's slot is the one taken");
        CHECK(strcmp(taken->param, "one_more") == 0 && !taken->orphaned,
              "the evicted slot must be rebound cleanly, got param=%s orphaned=%d",
              taken->param, taken->orphaned);
        CHECK(taken->evicted_orphan == 1,
              "an eviction must be RECORDED -- it is the one allocation that "
              "destroys something");

        /* ...but never one that is still driving. */
        lane_store_reset(&fs);
        for (int i = 0; i < LANE_MAX; i++) {
            snprintf(pname, sizeof pname, "q%d", i);
            lane_alloc(&fs, "synth", pname, 0, 0, &ffp);
        }
        fs.lanes[3].orphaned = 1;
        fs.lanes[3].driving = 1;
        CHECK(lane_alloc(&fs, "synth", "nope", 0, 0, &ffp) == NULL,
              "an orphan that is STILL DRIVING must not be evicted -- its "
              "override would never be handed back and the parameter would "
              "stay pinned where the automation left it");
    }

    /* ------------------------------------------------------------------
     * A P-LOCK ENDS AT ITS OWN STEP.
     *
     * "end at its own step! you're just editing a step!" -- and before the
     * span existed a single lock meant the whole bar AND the bar before it: a
     * held point stood until the next point, and the "before the first point"
     * rule held it backwards to phase 0. Measured on the device with one lock
     * at step 4: every one of the sixteen steps reported the locked value, and
     * playback was already at it before phase 1.
     * ------------------------------------------------------------------ */
    {
        lane_store_t st; lane_store_reset(&st);
        lane_fingerprint_t fp = { 0.0, 4.0, 1, 60 };
        lane_t *ln = lane_alloc(&st, "synth", "cutoff", 0, 0, &fp);
        CHECK(ln != NULL, "span: lane_alloc");
        if (ln) {
            float v = -1.0f;
            /* One lock on step 4 of a 1/16 grid: phase 1.0, span 0.25. */
            lane_write_span(ln, 1.0, 90.0f, 1, 0.25);

            CHECK(lane_eval(ln, 1.0, 0.0, 4.0, 0, &v) && v == 90.0f,
                  "the lock does not play at its own phase (%f)", v);
            CHECK(lane_eval(ln, 1.24, 0.0, 4.0, 0, &v) && v == 90.0f,
                  "the lock ended before its step did (%f)", v);
            /* ...and NOT one sample past it, in either direction. */
            CHECK(lane_eval(ln, 1.25, 0.0, 4.0, 0, &v) == 0,
                  "the lock outlived its own step -- it stood at %f", v);
            CHECK(lane_eval(ln, 0.0, 0.0, 4.0, 0, &v) == 0,
                  "the lock reached BACKWARDS to the downbeat (%f)", v);
            CHECK(lane_eval(ln, 3.9, 0.0, 4.0, 0, &v) == 0,
                  "the lock stood for the rest of the loop (%f)", v);

            /* THE CONTROL, and the old meaning: a held point with NO span is
             * still "until the next point", so every lane already on disk
             * behaves exactly as it did. Without this the test above could
             * pass because spans broke holding altogether. */
            lane_store_reset(&st);
            lane_t *lg = lane_alloc(&st, "synth", "cutoff", 0, 0, &fp);
            CHECK(lg != NULL, "span: legacy lane_alloc");
            if (lg) {
                lane_write(lg, 1.0, 90.0f, 1);
                CHECK(lane_eval(lg, 3.9, 0.0, 4.0, 0, &v) && v == 90.0f,
                      "a legacy held point stopped holding (%f)", v);
            }

            /* A RECORDED SWEEP UNDERNEATH KEEPS PLAYING outside the lock:
             * the lock owns its step and is invisible everywhere else, so the
             * curve interpolates across it as though it were not there. */
            lane_store_reset(&st);
            lane_t *lm = lane_alloc(&st, "synth", "cutoff", 0, 0, &fp);
            CHECK(lm != NULL, "span: mixed lane_alloc");
            if (lm) {
                lane_write(lm, 0.0, 0.0f, 0);          /* sweep 0 -> 40 over 4 beats */
                lane_write(lm, 4.0, 40.0f, 0);
                lane_write_span(lm, 1.0, 90.0f, 1, 0.25);
                CHECK(lane_eval(lm, 1.1, 0.0, 8.0, 0, &v) && v == 90.0f,
                      "the lock did not win inside its step over a sweep (%f)", v);
                CHECK(lane_eval(lm, 2.0, 0.0, 8.0, 0, &v) && v == 20.0f,
                      "the sweep did not resume after the lock's step (%f)", v);
                CHECK(lane_eval(lm, 0.5, 0.0, 8.0, 0, &v) && v == 5.0f,
                      "the lock disturbed the sweep BEFORE it (%f)", v);
            }
        }
    }

    /* ============ THE CLIP ROW WE COULD NOT READ YET ==================
     *
     * A lane is keyed by (track, clip row). A brand-new clip has no row for
     * 8-12 s -- measured on hardware -- because the row comes only from a
     * session pad LED (Session view) or Song.abl, and it has neither. The
     * gesture is recorded against LANE_SLOT_PENDING and re-keyed when the file
     * names the real row.
     *
     * What must NOT happen is binding to the wrong clip. */
    {
        lane_store_t st;
        lane_store_reset(&st);
        lane_fingerprint_t absent = { 0.0, 0.0, 0, -1 };
        lane_fingerprint_t real   = { 0.0, 8.0, 3, 60 };

        lane_t *ln = lane_alloc(&st, "synth", "cutoff", 1, LANE_SLOT_PENDING, &absent);
        CHECK(ln != NULL, "a lane could not be keyed to the pending row");
        if (ln) {
            ln->slot_pending = 1;
            ln->pending_len = 8.0;          /* two bars, off the bar strip */

            CHECK(lane_slot_is_pending(ln->slot), "premise: the row is pending");
            CHECK(!lane_slot_usable(-1), "a plain -1 must still mean NO clip");
            CHECK(lane_slot_usable(LANE_SLOT_PENDING),
                  "the pending row must be able to key a lane, or the gesture "
                  "is refused exactly as before");
            CHECK(lane_slot_usable(0), "a real row must still key a lane");

            /* THE WRONG TRACK NEVER BINDS. */
            CHECK(lane_adopt_slot(ln, 2, 3, 8.0, 8.0, NULL) == 0,
                  "a lane bound to a clip on a DIFFERENT track");

            /* A DIFFERENT LENGTH IS A DIFFERENT CLIP. This is the delete-and-
             * remake-inside-the-window case, and the whole reason the length
             * is kept. */
            CHECK(lane_adopt_slot(ln, 1, 3, 8.0, 4.0, &real) == 0,
                  "a lane bound to a clip of the wrong length -- a clip remade "
                  "inside the save window would inherit the previous take");
            CHECK(ln->slot_pending == 1 && lane_slot_is_pending(ln->slot),
                  "a refused adoption must leave the lane PENDING, not keyed");

            /* "Unknown" is not a match, in either direction. */
            CHECK(lane_adopt_slot(ln, 1, 3, 0.0, 8.0, NULL) == 0,
                  "a lane with no recorded length bound anyway");
            CHECK(lane_adopt_slot(ln, 1, 3, 8.0, 0.0, NULL) == 0,
                  "a lane bound to a clip of unknown length");

            /* AND THE RIGHT ONE DOES, with the strip's pixel-derived length
             * allowed to differ from the file's float by less than a bar. */
            CHECK(lane_adopt_slot(ln, 1, 3, 8.0, 8.0, &real) == 1,
                  "the matching clip was refused");
            CHECK(ln->slot == 3 && ln->slot_pending == 0,
                  "the lane was not re-keyed to row 3 (slot=%d pending=%d)",
                  ln->slot, ln->slot_pending);
            CHECK(lane_is_for_clip(ln, 1, 3),
                  "the re-keyed lane does not answer for its clip");

            /* THE IDENTITY CAME WITH THE ROW. Without it the lane is re-keyed
             * and then goes STALE the instant the clip appears, because an
             * ABSENT fingerprint is refused outright -- silent for good, which
             * is what hardware showed. */
            CHECK(!lane_fp_absent(&ln->fp),
                  "the re-keyed lane has no identity -- it will go stale and "
                  "never play");
            CHECK(lane_fingerprint_matches(ln, &real),
                  "the re-keyed lane does not match the clip it was bound to");
            CHECK(ln->stale == 0, "the re-keyed lane is stale");

            /* AND THE POINTS DID NOT MOVE. lane_adopt_fingerprint re-origins;
             * this must not, or a lock lands off the step that was pressed. */
            CHECK(ln->n == 0 || ln->pts[0].phase == ln->pts[0].phase,
                  "premise: the lane's points are readable");

            /* ONCE KEYED, NEVER RE-KEYED. A second file write must not move a
             * lane that is already bound. */
            CHECK(lane_adopt_slot(ln, 1, 5, 8.0, 8.0, NULL) == 0,
                  "an already-keyed lane was moved to another row");
        }

        /* A lane that was never blind is not adoptable, whatever arrives --
         * the same rule origin_pending carries, and for the same reason: on
         * disk the two are identical bytes. */
        lane_t *plain = lane_alloc(&st, "synth", "res", 1, 0, &absent);
        CHECK(plain != NULL, "a plain lane_alloc");
        if (plain)
            CHECK(lane_adopt_slot(plain, 1, 3, 8.0, 8.0, NULL) == 0,
                  "a lane that never recorded blind was re-keyed");
    }

    /* WHICH ROW A LANE IS MATCHED AGAINST — lane_effective_slot.
     *
     * The defect: an unknown row (-1) was compared as if it were a row, so
     * "we cannot name the clip" silenced lanes exactly like "a different clip
     * is playing". */
    CHECK(lane_effective_slot(3, 5) == 3,
          "a known row did not win over the remembered one");
    CHECK(lane_effective_slot(-1, 5) == 5,
          "an unknown row did not fall back to the last known one");
    /* PENDING is the row of a clip Move has not written yet. It must be held
     * like any other answer, or leaving that track stops its automation. */
    CHECK(lane_effective_slot(-1, LANE_SLOT_PENDING) == LANE_SLOT_PENDING,
          "a pending row was not held when the row went unknown");
    CHECK(lane_effective_slot(LANE_SLOT_PENDING, 4) == LANE_SLOT_PENDING,
          "a live pending row was overridden by the remembered one");
    /* Nothing ever known: stay unknown rather than inventing row 0, which is
     * a real row and would play its automation unasked. */
    CHECK(lane_effective_slot(-1, -1) == -1,
          "an unknown row with nothing remembered invented a row");

    /* DOUBLE LOOP MUST CARRY THE SPAN.
     *
     * `lane_write`'s 4-argument form leaves span 0, and span 0 MEANS "hold
     * until the next point" — the legacy behaviour the span field was added
     * to replace. So the doubled half's p-locks widened from one step to the
     * rest of the bar while the original half stayed correct, and the two
     * halves of a doubled loop stopped sounding the same, which is the entire
     * promise of the gesture. */
    {
        lane_store_t ds; memset(&ds, 0, sizeof(ds));
        lane_fingerprint_t dfp = { 0 }; dfp.first_note = 36; dfp.note_count = 2;
        lane_t *dl = lane_alloc(&ds, "synth", "ht_c_tune", 0, 0, &dfp);
        CHECK(dl != NULL, "lane_alloc refused for the double test");
        /* a p-lock: a held point with a SPAN of one sixteenth */
        lane_write_span(dl, 1.5, 0.75f, 1, 0.25);
        const int before = dl->n;
        const int made = lane_double(dl, 0.0, 4.0);
        CHECK(made == 1, "lane_double copied %d points, wanted 1", made);
        CHECK(dl->n == before + 1, "lane_double did not append one point");

        /* find the copy — one loop later */
        int found = -1;
        for (int i2 = 0; i2 < dl->n; i2++)
            if (fabs(dl->pts[i2].phase - 5.5) < 1e-9) { found = i2; break; }
        CHECK(found >= 0, "the doubled point is not at phase 5.5");
        if (found >= 0) {
            CHECK(dl->pts[found].hold == 1,
                  "the doubled point lost its hold flag");
            CHECK(fabs(dl->pts[found].span - 0.25) < 1e-9,
                  "the doubled point's span is %.4f, wanted 0.25 — span 0 means "
                  "'hold to the next point', so this lock smears across the bar",
                  dl->pts[found].span);
        }
    }

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: lane_store\n");
    return 0;
}
