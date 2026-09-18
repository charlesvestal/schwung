/*
 * shadow_slot_clip_phase() -- slot -> Move track -> where in the clip we are.
 *
 * This replaces five source greps in tests/host/test_lane_phase_seam.sh. Each
 * of them read stronger than it was, and this project's own history says why
 * that matters: a grep pin is blind inside a comment and inside a regex
 * literal, and 316 green tests once missed a dead UI door. The five:
 *
 *   - "identity survives a missing anchor" was two line NUMBERS compared. It
 *     could not see whether the CALLER keeps the identity it wrote when the
 *     return is 0 -- which is the whole contract.
 *   - "phase is rebased onto loop_start" grepped for `ph - r->loop_start`,
 *     which matches the expression sitting in a comment.
 *   - the bounds and NaN paths were not pinned at all.
 *
 * So drive the function. The unit is INCLUDED rather than linked, the way
 * test_master_fx_permute.c includes it: shadow_chain_mgmt.c is 5000 lines with
 * file-static state and no test seam, and adding one to production code to
 * make a test possible is worse than including the file.
 *
 * clip_phase_beats() is the REAL one (src/host/clip_state.c is linked), not a
 * fake. Case 3 exists to prove the rebasing subtraction, and a fake phase
 * function would have made that assertion a statement about the fake: the
 * subtraction is only correct because clip_phase_beats() adds loop_start back
 * on before returning, DESPITE its header comment saying it returns beats from
 * the loop start. Believing the comment over the body is the mistake this case
 * is here to catch, and it can only be caught against the body.
 *
 * RT note: everything under test runs on the SPI callback. Nothing here needs
 * a thread, which is itself part of the contract -- the resolver is table
 * reads only.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "shadow_chain_mgmt.c"
#include "step_strip.h"

/* No worker in this fixture, so nothing ever "newly appeared" and nothing is
 * selected: both answer "not known", which keeps shadow_slot_clip_phase on the
 * path it took before those signals existed. */
/* SETTABLE, because the ladder's exit from the blind state depends on it: the
 * worker publishes the row that newly appeared in Song.abl, and with this
 * nailed to -1 the fixture could only ever exercise the blind half — which is
 * exactly why the whole ladder went unpinned. -1 = nothing appeared. */
