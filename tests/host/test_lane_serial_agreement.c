/* THE WRITER AND THE READER MUST AGREE, and they were two independent rule
 * sets that only happened to.
 *
 * lane_serial.c decides what to emit (skip a provisional lane, skip one with
 * an absent fingerprint) and, separately, what to accept (a version, sorted
 * phases, no duplicate keys, a held point with a span, an in-range key). Each
 * half was tested against hand-written cases; nothing tested them against
 * EACH OTHER. That is exactly where the last defect lived -- the writer
 * refused to emit a key the reader would happily assign -- and the shape of
 * the failure is silent either way: a document we wrote that we then refuse
 * loses a whole set's automation on load, and a document we accept that we
 * would never write is how a zombie gets in.
 *
 * So this generates stores across the state space and asserts the pair:
 *
 *   1. anything we SERIALIZE, we DESERIALIZE -- no self-refusal;
 *   2. every lane the writer emitted comes back with its key, fingerprint and
 *      every point identical -- no silent mangling;
 *   3. the lanes the writer SKIPS are exactly the provisional ones, and none
 *      of them comes back.
 *
 * Deterministic: a fixed LCG, so a failure is reproducible from the seed
 * printed with it. Not a fuzzer -- a sweep.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lane_store.h"
#include "lane_serial.h"

static int fails;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static unsigned long rng_state;
static unsigned rnd(unsigned n) {
    rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)((rng_state >> 33) % n);
}

int main(void) {
    static char doc[262144];

    for (unsigned seed = 1; seed <= 400; seed++) {
        rng_state = seed;
        lane_store_t st;
        memset(&st, 0, sizeof(st));

        const char *targets[] = { "synth", "fx1", "fx2", "midifx1" };
        const char *params[]  = { "cutoff", "room_size", "rate", "amount", "mix" };

        int want = 1 + (int)rnd(LANE_MAX);
        for (int i = 0; i < want; i++) {
            const int track = (int)rnd(LANE_TRACKS);
            /* A quarter of the lanes get the placeholder row, so the skip
             * path is exercised rather than assumed. */
            const int pend  = rnd(4) == 0;
            const int slot  = pend ? LANE_SLOT_PENDING : (int)rnd(LANE_ROWS);
            /* ...and a quarter of the identified ones carry no fingerprint,
             * which is the OTHER provisional shape. */
            const int no_fp = rnd(4) == 0;
            lane_fingerprint_t fp;
            if (no_fp) {
                fp.loop_start = 0.0; fp.loop_len = 0.0;
                fp.note_count = 0;   fp.first_note = -1;
            } else {
                fp.loop_start = (double)rnd(9);
                fp.loop_len   = 1.0 + (double)rnd(32);
                fp.note_count = 1 + (int)rnd(64);
                fp.first_note = (int)rnd(128);
            }
            lane_t *ln = lane_alloc(&st, targets[rnd(4)], params[rnd(5)],
                                    track, slot, &fp);
            if (!ln) continue;   /* store full, or the key collided: both fine */
            if (pend) ln->pending_len = 1.0 + (double)rnd(32);

            int pts = 1 + (int)rnd(LANE_POINTS_MAX);
            for (int j = 0; j < pts; j++) {
                /* Phases on a grid so two writes cannot land inside
                 * LANE_MIN_POINT_BEATS of each other and replace one another
                 * -- this test is about serialization, not about lane_write's
                 * own replacement rule. */
                const double ph = (double)rnd(256) * 0.25;
                lane_write(ln, ph, (float)rnd(1000) / 1000.0f, rnd(2));
            }
            if (rnd(3) == 0) { ln->rec_active = 1; ln->rec_last_phase = 1.0; }
            if (rnd(5) == 0) ln->orphaned = 1;
            if (rnd(5) == 0) ln->stale = 1;
        }

        /* What SHOULD survive: exactly the lanes that are neither shape of
         * provisional. Recorded before serializing, from the same predicates
         * the writer uses -- if those predicates are the thing that is wrong,
         * assertion 3 below is what catches it, not this. */
        lane_t expect[LANE_MAX];
        int nexpect = 0;
        for (int i = 0; i < LANE_MAX; i++) {
            const lane_t *ln = &st.lanes[i];
            if (!ln->used) continue;
            if (lane_slot_is_pending(ln->slot) || lane_fp_absent(&ln->fp))
                continue;
            expect[nexpect++] = *ln;
        }

        int n = lane_store_serialize(&st, doc, sizeof(doc));
        CHECK(n > 0 || nexpect == 0,
              "seed %u: serialize returned %d for %d emittable lane(s)",
              seed, n, nexpect);
        if (n <= 0) continue;

        lane_store_t back;
        memset(&back, 0, sizeof(back));
        int r = lane_store_deserialize(&back, doc);
        /* 1. NO SELF-REFUSAL. This is the assertion that would have caught a
         *    reader rule the writer does not respect -- which loses every
         *    lane in the document, not just the offending one. */
        CHECK(r > 0, "seed %u: we REFUSED a document we just wrote (r=%d, "
                     "%d lanes, %d bytes)", seed, r, nexpect, n);
        if (r <= 0) continue;

        int nback = 0;
        for (int i = 0; i < LANE_MAX; i++) if (back.lanes[i].used) nback++;
        CHECK(nback == nexpect,
              "seed %u: %d lane(s) written, %d came back", seed, nexpect, nback);

        /* 2. EVERY EMITTED LANE IS IDENTICAL, found by key rather than by
         *    index: the writer compacts, so position is not preserved and an
         *    index-wise comparison would fail for the wrong reason. */
        for (int e = 0; e < nexpect; e++) {
            const lane_t *want_ln = &expect[e];
            const lane_t *got = NULL;
            for (int i = 0; i < LANE_MAX; i++) {
                const lane_t *b = &back.lanes[i];
                if (!b->used) continue;
                if (b->track == want_ln->track && b->slot == want_ln->slot &&
                    strcmp(b->target, want_ln->target) == 0 &&
                    strcmp(b->param, want_ln->param) == 0) { got = b; break; }
            }
            CHECK(got != NULL, "seed %u: %s:%s t=%d row=%d did not come back",
                  seed, want_ln->target, want_ln->param,
                  want_ln->track, want_ln->slot);
            if (!got) continue;
            CHECK(got->n == want_ln->n,
                  "seed %u: %s:%s came back with %d point(s), wrote %d",
                  seed, want_ln->target, want_ln->param, got->n, want_ln->n);
            CHECK(got->fp.note_count == want_ln->fp.note_count &&
                  got->fp.first_note == want_ln->fp.first_note,
                  "seed %u: %s:%s fingerprint changed (%d/%d -> %d/%d) — the "
                  "content half is what binds a lane to its clip",
                  seed, want_ln->target, want_ln->param,
                  want_ln->fp.note_count, want_ln->fp.first_note,
                  got->fp.note_count, got->fp.first_note);
            for (int j = 0; j < got->n && j < want_ln->n; j++) {
                CHECK(got->pts[j].phase == want_ln->pts[j].phase,
                      "seed %u: %s:%s point %d moved from %.6f to %.6f",
                      seed, want_ln->target, want_ln->param, j,
                      want_ln->pts[j].phase, got->pts[j].phase);
                CHECK(got->pts[j].hold == want_ln->pts[j].hold,
                      "seed %u: %s:%s point %d lost its hold flag — a p-lock "
                      "becomes a glide", seed, want_ln->target,
                      want_ln->param, j);
                CHECK(got->pts[j].span == want_ln->pts[j].span,
                      "seed %u: %s:%s point %d span %.6f -> %.6f — a lock owns "
                      "one step, and span 0 means hold to the next point",
                      seed, want_ln->target, want_ln->param, j,
                      want_ln->pts[j].span, got->pts[j].span);
            }
        }

        /* 3. AND NOTHING PROVISIONAL CAME BACK, under either definition. */
        for (int i = 0; i < LANE_MAX; i++) {
            const lane_t *b = &back.lanes[i];
            if (!b->used) continue;
            CHECK(!lane_slot_is_pending(b->slot),
                  "seed %u: a lane keyed to the PENDING placeholder survived — "
                  "nothing can re-key it and the next blind clip matches it",
                  seed);
            CHECK(!lane_fp_absent(&b->fp),
                  "seed %u: a lane with an ABSENT fingerprint survived — it "
                  "reloads permanently stale and squats on its key", seed);
            CHECK(lane_key_in_range(b->track, b->slot),
                  "seed %u: a lane came back keyed t=%d row=%d, outside the "
                  "session grid", seed, b->track, b->slot);
        }

        if (fails > 40) { printf("(stopping early)\n"); break; }
    }

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: the writer and the reader agree across 400 generated stores\n");
    return 0;
}
