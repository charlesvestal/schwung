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
        clip_state_on_led(st, (uint8_t)st_b, (uint8_t)d1, (uint8_t)d2, pul, 1, 1);
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
    clip_state_on_led(&st, 0x9E, 93, 122, 480, 1, 1);   /* ch14 QUEUED */
    clip_state_on_led(&st, 0x99, 93, 122, 500, 1, 1);   /* ch9 ON  -> anchor */
    CHECK(st.tracks[0].anchor_valid && st.tracks[0].anchor_pulse == 500,
          "setup: expected anchor 500, got valid=%d pulse=%u",
          st.tracks[0].anchor_valid, st.tracks[0].anchor_pulse);

    /* Now a grid refresh 1000 pulses later: same pad, ch-9 ON, no queue.
     * This is what entering Session mode emits for a clip that has been
     * playing all along. It must change NOTHING. */
    clip_state_on_led(&st, 0x99, 93, 122, 1500, 1, 1);
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
    clip_state_on_led(&st, 0x9E, 93, 122, 480, 1, 1);
    clip_state_on_led(&st, 0x99, 93, 122, 500, 1, 1);
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

    clip_state_on_led(&st, 0x99, 93, 122, 100, 1, 1);   /* refresh: identity only */
    CHECK(st.tracks[0].identity_valid, "setup: identity");
    CHECK(!clip_phase_beats(&st.tracks[0], 200, 0.0, 4.0, &beats),
          "identity without an anchor must NOT produce phase 0 -- unknown is "
          "not zero, and a lane recording at a guessed zero is the bug this "
          "whole tri-state exists to prevent");

    /* With a real anchor, the arithmetic -- including a non-zero loop start,
     * which Song.abl does carry (a loop beginning at bar 3 is normal). */
    clip_state_reset(&st);
    clip_state_on_led(&st, 0x9E, 93, 122, 0, 1, 1);
    clip_state_on_led(&st, 0x99, 93, 122, 0, 1, 1);
    CHECK(clip_phase_beats(&st.tracks[0], 24 * 5, 0.0, 4.0, &beats), "should resolve");
    CHECK(beats > 0.99 && beats < 1.01, "5 beats into a 4-beat loop = 1.0, got %f", beats);

    CHECK(clip_phase_beats(&st.tracks[0], 24 * 5, 8.0, 4.0, &beats), "should resolve");
    CHECK(beats > 8.99 && beats < 9.01,
          "loop starting at beat 8, 5 beats in = 9.0, got %f", beats);
}

/* Round 5 on hardware: a set loads with a clip SELECTED on every track and
 * nothing playing; choosing one both selects it and starts the transport.
 * The chosen track went c6 -> (stop) -> c3, and because only a ch-14 queue
 * counted as evidence of a launch, the one track the user actually picked was
 * left unanchored for good while three untouched tracks anchored fine. */
static void test_chosen_clip_on_a_stopped_track_anchors(void)
{
    printf("choosing a clip on a stopped track anchors THAT track\n");
    clip_state_t st; clip_state_reset(&st);

    /* Set loads: clip 6 selected on track 1, transport STOPPED. A selection
     * is not a launch -- it must not anchor. */
    clip_state_on_led(&st, 0x99, 97, 122, 0, 0, 1);
    CHECK(st.tracks[0].identity_valid && st.tracks[0].clip_slot == 5,
          "selection should give identity, got valid=%d slot=%d",
          st.tracks[0].identity_valid, st.tracks[0].clip_slot);
    CHECK(!st.tracks[0].anchor_valid,
          "a selection with the transport stopped must NOT anchor");

    /* User picks clip 3 on that track: the old one goes out, the new one
     * comes in, and the transport is now running. */
    clip_state_on_led(&st, 0x89, 97, 0, 0, 0, 1);      /* c6 off */
    clip_state_on_led(&st, 0x99, 94, 122, 4, 1, 1);    /* c3 on, running */
    CHECK(st.tracks[0].clip_slot == 2, "should now be clip 3, got %d",
          st.tracks[0].clip_slot);
    CHECK(st.tracks[0].anchor_valid,
          "the clip the user chose must be ANCHORED -- we watched it start");
    CHECK(st.tracks[0].anchor_pulse == 4,
          "anchor should be the pulse we saw it start, got %u",
          st.tracks[0].anchor_pulse);
}