static int fake_new_slot[CLIP_TRACKS] = { -1, -1, -1, -1 };
int shadow_clip_new_slot(int track) {
    if (track < 0 || track >= CLIP_TRACKS) return -1;
    return fake_new_slot[track];
}

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); \
                   printf("\n      (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* ------------------------------------------------------------------ fakes */
/* The two tables the resolver reads, plus the transport. Both are plain
 * structs in production too, so a fake table is the real interface and not a
 * stand-in for one. */
static clip_state_t   fake_state;
static clip_regions_t fake_regions;
static int            fake_state_present = 1;
static int            fake_regions_present = 1;

const clip_state_t *clip_state_current(void) {
    return fake_state_present ? &fake_state : NULL;
}
const clip_regions_t *shadow_clip_regions(void) {
    return fake_regions_present ? &fake_regions : NULL;
}

/* ------------------------------------------------------------------ stubs */
/* Everything else shadow_chain_mgmt.c pulls in. Same set as the stubs.c
 * heredoc in tests/host/test_master_fx_permute.sh. */
/* The pulse counter the resolver reads. Definition, not a stub: it is a plain
 * int in production too (shadow_sampler.h). */
int shadow_transport_pulses = 0;

char sampler_current_set_name[128];
char sampler_current_set_uuid[64];

_Atomic int schwung_trace_on = 0;
uint32_t schwung_trace_intern(const char *name) { (void)name; return 0; }
uint64_t schwung_trace_now_ns(void) { return 0; }
void schwung_trace_span_explicit(uint32_t a, uint64_t b, uint64_t c,
                                 uint64_t d, uint64_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
}

int set_page_current = 0;
int set_page_read_persisted(void) { return 0; }
void shadow_batch_migrate_sets(void) {}
int shadow_load_config_from_dir(const char *dir) { (void)dir; return 0; }
void shadow_save_state(void) {}
int shadow_chain_midi_inject(const uint8_t *msg, int len) {
    (void)msg; (void)len; return 0;
}
void unified_log(const char *source, int level, const char *fmt, ...) {
    (void)source; (void)level; (void)fmt;
}

/* --------------------------------------------------------------- scenarios */

/* A track playing slot `cslot`, anchored `pulses_in` pulses ago. */
static void set_track(int track, int identity, int cslot, int anchored,
                      uint32_t anchor_pulse) {
    clip_track_state_t *tr = &fake_state.tracks[track];
    memset(tr, 0, sizeof(*tr));
    tr->identity_valid = identity;
    tr->clip_slot = cslot;
    tr->anchor_valid = anchored;
    tr->anchor_pulse = anchor_pulse;
    tr->anchor_source = anchored ? CLIP_ANCHOR_START : CLIP_ANCHOR_NONE;
}

static void set_region(int track, int cslot, double loop_start,
                       double loop_len) {
    clip_region_t *r = &fake_regions.slots[track][cslot];
    memset(r, 0, sizeof(*r));
    r->exists = 1;
    r->loop_start = loop_start;
    r->loop_len = loop_len;
    /* The ABSENT content fingerprint, which is what the parser writes into a
     * clip with no notes -- not the zero memset leaves. Note 0 is a real note
     * number, so the default here has to be the one the parser would produce. */
    r->note_count = 0;
    r->first_note = -1;
}

/* A clip's CONTENT, set separately: most cases here only care about geometry,
 * and the two halves of a fingerprint answer different questions. */
static void set_region_notes(int track, int cslot, int note_count,
                             int first_note) {
    clip_region_t *r = &fake_regions.slots[track][cslot];
    r->note_count = note_count;
    r->first_note = first_note;
}

static void reset_world(void) {
    memset(&fake_state, 0, sizeof(fake_state));
    memset(&fake_regions, 0, sizeof(fake_regions));
    fake_state_present = 1;
    fake_regions_present = 1;
    fake_regions.valid = 1;
    fake_regions.step_resolution = 0.25;
    for (int t = 0; t < CLIP_TRACKS; t++) fake_state.tracks[t].clip_slot = -1;
    shadow_transport_pulses = 0;
}

/* The call, with the outputs pre-poisoned to values nothing under test could
 * legitimately produce -- so "left untouched" is distinguishable from "written
 * with the right answer", which is what Finding 3 was about. */
#define POISON 424242.0

/* Paint an N-bar strip for `track` through the REAL decoder and its two-frame
 * confirmation, the same way the saved-clip case above does. */
static void paint_strip(int track, int bars) {
    static uint8_t fb[1024];
    memset(fb, 0, sizeof(fb));
    const int X0 = 1, X1 = 126;
    int usable = (X1 - X0 + 1) - 2 * (bars - 1);
    int base = usable / bars, rem = usable % bars, x = X0;
    for (int i = 0; i < bars; i++) {
        int w = base + (i < rem ? 1 : 0);
        for (int c = x; c < x + w; c++) {
            fb[(59 / 8) * 128 + c] |= (uint8_t)(1u << (59 % 8));
            if (i == 0) {
                fb[(58 / 8) * 128 + c] |= (uint8_t)(1u << (58 % 8));
                fb[(60 / 8) * 128 + c] |= (uint8_t)(1u << (60 % 8));
            }
        }
        x += w + 2;
    }
    step_strip_reset();
    step_strip_observe(fb, track);
    step_strip_observe(fb, track);
}

static int call(int slot, double *ph, double *len, int *cs, int *fpv,
                double *fp) {
    *ph = POISON;
    *len = POISON;
    *cs = -99;
    *fpv = -99;
    for (int i = 0; i < 4; i++) fp[i] = POISON;
    return shadow_slot_clip_phase(slot, ph, len, cs, fpv, fp);
}

int main(void) {
    double ph, len, fp[4];
    int cs, fpv, rc;

    /* 1. NO IDENTITY. Nothing is known, so the answer is "unknown" and the
     *    identity outputs say so explicitly rather than being left as the
     *    caller found them. */
    reset_world();
    set_track(0, 0, -1, 0, 0);
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0, "no identity returned %d, expected 0 (phase UNKNOWN)", rc);
    CHECK(cs == -1, "no identity left *clip_slot = %d, expected -1", cs);
    CHECK(fpv == 0, "no identity left *fp_valid = %d, expected 0", fpv);
    /* Finding 3: the phase outputs must not survive a failure. A caller that
     * hoists these buffers out of its loop would otherwise get the PREVIOUS
     * block's phase for an unknown block -- a live wrong phase, not a
     * harmless zero. */
    CHECK(!isfinite(ph), "failure left *phase_beats = %f (stale or a guess)", ph);
    CHECK(!isfinite(len), "failure left *loop_len = %f (stale or a guess)", len);

    /* Same, one layer up: no table at all. */
    reset_world();
    fake_state_present = 0;
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0 && cs == -1 && fpv == 0 && !isfinite(ph) && !isfinite(len),
          "a missing clip_state table did not read as fully unknown "
          "(rc=%d cs=%d fpv=%d ph=%f len=%f)", rc, cs, fpv, ph, len);

    /* 2. IDENTITY BUT NO ANCHOR -- the criterion clip_state.h is emphatic
     *    about, and the one the old line-order grep could only approximate.
     *    Entering Session view refreshes the grid: identity arrives with no
     *    anchor. A lane needs to know it is bound to the right clip WHILE it
     *    waits for a phase, so identity and the fingerprint must be published
     *    even though the return is 0. Collapsing the two is how a lane binds
     *    to the right clip at the wrong phase. */
    reset_world();
    set_track(2, 1, 5, 0, 0);
    set_region(2, 5, 8.0, 4.0);
    rc = call(2, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0, "an unanchored track returned %d, expected 0", rc);
    CHECK(cs == 5, "an unanchored track did not publish its clip slot (%d)", cs);
    CHECK(fpv == 1, "an unanchored track did not publish a fingerprint");
    CHECK(fp[0] == 8.0 && fp[1] == 4.0,
          "fingerprint geometry is %f/%f, expected 8/4", fp[0], fp[1]);
    /* THE CONTENT HALF REACHES THE CHAIN, and it is the clip's own numbers.
     * The seam shipped with these two hard-coded to 0 / -1 as a placeholder --
     * which makes every clip at loop_start 0.0 fingerprint identically, so a
     * lane binds to the wrong clip and plays. A test that only checked
     * fp[0]/fp[1] was green throughout that. */
    CHECK(fp[2] == 0.0 && fp[3] == -1.0,
          "a note-free clip published %f/%f, expected 0/-1 (absent, not note 0)",
          fp[2], fp[3]);
    set_region_notes(2, 5, 14, 41);
    rc = call(2, &ph, &len, &cs, &fpv, fp);
    CHECK(fp[2] == 14.0 && fp[3] == 41.0,
          "the clip's note data did not reach the fingerprint: %f/%f, "
          "expected 14/41 -- still the placeholder?", fp[2], fp[3]);
    CHECK(rc == 0 && fpv == 1,
          "publishing note data changed the phase answer (rc=%d fpv=%d)",
          rc, fpv);
    CHECK(!isfinite(ph) && !isfinite(len),
          "an unanchored track produced a phase anyway (%f/%f)", ph, len);

    /* 3. ANCHOR + A LOOP THAT DOES NOT START AT 0, and the phase is CLIP
     *    TIME -- quarters from the clip's start, the coordinate Move's own
     *    notes are in.
     *
     *    THIS TEST PINNED THE OPPOSITE UNTIL 2026-09-12, and it was defending
     *    a defect: the resolver subtracted loop_start to hand the lane
     *    0..loop_len, so a sweep recorded one beat into a loop at 8..20 was
     *    stored as 1.0 rather than 9.0. Open that loop out to the whole clip
     *    and it replays at beat 1 -- two bars early, on the wrong notes. A
     *    step p-lock has the same problem in reverse: "bar 3, step 5" cannot
     *    be turned into a loop-relative phase without knowing where the loop
     *    begins.
     *
     *    Measured in Move's own file: a clip whose region/loop is 8..20
     *    carries notes at startTime 0.0, 9.5, 16.5. The note at 0.0 is
     *    outside the loop and does not play -- absolute clip time, with the
     *    loop as a window over it. loop_start travels separately (fp[0]) so
     *    the lane knows which part of itself is audible.
     *
     *    Checked across a whole loop so an off-by-loop_start cannot hide in
     *    one sample. */
    reset_world();
    set_region(1, 3, 8.0, 4.0);
    for (int p = 0; p < 4 * 24; p += 7) {
        set_track(1, 1, 3, 1, 0);
        shadow_transport_pulses = p;
        rc = call(1, &ph, &len, &cs, &fpv, fp);
        double want = 8.0 + (double)p / 24.0;
        CHECK(rc == 1, "anchored track at pulse %d returned %d, expected 1",
              p, rc);
        CHECK(ph >= 8.0 && ph < 12.0,
              "phase %f at pulse %d is outside the clip window [8,12) -- "
              "loop_start was subtracted off, which is loop time, not clip "
              "time", ph, p);
        CHECK(fabs(ph - want) < 1e-9,
              "phase %f at pulse %d, expected %f", ph, p, want);
        CHECK(len == 4.0, "loop length is %f, expected 4", len);
        CHECK(fp[0] == 8.0,
              "the window's start did not travel with the phase: fp[0]=%f, "
              "expected 8 -- the lane cannot tell which part of itself is "
              "audible without it", fp[0]);
        CHECK(cs == 3 && fpv == 1,
              "a known phase lost its identity (cs=%d fpv=%d)", cs, fpv);
    }
    /* And it WRAPS at the window's end rather than running on -- back to the
     * window's START (8.0), not to zero. */
    set_track(1, 1, 3, 1, 0);
    shadow_transport_pulses = 5 * 24;        /* 5 beats into a 4-beat loop */
    rc = call(1, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 1 && fabs(ph - 9.0) < 1e-9,
          "phase past the loop end is %f, expected 9.0 (wrapped to the "
          "window's start, not to 0)", ph);

    /* 4. AN OUT-OF-RANGE CLIP SLOT MUST NOT INDEX THE REGIONS TABLE.
     *    clip_track_state_t::clip_slot is -1 for "nothing playing", and a
     *    torn or stale table can carry CLIP_SLOTS. Either one indexes
     *    rg->slots[slot][cslot] out of bounds. */
    reset_world();
    set_track(0, 1, CLIP_SLOTS, 1, 0);
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0, "clip_slot == CLIP_SLOTS returned %d, expected 0", rc);
    CHECK(cs == -1 && fpv == 0,
          "an out-of-range clip slot was published (cs=%d fpv=%d)", cs, fpv);

    reset_world();
    set_track(0, 1, -1, 1, 0);
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0, "clip_slot == -1 returned %d, expected 0", rc);
    CHECK(cs == -1 && fpv == 0,
          "\"nothing playing\" was published as a clip (cs=%d fpv=%d)",
          cs, fpv);

    /* The slot index itself, same reasoning from the other end. */
    reset_world();
    set_track(0, 1, 0, 1, 0);
    set_region(0, 0, 0.0, 4.0);
    ph = len = POISON; cs = fpv = -99;
    CHECK(shadow_slot_clip_phase(-1, &ph, &len, &cs, &fpv, fp) == 0,
          "slot -1 was resolved");
    CHECK(shadow_slot_clip_phase(CLIP_TRACKS, &ph, &len, &cs, &fpv, fp) == 0,
          "slot CLIP_TRACKS was resolved");
    CHECK(shadow_slot_clip_phase(0, NULL, &len, &cs, &fpv, fp) == 0,
          "a NULL output was accepted");

    /* 5. A TORN READ OF THE REGIONS TABLE: loop_len is NaN. The answer must be
     *    UNKNOWN, never a NaN phase with rc == 1 -- a confidently-wrong answer
     *    is strictly worse than an absent one.
     *
     *    MEASURED, not assumed: this is guarded TWICE, and reverting the
     *    resolver's own guard to the NaN-blind `loop_len <= 0.0` leaves all 95
     *    checks here green, because clip_phase_beats() spells its own length
     *    check `!(loop_len > 0.0)` and refuses. So this case pins the PAIR, and
     *    cannot say which half fired; that the two sites agree on the spelling
     *    is a source fact and is pinned in test_lane_phase_seam.sh. The case
     *    stays because the backstop is one function away from being loosened,
     *    and then this is the only thing standing between a torn table read and
     *    a lane playing at a NaN phase. */
    reset_world();
    set_track(0, 1, 0, 1, 0);
    set_region(0, 0, 0.0, NAN);
    shadow_transport_pulses = 24;
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0, "a NaN loop_len returned %d, expected 0 (UNKNOWN)", rc);
    CHECK(!isfinite(ph),
          "a NaN loop_len produced phase %f and claimed it was known", ph);

    /* A zero and a negative length, for the same guard. */
    reset_world();
    set_track(0, 1, 0, 1, 0);
    set_region(0, 0, 0.0, 0.0);
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0, "a zero loop_len returned %d, expected 0", rc);
    reset_world();
    set_track(0, 1, 0, 1, 0);
    set_region(0, 0, 0.0, -4.0);
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0, "a negative loop_len returned %d, expected 0", rc);

    /* An unparsed regions table gives identity and no phase, not a phase
     * against garbage geometry. */
    reset_world();
    set_track(0, 1, 0, 1, 0);
    set_region(0, 0, 0.0, 4.0);
    fake_regions.valid = 0;
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0 && cs == 0 && fpv == 0 && !isfinite(ph),
          "an unparsed regions table did not read as identity-only "
          "(rc=%d cs=%d fpv=%d ph=%f)", rc, cs, fpv, ph);

    /* A clip that does not exist in the file: identity is known (the LEDs saw
     * it), the geometry is not. */
    reset_world();
    set_track(0, 1, 4, 1, 0);
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0 && cs == 4 && fpv == 0,
          "a clip absent from the regions table (rc=%d cs=%d fpv=%d)",
          rc, cs, fpv);

    /* ============ THE CLIP MOVE HAS NOT SAVED YET =====================
     *
     * Measured: Move writes a new clip to Song.abl about 10 s after it is
     * made. Before that there is no file entry, so no length, so no phase --
     * and recording automation on a clip you just made refused outright.
     *
     * Move's step editor draws the length on its bar strip, so the resolver
     * falls back to reading it (step_strip.h). Two things are then true and
     * both are reported honestly: the length is BAR RESOLUTION, and the origin
     * is ASSUMED ZERO because the strip does not show where a loop begins.
     * `fp_valid == 0 with a valid phase` is the signal for that -- a state
     * that cannot otherwise occur, since the fingerprint is filled in before
     * the anchor is even checked, and it needs no new argument on a dlsym'd
     * seam that cannot safely take one.
     *
     * The frame is DRAWN rather than poked through a test-only setter: it goes
     * through the real decoder and the real two-frame confirmation, so this
     * case fails if either stops working.
     */
    printf("\nthe clip Move has not saved yet: the strip supplies the length\n");
    {
        static uint8_t fb[1024];
        memset(fb, 0, sizeof(fb));
        const int X0 = 1, X1 = 126, bars = 4;
        int usable = (X1 - X0 + 1) - 2 * (bars - 1);
        int base = usable / bars, rem = usable % bars, x = X0;
        for (int i = 0; i < bars; i++) {
            int w = base + (i < rem ? 1 : 0);
            for (int c = x; c < x + w; c++) {
                fb[(59 / 8) * 128 + c] |= (uint8_t)(1u << (59 % 8));
                if (i == 0) {
                    fb[(58 / 8) * 128 + c] |= (uint8_t)(1u << (58 % 8));
                    fb[(60 / 8) * 128 + c] |= (uint8_t)(1u << (60 % 8));
                }
            }
            x += w + 2;
        }
        reset_world();
        step_strip_reset();
        /* Twice: the cache requires two consecutive agreeing frames, because a
         * frame assembled from six slices can straddle two screen updates. */
        step_strip_observe(fb, 2);
        step_strip_observe(fb, 2);
        CHECK(step_strip_segments_for_track(2) == 4,
              "premise: the strip should report 4 bars, got %d",
              step_strip_segments_for_track(2));

        set_track(2, 1, 5, 1, 0);                 /* playing a clip the file lacks */
        shadow_transport_pulses = 24;             /* one quarter in */
        rc = call(2, &ph, &len, &cs, &fpv, fp);
        CHECK(rc == 1, "a clip absent from the file produced no phase (rc=%d) "
              "-- the hole that refused to record on a new clip", rc);
        CHECK(cs == 5, "identity lost: cs=%d", cs);
        CHECK(fpv == 0,
              "fp_valid=%d -- a provisional answer must publish NO fingerprint, "
              "which is the chain's only signal that the origin is assumed", fpv);
        CHECK(fabs(len - 16.0) < 1e-9,
              "length from the strip is %f, expected 16 (4 bars x 4)", len);
        CHECK(fabs(ph - 1.0) < 1e-9,
              "phase %f one quarter in, expected 1.0 against an assumed "
              "origin of 0", ph);

        /* NO STRIP READING, NO ANSWER: a reading, not a guess. */
        step_strip_reset();
        set_track(2, 1, 5, 1, 0);
        rc = call(2, &ph, &len, &cs, &fpv, fp);
        CHECK(rc == 0 && !isfinite(ph) && !isfinite(len),
              "with no strip reading the resolver invented a phase (rc=%d "
              "ph=%f len=%f)", rc, ph, len);
        CHECK(cs == 5 && fpv == 0,
              "identity must survive the refusal (cs=%d fpv=%d)", cs, fpv);

        /* AND NO ANCHOR, NO ANSWER: a length is not a position, and phase 0
         * is a real one. */
        step_strip_observe(fb, 2);
        step_strip_observe(fb, 2);
        set_track(2, 1, 5, 0, 0);
        rc = call(2, &ph, &len, &cs, &fpv, fp);
        CHECK(rc == 0 && !isfinite(ph),
              "an unanchored track got a phase from the strip (rc=%d ph=%f)",
              rc, ph);

        /* THE FILE WINS THE MOMENT IT HAS THE CLIP: exact, with an identity,
         * against the strip's bar resolution and assumed origin. */
        set_region(2, 5, 8.0, 12.0);
        set_region_notes(2, 5, 3, 60);
        set_track(2, 1, 5, 1, 0);
        shadow_transport_pulses = 24;
        rc = call(2, &ph, &len, &cs, &fpv, fp);
        CHECK(rc == 1 && fpv == 1,
              "the file did not take over (rc=%d fpv=%d)", rc, fpv);
        CHECK(fabs(len - 12.0) < 1e-9 && fabs(ph - 9.0) < 1e-9,
              "the file's geometry was not used: len=%f ph=%f (want 12, 9)",
              len, ph);
        CHECK(fp[0] == 8.0 && fp[2] == 3.0 && fp[3] == 60.0,
              "the fingerprint is wrong: loop_start=%f notes=%f first=%f",
              fp[0], fp[2], fp[3]);
        step_strip_reset();
    }

    /* ---------------------------------------------------------------
     * A CLIP THAT HAS NEVER PLAYED STILL HAS AN IDENTITY.
     *
     * `identity_valid` is set by a ch-9 ON -- a clip PLAYING -- and the
     * decoder only runs in SESSION view, which is not where steps are
     * edited. So two everyday states carry no identity at all: a clip you
     * just made, and a clip playing since before you last looked at the
     * session grid. Both reported from the device: "no clip on this track"
     * while plainly looking at one, and "once I stopped and restarted it had
     * the clip" -- a restart being what re-emits the LEDs.
     *
     * The WRITE path had already resolved this the other way ("a p-lock edits
     * the clip on SCREEN, which is the SELECTED clip"), so the two halves of
     * one gesture disagreed and the refusing half won. */
    printf("identity falls back to the SELECTED clip when none has played\n");
    reset_world();
    set_track(0, 0, -1, 0, 0);                  /* nothing witnessed */
    set_region(0, 3, 0.0, 4.0);
    fake_regions.slots[0][3].is_playing = 1;    /* Move's own selection */
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(cs == 3, "expected the selected clip 3, got %d", cs);
    /* ...and the PHASE is still unknown, which is the honest answer: nothing
     * anchored it. Identity and anchor are separately valid. */
    CHECK(rc == 0, "a never-played clip must not claim a phase");

    /* A PLAYING CLIP STILL WINS. The fallback may not override a witnessed
     * identity, or a track playing clip 1 while clip 3 is selected would bind
     * its lanes to the wrong clip -- silently, which is the failure this
     * whole gate exists to prevent. */
    printf("a witnessed identity is never overridden by the selection\n");
    reset_world();
    set_track(0, 1, 1, 1, 0);
    set_region(0, 1, 0.0, 4.0);
    set_region(0, 3, 0.0, 4.0);
    fake_regions.slots[0][3].is_playing = 1;
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(cs == 1, "the playing clip 1 must win over the selected 3, got %d", cs);

    printf("\nthe selection ladder: a new clip on a POPULATED track\n");
    {
        /* This was the worst defect in the feature and nothing pinned it. The
         * ambiguity gate counts clips IN THE FILE, which cannot see a clip
         * made seconds ago — so one old clip plus one new one counted as
         * "unambiguous" and every p-lock was keyed to the OLD clip, silently.
         * Measured on hardware as four of five permutations writing to the
         * wrong clip. The fixture stubbed the two new signals to "nothing",
         * with a comment saying it kept the resolver on its pre-fix path. */
        reset_world();
        set_region(1, 2, 0.0, 4.0);  set_region_notes(1, 2, 4, 36);
        set_region(1, 5, 0.0, 4.0);  set_region_notes(1, 5, 4, 38);
        set_track(1, 0, -1, 0, 0);          /* no identity, nothing playing */
        paint_strip(1, 1);                  /* a clip IS being edited */

        /* An EMPTY slot is selected — a clip is being made. The row must be
         * the placeholder, never one of the two clips already in the file. */
        fake_state.tracks[1].selected_slot = CLIP_SEL_EMPTY;
        rc = call(1, &ph, &len, &cs, &fpv, fp);
        CHECK(cs == LANE_SLOT_PENDING,
              "an empty-slot selection resolved to row %d — a p-lock would "
              "land on an existing clip", cs);
        CHECK(fpv == 0, "the placeholder came with a fingerprint (fpv=%d)", fpv);

        /* A POSITIVELY NAMED ROW IS NOT TRUSTED EITHER: Move paints the same
         * colour on more than one pad, and naming a row put the contamination
         * back through another door — it answered slot 2 while the user was
         * elsewhere, the file had a clip at 2, and the lock landed on it. */
        fake_state.tracks[1].selected_slot = 2;
        rc = call(1, &ph, &len, &cs, &fpv, fp);
        CHECK(cs == LANE_SLOT_PENDING,
              "a decoded selection was used to NAME row %d", cs);

        /* AND THE BLIND STATE HAS TO END, or the pending lane never adopts —
         * which is how every permutation on a clean track failed. */
        set_region(1, 6, 0.0, 4.0);  set_region_notes(1, 6, 4, 40);
        fake_new_slot[1] = 6;
        rc = call(1, &ph, &len, &cs, &fpv, fp);
        CHECK(cs == 6, "the file caught up and the row is still %d", cs);
        /* IDENTITY YES, PHASE NO. Nothing is playing, so there is no anchor —
         * and rc == 0 with a valid fingerprint is exactly the tri-state this
         * resolver exists to keep: "we know WHICH clip, not WHERE in it".
         * Expecting rc == 1 here was the test being wrong, not the code. */
        CHECK(rc == 0 && fpv == 1,
              "the newly-named row came without an identity (rc=%d fpv=%d)",
              rc, fpv);
        fake_new_slot[1] = -1;

        /* NOTHING ON SCREEN: with no strip there is no clip being edited to
         * contradict the file, so its answer is used exactly as before. */
        step_strip_reset();
        reset_world();
        set_region(1, 3, 0.0, 4.0);  set_region_notes(1, 3, 4, 36);
        fake_regions.slots[1][3].is_playing = 1;
        set_track(1, 0, -1, 0, 0);
        fake_state.tracks[1].selected_slot = -1;
        rc = call(1, &ph, &len, &cs, &fpv, fp);
        CHECK(cs == 3,
              "with one clip and no strip the file's answer was dropped "
              "(cs=%d)", cs);
        step_strip_reset();
    }

    if (failures == 0) {
        printf("PASS: shadow_slot_clip_phase (%d checks)\n", checks);
        return 0;
    }
    printf("\n%d of %d check(s) failed\n", failures, checks);
    return 1;
}
