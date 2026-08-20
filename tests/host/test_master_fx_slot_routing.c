/*
 * Tests for master_fx_key.h — the routing that decides WHICH Master FX slot a
 * param write or an LFO target refers to.
 *
 * Three call sites in shadow_chain_mgmt.c share this, and all three used to
 * hand-enumerate the range instead. Their failure modes when the cap moves
 * past what the ladder spells out:
 *
 *   (a) shadow_direct_set_param (web set-ring): no match -> mfx_slot stays -1
 *       -> the write is dropped, silently.
 *   (b) the shadow_param handler: no match -> the else-branch assigns SLOT 0
 *       with the whole key as the param name. Not a no-op — a write into a
 *       DIFFERENT running module under a garbage key. This is the one that
 *       corrupts rather than ignores, so it gets the out-of-range cases below.
 *   (c) shadow_master_fx_lfo_tick: no match -> target_slot -1 -> `continue`
 *       -> the LFO just stops modulating.
 *
 * The cap comes in on the command line, read out of shadow_chain_mgmt.h by the
 * shell wrapper. It is not restated here: the point is that whatever the
 * shipped cap is, EVERY position within it routes and the first position past
 * it is rejected. If MASTER_FX_SLOTS rises to 8 or 16, this test widens
 * automatically instead of quietly covering half the range.
 */
#include <stdio.h>
#include <string.h>
#include "master_fx_key.h"

#ifndef TEST_MASTER_FX_SLOTS
#error "TEST_MASTER_FX_SLOTS must be defined by the test wrapper"
#endif

#define CAP TEST_MASTER_FX_SLOTS

static int failures = 0;
static int checks = 0;

static void fail(const char *what, const char *key) {
    fprintf(stderr, "FAIL [%s] key=\"%s\"\n", what, key ? key : "(null)");
    failures++;
}

/* Site (a) and (b) share this parse; the difference is what each does with a
 * miss, which is asserted separately below. */
static void expect_param_routes(const char *key, int want_slot, const char *want_param) {
    int slot = -12345;
    const char *param = NULL;
    checks++;
    if (!master_fx_route_param_key(key, CAP, &slot, &param)) {
        fail("expected a match, got none", key);
        return;
    }
    if (slot != want_slot) {
        fprintf(stderr, "FAIL \"%s\": slot %d, want %d\n", key, slot, want_slot);
        failures++;
    }
    if (!param || strcmp(param, want_param) != 0) {
        fprintf(stderr, "FAIL \"%s\": param \"%s\", want \"%s\"\n",
                key, param ? param : "(null)", want_param);
        failures++;
    }
}

static void expect_param_rejected(const char *key) {
    /* Pre-seed exactly as the callers do, and assert the outputs are left
     * alone — site (b) relies on being able to install its own fallback. */
    int slot = -1;
    const char *param = key;
    checks++;
    if (master_fx_route_param_key(key, CAP, &slot, &param)) {
        fail("expected NO match, got one", key);
        return;
    }
    if (slot != -1 || param != key) {
        fail("rejection clobbered the caller's outputs", key);
    }
}

static void expect_target(const char *target, int want_slot) {
    checks++;
    int got = master_fx_route_target(target, CAP);
    if (got != want_slot) {
        fprintf(stderr, "FAIL target \"%s\": slot %d, want %d\n",
                target ? target : "(null)", got, want_slot);
        failures++;
    }
}

int main(void) {
    char key[64];

    /* ---- Every slot the shipped cap actually allows ---- */
    for (int n = 1; n <= CAP; n++) {
        snprintf(key, sizeof(key), "fx%d:cutoff", n);
        expect_param_routes(key, n - 1, "cutoff");        /* sites (a) and (b) */

        snprintf(key, sizeof(key), "fx%d", n);
        expect_target(key, n - 1);                        /* site (c) */
    }

    /* The keys the shadow_param handler special-cases per slot. Same routing,
     * but these are the ones that load and bypass modules. */
    for (int n = 1; n <= CAP; n++) {
        snprintf(key, sizeof(key), "fx%d:module", n);
        expect_param_routes(key, n - 1, "module");
        snprintf(key, sizeof(key), "fx%d:bypassed", n);
        expect_param_routes(key, n - 1, "bypassed");
    }

    /* ---- The first slot PAST the cap must be rejected, not misrouted ----
     * This is the assertion that matters most. At site (b) a false match here
     * does not mean "dropped" — it means the write lands in slot 0. */
    {
        char over[64];
        snprintf(over, sizeof(over), "fx%d:cutoff", CAP + 1);
        expect_param_rejected(over);
        snprintf(over, sizeof(over), "fx%d:module", CAP + 1);
        expect_param_rejected(over);
        snprintf(over, sizeof(over), "fx%d", CAP + 1);
        expect_target(over, -1);

        /* And far past it, including the multi-digit case the old
         * single-character parse mis-read as slot 1. */
        expect_param_rejected("fx99:cutoff");
        expect_target("fx99", -1);
        expect_param_rejected("fx1000000000000:cutoff");
        expect_target("fx1000000000000", -1);
    }

    /* ---- Un-prefixed master_fx:* keys. These MUST NOT match: they are the
     * LFO and shim-special keys, and site (b) deliberately sends them to slot
     * 0 as whole-key param names. A match here would strip them. ---- */
    expect_param_rejected("lfo1:rate_hz");
    expect_param_rejected("lfo2:target");
    expect_param_rejected("resample_bridge");
    expect_param_rejected("usbc_out_persist");
    expect_param_rejected("jack:foo");
    expect_param_rejected("suspend_overtake");

    /* ---- Malformed indices. "fx0" is not an id we emit (keys are 1-based)
     * and leading zeros are rejected, matching chain_key_index.h. ---- */
    expect_param_rejected("fx0:cutoff");
    expect_param_rejected("fx01:cutoff");
    expect_param_rejected("fx:cutoff");
    expect_param_rejected("fxa:cutoff");
    expect_param_rejected("fx-1:cutoff");
    expect_target("fx0", -1);
    expect_target("fx01", -1);
    expect_target("fx", -1);

    /* ---- Shape errors. A param key needs the colon; a target must NOT have
     * one, and must not carry trailing junk — the old character-range test
     * accepted "fx1x" and "fx1:whatever" as slot 0. ---- */
    expect_param_rejected("fx1");
    expect_param_rejected("fx1cutoff");
    expect_target("fx1:cutoff", -1);
    expect_target("fx1x", -1);
    expect_target("lfo1", -1);
    expect_target("synth", -1);
    expect_target("", -1);
    expect_target(NULL, -1);

    /* An empty param after the colon still routes — the caller compares it
     * against its known keys and errors out. What matters is the SLOT. */
    expect_param_routes("fx1:", 0, "");

    /* The prefix must match from the start; a substring must not count. */
    expect_param_rejected("midi_fx1:cutoff");
    expect_param_rejected("slot_fx1:cutoff");
    expect_param_rejected("xfx1:cutoff");
    expect_param_rejected("master_fx:fx1:cutoff");
    expect_param_rejected("");
    expect_param_rejected(NULL);

    if (failures) {
        fprintf(stderr, "%d/%d case(s) FAILED\n", failures, checks);
        return 1;
    }
    printf("test_master_fx_slot_routing: %d cases pass (MASTER_FX_SLOTS = %d)\n",
           checks, CAP);
    return 0;
}
