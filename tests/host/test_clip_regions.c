/*
 * test_clip_regions — parses a REAL Song.abl and checks every clip against
 * expectations produced by an INDEPENDENT reader (python's json module), not
 * by the code under test. A fixture written from our own parser's output
 * would only prove the parser agrees with itself.
 *
 * The fixture keeps a few notes per clip on purpose. `notes` holds objects
 * carrying "startTime" and "duration"; a fixture with none would not exercise
 * the depth tracking that stops those being read as loop geometry, which is
 * the single thing most likely to break here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clip_regions.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static int close_enough(double a, double b) { return (a - b) < 1e-6 && (b - a) < 1e-6; }

int main(void)
{
    clip_regions_t rg;
    printf("parse a real Song.abl\n");
    if (!clip_regions_parse_file("../fixtures/song_abl_sample.json", &rg)) {
        printf("  FAIL: parse failed outright\n");
        return 1;
    }
    CHECK(rg.valid, "regions should be valid");
    CHECK(close_enough(rg.step_resolution, 0.25),
          "1/16 should be 0.25 beats, got %f", rg.step_resolution);

    FILE *f = fopen("../fixtures/song_abl_expected.txt", "r");
    if (!f) { printf("  FAIL: no expectations file\n"); return 1; }

    int seen[CLIP_TRACKS][CLIP_SLOTS];
    memset(seen, 0, sizeof(seen));

    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        int t, s, playing, nc, fn; double ls, ll;
        if (sscanf(line, "%d %d %d %lf %lf %d %d",
                   &t, &s, &playing, &ls, &ll, &nc, &fn) != 7) continue;
        n++;
        seen[t][s] = 1;
        const clip_region_t *r = &rg.slots[t][s];
        CHECK(r->exists, "T%d c%d should exist", t + 1, s + 1);
        CHECK(r->is_playing == playing, "T%d c%d is_playing %d, want %d",
              t + 1, s + 1, r->is_playing, playing);
        CHECK(close_enough(r->loop_start, ls), "T%d c%d loop_start %f, want %f",
              t + 1, s + 1, r->loop_start, ls);
        CHECK(close_enough(r->loop_len, ll), "T%d c%d loop_len %f, want %f",
              t + 1, s + 1, r->loop_len, ll);
        /* The fingerprint's CONTENT half. A clip copied into another slot has
         * the same geometry and different notes -- geometry alone cannot tell
         * them apart, which is the whole reason these two fields exist.
         *
         * Asserted per clip against python's count, not just on one clip: a
         * scan that ran past the clip's own span would still get T1c1 right
         * and every clip after it wrong, and the fixture holds 24 of them
         * each with exactly 3 notes, so an unbounded count reads as 72-ish. */
        CHECK(r->note_count == nc, "T%d c%d note_count %d, want %d",
              t + 1, s + 1, r->note_count, nc);
        CHECK(r->first_note == fn, "T%d c%d first_note %d, want %d",
              t + 1, s + 1, r->first_note, fn);
    }
    fclose(f);
    CHECK(n == 24, "expected 24 clips in the fixture, read %d", n);

    /* An empty slot must NOT be invented. Getting this wrong turns a hole in
     * the grid into a clip with a zero-length loop, and a zero loop length
     * would make every phase calculation on it undefined. */
    printf("empty slots stay empty\n");
    for (int t = 0; t < CLIP_TRACKS; t++)
        for (int s = 0; s < CLIP_SLOTS; s++)
            if (!seen[t][s]) {
                CHECK(!rg.slots[t][s].exists,
                      "T%d c%d is empty in the file but was parsed as present",
                      t + 1, s + 1);
                /* And an empty slot's fingerprint is the ABSENT one: -1, never
                 * 0. Note 0 is a real note number, so a zeroed first_note
                 * would be a lane matching a clip it was never recorded
                 * against. This also catches a note scan that overran its
                 * clip's span -- an empty slot is the one place a stray count
                 * has nowhere to hide. */
                CHECK(rg.slots[t][s].note_count == 0,
                      "T%d c%d is empty but reported note_count %d",
                      t + 1, s + 1, rg.slots[t][s].note_count);
                CHECK(rg.slots[t][s].first_note == -1,
                      "T%d c%d is empty but reported first_note %d (0 is a "
                      "real note number; absent must be -1)",
                      t + 1, s + 1, rg.slots[t][s].first_note);
            }

    /* Seeding: fills only what we have NOT observed, and never anchors. */
    printf("seeding fills gaps without overwriting observations or anchoring\n");
    clip_state_t st; clip_state_reset(&st);
    /* Pretend the LED stream already told us track 1 is on clip 8. */
    st.tracks[0].identity_valid = 1;
    st.tracks[0].clip_slot = 7;
    st.tracks[0].anchor_valid = 1;
    st.tracks[0].anchor_pulse = 999;

    clip_regions_seed_state(&rg, &st);

    CHECK(st.tracks[0].clip_slot == 7,
          "an observed track must NOT be overwritten by the file, got %d",
          st.tracks[0].clip_slot);
    CHECK(st.tracks[0].anchor_pulse == 999, "and its anchor must survive");
    for (int t = 1; t < CLIP_TRACKS; t++) {
        CHECK(st.tracks[t].identity_valid, "track %d should be seeded", t + 1);
        CHECK(st.tracks[t].clip_slot == 2,
              "track %d should be seeded to clip 3, got %d", t + 1,
              st.tracks[t].clip_slot);
        CHECK(!st.tracks[t].anchor_valid,
              "seeding must NEVER anchor -- the file says WHAT is selected, "
              "never WHEN it started");
    }

    /* A seeded track is exactly what MIDI Start needs in order to anchor:
     * that ordering is the whole reason seeding exists. */
    printf("a Start anchors what was seeded\n");
    clip_state_on_transport_start(&st);
    for (int t = 1; t < CLIP_TRACKS; t++) {
        CHECK(st.tracks[t].anchor_valid && st.tracks[t].anchor_pulse == 0,
              "track %d should anchor to 0 on Start, got valid=%d pulse=%u",
              t + 1, st.tracks[t].anchor_valid, st.tracks[t].anchor_pulse);
    }

    /* The loop-vs-region branch. Every clip in the sample set has its loop
     * ENABLED and coincident with the region, so the fixture cannot tell the
     * two branches apart -- a mutation that always took the region went
     * undetected. These two say which is which.
     *
     * The case is real: a loop starting at bar 3 inside a longer clip is
     * ordinary, and phase is relative to loop_start, so taking the region
     * would put every lane a bar or more out. */
    printf("loop wins over region when enabled, and region when not\n");
    {
        static const char withloop[] =
            "{\"tracks\":[{\"clipSlots\":[{\"clip\":{\"isPlaying\":true,"
            "\"region\":{\"start\":0.0,\"end\":32.0,"
            "\"loop\":{\"start\":8.0,\"end\":16.0,\"isEnabled\":true}},"
            "\"notes\":[{\"noteNumber\":36,\"startTime\":0.0,\"duration\":0.25}]}}]}]}";
        clip_regions_t r2;
        CHECK(clip_regions_parse(withloop, sizeof(withloop) - 1, &r2), "should parse");
        CHECK(close_enough(r2.slots[0][0].loop_start, 8.0),
              "enabled loop start should be 8.0, got %f", r2.slots[0][0].loop_start);
        CHECK(close_enough(r2.slots[0][0].loop_len, 8.0),
              "enabled loop len should be 8.0 (16-8), got %f", r2.slots[0][0].loop_len);

        static const char noloop[] =
            "{\"tracks\":[{\"clipSlots\":[{\"clip\":{\"isPlaying\":true,"
            "\"region\":{\"start\":2.0,\"end\":6.0,"
            "\"loop\":{\"start\":8.0,\"end\":16.0,\"isEnabled\":false}},"
            "\"notes\":[]}}]}]}";
        clip_regions_t r3;
        CHECK(clip_regions_parse(noloop, sizeof(noloop) - 1, &r3), "should parse");
        CHECK(close_enough(r3.slots[0][0].loop_start, 2.0),
              "disabled loop should fall back to region start 2.0, got %f",
              r3.slots[0][0].loop_start);
        CHECK(close_enough(r3.slots[0][0].loop_len, 4.0),
              "disabled loop should fall back to region len 4.0, got %f",
              r3.slots[0][0].loop_len);
    }

    /* The earliest note, not the textually first. Every clip in the sample set
     * happens to list its notes in time order, so the fixture cannot tell a
     * min-by-startTime from a take-the-first -- and a take-the-first is the
     * cheaper thing to write.
     *
     * The same document carries a non-empty `envelopes`, whose breakpoint
     * objects sit at exactly the depth a note object does. If the scan is
     * armed by anything other than the clip's own "notes" key, these three
     * breakpoints land in the count. */
    printf("first_note is the earliest note, and envelopes are not notes\n");
    {
        static const char ooo[] =
            "{\"tracks\":[{\"clipSlots\":[{\"clip\":{\"isPlaying\":true,"
            "\"region\":{\"start\":0.0,\"end\":8.0,"
            "\"loop\":{\"start\":0.0,\"end\":8.0,\"isEnabled\":true}},"
            "\"notes\":["
            "{\"noteNumber\":72,\"startTime\":4.0,\"duration\":0.25},"
            "{\"noteNumber\":0,\"startTime\":1.0,\"duration\":0.25},"
            "{\"noteNumber\":60,\"startTime\":2.0,\"duration\":0.25}],"
            "\"envelopes\":[{\"parameterId\":7,\"breakpoints\":["
            "{\"time\":0.0,\"value\":0.0},{\"time\":1.0,\"value\":1.0},"
            "{\"time\":2.0,\"value\":0.5}]}]}}]}]}";
        clip_regions_t r7;
        CHECK(clip_regions_parse(ooo, sizeof(ooo) - 1, &r7), "should parse");
        CHECK(r7.slots[0][0].note_count == 3,
              "note_count is %d, want 3 -- envelopes are not notes",
              r7.slots[0][0].note_count);
        /* 0, and it must arrive as 0 rather than as "absent": the earliest
         * note here IS note 0, which is exactly the value the absent case
         * uses -1 to stay clear of. */
        CHECK(r7.slots[0][0].first_note == 0,
              "first_note is %d, want 0 (the earliest note, note number 0)",
              r7.slots[0][0].first_note);
    }

    /* A clip with notes must not leak them into the NEXT clip, in either
     * direction: the count belongs to the span it was read from. */
    printf("a clip's notes stay inside that clip\n");
    {
        static const char pair[] =
            "{\"tracks\":[{\"clipSlots\":[{\"clip\":{\"isPlaying\":true,"
            "\"region\":{\"start\":0.0,\"end\":4.0,"
            "\"loop\":{\"start\":0.0,\"end\":4.0,\"isEnabled\":true}},"
            "\"notes\":[{\"noteNumber\":36,\"startTime\":0.0,\"duration\":0.25},"
            "{\"noteNumber\":38,\"startTime\":1.0,\"duration\":0.25}]}},"
            "{\"clip\":{\"isPlaying\":false,"
            "\"region\":{\"start\":0.0,\"end\":4.0,"
            "\"loop\":{\"start\":0.0,\"end\":4.0,\"isEnabled\":true}},"
            "\"notes\":[]}}]}]}";
        clip_regions_t r8;
        CHECK(clip_regions_parse(pair, sizeof(pair) - 1, &r8), "should parse");
        CHECK(r8.slots[0][0].note_count == 2 && r8.slots[0][0].first_note == 36,
              "clip 1 reports %d notes / first %d, want 2 / 36",
              r8.slots[0][0].note_count, r8.slots[0][0].first_note);
        CHECK(r8.slots[0][1].exists, "clip 2 should exist");
        CHECK(r8.slots[0][1].note_count == 0 && r8.slots[0][1].first_note == -1,
              "clip 2 has no notes but reports %d / first %d -- it inherited "
              "its neighbour's",
              r8.slots[0][1].note_count, r8.slots[0][1].first_note);
    }

    /* A set with NO playing clips must seed nothing -- and must not leave
     * a previous set's answers standing. The caller resets on a set change;
     * this pins that seeding alone cannot invent identity. */
    printf("a set with nothing selected seeds nothing\n");
    {
        static const char empty[] =
            "{\"tracks\":[{\"clipSlots\":[{\"clip\":null},{\"clip\":null}]}]}";
        clip_regions_t r4;
        CHECK(clip_regions_parse(empty, sizeof(empty) - 1, &r4), "should parse");
        clip_state_t st4; clip_state_reset(&st4);
        clip_regions_seed_state(&r4, &st4);
        for (int t = 0; t < CLIP_TRACKS; t++)
            CHECK(!st4.tracks[t].identity_valid,
                  "track %d must stay unknown when the set has no clips", t + 1);
    }

    /* Deleted vs newly-copied: both are absent from the file, and telling
     * them apart is the whole point. Observed on hardware -- a deleted clip
     * left the track asserting it was still playing. */
    printf("a deleted clip drops identity; a new one does not\n");
    {
        static const char two[] =
            "{\"tracks\":[{\"clipSlots\":[{\"clip\":{\"isPlaying\":true,"
            "\"region\":{\"start\":0.0,\"end\":4.0,"
            "\"loop\":{\"start\":0.0,\"end\":4.0,\"isEnabled\":true}},\"notes\":[]}},"
            "{\"clip\":{\"isPlaying\":false,"
            "\"region\":{\"start\":0.0,\"end\":4.0,"
            "\"loop\":{\"start\":0.0,\"end\":4.0,\"isEnabled\":true}},\"notes\":[]}}]}]}";
        static const char one[] =
            "{\"tracks\":[{\"clipSlots\":[{\"clip\":{\"isPlaying\":true,"
            "\"region\":{\"start\":0.0,\"end\":4.0,"
            "\"loop\":{\"start\":0.0,\"end\":4.0,\"isEnabled\":true}},\"notes\":[]}},"
            "{\"clip\":null}]}]}";
        clip_regions_t before, after;
        CHECK(clip_regions_parse(two, sizeof(two) - 1, &before), "parse before");
        CHECK(clip_regions_parse(one, sizeof(one) - 1, &after), "parse after");

        /* We believe track 1 is playing slot 2, which has just been deleted. */
        clip_state_t st5; clip_state_reset(&st5);
        st5.tracks[0].identity_valid = 1;
        st5.tracks[0].clip_slot = 1;
        st5.tracks[0].anchor_valid = 1;
        st5.tracks[0].anchor_pulse = 100;
        uint32_t del5 = 0;
        clip_regions_forget_deleted(&before, &after, &st5, &del5);
        /* THE MASK IS WHAT THE LANE SIDE ACTS ON, and it is not the same
         * question as identity: identity is about the one clip a track is
         * playing, while a lane can be bound to any of the eight positions.
         * So the mask covers every slot, not just the identified one. */
        CHECK(del5 == (1u << (0 * CLIP_SLOTS + 1)),
              "deleted mask is 0x%x, want only T1 c2 (bit %d)",
              del5, 0 * CLIP_SLOTS + 1);
        CHECK(st5.tracks[0].clip_slot == -1,
              "a deleted clip must stop being reported as playing, got %d",
              st5.tracks[0].clip_slot);
        CHECK(!st5.tracks[0].anchor_valid,
              "and its anchor describes a clip that no longer exists");

        /* The reverse: a clip that appears (a copy Move has now saved) must
         * not disturb anything. */
        clip_state_t st6; clip_state_reset(&st6);
        st6.tracks[0].identity_valid = 1;
        st6.tracks[0].clip_slot = 1;
        st6.tracks[0].anchor_valid = 1;
        st6.tracks[0].anchor_pulse = 100;
        uint32_t del6 = 0xdeadbeefu;   /* must be OVERWRITTEN, not OR'd into */
        clip_regions_forget_deleted(&after, &before, &st6, &del6);
        CHECK(del6 == 0,
              "a clip APPEARING reported deletions: 0x%x", del6);
        CHECK(st6.tracks[0].clip_slot == 1,
              "a clip APPEARING must not drop identity, got %d",
              st6.tracks[0].clip_slot);
        CHECK(st6.tracks[0].anchor_valid, "nor its anchor");

        /* A clip deleted at a position the track is not playing. Identity has
         * nothing to say about it -- and a lane bound there still has to be
         * told, or the lane of any clip but the live one is never orphaned. */
        clip_state_t st7; clip_state_reset(&st7);
        st7.tracks[0].identity_valid = 1;
        st7.tracks[0].clip_slot = 0;      /* playing c1; c2 is the one deleted */
        st7.tracks[0].anchor_valid = 1;
        uint32_t del7 = 0;
        clip_regions_forget_deleted(&before, &after, &st7, &del7);
        CHECK(del7 == (1u << 1),
              "a deletion away from the playhead did not reach the mask (0x%x)",
              del7);
        CHECK(st7.tracks[0].clip_slot == 0 && st7.tracks[0].anchor_valid,
              "...and it must not disturb the identity of the clip that IS "
              "playing (slot %d, anchor %d)",
              st7.tracks[0].clip_slot, st7.tracks[0].anchor_valid);

        /* A NULL mask is legal: a caller that only wants the identity half
         * must not have to invent an out-parameter. */
        clip_state_t st8; clip_state_reset(&st8);
        st8.tracks[0].identity_valid = 1;
        st8.tracks[0].clip_slot = 1;
        clip_regions_forget_deleted(&before, &after, &st8, NULL);
        CHECK(st8.tracks[0].clip_slot == -1, "a NULL mask broke the identity half");
    }

    /* Geometry comparison: a re-save that changes nothing must not read as a
     * change, or every periodic save wipes a running measurement. */
    printf("an unchanged re-parse is not a geometry change\n");
    {
        clip_regions_t again;
        CHECK(clip_regions_parse_file("../fixtures/song_abl_sample.json", &again),
              "re-parse");
        CHECK(!clip_regions_geometry_differs(&rg, &again),
              "the same file must not read as changed");
        clip_regions_t shorter = again;
        shorter.slots[0][2].loop_len = 8.0;
        CHECK(clip_regions_geometry_differs(&rg, &shorter),
              "a changed loop length must read as changed");
        clip_regions_t gone = again;
        gone.slots[0][2].exists = 0;
        CHECK(clip_regions_geometry_differs(&rg, &gone),
              "a removed clip must read as changed");
        clip_regions_t played = again;
        played.slots[0][0].is_playing = !played.slots[0][0].is_playing;
        CHECK(!clip_regions_geometry_differs(&rg, &played),
              "isPlaying is the restored SELECTION and has no bearing on how a "
              "phase sample is scored -- it must not invalidate a tally");
    }

    /* A truncated file is a FAILURE, not a smaller document. */
    printf("a truncated document is refused, not half-believed\n");
    clip_regions_t bad;
    CHECK(!clip_regions_parse("{\"tracks\":", 10, &bad) || !bad.valid,
          "a truncated document must not parse as valid");
    CHECK(!clip_regions_parse_file("../fixtures/does_not_exist.json", &bad),
          "a missing file must fail");

    if (failures) { printf("\n%d CHECK(s) failed\n", failures); return 1; }
    printf("\nall clip_regions checks passed (%d clips)\n", n);
    return 0;
}