/* The other half: a slot that merely DIFFERS from memory, with no witnessed
 * stop and no queue, is not evidence. We may just have been blind. */
static void test_unwitnessed_slot_change_does_not_anchor(void)
{
    printf("an unwitnessed slot change reports unknown phase, not a guess\n");
    clip_state_t st; clip_state_reset(&st);
    clip_state_on_led(&st, 0x9E, 93, 122, 100, 1, 1);
    clip_state_on_led(&st, 0x99, 93, 122, 120, 1, 1);
    CHECK(st.tracks[0].anchor_valid, "setup");

    /* A different slot appears with no stop and no queue observed. */
    clip_state_on_led(&st, 0x99, 95, 122, 900, 1, 1);
    CHECK(st.tracks[0].clip_slot == 3, "identity should follow");
    CHECK(!st.tracks[0].anchor_valid,
          "an unwitnessed change must leave phase UNKNOWN, not anchor at 900");
}

/* Measured: the only two ch-9 events that named a clip nobody was playing
 * arrived at mode 3 (Set Overview) and mode 0 (boot, mode not yet known).
 * Ungated they put track 4 on clip 5 AND anchored it -- wrong, and asserted
 * with confidence, for 14 beats until a Session refresh corrected it. */
static void test_non_session_modes_are_ignored(void)
{
    printf("pad events outside Session mode are not clip state\n");
    clip_state_t st; clip_state_reset(&st);

    clip_state_on_led(&st, 0x99, 72, 9, 0, 1, 3);   /* Set Overview */
    CHECK(!st.tracks[3].identity_valid,
          "a Set Overview pad is a SET, not a clip -- it must not set identity");

    clip_state_on_led(&st, 0x99, 72, 9, 0, 1, 0);   /* mode not yet known */
    CHECK(!st.tracks[3].identity_valid,
          "with the mode unknown we cannot say what the pads mean");

    clip_state_on_led(&st, 0x99, 72, 9, 0, 1, 2);   /* Note mode: pads are keys */
    CHECK(!st.tracks[3].identity_valid,
          "in Note mode the pads are the instrument, not the clip grid");

    /* The same event in Session mode is real. */
    clip_state_on_led(&st, 0x99, 72, 122, 0, 1, 1);
    CHECK(st.tracks[3].identity_valid && st.tracks[3].clip_slot == 4,
          "Session mode should give T4 c5, got valid=%d slot=%d",
          st.tracks[3].identity_valid, st.tracks[3].clip_slot);
}

/* The mode gate is SILENT by design, so a rejected event looks exactly like
 * no event. Recording the mode regardless is what makes a wrong grid
 * diagnosable instead of a mystery. */
static void test_ui_mode_is_recorded_even_when_rejected(void)
{
    printf("the rejected mode is still recorded, so the gate is visible\n");
    clip_state_t st; clip_state_reset(&st);
    clip_state_on_led(&st, 0x99, 72, 9, 0, 1, 3);
    CHECK(st.last_ui_mode == 3, "mode 3 should be recorded, got %d", st.last_ui_mode);
    CHECK(!st.tracks[3].identity_valid, "and still rejected");
}

/* Hardware: a track fell silent, the transport restarted, its clip came back
 * -- and it stayed unanchored for good, because on_transport_start cleared
 * saw_stop and destroyed the evidence that would have anchored it. */
