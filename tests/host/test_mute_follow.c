/* Slot mute follows Move's track mute (src/host/mute_follow.h).
 *
 * The attribution cases are the point: the removed D-Bus auto-correct applied
 * every " muted" suffix to the selected slot, so a drum-CELL mute ("Tom 808 Low
 * 2 muted") muted a slot and persisted it. An announcement may only reach a
 * slot the GESTURE named, and Mute+pad names none.
 */
#include <stdio.h>
#include "mute_follow.h"

static int failures = 0;
#define EXPECT(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); failures++; } } while (0)

int main(void)
{
    /* ---- the text ---- */
    EXPECT(mute_announce_classify("Sudan Archives Kit muted") == MUTE_ANNOUNCE_MUTED, "kit muted");
    EXPECT(mute_announce_classify("Sudan Archives Kit unmuted") == MUTE_ANNOUNCE_UNMUTED, "unmuted is not muted");
    EXPECT(mute_announce_classify("Grand Piano MUTED\n") == MUTE_ANNOUNCE_MUTED, "case + trailing newline");
    EXPECT(mute_announce_classify("muted") == MUTE_ANNOUNCE_NONE, "bare word names nothing");
    EXPECT(mute_announce_classify(" unmuted") == MUTE_ANNOUNCE_NONE, "bare suffix names nothing");
    EXPECT(mute_announce_classify("Kit soloed") == MUTE_ANNOUNCE_NONE, "solo is not mute");
    EXPECT(mute_announce_classify("Transmuted") == MUTE_ANNOUNCE_NONE, "suffix needs a word break");
    EXPECT(mute_announce_classify("Muted Trumpet") == MUTE_ANNOUNCE_NONE, "prefix is not a state");
    EXPECT(mute_announce_classify(NULL) == MUTE_ANNOUNCE_NONE, "null");

    mute_follow_t f;

    /* ---- nothing pressed: nothing attributed ---- */
    mute_follow_reset(&f);
    EXPECT(mute_follow_target(&f, 100) == -1, "idle attributes nothing");

    /* ---- Mute + Track 3 ---- */
    mute_follow_reset(&f);
    mute_follow_on_mute_press(&f, 0, 1, 4);
    mute_follow_on_track_press(&f, 2, 4);
    EXPECT(mute_follow_target(&f, 1000) == 2, "mute+track names the track while held");
    mute_follow_on_mute_release(&f, 1000);
    EXPECT(mute_follow_target(&f, 1000 + MUTE_FOLLOW_WINDOW_MS) == 2, "still open at window end");
    EXPECT(mute_follow_target(&f, 1001 + MUTE_FOLLOW_WINDOW_MS) == -1, "closed after window");

    /* ---- plain Mute tap: the selected track, once known ---- */
    mute_follow_reset(&f);
    mute_follow_on_mute_press(&f, 1, 1, 4);
    mute_follow_on_mute_release(&f, 50);
    EXPECT(mute_follow_target(&f, 60) == 1, "plain tap names the selected track");

    mute_follow_reset(&f);
    mute_follow_on_mute_press(&f, 1, 0, 4);
    mute_follow_on_mute_release(&f, 50);
    EXPECT(mute_follow_target(&f, 60) == -1, "plain tap before any track press names nothing");

    /* ---- Mute + pad: a drum-cell mute is never a slot's ---- */
    mute_follow_reset(&f);
    mute_follow_on_mute_press(&f, 0, 1, 4);
    mute_follow_on_other_press(&f);
    EXPECT(mute_follow_target(&f, 10) == -1, "mute+pad names nothing while held");
    mute_follow_on_mute_release(&f, 20);
    EXPECT(mute_follow_target(&f, 30) == -1, "mute+pad names nothing after release");

    /* Mute + Track after a pad in the same hold: the track re-names it. */
    mute_follow_reset(&f);
    mute_follow_on_mute_press(&f, 0, 1, 4);
    mute_follow_on_other_press(&f);
    mute_follow_on_track_press(&f, 3, 4);
    EXPECT(mute_follow_target(&f, 10) == 3, "track after pad re-targets");

    /* ---- presses without Mute held change nothing ---- */
    mute_follow_reset(&f);
    mute_follow_on_track_press(&f, 2, 4);
    EXPECT(mute_follow_target(&f, 10) == -1, "track without mute names nothing");

    /* A new Mute press closes the previous window's target. */
    mute_follow_reset(&f);
    mute_follow_on_mute_press(&f, 0, 1, 4);
    mute_follow_on_track_press(&f, 2, 4);
    mute_follow_on_mute_release(&f, 10);
    mute_follow_on_mute_press(&f, 0, 0, 4);
    EXPECT(mute_follow_target(&f, 20) == -1, "new press replaces the old target");

    /* Out-of-range slots are refused. */
    mute_follow_reset(&f);
    mute_follow_on_mute_press(&f, 7, 1, 4);
    EXPECT(mute_follow_target(&f, 1) == -1, "selected slot out of range");
    mute_follow_on_track_press(&f, -1, 4);
    EXPECT(mute_follow_target(&f, 1) == -1, "track out of range");

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_mute_follow: all passed\n");
    return 0;
}
