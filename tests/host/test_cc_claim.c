/* cc_claim.h: who owns an external CC -- the generic CC map's claim table. */
#include <stdio.h>
#include <string.h>
#include "cc_claim.h"

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)

int main(void) {
    uint8_t bits[CC_CLAIM_BYTES];
    memset(bits, 0, sizeof bits);
    CHECK(CC_CLAIM_BYTES == 256, "the table is 16 channels x 128 CCs = 256 bytes");

    CHECK(cc_claim_route(0, 0, bits, 0xB0, 74) == 0, "empty table, not learning: nothing is ours");
    cc_claim_set(bits, 1, 74, 1);
    CHECK(cc_claim_owns(bits, 1, 74), "a bound (channel, cc) is owned");
    CHECK(!cc_claim_owns(bits, 0, 74), "...on that channel only");
    CHECK(!cc_claim_owns(bits, 1, 75), "...and that cc only");
    CHECK(cc_claim_route(1, 0, bits, 0xB1, 74) == (CC_ROUTE_PUBLISH | CC_ROUTE_SWALLOW),
          "a bound CC is published to the UI AND swallowed from Move");
    CHECK(cc_claim_route(1, 0, bits, 0xB0, 74) == 0, "the same cc on another channel passes");
    CHECK(cc_claim_route(1, 0, bits, 0x91, 74) == 0, "a NOTE with the same number is never claimed");
    CHECK(cc_claim_route(1, 1, bits, 0xB0, 20) == CC_ROUTE_PUBLISH,
          "while learning every CC is published -- and NOT swallowed");
    CHECK(cc_claim_route(1, 1, bits, 0xB1, 74) == (CC_ROUTE_PUBLISH | CC_ROUTE_SWALLOW),
          "...a bound one while learning is still swallowed");
    CHECK(cc_claim_route(0, 0, bits, 0xB1, 74) == 0,
          "count 0 is the fast path: a stale bit is not consulted");
    cc_claim_set(bits, 1, 74, 0);
    CHECK(!cc_claim_owns(bits, 1, 74), "a bit clears");
    cc_claim_set(bits, 15, 127, 1);
    CHECK(cc_claim_owns(bits, 15, 127) && bits[255] == 0x80, "the last bit is the last byte top bit");
    CHECK(!cc_claim_owns(bits, 16, 0) && !cc_claim_owns(bits, 0, 128), "out of range is never owned");

    printf(fails ? "FAILED %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