static void test_clip_returning_after_a_start_anchors_at_the_start(void)
{
    printf("a clip appearing just after a Start anchors AT the start\n");
    clip_state_t st; clip_state_reset(&st);

    /* Playing, then its clip stops (a clip change in progress). */
    clip_state_on_led(&st, 0x99, 97, 122, 4000, 1, 1);   /* T1 c6 */
    clip_state_on_led(&st, 0x89, 97, 0, 4600, 1, 1);
    CHECK(st.tracks[0].clip_slot == -1, "setup: nothing playing");

    /* Transport restarts while this track has no clip. */
    clip_state_on_transport_start(&st);

    /* The clip comes up 40 pulses later. It began at the START, not here. */
    clip_state_on_led(&st, 0x99, 94, 122, 40, 1, 1);     /* T1 c3 */
    CHECK(st.tracks[0].clip_slot == 2, "identity should follow, got %d",
          st.tracks[0].clip_slot);
    CHECK(st.tracks[0].anchor_valid, "it must anchor, not sit unknown forever");
    CHECK(st.tracks[0].anchor_pulse == 0,
          "anchor should be the START (0), not where we noticed (40) -- "
          "anchoring at the sighting puts the lane 1.67 beats out; got %u",
          st.tracks[0].anchor_pulse);
}

/* The grace is bounded: a clip appearing well after a Start is somebody
 * pressing a pad, and that anchors where it actually started. */
static void test_start_grace_expires(void)
{
    printf("the post-Start grace expires, so a later launch is not back-dated\n");
    clip_state_t st; clip_state_reset(&st);
    clip_state_on_led(&st, 0x99, 97, 122, 4000, 1, 1);
    clip_state_on_led(&st, 0x89, 97, 0, 4600, 1, 1);
    clip_state_on_transport_start(&st);

    clip_state_on_led(&st, 0x99, 94, 122, 500, 1, 1);   /* ~20 beats later */
    CHECK(st.tracks[0].anchor_valid, "still anchors (we witnessed the stop)");
    CHECK(st.tracks[0].anchor_pulse == 500,
          "but at the sighting, NOT back-dated to 0; got %u",
          st.tracks[0].anchor_pulse);
}

/* Hardware: loading a set restarts the transport immediately, but identity
 * arrives from the Song.abl poll ~1.4 s later -- so the Start had nothing to
 * anchor and every track sat at "phase unknown" until the next Play. */
static void test_identity_arriving_after_a_start_still_anchors(void)
{
    printf("identity arriving after a Start still gets anchored\n");
    clip_state_t st; clip_state_reset(&st);

    /* Start with nothing known at all (a fresh set load). */
    clip_state_on_transport_start(&st);
    for (int t = 0; t < CLIP_TRACKS; t++)
        CHECK(!st.tracks[t].anchor_valid, "nothing to anchor yet");

    /* Seeding supplies identity a moment later, with no anchor of its own. */
    st.tracks[2].identity_valid = 1;
    st.tracks[2].clip_slot = 4;
    CHECK(!st.tracks[2].anchor_valid, "seeding must not anchor by itself");

    clip_state_anchor_pending(&st, 30, 1);
    CHECK(st.tracks[2].anchor_valid && st.tracks[2].anchor_pulse == 0,
          "should anchor at the Start (0), got valid=%d pulse=%u",
          st.tracks[2].anchor_valid, st.tracks[2].anchor_pulse);

    /* Bounded, and never while stopped. */
    clip_state_t st2; clip_state_reset(&st2);
    clip_state_on_transport_start(&st2);
    st2.tracks[0].identity_valid = 1; st2.tracks[0].clip_slot = 1;
    clip_state_anchor_pending(&st2, 5000, 1);
    CHECK(!st2.tracks[0].anchor_valid, "past the grace window it must not anchor");
    clip_state_anchor_pending(&st2, 10, 0);
    CHECK(!st2.tracks[0].anchor_valid, "stopped transport must not anchor");
}

