/*
 * Unit test for shadow_me_post_snapshot_fx_active() — "has anything been added
 * to the ME bus since native_bridge_me_component was snapshotted?"
 *
 * The native resample bridge reconstructs a mix from that snapshot plus the
 * Move component. Master FX has always been excluded from the snapshot and the
 * bridge knew it; the SEND RETURNS land at the same point and were not covered,
 * so a reverb in Send A with its return up produced a resample that quietly
 * differed from the DAC and from the sampler/skipback captures.
 *
 * The interesting case is therefore "a send is active and Master FX is not":
 * a predicate that only asked about Master FX passes every other assertion
 * here.
 *
 * Bounds come from the shipped header (SEND_BUSES / SEND_FX_SLOTS /
 * MASTER_FX_SLOTS), so raising a cap widens the coverage rather than leaving
 * half of it untested.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "shadow_chain_mgmt.h"

/* The globals the inline predicates read. Defined here so the test links
 * without the shim. */
master_fx_slot_t shadow_master_fx_slots[MASTER_FX_SLOTS];
master_fx_slot_t shadow_send_fx_slots[SEND_BUSES][SEND_FX_SLOTS];
volatile int shadow_send_return_level[SEND_BUSES];
volatile int shadow_send_a_to_b;

static void dummy_process(void *instance, int16_t *buf, int frames) {
    (void)instance; (void)buf; (void)frames;
}
static audio_fx_api_v2_t dummy_api;
static int dummy_instance;

static void reset_all(void) {
    memset(shadow_master_fx_slots, 0, sizeof(shadow_master_fx_slots));
    memset(shadow_send_fx_slots, 0, sizeof(shadow_send_fx_slots));
    for (int sb = 0; sb < SEND_BUSES; sb++) shadow_send_return_level[sb] = 0;
    shadow_send_a_to_b = 0;
}

static void load(master_fx_slot_t *s) {
    s->instance = &dummy_instance;
    s->api = &dummy_api;
}

static void test_idle(void) {
    reset_all();
    assert(shadow_master_fx_chain_active() == 0);
    assert(shadow_me_post_snapshot_fx_active() == 0);
    printf("  idle: ok\n");
}

static void test_master_fx_positions(void) {
    for (int fx = 0; fx < MASTER_FX_SLOTS; fx++) {
        reset_all();
        load(&shadow_master_fx_slots[fx]);
        assert(shadow_master_fx_chain_active() == 1);
        assert(shadow_me_post_snapshot_fx_active() == 1);
    }
    printf("  master fx, every position: ok\n");
}

/* THE REGRESSION. Master FX is idle in every one of these, so
 * shadow_master_fx_chain_active() answers 0 and the bridge would take the
 * reconstruction path with the return missing from it. */
static void test_send_return_level_alone(void) {
    for (int sb = 0; sb < SEND_BUSES; sb++) {
        reset_all();
        shadow_send_return_level[sb] = 1;
        assert(shadow_master_fx_chain_active() == 0);
        assert(shadow_send_bus_active(sb) == 1);
        assert(shadow_me_post_snapshot_fx_active() == 1);
    }
    printf("  send return level alone, every bus: ok\n");
}

static void test_send_fx_loaded_alone(void) {
    for (int sb = 0; sb < SEND_BUSES; sb++) {
        for (int fx = 0; fx < SEND_FX_SLOTS; fx++) {
            reset_all();
            load(&shadow_send_fx_slots[sb][fx]);
            assert(shadow_master_fx_chain_active() == 0);
            assert(shadow_me_post_snapshot_fx_active() == 1);
        }
    }
    printf("  send fx loaded alone, every bus and position: ok\n");
}

/* A loaded position with no process_block cannot add anything, and a bus that
 * is only the target of A->B still needs its own reason to be active — the
 * shim's send loop skips an inactive bus entirely, so nothing of it reaches
 * fx_target and the snapshot is still the whole ME bus. */
static void test_inert_states(void) {
    reset_all();
    shadow_master_fx_slots[0].instance = &dummy_instance;  /* no api */
    assert(shadow_me_post_snapshot_fx_active() == 0);

    reset_all();
    shadow_send_fx_slots[0][0].instance = &dummy_instance;  /* no api */
    assert(shadow_me_post_snapshot_fx_active() == 0);

    reset_all();
    shadow_send_a_to_b = 127;
    assert(shadow_me_post_snapshot_fx_active() == 0);
    printf("  inert states: ok\n");
}

int main(void) {
    dummy_api.process_block = dummy_process;

    printf("test_me_post_snapshot_fx:\n");
    test_idle();
    test_master_fx_positions();
    test_send_return_level_alone();
    test_send_fx_loaded_alone();
    test_inert_states();
    printf("PASS\n");
    return 0;
}
