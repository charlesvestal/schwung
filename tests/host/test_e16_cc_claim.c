/* Unit tests for the E16-vs-CC-Map ownership predicate.
 *
 * e16_claim.h is pure and header-only for the same reason as
 * fx_midi_filter.h and relative_cc.h: its call site is chain_midi.c's
 * v2_on_midi, the SPI callback, which cannot be compiled natively.
 *
 * Build/run: bash tests/host/test_e16_cc_claim.sh
 */
#include <stdio.h>
#include "e16_claim.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } } while (0)

/* While the surface is inactive, nothing is ever claimed — this is the
 * "CC Map behaviour is unchanged" half of the contract. */
static void test_surface_inactive_claims_nothing(void)
{
    CHECK(!e16_claims_cc(0, 0, 1),  "inactive: ch1/cc1 not claimed");
    CHECK(!e16_claims_cc(0, 0, 16), "inactive: ch1/cc16 not claimed");
    CHECK(!e16_claims_cc(0, 0, 8),  "inactive: ch1/cc8 not claimed");
    for (int cc = 0; cc <= 127; cc++)
        CHECK(!e16_claims_cc(0, 0, cc), "inactive: no cc on ch1 is ever claimed");
}

/* While active, CC 1-16 on channel 1 (wire value 0) are claimed. Hardcoded
 * 1..16 here rather than driven off E16_CLAIMED_CC_LOW/HIGH, deliberately:
 * a loop bounded by the header's own constants cannot catch the header's
 * bound drifting (e.g. HIGH shrinking to 15) — it would just re-derive a
 * smaller, still-internally-consistent range and keep passing. The literal
 * spec (E16 spec sheet: CC 1-16) belongs in the test independent of it. */
static void test_surface_active_claims_range(void)
{
    for (int cc = 1; cc <= 16; cc++)
        CHECK(e16_claims_cc(1, 0, cc), "active: cc in 1-16 on ch1 claimed");
}

/* Boundaries: one below and one above the claimed CC range, on the claimed
 * channel, are never claimed. Also hardcoded, for the same reason. */
static void test_cc_boundaries(void)
{
    CHECK(!e16_claims_cc(1, 0, 0),  "active: cc0 (just below) not claimed");
    CHECK(!e16_claims_cc(1, 0, 17), "active: cc17 (just above) not claimed");
}

/* Boundary: channel 2 (wire value 1) is never claimed, even for cc 1-16. */
static void test_channel_boundary(void)
{
    for (int cc = 1; cc <= 16; cc++)
        CHECK(!e16_claims_cc(1, 1, cc), "active: channel 2 never claimed");
    CHECK(!e16_claims_cc(1, 15, 5), "active: channel 16 never claimed");
}

int main(void)
{
    test_surface_inactive_claims_nothing();
    test_surface_active_claims_range();
    test_cc_boundaries();
    test_channel_boundary();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS: e16_claims_cc surface ownership\n");
    return 0;
}