/* Loading a set while the transport keeps running produces NO Start and no
 * witnessed launch -- observed on hardware, pulses ran straight through the
 * switch -- so a track had identity and an unknowable phase forever. The
 * playhead plus the page solves it from a single sighting. */
static void test_anchor_derived_from_playhead(void)
{
    printf("an anchor can be solved from one playhead sighting\n");
    clip_track_state_t tr = {0};
    tr.identity_valid = 1; tr.clip_slot = 0;

    /* 1/16 steps, 16-beat loop. Bar 3, step index 2 => step 34 => 8.5 beats
     * in. At pulse 1000 the anchor is 1000 - 8.5*24 = 796. */
    CHECK(clip_state_derive_anchor(&tr, 1000, 3, 2, 0.25, 0.0, 16.0),
          "should solve");
    CHECK(tr.anchor_pulse == 796, "anchor should be 796, got %u", tr.anchor_pulse);
    CHECK(tr.anchor_source == CLIP_ANCHOR_DERIVED, "and be marked derived");

    /* Round-trips: the solved anchor reproduces the position it came from. */
    double ph;
    CHECK(clip_phase_beats(&tr, 1000, 0.0, 16.0, &ph), "phase resolves");
    CHECK(ph > 8.49 && ph < 8.51, "should read back 8.5 beats, got %f", ph);

    /* Real evidence is never overwritten by a derived answer. */
    clip_track_state_t tr2 = {0};
    tr2.identity_valid = 1; tr2.clip_slot = 0;
    tr2.anchor_valid = 1; tr2.anchor_pulse = 42;
    tr2.anchor_source = CLIP_ANCHOR_START;
    CHECK(!clip_state_derive_anchor(&tr2, 1000, 3, 2, 0.25, 0.0, 16.0),
          "must not overwrite an existing anchor");
    CHECK(tr2.anchor_pulse == 42, "and must leave it alone");

    /* A page past the end of the loop means the bar and the clip disagree --
     * a stale page, or the wrong track. Refuse rather than fold it. */
    clip_track_state_t tr3 = {0};
    tr3.identity_valid = 1; tr3.clip_slot = 0;
    CHECK(!clip_state_derive_anchor(&tr3, 1000, 9, 0, 0.25, 0.0, 16.0),
          "bar 9 of a 4-bar loop is not a position; it must refuse");

    /* Never a negative anchor. At pulse 100 -- 4.17 beats since the Start --
     * a clip cannot be 8.5 beats into its loop unless its anchor predates
     * pulse 0, which is unrepresentable (the counter zeroes on Start). The
     * honest answer is to refuse, not to wrap into a plausible-looking
     * anchor that is a whole loop out. */
    clip_track_state_t tr4 = {0};
    tr4.identity_valid = 1; tr4.clip_slot = 0;
    CHECK(!clip_state_derive_anchor(&tr4, 100, 3, 2, 0.25, 0.0, 16.0),
          "a position earlier than the transport allows must be refused");
    CHECK(!tr4.anchor_valid, "and must leave the track unanchored");

    /* One loop later the same sighting is representable and solves. */
    clip_track_state_t tr5 = {0};
    tr5.identity_valid = 1; tr5.clip_slot = 0;
    CHECK(clip_state_derive_anchor(&tr5, 600, 3, 2, 0.25, 0.0, 16.0),
          "should solve once enough time has elapsed");
    CHECK(tr5.anchor_pulse == 396, "anchor should be 600-204=396, got %u",
          tr5.anchor_pulse);
}

int main(void)
{
    test_anchor_derived_from_playhead();
    test_identity_arriving_after_a_start_still_anchors();
    test_clip_returning_after_a_start_anchors_at_the_start();
    test_start_grace_expires();
    test_ui_mode_is_recorded_even_when_rejected();
    test_non_session_modes_are_ignored();
    test_chosen_clip_on_a_stopped_track_anchors();
    test_unwitnessed_slot_change_does_not_anchor();
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
