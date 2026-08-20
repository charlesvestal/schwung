/*
 * Tests for chain_key_index.h — the parser every "fx<N>:" / "midi_fx<N>:"
 * parameter now routes through.
 *
 * chain_host.c used to carry one hand-written branch per FX slot; they are one
 * indexed branch each now, so this function alone decides which FX slot a
 * parameter lands on. An off-by-one here does not fail to compile and does not
 * fail the source-pin test either — it just quietly sends a cutoff sweep to
 * the wrong effect. Hence a test that actually runs it.
 */
#include <stdio.h>
#include <string.h>
#include "chain_key_index.h"

/*
 * The caps come in on the command line, read out of chain_internal.h by the
 * shell wrapper, so these cases track the shipped limits instead of a copy of
 * them that could drift. Named TEST_* so nothing here can shadow the real
 * MAX_AUDIO_FX / MAX_MIDI_FX.
 */
#ifndef TEST_MAX_FX
#error "TEST_MAX_FX must be defined by the test wrapper"
#endif
#ifndef TEST_MAX_MIDI_FX
#error "TEST_MAX_MIDI_FX must be defined by the test wrapper"
#endif

static int failures = 0;

static void expect_idx(const char *key, const char *prefix, int max, int want) {
    int got = chain_fx_index_from_key(key, prefix, max, NULL);
    if (got != want) {
        fprintf(stderr, "FAIL %s (prefix \"%s\", max %d): got %d, want %d\n",
                key, prefix, max, got, want);
        failures++;
    }
}

static void expect_subkey(const char *key, const char *prefix, int max,
                          int want_idx, const char *want_sub) {
    const char *sub = NULL;
    int got = chain_fx_index_from_key(key, prefix, max, &sub);
    if (got != want_idx) {
        fprintf(stderr, "FAIL %s: got index %d, want %d\n", key, got, want_idx);
        failures++;
        return;
    }
    if (!sub || strcmp(sub, want_sub) != 0) {
        fprintf(stderr, "FAIL %s: subkey is \"%s\", want \"%s\"\n",
                key, sub ? sub : "(null)", want_sub);
        failures++;
    }
}

int main(void) {
    /* The two indices that existed before this was indexed. They must land
     * exactly where the old fx1:/fx2: branches put them. */
    expect_idx("fx1:cutoff", "fx", TEST_MAX_FX, 0);
    expect_idx("fx2:cutoff", "fx", TEST_MAX_FX, 1);

    /* The top of the range and the first key past it, both BUILT from the cap
     * so a future cap bump does not turn this into "got 8, want -1" -- which
     * reads as a parser bug and is really just a stale test. */
    {
        char key[32];
        snprintf(key, sizeof(key), "fx%d:cutoff", TEST_MAX_FX);
        expect_idx(key, "fx", TEST_MAX_FX, TEST_MAX_FX - 1);
        snprintf(key, sizeof(key), "fx%d:cutoff", TEST_MAX_FX + 1);
        expect_idx(key, "fx", TEST_MAX_FX, -1);

        snprintf(key, sizeof(key), "midi_fx%d:rate", TEST_MAX_MIDI_FX);
        expect_idx(key, "midi_fx", TEST_MAX_MIDI_FX, TEST_MAX_MIDI_FX - 1);
        snprintf(key, sizeof(key), "midi_fx%d:rate", TEST_MAX_MIDI_FX + 1);
        expect_idx(key, "midi_fx", TEST_MAX_MIDI_FX, -1);
    }

    /* There is no slot zero, and we never emit a padded index — accepting
     * "fx01:" would make two spellings of one slot. */
    expect_idx("fx0:cutoff", "fx", TEST_MAX_FX, -1);
    expect_idx("fx01:cutoff", "fx", TEST_MAX_FX, -1);

    /* Not an index at all. */
    expect_idx("fxa:cutoff", "fx", TEST_MAX_FX, -1);
    expect_idx("fx1x:cutoff", "fx", TEST_MAX_FX, -1);
    expect_idx("fx:cutoff", "fx", TEST_MAX_FX, -1);
    expect_idx("fx", "fx", TEST_MAX_FX, -1);

    /* No colon means no subkey, so there is nothing to route. */
    expect_idx("fx1", "fx", TEST_MAX_FX, -1);
    expect_idx("fx1cutoff", "fx", TEST_MAX_FX, -1);

    /* The prefixes must not collide in either direction. strncmp is anchored,
     * so "midi_fx1:" is not an "fx" key even though "fx1:" is inside it. */
    expect_idx("midi_fx1:rate", "fx", TEST_MAX_FX, -1);
    expect_idx("midi_fx3:rate", "midi_fx", TEST_MAX_MIDI_FX, 2);
    expect_idx("fx1:rate", "midi_fx", TEST_MAX_MIDI_FX, -1);

    /* Unrelated keys the router sees on the same call. */
    expect_idx("synth:cutoff", "fx", TEST_MAX_FX, -1);
    expect_idx("fx1_module", "fx", TEST_MAX_FX, -1);

    /* A hand-edited patch under /data/UserData can hold anything. An
     * unbounded digit accumulate would be signed overflow (UB) here. */
    expect_idx("fx99999999999999:cutoff", "fx", TEST_MAX_FX, -1);
    expect_idx("fx2147483648:cutoff", "fx", TEST_MAX_FX, -1);
    expect_idx("fx11111111111111111111111111:x", "fx", TEST_MAX_FX, -1);

    /* On success the caller forwards *subkey to the sub-plugin verbatim, so
     * it has to point past the colon — not at it, and not at the digit. */
    expect_subkey("fx1:cutoff", "fx", TEST_MAX_FX, 0, "cutoff");
    {
        char key[32];
        snprintf(key, sizeof(key), "fx%d:module", TEST_MAX_FX);
        expect_subkey(key, "fx", TEST_MAX_FX, TEST_MAX_FX - 1, "module");
    }
    expect_subkey("midi_fx2:bypassed", "midi_fx", TEST_MAX_MIDI_FX, 1, "bypassed");
    expect_subkey("fx3:", "fx", TEST_MAX_FX, 2, "");  /* empty, but well-formed */

    /* Building the id back is the inverse: chain_mod_* is keyed by it. */
    {
        char buf[16];
        chain_fx_component_id(buf, sizeof(buf), "fx", 0);
        if (strcmp(buf, "fx1") != 0) {
            fprintf(stderr, "FAIL component id: got %s, want fx1\n", buf);
            failures++;
        }
        char want[16];
        snprintf(want, sizeof(want), "midi_fx%d", TEST_MAX_MIDI_FX);
        chain_fx_component_id(buf, sizeof(buf), "midi_fx", TEST_MAX_MIDI_FX - 1);
        if (strcmp(buf, want) != 0) {
            fprintf(stderr, "FAIL component id: got %s, want %s\n", buf, want);
            failures++;
        }
        /* Round trip: every id we build must parse back to what built it. */
        for (int i = 0; i < TEST_MAX_FX; i++) {
            char key[32];
            chain_fx_component_id(buf, sizeof(buf), "fx", i);
            snprintf(key, sizeof(key), "%s:gain", buf);
            expect_idx(key, "fx", TEST_MAX_FX, i);
        }
    }

    if (failures) {
        fprintf(stderr, "FAIL: %d chain_key_index check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: chain_fx_index_from_key routes by index\n");
    return 0;
}
