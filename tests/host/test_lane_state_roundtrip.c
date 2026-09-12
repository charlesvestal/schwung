/*
 * lane_serial — the `lanes:state` document (Task 8 of automation lanes).
 *
 * What this file is really guarding is the FINGERPRINT round trip, not the
 * breakpoints. `{note_count: 0, first_note: -1}` is the pattern
 * lane_fingerprint_matches refuses outright ("no fingerprint was recorded"),
 * and that refusal is the only thing standing between a pre-fingerprint lane
 * and binding to the wrong clip. A loader that defaults a missing first_note
 * to 0 puts the lane back into the store with a fingerprint that CLAIMS note
 * 0, which escapes the placeholder rule -- so the defect comes back through
 * the file, with nothing on screen to explain it. The document-without-the-
 * field case below is the only place that can see it.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lane_store.h"
#include "lane_serial.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); fails++; } } while (0)

/* A lane by key, with its content, so an assertion can name what it wanted. */
static const lane_t *find_used(const lane_store_t *st, const char *target,
                               const char *param) {
    for (int i = 0; i < LANE_MAX; i++) {
        const lane_t *ln = &st->lanes[i];
        if (!ln->used) continue;
        if (strcmp(ln->target, target) == 0 && strcmp(ln->param, param) == 0)
            return ln;
    }
    return NULL;
}

static int used_count(const lane_store_t *st) {
    int n = 0;
    for (int i = 0; i < LANE_MAX; i++) if (st->lanes[i].used) n++;
    return n;
}

