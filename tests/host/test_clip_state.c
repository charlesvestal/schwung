/*
 * test_clip_state — replays REAL Move LED captures through the decoder.
 *
 * The fixtures are not synthetic. tests/fixtures/clip_led_*.txt were captured
 * from a Move on 2026-09-12 with led_capture_on armed, and the expected
 * answers below were established independently -- from Song.abl's isPlaying
 * flags, and from the user's own record of which pads they pressed.
 *
 * A synthetic fixture here would prove only that the decoder agrees with my
 * model of Move, which is exactly the thing that was wrong four times over
 * while this feature was being designed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clip_state.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static int feed(clip_state_t *st, const char *path, uint32_t stop_after_pulse)
{
    FILE *f = fopen(path, "r");
    if (!f) { printf("  FAIL: cannot open %s\n", path); failures++; return 0; }
    char line[256]; int n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned pul, st_b, d1, d2;
        if (sscanf(line, "%u %u %u %u", &pul, &st_b, &d1, &d2) != 4) continue;
        if (stop_after_pulse && pul > stop_after_pulse) break;
        clip_state_on_led(st, (uint8_t)st_b, (uint8_t)d1, (uint8_t)d2, pul);
        n++;
    }
    fclose(f);
    return n;
}

static void test_pad_decode(void)
{
    printf("pad note -> (track, slot)\n");
    int t, s;
    /* The two pads the user actually pressed, and the row edges. */
    CHECK(clip_pad_decode(93, &t, &s) && t == 0 && s == 1, "note 93 -> T%d S%d, want T0 S1", t, s);
    CHECK(clip_pad_decode(78, &t, &s) && t == 2 && s == 2, "note 78 -> T%d S%d, want T2 S2", t, s);
    CHECK(clip_pad_decode(92, &t, &s) && t == 0 && s == 0, "note 92 -> T%d S%d, want T0 S0", t, s);
    CHECK(clip_pad_decode(99, &t, &s) && t == 0 && s == 7, "note 99 -> T%d S%d, want T0 S7", t, s);
    CHECK(clip_pad_decode(68, &t, &s) && t == 3 && s == 0, "note 68 -> T%d S%d, want T3 S0", t, s);
    CHECK(clip_pad_decode(75, &t, &s) && t == 3 && s == 7, "note 75 -> T%d S%d, want T3 S7", t, s);
    /* Not grid pads. 67/100 are off the ends; 16-31 are the STEP row, and
     * feeding those to the grid decoder would invent clips out of a playhead. */
    CHECK(!clip_pad_decode(67, &t, &s), "note 67 must not decode");
    CHECK(!clip_pad_decode(100, &t, &s), "note 100 must not decode");
    CHECK(!clip_pad_decode(20, &t, &s), "step note 20 must not decode as a pad");
}

static void test_grid_refresh_matches_song_abl(void)
{
    printf("grid refresh identifies the four playing clips (vs Song.abl)\n");
    clip_state_t st; clip_state_reset(&st);
    /* Stop before the user's first launch (pulse 0, seq ~174) is irrelevant --
     * the refresh burst and the launch both sit at pulse 0, so feed the whole
     * pre-transport section and check the LAUNCH outcome separately below. */
    feed(&st, "../fixtures/clip_led_launch.txt", 0);

    /* Song.abl isPlaying: tracks[0].clipSlots[2], [1][0], [2][0], [3][3].
     * Track 0 and track 2 were changed by the user's two launches, so the
     * untouched two are the ones this assertion can still speak to. */
    CHECK(st.tracks[1].identity_valid && st.tracks[1].clip_slot == 0,
          "track 2 should be playing clip 1, got valid=%d slot=%d",
          st.tracks[1].identity_valid, st.tracks[1].clip_slot);
    CHECK(st.tracks[3].identity_valid && st.tracks[3].clip_slot == 3,
          "track 4 should be playing clip 4, got valid=%d slot=%d",
          st.tracks[3].identity_valid, st.tracks[3].clip_slot);
}

static void test_launch_sets_anchor(void)
{
    printf("a queued launch sets the anchor at the ch-9 ON\n");
    clip_state_t st; clip_state_reset(&st);
    feed(&st, "../fixtures/clip_led_launch.txt", 0);

    /* User launched clip 2 on track 1, then clip 3 on track 3. */
    CHECK(st.tracks[0].identity_valid && st.tracks[0].clip_slot == 1,
          "track 1 should be playing clip 2, got slot=%d", st.tracks[0].clip_slot);
    CHECK(st.tracks[2].identity_valid && st.tracks[2].clip_slot == 2,
          "track 3 should be playing clip 3, got slot=%d", st.tracks[2].clip_slot);

    /* The measured anchor: queued at pulse 684, quantised boundary at 768,
     * ch-9 ON observed at 770. */
    CHECK(st.tracks[2].anchor_valid, "track 3 must be anchored after a launch");
    CHECK(st.tracks[2].anchor_pulse == 770,
          "track 3 anchor should be pulse 770, got %u", st.tracks[2].anchor_pulse);
}

