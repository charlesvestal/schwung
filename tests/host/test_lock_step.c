/* Parameter-lock step derivation and lane bookkeeping.
 *
 * The step index is the whole feature's correctness: a lock that lands one step
 * early is indistinguishable from a lock that was never saved, and neither
 * shows up as an error. So the arithmetic is tested standalone, the way
 * lfo_synced_phase is, rather than inferred from something audible. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "host/lock_common.h"

static void fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }

static void eq(int got, int want, const char *m) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %d want %d)\n", m, got, want);
        exit(1);
    }
}

/* Find a division by label so the test does not hardcode table order — the
 * same reason test_lfo_synced_phase looks its divisions up by beats. */
static int div_by_label(const char *label) {
    for (int i = 0; i < LFO_NUM_DIVISIONS; i++)
        if (strcmp(lfo_divisions[i].label, label) == 0) return i;
    fail("division label not found in table");
    return -1;
}

int main(void) {
    /* LOCK_DEFAULT_RATE_DIV is an index into a table that has already been
     * renumbered once (lfo_division_migrate_14_to_27). Assert what it points
     * at, so a future edit to the table fails here instead of silently moving
     * every lock to a different rate. */
    eq(LOCK_DEFAULT_RATE_DIV, div_by_label("1/16"),
       "LOCK_DEFAULT_RATE_DIV must name 1/16");

    const int s16 = div_by_label("1/16");
    const int s4  = div_by_label("1/4");

    /* 1/16 steps, 16-step pattern: four steps per beat. */
    eq(lock_step_at(0.0,    s16, 16), 0,  "beat 0 -> step 0");
    eq(lock_step_at(0.24,   s16, 16), 0,  "inside step 0");
    eq(lock_step_at(0.25,   s16, 16), 1,  "quarter beat -> step 1");
    eq(lock_step_at(2.0,    s16, 16), 8,  "beat 2 -> step 8");
    eq(lock_step_at(2.25,   s16, 16), 9,  "beat 2.25 -> step 9");
    eq(lock_step_at(3.75,   s16, 16), 15, "last step of the bar");

    /* Wrap: step 16 is step 0 of the next pass, not step 16. */
    eq(lock_step_at(4.0,    s16, 16), 0,  "pattern wraps at 16");
    eq(lock_step_at(6.25,   s16, 16), 9,  "step 9 again one bar later");

    /* No drift. A pure function of song position holds at bar 400 exactly as
     * it does at bar 1 — this is the property that makes the LFO's approach
     * worth copying, so it is asserted rather than assumed. */
    eq(lock_step_at(1600.0 + 2.25, s16, 16), 9, "step 9 at bar 401");
    eq(lock_step_at(40000.0 + 2.25, s16, 16), 9, "step 9 ten thousand bars in");

    /* A 32-step clip is the case the "assume 16" shortcut gets wrong. */
    eq(lock_step_at(4.0,  s16, 32), 16, "32-step pattern reaches step 16");
    eq(lock_step_at(8.0,  s16, 32), 0,  "32-step pattern wraps at 32");

    /* A different step rate moves every step, which is why rate is a setting. */
    eq(lock_step_at(2.0, s4, 16), 2, "1/4 steps -> beat 2 is step 2");

    /* Stopped transport: get_beat_position answers < 0, and every lock must
     * stand down rather than latch its last value. */
    eq(lock_step_at(-1.0, s16, 16), LOCK_STEP_NONE, "no transport -> no step");

    /* Out-of-range inputs clamp, never crash or index out of the table. */
    (void)lock_step_at(1.0, -5, 16);
    (void)lock_step_at(1.0, LFO_NUM_DIVISIONS + 5, 16);
    eq(lock_step_at(1.0, s16, 0), 0, "pattern_len 0 clamps to 1 step");
    if (lock_step_at(1.0, s16, LOCK_MAX_STEPS + 99) >= LOCK_MAX_STEPS)
        fail("pattern_len above LOCK_MAX_STEPS must clamp");

    /* ---- lane bookkeeping ---- */
    lock_state_t st;
    lock_state_init(&st);
    eq(st.pattern_len, LOCK_DEFAULT_STEPS, "init sets a usable pattern length");
    eq(st.cur_step, LOCK_STEP_NONE, "init parks the playhead");
    eq(st.lane_count, 0, "init has no lanes");

    lock_lane_t *a = lock_alloc_lane(&st, "synth", "sd_c_snappy");
    if (!a) fail("first lane must allocate");
    eq(st.lane_count, 1, "lane_count tracks allocation");
    if (lock_alloc_lane(&st, "synth", "sd_c_snappy") != a)
        fail("same target:param must reuse its lane");
    eq(st.lane_count, 1, "reuse does not grow lane_count");

    lock_lane_set(a, 9, 0.2f);
    if (!lock_lane_has_step(a, 9)) fail("step 9 should hold a lock");
    if (lock_lane_has_step(a, 8)) fail("step 8 should not");
    if (fabsf(a->values[9] - 0.2f) > 1e-6f) fail("locked value not stored");

    /* Steps beyond the current pattern length are kept, not discarded:
     * shortening a clip and lengthening it again must not eat locks. */
    lock_lane_set(a, 40, 0.9f);
    if (!lock_lane_has_step(a, 40)) fail("step 40 should hold a lock");

    lock_lane_clear(a, 9);
    if (lock_lane_has_step(a, 9)) fail("cleared step still reads as locked");
    lock_retire_lane_if_empty(&st, a);
    eq(st.lane_count, 1, "lane with locks left must survive retirement");

    lock_lane_clear(a, 40);
    lock_retire_lane_if_empty(&st, a);
    eq(st.lane_count, 0, "emptied lane is retired and its slot returned");

    /* Exhaustion is bounded and reported by a NULL, never by overrunning. */
    char pname[32];
    for (int i = 0; i < LOCK_MAX_LANES; i++) {
        snprintf(pname, sizeof(pname), "p%d", i);
        if (!lock_alloc_lane(&st, "synth", pname)) fail("lane should allocate");
    }
    if (lock_alloc_lane(&st, "synth", "one_too_many"))
        fail("allocation past LOCK_MAX_LANES must fail, not overrun");

    /* Source ids must be distinct per lane, or one lane's clear silences
     * another's lock. */
    char id_a[8], id_b[8];
    lock_source_id(0, id_a, sizeof(id_a));
    lock_source_id(15, id_b, sizeof(id_b));
    if (strcmp(id_a, "plk0") != 0) fail("lane 0 source id");
    if (strcmp(id_b, "plk15") != 0) fail("lane 15 source id");

    /* ---- JSON round trip ----
     * The writer and the reader are the only things standing between a saved
     * lock and a lost one, so the trip is asserted rather than eyeballed. */
    lock_state_t w;
    lock_state_init(&w);
    w.pattern_len = 32;
    w.rate_div = div_by_label("1/8");
    w.enabled = 1;

    lock_lane_t *l1 = lock_alloc_lane(&w, "synth", "sd_c_snappy");
    lock_lane_set(l1, 9, 0.25f);
    lock_lane_set(l1, 13, 0.75f);
    lock_lane_t *l2 = lock_alloc_lane(&w, "fx1", "mix");
    lock_lane_set(l2, 0, 1.0f);
    lock_lane_set(l2, 31, 0.5f);

    /* An empty lane must not reach the file — it would occupy a slot and a
     * modulation entry on every future load for no locks at all. */
    (void)lock_alloc_lane(&w, "fx2", "never_locked");

    char json[8192];
    const int written = lock_to_json(&w, json, (int)sizeof(json));
    if (written <= 0 || written >= (int)sizeof(json)) fail("serialise produced nothing");
    if (strstr(json, "never_locked")) fail("an empty lane must not be serialised");

    lock_state_t r;
    lock_from_json(&r, json, NULL);

    eq(r.pattern_len, 32, "pattern_len survives the round trip");
    eq(r.rate_div, div_by_label("1/8"), "rate_div survives the round trip");
    eq(r.enabled, 1, "enabled survives the round trip");
    eq(r.lane_count, 2, "exactly the two non-empty lanes come back");

    lock_lane_t *g1 = lock_find_lane(&r, "synth", "sd_c_snappy");
    if (!g1) fail("synth lane did not survive");
    if (!lock_lane_has_step(g1, 9) || !lock_lane_has_step(g1, 13))
        fail("locked steps did not survive");
    if (lock_lane_has_step(g1, 10)) fail("an unlocked step came back locked");
    if (fabsf(g1->values[9] - 0.25f) > 1e-5f) fail("step 9 value drifted");
    if (fabsf(g1->values[13] - 0.75f) > 1e-5f) fail("step 13 value drifted");

    lock_lane_t *g2 = lock_find_lane(&r, "fx1", "mix");
    if (!g2) fail("fx1 lane did not survive");
    if (!lock_lane_has_step(g2, 0) || !lock_lane_has_step(g2, 31))
        fail("edge steps did not survive");

    /* A patch with no locks key: defaults, not zeroes, and never the previous
     * patch's lanes. */
    lock_state_t none;
    lock_state_init(&none);
    (void)lock_alloc_lane(&none, "synth", "stale");
    lock_from_json(&none, NULL, NULL);
    eq(none.lane_count, 0, "absent locks clear stale lanes");
    eq(none.pattern_len, LOCK_DEFAULT_STEPS, "absent locks restore the default length");

    /* Garbage must not crash or invent lanes. */
    lock_from_json(&r, "{\"lanes\":[{\"target\":\"synth\"}]}", NULL);
    eq(r.lane_count, 0, "a lane with no param is not created");
    lock_from_json(&r, "not json at all", NULL);
    eq(r.lane_count, 0, "unparseable input yields no lanes");
    lock_from_json(&r, "{\"lanes\":[", NULL);
    eq(r.lane_count, 0, "truncated input yields no lanes");

    /* Out-of-range steps in a hand-edited patch are dropped, not written past
     * the end of the value array. */
    lock_from_json(&r, "{\"lanes\":[{\"target\":\"synth\",\"param\":\"p\","
                       "\"steps\":[[999,0.5],[4,0.6]]}]}", NULL);
    lock_lane_t *gz = lock_find_lane(&r, "synth", "p");
    if (!gz) fail("lane with one valid step should exist");
    if (!lock_lane_has_step(gz, 4)) fail("valid step alongside an invalid one was dropped");

    printf("PASS: test_lock_step\n");
    return 0;
}