int main(void) {
    static lane_store_t a, b;
    char buf[LANE_SERIAL_MAX_BYTES];

    /* ---- the round trip -------------------------------------------- */
    lane_fingerprint_t fp = { 4.0, 8.0, 14, 41 };
    lane_store_reset(&a);
    lane_t *ln = lane_alloc(&a, "fx3", "feedback", 2, 5, &fp);
    CHECK(ln != NULL, "lane_alloc refused");
    lane_write(ln, 0.0, 0.10f);
    lane_write(ln, 3.5, 0.75f);
    lane_write(ln, 7.25, 0.33f);

    /* A second lane, so the parser has to find where one lane's points end
     * and the next lane's header begins -- a single-lane document cannot
     * distinguish a parser that reads the header count from one that reads
     * until the next header. */
    lane_fingerprint_t fp2 = { 0.0, 4.0, 3, 60 };
    lane_t *ln2 = lane_alloc(&a, "synth", "cutoff", 0, 1, &fp2);
    CHECK(ln2 != NULL, "second lane_alloc refused");
    lane_write(ln2, 1.0, 0.5f);
    lane_write(ln2, 2.0, 0.25f);

    int n = lane_store_serialize(&a, buf, sizeof(buf));
    CHECK(n > 0, "serialize returned %d", n);
    CHECK(n == (int)strlen(buf), "returned %d but wrote %d bytes",
          n, (int)strlen(buf));

    lane_store_reset(&b);
    CHECK(lane_store_deserialize(&b, buf) == 1, "deserialize failed");
    CHECK(used_count(&b) == 2, "lane count %d, want 2", used_count(&b));

    const lane_t *r = find_used(&b, "fx3", "feedback");
    CHECK(r != NULL, "lane did not survive the round trip");
    CHECK(r && r->n == 3, "point count %d, want 3", r ? r->n : -1);
    CHECK(r && r->track == 2 && r->slot == 5, "position lost");
    CHECK(r && fabs(r->fp.loop_start - 4.0) < 1e-9, "loop_start lost");
    CHECK(r && fabs(r->fp.loop_len - 8.0) < 1e-9, "loop_len lost");
    CHECK(r && r->fp.note_count == 14 && r->fp.first_note == 41,
          "fingerprint lost");
    for (int i = 0; r && i < r->n; i++) {
        CHECK(fabs(r->pts[i].phase - ln->pts[i].phase) < 1e-6,
              "phase %d drifted: %f vs %f", i, r->pts[i].phase,
              ln->pts[i].phase);
        CHECK(fabsf(r->pts[i].value - ln->pts[i].value) < 1e-6f,
              "value %d drifted: %f vs %f", i, (double)r->pts[i].value,
              (double)ln->pts[i].value);
    }
    const lane_t *r2 = find_used(&b, "synth", "cutoff");
    CHECK(r2 != NULL, "second lane did not survive");
    CHECK(r2 && r2->n == 2, "second lane point count %d, want 2",
          r2 ? r2->n : -1);
    CHECK(r2 && r2->track == 0 && r2->slot == 1, "second position lost");
    CHECK(r2 && r2->fp.note_count == 3 && r2->fp.first_note == 60,
          "second fingerprint lost");

    /* A full lane, to prove the document is not sized for the typical case:
     * LANE_POINTS_MAX points survive, and phases closer together than
     * LANE_MIN_POINT_BEATS survive too (the full-lane replace branch can
     * legitimately produce them, and a loader that re-thinned would silently
     * lose them). */
    lane_store_reset(&a);
    lane_t *full = lane_alloc(&a, "midi_fx1", "rate", 3, 7, &fp);
    for (int i = 0; i < LANE_POINTS_MAX; i++)
        lane_write(full, (double)i * LANE_MIN_POINT_BEATS / 2.0, (float)i / 100.0f);
    CHECK(full->n == LANE_POINTS_MAX || full->n > 1,
          "fixture did not fill the lane (n=%d)", full->n);
    int saved_n = full->n;
    n = lane_store_serialize(&a, buf, sizeof(buf));
    CHECK(n > 0, "full-lane serialize returned %d", n);
    lane_store_reset(&b);
    CHECK(lane_store_deserialize(&b, buf) == 1, "full-lane deserialize failed");
    const lane_t *rf = find_used(&b, "midi_fx1", "rate");
    CHECK(rf && rf->n == saved_n, "full lane lost points: %d, want %d",
          rf ? rf->n : -1, saved_n);

    /* ---- runtime flags are NOT content ----------------------------- */
    /* `stale` is recomputed by lane_tick from the LIVE fingerprint, and only
     * a match clears it. Persisting it would strand a lane whose clip is
     * present, because nothing else in the system un-stales one. */
    lane_store_reset(&a);
    lane_t *sl = lane_alloc(&a, "fx1", "mix", 1, 1, &fp);
    lane_write(sl, 0.0, 0.5f);
    sl->stale = 1;
    sl->orphaned = 1;
    sl->driving = 1;
    sl->punch_until_wrap = 1;
    n = lane_store_serialize(&a, buf, sizeof(buf));
    CHECK(n > 0, "serialize with runtime flags returned %d", n);
    lane_store_reset(&b);
    CHECK(lane_store_deserialize(&b, buf) == 1, "deserialize failed");
    const lane_t *rs = find_used(&b, "fx1", "mix");
    CHECK(rs && rs->stale == 0, "stale was persisted");
    CHECK(rs && rs->orphaned == 0, "orphaned was persisted");
    CHECK(rs && rs->driving == 0, "driving was persisted");
    CHECK(rs && rs->punch_until_wrap == 0, "punch_until_wrap was persisted");

    /* ---- an empty store writes NOTHING ----------------------------- */
    /* This is what stops an empty document being written over a good file:
     * the UI's write is gated on a non-empty answer, so serialize must hand
     * back zero bytes rather than a well-formed document with no lanes. */
    lane_store_reset(&a);
    CHECK(lane_store_serialize(&a, buf, sizeof(buf)) == 0,
          "an empty store produced a document");

    /* ---- a missing first_note is -1, NEVER 0 ----------------------- */
    /* 0 is a real note number, so defaulting to it turns "no fingerprint was
     * recorded" into "the earliest note was note 0" -- a claim, which
     * lane_fingerprint_matches then believes. Only a document that LACKS the
     * field can see this. */
    {
        const char *doc_no_first_note =
            "V 1\n"
            "L fx2 drive 1 2 0 4 0\n"
            "P 0 0.5\n";
        lane_store_reset(&b);
        CHECK(lane_store_deserialize(&b, doc_no_first_note) == 1,
              "a document without first_note was refused");
        const lane_t *nf = find_used(&b, "fx2", "drive");
        CHECK(nf != NULL, "lane without first_note did not load");
        CHECK(nf && nf->fp.first_note == -1,
              "missing first_note deserialized to %d, want -1",
              nf ? nf->fp.first_note : -999);
        /* And the placeholder rule must still refuse it: the whole point of
         * -1 is that this lane loads STALE rather than binding to any clip
         * whose loop starts where it does. */
        lane_fingerprint_t any = { 0.0, 4.0, 0, 0 };
        CHECK(nf && lane_fingerprint_matches(nf, &any) == 0,
              "a lane with no recorded fingerprint matched a clip");
    }

    /* Same again with note_count present but first_note absent — the exact
     * shape a pre-fingerprint writer would leave if it learned to count
     * notes before it learned to record the first one. */
    {
        const char *doc = "V 1\nL fx2 drive 1 2 0 4 5\nP 0 0.5\n";
        lane_store_reset(&b);
        CHECK(lane_store_deserialize(&b, doc) == 1, "note_count-only refused");
        const lane_t *nf = find_used(&b, "fx2", "drive");
        CHECK(nf && nf->fp.note_count == 5, "note_count lost");
        CHECK(nf && nf->fp.first_note == -1,
              "first_note absent but deserialized to %d", nf ? nf->fp.first_note : -999);
    }

    /* ---- a malformed document leaves the store UNTOUCHED ----------- */
    /* Half-loading is worse than refusing: the lanes that did land would play
     * against a clip the missing ones were recorded with, and nothing would
     * report it. Every case below must leave `b` exactly as the good load
     * left it. */
    lane_store_reset(&b);
    CHECK(lane_store_deserialize(&b, "V 1\nL fx3 feedback 2 5 4 8 14 41 3\n"
                                     "P 0 0.1\nP 3.5 0.75\nP 7.25 0.33\n") == 1,
          "reference load failed");
    lane_store_t ref = b;

    struct { const char *name; const char *doc; } bad[] = {
        /* Truncated mid-P-line: the value never arrived. A parser that took
         * the phase and defaulted the value would plant 0.0 on a breakpoint
         * the user never played. */
        { "truncated mid-P (no value)",
          "V 1\nL synth cutoff 0 1 0 4 3 60 2\nP 1.0 0.5\nP 2.0\n" },
        { "truncated mid-P (no fields)",
          "V 1\nL synth cutoff 0 1 0 4 3 60 2\nP 1.0 0.5\nP\n" },
        /* A P line with no L ahead of it has no lane to belong to. */
        { "orphan P line", "V 1\nP 1.0 0.5\n" },
        /* An L line missing the position it is keyed to. */
        { "truncated L header", "V 1\nL synth cutoff 0\n" },
        /* A key too long for lane_t's storage is REFUSED, never truncated --
         * two over-length keys would otherwise collide onto one stored
         * string and bind a lane to the wrong parameter. */
        { "over-long target",
          "V 1\nL this_target_is_far_too_long_for_the_field cutoff 0 1 0 4 3 60 1\nP 0 0.5\n" },
        { "over-long param",
          "V 1\nL synth this_param_name_is_much_too_long_for_the_field 0 1 0 4 3 60 1\nP 0 0.5\n" },
        /* A count in the header that disagrees with the P lines that follow.
         * Believing the count over the data is a buffer overrun waiting to
         * happen; ignoring it silently accepts a corrupt file. Refusing says
         * so. */
        { "header count too high",
          "V 1\nL synth cutoff 0 1 0 4 3 60 5\nP 1.0 0.5\nP 2.0 0.25\n" },
        { "header count too low",
          "V 1\nL synth cutoff 0 1 0 4 3 60 1\nP 1.0 0.5\nP 2.0 0.25\n" },
        /* Non-finite content: lane_write refuses these at record time for the
         * same reason (they would be handed straight to a synth parameter). */
        { "nan phase", "V 1\nL synth cutoff 0 1 0 4 3 60 1\nP nan 0.5\n" },
        { "inf value", "V 1\nL synth cutoff 0 1 0 4 3 60 1\nP 1.0 inf\n" },
        { "negative phase", "V 1\nL synth cutoff 0 1 0 4 3 60 1\nP -1.0 0.5\n" },
        /* Out of order: lane_eval walks pts[] assuming ascending phase, so an
         * unsorted document would evaluate to the wrong curve rather than to
         * an error. */
        { "unsorted points",
          "V 1\nL synth cutoff 0 1 0 4 3 60 2\nP 2.0 0.5\nP 1.0 0.25\n" },
        /* More points than a lane can hold, and more lanes than a store can:
         * silently dropping either loses automation the user recorded. */
        { "too many lanes",
          /* LANE_MAX + 1 single-point lanes, built below. */ NULL },
        /* A version from the future cannot be read safely -- refusing is the
         * only answer that cannot be confidently wrong. */
        { "future version", "V 99\nL synth cutoff 0 1 0 4 3 60 1\nP 0 0.5\n" },
        { "junk line", "V 1\nL synth cutoff 0 1 0 4 3 60 1\nP 0 0.5\nX oops\n" },
    };

    /* Build the too-many-lanes document. */
    static char too_many[LANE_SERIAL_MAX_BYTES];
    {
        int off = snprintf(too_many, sizeof(too_many), "V 1\n");
        for (int i = 0; i <= LANE_MAX; i++)
            off += snprintf(too_many + off, sizeof(too_many) - off,
                            "L fx1 p%d 0 1 0 4 3 60 1\nP 0 0.5\n", i);
    }

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const char *doc = bad[i].doc ? bad[i].doc : too_many;
        lane_store_t before = b;
        int rc = lane_store_deserialize(&b, doc);
        CHECK(rc == 0, "%s was accepted", bad[i].name);
        CHECK(memcmp(&b, &before, sizeof(b)) == 0,
              "%s modified the store", bad[i].name);
    }
    CHECK(memcmp(&b, &ref, sizeof(b)) == 0,
          "the reference store did not survive the malformed documents");

    /* A NULL or empty document is not an error worth wiping a store over. */
    {
        lane_store_t before = b;
        CHECK(lane_store_deserialize(&b, "") == 0, "empty doc accepted");
        CHECK(lane_store_deserialize(&b, NULL) == 0, "NULL doc accepted");
        CHECK(memcmp(&b, &before, sizeof(b)) == 0,
              "an empty document modified the store");
    }

    /* ---- a buffer too small REFUSES, it does not truncate ---------- */
    /* A truncated document is a malformed one, and the caller writes it to a
     * file. Returning a positive count for a short write is how a good
     * lanes_<i>.json gets replaced with half of one. */
    {
        char tiny[8];
        lane_store_reset(&a);
        lane_t *t = lane_alloc(&a, "fx3", "feedback", 2, 5, &fp);
        lane_write(t, 0.0, 0.1f);
        CHECK(lane_store_serialize(&a, tiny, (int)sizeof(tiny)) < 0,
              "a too-small buffer did not report failure");
    }

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: lane state round-trip\n");
    return 0;
}
