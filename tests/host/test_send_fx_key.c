/*
 * Unit test for send_fx_key.h — "send<N>:fx<M>:<param>" and "send<N>:<param>".
 *
 * The caps are passed in by the build (from shadow_chain_mgmt.h) rather than
 * restated here, so raising SEND_BUSES or SEND_FX_SLOTS widens the coverage
 * automatically. A test that hard-coded 2 and 8 would go on passing while
 * covering a fraction of the range — which is the exact failure master_fx_key.h
 * exists to prevent.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "send_fx_key.h"

#ifndef TEST_SEND_BUSES
#error "TEST_SEND_BUSES must be defined by the build"
#endif
#ifndef TEST_SEND_FX_SLOTS
#error "TEST_SEND_FX_SLOTS must be defined by the build"
#endif

static void test_full_range(void) {
    for (int s = 1; s <= TEST_SEND_BUSES; s++) {
        for (int f = 1; f <= TEST_SEND_FX_SLOTS; f++) {
            char key[48];
            snprintf(key, sizeof(key), "send%d:fx%d:cutoff", s, f);
            int send = -99, slot = -99;
            const char *param = NULL;
            assert(send_fx_route(key, TEST_SEND_BUSES, TEST_SEND_FX_SLOTS,
                                 &send, &slot, &param) == 1);
            assert(send == s - 1);
            assert(slot == f - 1);
            assert(param && strcmp(param, "cutoff") == 0);
        }
    }
    printf("  full range: ok\n");
}

static void test_bus_level_keys(void) {
    for (int s = 1; s <= TEST_SEND_BUSES; s++) {
        char key[48];
        snprintf(key, sizeof(key), "send%d:return", s);
        int send = -99, slot = -99;
        const char *param = NULL;
        assert(send_fx_route(key, TEST_SEND_BUSES, TEST_SEND_FX_SLOTS,
                             &send, &slot, &param) == 1);
        assert(send == s - 1);
        assert(slot == -1);                    /* a bus-level key, not an FX */
        assert(param && strcmp(param, "return") == 0);
    }
    printf("  bus-level keys: ok\n");
}

static void test_past_caps_rejected(void) {
    int send = -99, slot = -99;
    const char *param = (const char *)0x1;
    char key[48];

    snprintf(key, sizeof(key), "send%d:return", TEST_SEND_BUSES + 1);
    assert(send_fx_route(key, TEST_SEND_BUSES, TEST_SEND_FX_SLOTS,
                         &send, &slot, &param) == 0);
    assert(send == -99 && slot == -99 && param == (const char *)0x1);

    snprintf(key, sizeof(key), "send1:fx%d:cutoff", TEST_SEND_FX_SLOTS + 1);
    assert(send_fx_route(key, TEST_SEND_BUSES, TEST_SEND_FX_SLOTS,
                         &send, &slot, &param) == 0);
    assert(send == -99 && slot == -99 && param == (const char *)0x1);

    /* An absurd digit run must not wrap into a plausible index. 4294967297 is
     * chosen, not arbitrary: unclamped int accumulation wraps it to exactly 1
     * on two's-complement, so a missing clamp is a silent MISROUTE into send 1
     * / position 1 rather than a visibly wrong number. Anything less specific
     * lands out of range by luck and pins nothing. */
    assert(send_fx_route("send4294967297:return", TEST_SEND_BUSES,
                         TEST_SEND_FX_SLOTS, &send, &slot, &param) == 0);
    assert(send_fx_route("send1:fx4294967297:cutoff", TEST_SEND_BUSES,
                         TEST_SEND_FX_SLOTS, &send, &slot, &param) == 0);
    assert(send == -99 && slot == -99 && param == (const char *)0x1);
    printf("  past caps rejected: ok\n");
}

static void test_malformed(void) {
    int send = -99, slot = -99;
    const char *param = (const char *)0x1;

#define REJECT(k) do { \
    assert(send_fx_route((k), TEST_SEND_BUSES, TEST_SEND_FX_SLOTS, \
                         &send, &slot, &param) == 0); \
    assert(send == -99 && slot == -99 && param == (const char *)0x1); \
} while (0)

    REJECT("send0:return");      /* 1-based on the wire; there is no send 0 */
    REJECT("send01:return");     /* leading zero is not an id we emit */
    REJECT("send1");             /* no colon, so it names no param */
    REJECT("send1:");            /* colon but nothing after it */
    REJECT("send1x:return");     /* junk between the index and the colon */
    REJECT("sendA:return");
    REJECT("send:return");
    REJECT("fx1:cutoff");        /* a Master FX key, not a send key */
    /* Another key with a digit at byte 4: without the "send" prefix test this
     * parses as send 1, so it is what makes that guard load-bearing. */
    REJECT("slot1:volume");
    REJECT("sendo:off");         /* shares the "send" prefix, nothing more */
    REJECT("");
    REJECT(NULL);

    /* fx-SHAPED but not an in-range position: rejected, never handed back as
     * a bus-level param whose name happens to start with "fx". */
    REJECT("send1:fx0:cutoff");
    REJECT("send1:fx01:cutoff");
    REJECT("send1:fx1");         /* no colon after the position */
    REJECT("send1:fx");

#undef REJECT
    printf("  malformed: ok\n");
}

/* NULL out-params must be tolerated: a caller that only wants to know whether
 * a key is a send key passes none of them. */
static void test_null_outparams(void) {
    assert(send_fx_route("send1:fx1:cutoff", TEST_SEND_BUSES,
                         TEST_SEND_FX_SLOTS, NULL, NULL, NULL) == 1);
    assert(send_fx_route("send1:return", TEST_SEND_BUSES,
                         TEST_SEND_FX_SLOTS, NULL, NULL, NULL) == 1);
    printf("  null out-params: ok\n");
}

int main(void) {
    printf("test_send_fx_key (buses=%d slots=%d):\n",
           TEST_SEND_BUSES, TEST_SEND_FX_SLOTS);
    test_full_range();
    test_bus_level_keys();
    test_past_caps_rejected();
    test_malformed();
    test_null_outparams();
    printf("PASS\n");
    return 0;
}
