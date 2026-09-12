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
    CHECK(!isfinite(ph) && !isfinite(len),
          "an unanchored track produced a phase anyway (%f/%f)", ph, len);

    /* 3. ANCHOR + A LOOP THAT DOES NOT START AT 0. This is the rebasing
     *    proof. loop_start = 8, loop_len = 4: clip_phase_beats() returns
     *    8..12, and the resolver subtracts loop_start to give 0..4, which is
     *    the only thing a lane can index a breakpoint list with. Checked
     *    across a whole loop so an off-by-loop_start cannot hide in one
     *    sample. */
    reset_world();
    set_region(1, 3, 8.0, 4.0);
    for (int p = 0; p < 4 * 24; p += 7) {
        set_track(1, 1, 3, 1, 0);
        shadow_transport_pulses = p;
        rc = call(1, &ph, &len, &cs, &fpv, fp);
        double want = (double)p / 24.0;
        CHECK(rc == 1, "anchored track at pulse %d returned %d, expected 1",
              p, rc);
        CHECK(ph >= 0.0 && ph < 4.0,
              "phase %f at pulse %d is outside [0,4) -- loop_start (8.0) was "
              "not subtracted back off", ph, p);
        CHECK(fabs(ph - want) < 1e-9,
              "phase %f at pulse %d, expected %f", ph, p, want);
        CHECK(len == 4.0, "loop length is %f, expected 4", len);
        CHECK(cs == 3 && fpv == 1,
              "a known phase lost its identity (cs=%d fpv=%d)", cs, fpv);
    }
    /* And it WRAPS at loop_len rather than running on. */
    set_track(1, 1, 3, 1, 0);
    shadow_transport_pulses = 5 * 24;        /* 5 beats into a 4-beat loop */
    rc = call(1, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 1 && fabs(ph - 1.0) < 1e-9,
          "phase past the loop end is %f, expected 1.0 (wrapped)", ph);

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

    if (failures == 0) {
        printf("PASS: shadow_slot_clip_phase (%d checks)\n", checks);
        return 0;
    }
    printf("\n%d of %d check(s) failed\n", failures, checks);
    return 1;
}
