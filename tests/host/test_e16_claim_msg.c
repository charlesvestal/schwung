#include <stdio.h>
#include "e16_claim.h"
static int fails = 0;
static void eq(const char *n, int g, int w) {
    if (g != w) { printf("FAIL %s got %d want %d\n", n, g, w); fails++; }
    else printf("ok   %s\n", n);
}
int main(void) {
    /* The surface's own three message kinds, all channel 1. */
    eq("cc 1 ch1",        e16_claims_msg(1, 0xB0, 1), 1);
    eq("cc 16 ch1",       e16_claims_msg(1, 0xB0, 16), 1);
    eq("note 0 ch1",      e16_claims_msg(1, 0x90, 0), 1);
    eq("note 16 (shift)", e16_claims_msg(1, 0x90, 16), 1);
    eq("note off ch1",    e16_claims_msg(1, 0x80, 3), 1);

    /* Everything else must FALL THROUGH to the CC Map and the chain. This is
     * the whole point of narrowing: another device on cable 2 keeps working
     * while the surface is on. */
    eq("cc 0 is not ours",     e16_claims_msg(1, 0xB0, 0), 0);
    eq("cc 17 is not ours",    e16_claims_msg(1, 0xB0, 17), 0);
    eq("cc 74 is not ours",    e16_claims_msg(1, 0xB0, 74), 0);
    eq("note 17 is not ours",  e16_claims_msg(1, 0x90, 17), 0);
    eq("channel 2 is not ours",e16_claims_msg(1, 0xB1, 1), 0);
    eq("pitch bend not ours",  e16_claims_msg(1, 0xE0, 1), 0);
    eq("aftertouch not ours",  e16_claims_msg(1, 0xD0, 1), 0);

    /* Off means off: nothing is claimed, so behaviour is exactly as before. */
    eq("off claims no cc",   e16_claims_msg(0, 0xB0, 1), 0);
    eq("off claims no note", e16_claims_msg(0, 0x90, 0), 0);

    printf(fails ? "FAILED %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
