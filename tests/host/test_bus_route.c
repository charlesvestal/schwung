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

static void test_target_key_roundtrip(void) {
    char out[BUS_TARGET_KEY_LEN];
    for (int n = 1; n <= TEST_SLOT_BUSES; n++) {
        assert(bus_route_target(out, sizeof(out), n) == 1);
        const char *end = NULL;
        assert(bus_route_parse_index(out, &end) == n);
        assert(*end == '\0');
    }
    printf("  target roundtrip: ok\n");
}

static void test_voice_index_lookup(void) {
    const char *ids[4] = { "kick", "snare", "chh", "ohh" };
    assert(bus_voice_index(ids, 4, "kick") == 0);
    assert(bus_voice_index(ids, 4, "ohh")  == 3);
    assert(bus_voice_index(ids, 4, "ride") == -1);   /* orphan: module changed */
    assert(bus_voice_index(ids, 4, NULL)   == -1);
    assert(bus_voice_index(NULL, 0, "kick") == -1);
    printf("  voice lookup: ok\n");
}

int main(void) {
    printf("test_bus_route (cap=%d):\n", TEST_SLOT_BUSES);
    test_full_range_parses();
    test_past_the_cap_is_rejected_not_routed_to_zero();
    test_malformed_ids();
    test_multi_digit_is_not_read_from_one_char();
    test_target_key_roundtrip();
    test_voice_index_lookup();
    printf("PASS\n");
    return 0;
}
