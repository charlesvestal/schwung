/*
 * Who owns shadow AUDIO_IN when both writers want it.
 *
 * native_resample_bridge_apply() writes Schwung's mix into the shadow
 * mailbox's AUDIO_IN region, then — later in the SAME shim_post_transfer —
 * the overtake restore writes the hardware jack over the top. Second writer
 * wins, so without a guard the bridge is a no-op for as long as an overtake
 * module is loaded, and Move's Resample records the jack. Nothing logs it.
 *
 * The precedence is a one-line predicate rather than a comment because that
 * is the only form tests/host can run: schwung_shim.c does not build on the
 * dev machine.
 */
#include <assert.h>
#include <stdio.h>
#include "audio_in_restore.h"

int main(void) {
    /* No overtake instance: nothing reads audio_in_offset, so no restore
     * whatever the bridge is doing. */
    assert(shadow_audio_in_restore_allowed(0, 1, 0) == 0);
    assert(shadow_audio_in_restore_allowed(0, 1, 1) == 0);

    /* No hardware mailbox: there is nothing to copy FROM. Reading
     * hardware_mmap_addr when it is NULL is the crash this prevents. */
    assert(shadow_audio_in_restore_allowed(1, 0, 0) == 0);

    /* The ordinary case the feature exists for: an overtake module loaded
     * (either role — the caller ORs the two instances), bridge off. */
    assert(shadow_audio_in_restore_allowed(1, 1, 0) == 1);

    /* Bridge ON and actually applying — it wrote the region this frame, so
     * the restore stands down. This is the assertion the fix is FOR: before
     * it, this case returned 1 and silently reverted the bridge. */
    assert(shadow_audio_in_restore_allowed(1, 1, 1) == 0);

    /* THE FOURTH ARGUMENT IS GONE. It was "the bridge's SOURCE gate allows
     * this apply", and this test used to assert that a blocked source let the
     * restore proceed. That case is unreachable and always was: the gate
     * returned 1 unconditionally in OVERWRITE, and the only mode that
     * consulted it (the retired mode 1) could not be selected from any shipped
     * UI. Removing the argument removes the branch — deleting the assertion
     * without deleting the parameter would have left the dead path untested
     * rather than gone. */

    printf("test_audio_in_restore: PASS\n");
    return 0;
}
