/*
 * When the post-ioctl restore re-copies the hardware jack over the shadow
 * mailbox's AUDIO_IN, for an overtake plugin reading host->audio_in_offset.
 *
 * The resample bridge used to be a third input here: it wrote the region
 * EARLIER in the same pass, so the restore stood down while it was on (#457).
 * The bridge now writes LAST (tests/host/test_resample_bridge_after_render.sh),
 * so it cannot be undone and the stand-down is gone -- keeping it would only
 * hand an overtake plugin the previous frame's mix instead of its input.
 *
 * A one-line predicate rather than a comment because that is the only form
 * tests/host can run: schwung_shim.c does not build on the dev machine.
 */
#include <assert.h>
#include <stdio.h>
#include "audio_in_restore.h"

int main(void) {
    /* No overtake instance: nothing reads audio_in_offset through this path. */
    assert(shadow_audio_in_restore_allowed(0, 1) == 0);

    /* No hardware mailbox: there is nothing to copy FROM. Reading
     * hardware_mmap_addr when it is NULL is the crash this prevents. */
    assert(shadow_audio_in_restore_allowed(1, 0) == 0);

    /* An overtake module loaded (either role -- the caller ORs the two
     * instances): restore, whatever the resample bridge is set to. */
    assert(shadow_audio_in_restore_allowed(1, 1) == 1);

    printf("test_audio_in_restore: PASS\n");
    return 0;
}
