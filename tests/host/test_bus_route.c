/* Unit tests for bus_route.h.
 *
 * The cap comes in as -DTEST_SLOT_BUSES, read out of chain_internal.h by the
 * shell wrapper, so this covers whatever range buses actually run with rather
 * than a number restated here. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bus_route.h"

#ifndef TEST_SLOT_BUSES
#error "TEST_SLOT_BUSES must be defined by the build"
#endif

static void test_full_range_parses(void) {
    for (int n = 1; n <= TEST_SLOT_BUSES; n++) {
        char key[32];
        snprintf(key, sizeof(key), "bus%d:fx1:cutoff", n);
        int bus = -99;
        const char *rest = NULL;
        assert(bus_route_param_key(key, TEST_SLOT_BUSES, &bus, &rest) == 1);
        assert(bus == n - 1);
        assert(strcmp(rest, "fx1:cutoff") == 0);
    }
    printf("  full range: ok\n");
}

static void test_past_the_cap_is_rejected_not_routed_to_zero(void) {
    char key[32];
    snprintf(key, sizeof(key), "bus%d:fx1:cutoff", TEST_SLOT_BUSES + 1);
    int bus = -99;
    const char *rest = (const char *)0x1;
    assert(bus_route_param_key(key, TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus == -99);                  /* untouched, NOT assigned slot 0 */
    assert(rest == (const char *)0x1);
    printf("  past cap rejected: ok\n");
}

static void test_malformed_ids(void) {
    int bus = -99;
    const char *rest = NULL;
    assert(bus_route_param_key("bus0:x",  TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus_route_param_key("bus01:x", TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus_route_param_key("bus1",    TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus_route_param_key("bus1fx",  TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus_route_param_key("busx:y",  TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus_route_param_key("fx1:cut", TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus_route_param_key(NULL,      TEST_SLOT_BUSES, &bus, &rest) == 0);
    printf("  malformed ids: ok\n");
}

static void test_multi_digit_is_not_read_from_one_char(void) {
    const char *end = NULL;
    assert(bus_route_parse_index("bus12:x", &end) == 12);
    assert(*end == ':');
    printf("  multi-digit: ok\n");
}

static void test_voice_index_lookup(void) {
    const char *ids[4] = { "kick", "snare", "chh", "ohh" };
    assert(bus_voice_index(ids, 4, "kick") == 0);
    assert(bus_voice_index(ids, 4, "ohh")  == 3);
    assert(bus_voice_index(ids, 4, "ride") == -1);   /* orphan: module changed */
    assert(bus_voice_index(ids, 4, NULL)   == -1);
    assert(bus_voice_index(NULL, 0, "kick") == -1);
    /* n_ids 0 returns through the LOOP, not the guard. A stale count over a
     * list that is gone is the case that would actually dereference. */
    assert(bus_voice_index(NULL, 4, "kick") == -1);

    /* A hole in the list built by a caller with its own pointer array --
     * distinct from split_voices_parse's holes, which are empty strings,
     * not NULLs (the chain host's table is char[N][32] and can never hold a
     * NULL entry). */
    const char *holed[4] = { "kick", NULL, "chh", NULL };
    assert(bus_voice_index(holed, 4, "chh")  == 2);
    assert(bus_voice_index(holed, 4, "ride") == -1);

    /* The real hole shape: split_voices_parse stores an empty string at its
     * own index, never compacted away. It must never match a lookup. */
    const char *empty_holed[3] = { "kick", "", "chh" };
    assert(bus_voice_index(empty_holed, 3, "chh") == 2);
    assert(bus_voice_index(empty_holed, 3, "")    == -1);
    printf("  voice lookup: ok\n");
}


/* ==========================================================================
 * THE PER-VOICE SEND KEY
 *
 * The spelling bus_model.mjs writes ("buses:voice7:send2", the "buses:" prefix
 * stripped by the slot route before it reaches here) and the only thing that
 * reads it. Both halves of a key that lived in a translation unit the dev
 * machine cannot build.
 * ========================================================================== */
#define TEST_VOICES 32
#define TEST_SENDS  2

static void test_voice_send_routes(void) {
    for (int v = 1; v <= TEST_VOICES; v++) {
        for (int sd = 1; sd <= TEST_SENDS; sd++) {
            char key[32];
            snprintf(key, sizeof(key), "voice%d:send%d", v, sd);
            int gv = -99, gs = -99;
            assert(bus_route_voice_send(key, TEST_VOICES, TEST_SENDS, &gv, &gs) == 1);
            assert(gv == v - 1);   /* 0-BASED: it is the render index */
            assert(gs == sd);      /* 1-BASED: it is the send NUMBER */
        }
    }
    printf("  voice send routes: ok\n");
}

static void test_voice_send_bounds_and_refusals(void) {
    int v = -99, sd = -99;
    /* Past either cap, and at zero. An out-of-range key must be REFUSED, not
       clamped onto voice 0 or send 1 -- that is the Master FX else-branch that
       wrote a garbage param into a different running module. */
    char key[32];
    snprintf(key, sizeof(key), "voice%d:send1", TEST_VOICES + 1);
    assert(bus_route_voice_send(key, TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    snprintf(key, sizeof(key), "voice1:send%d", TEST_SENDS + 1);
    assert(bus_route_voice_send(key, TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    assert(bus_route_voice_send("voice0:send1",  TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    assert(bus_route_voice_send("voice1:send0",  TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    /* Leading zeros, the same rule bus<N> has. */
    assert(bus_route_voice_send("voice01:send1", TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    assert(bus_route_voice_send("voice1:send01", TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    /* Malformed, and the near-misses that would otherwise fall through to a
       bus route or to the synth plugin. */
    assert(bus_route_voice_send("voice1",        TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    assert(bus_route_voice_send("voice1:",       TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    assert(bus_route_voice_send("voice1:sends1", TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    assert(bus_route_voice_send("voices:send1",  TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    assert(bus_route_voice_send("voice1:gain",   TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    assert(bus_route_voice_send("bus1:send1",    TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    assert(bus_route_voice_send("main_send1",    TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    assert(bus_route_voice_send("config",        TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    assert(bus_route_voice_send(NULL,            TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    /* THE WHOLE KEY IS CONSUMED. Routing on the prefix would send
       "voice1:send1:extra" to voice 1 send 1 and drop the rest silently. */
    assert(bus_route_voice_send("voice1:send1:extra", TEST_VOICES, TEST_SENDS, &v, &sd) == 0);
    /* Every refusal left the out-params ALONE. */
    assert(v == -99 && sd == -99);
    printf("  voice send bounds: ok\n");
}

int main(void) {
    printf("test_bus_route (cap=%d):\n", TEST_SLOT_BUSES);
    test_full_range_parses();
    test_past_the_cap_is_rejected_not_routed_to_zero();
    test_malformed_ids();
    test_multi_digit_is_not_read_from_one_char();
    test_voice_index_lookup();
        test_voice_send_routes();
    test_voice_send_bounds_and_refusals();
    printf("PASS\n");
    return 0;
}
