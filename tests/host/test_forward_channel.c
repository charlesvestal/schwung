/* forward_channel_resolve: AUTO asks the module before the receive channel. */
#include <stdio.h>
#include "../../src/host/forward_channel.h"

static int fails;
static void chk(const char *what, int got, int want)
{
    if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); fails++; }
}

int main(void)
{
    const int AUTO = SHADOW_FORWARD_AUTO, THRU = SHADOW_FORWARD_THRU;
    const int NONE = -1;   /* module declared nothing */

    /* An explicit user channel always wins, module default or not. */
    chk("explicit ch5 beats module default", forward_channel_resolve(4, 2, 0), 4);
    chk("explicit ch1 beats module default", forward_channel_resolve(0, 2, 3), 0);
    chk("explicit THRU beats module default", forward_channel_resolve(THRU, 2, 0), THRU);

    /* AUTO consults the module. This is the JP-8000 case: receive=1 would have
     * forwarded ch1 and never reached the arpeggiator's remote channel. */
    chk("auto + module ch3", forward_channel_resolve(AUTO, 2, 0), 2);
    chk("auto + module ch3, recv=All", forward_channel_resolve(AUTO, 2, -1), 2);
    chk("auto + module THRU", forward_channel_resolve(AUTO, THRU, 0), THRU);

    /* AUTO with no module preference keeps the old meaning exactly. */
    chk("auto, no module, recv=1", forward_channel_resolve(AUTO, NONE, 0), 0);
    chk("auto, no module, recv=4", forward_channel_resolve(AUTO, NONE, 3), 3);
    chk("auto, no module, recv=All", forward_channel_resolve(AUTO, NONE, -1), THRU);

    /* Out-of-range module values are "said nothing", not silently clamped. */
    chk("auto, module 16 ignored", forward_channel_resolve(AUTO, 16, 2), 2);
    chk("auto, module -7 ignored", forward_channel_resolve(AUTO, -7, 2), 2);

    if (fails) { printf("test_forward_channel: %d FAILED\n", fails); return 1; }
    printf("test_forward_channel: PASS\n");
    return 0;
}
