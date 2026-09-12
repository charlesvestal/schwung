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
    CHECK(lane_find(&st, long_target, "cutoff") == NULL,
          "over-length target should never be findable");
    const char *long_param =
        "some_really_long_param_name_that_is_definitely_over_thirty_two_bytes";
    lane_t *a2 = lane_alloc(&st, "synth", long_param, 0, 0, NULL);
    CHECK(a2 == NULL, "over-length param should be refused by lane_alloc, got %p",
          (void *)a2);
    CHECK(lane_find(&st, "synth", long_param) == NULL,
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

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: lane_store\n");
    return 0;
}