static void test_bare_ch9_is_a_refresh_not_an_anchor(void)
{
    printf("RULE 2: a bare ch-9 ON must NOT move an anchor\n");
    clip_state_t st; clip_state_reset(&st);

    /* Establish a real, queued launch: track 1 clip 2, anchored at pulse 500. */
    clip_state_on_led(&st, 0x9E, 93, 122, 480);   /* ch14 QUEUED */
    clip_state_on_led(&st, 0x99, 93, 122, 500);   /* ch9 ON  -> anchor */
    CHECK(st.tracks[0].anchor_valid && st.tracks[0].anchor_pulse == 500,
          "setup: expected anchor 500, got valid=%d pulse=%u",
          st.tracks[0].anchor_valid, st.tracks[0].anchor_pulse);

    /* Now a grid refresh 1000 pulses later: same pad, ch-9 ON, no queue.
     * This is what entering Session mode emits for a clip that has been
     * playing all along. It must change NOTHING. */
    clip_state_on_led(&st, 0x99, 93, 122, 1500);
    CHECK(st.tracks[0].anchor_pulse == 500,
          "a refresh re-anchored the track to %u -- phase destroyed",
          st.tracks[0].anchor_pulse);
    CHECK(st.tracks[0].clip_slot == 1, "refresh should keep identity");
}

static void test_coldstart_refresh_burst_anchors_nothing(void)
{
    printf("the real cold-start capture has 0 queues, so 0 observed anchors\n");
    clip_state_t st; clip_state_reset(&st);
    int n = feed(&st, "../fixtures/clip_led_coldstart.txt", 0);
    CHECK(n > 100, "fixture should have plenty of events, got %d", n);

    /* Round 3: the user loaded sets and pressed play but never launched a clip
     * by hand, so every ch-9 ON in it is a refresh. Any anchor present must
     * therefore have come from a transport restart (pulse 0), never from an
     * observed launch. */
    for (int t = 0; t < CLIP_TRACKS; t++) {
        if (st.tracks[t].anchor_valid) {
            CHECK(st.tracks[t].anchor_pulse == 0,
                  "track %d anchored at pulse %u with no queue ever seen -- "
                  "a refresh was mistaken for a launch",
                  t + 1, st.tracks[t].anchor_pulse);
        }
    }
}

static void test_restart_reanchors_to_zero(void)
{
    printf("MIDI Start re-anchors known-playing tracks to 0\n");
    clip_state_t st; clip_state_reset(&st);
    clip_state_on_led(&st, 0x9E, 93, 122, 480);
    clip_state_on_led(&st, 0x99, 93, 122, 500);
    CHECK(st.tracks[0].anchor_pulse == 500, "setup");

    /* The REAL 0xFA, not a backwards pulse step. The proxy this replaces only
     * ever detected a re-start: after a reboot the counter is already 0 and
     * the first Start also begins at 0, so nothing moves backwards. On
     * hardware that left three of four tracks unanchored through a full
     * minute of playback while this test passed, because the test fed a
     * transition the device never makes. */
    clip_state_on_transport_start(&st);
    CHECK(st.tracks[0].anchor_valid && st.tracks[0].anchor_pulse == 0,
          "after a restart track 1 should be anchored at 0, got valid=%d pulse=%u",
          st.tracks[0].anchor_valid, st.tracks[0].anchor_pulse);

    /* A track we never identified must stay UNKNOWN, not be guessed to 0 --
     * its clip could equally have been launched after the start. */
    CHECK(!st.tracks[1].anchor_valid,
          "an unidentified track must not be anchored by a restart");
}

static void test_phase_is_never_guessed(void)
{
    printf("phase refuses rather than defaulting\n");
    clip_state_t st; clip_state_reset(&st);
    double beats;

    CHECK(!clip_phase_beats(&st.tracks[0], 100, 0.0, 4.0, &beats),
          "unknown identity must not produce a phase");

    clip_state_on_led(&st, 0x99, 93, 122, 100);   /* refresh: identity only */
    CHECK(st.tracks[0].identity_valid, "setup: identity");
    CHECK(!clip_phase_beats(&st.tracks[0], 200, 0.0, 4.0, &beats),
          "identity without an anchor must NOT produce phase 0 -- unknown is "
          "not zero, and a lane recording at a guessed zero is the bug this "
          "whole tri-state exists to prevent");

    /* With a real anchor, the arithmetic -- including a non-zero loop start,
     * which Song.abl does carry (a loop beginning at bar 3 is normal). */
    clip_state_reset(&st);
    clip_state_on_led(&st, 0x9E, 93, 122, 0);
    clip_state_on_led(&st, 0x99, 93, 122, 0);
    CHECK(clip_phase_beats(&st.tracks[0], 24 * 5, 0.0, 4.0, &beats), "should resolve");
    CHECK(beats > 0.99 && beats < 1.01, "5 beats into a 4-beat loop = 1.0, got %f", beats);

    CHECK(clip_phase_beats(&st.tracks[0], 24 * 5, 8.0, 4.0, &beats), "should resolve");
    CHECK(beats > 8.99 && beats < 9.01,
          "loop starting at beat 8, 5 beats in = 9.0, got %f", beats);
}

int main(void)
{
    test_pad_decode();
    test_grid_refresh_matches_song_abl();
    test_launch_sets_anchor();
    test_bare_ch9_is_a_refresh_not_an_anchor();
    test_coldstart_refresh_burst_anchors_nothing();
    test_restart_reanchors_to_zero();
    test_phase_is_never_guessed();

    if (failures) { printf("\n%d CHECK(s) failed\n", failures); return 1; }
    printf("\nall clip_state checks passed\n");
    return 0;
}
