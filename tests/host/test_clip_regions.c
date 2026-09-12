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
        int t, s, playing; double ls, ll;
        if (sscanf(line, "%d %d %d %lf %lf", &t, &s, &playing, &ls, &ll) != 5) continue;
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
    }
    fclose(f);
    CHECK(n == 24, "expected 24 clips in the fixture, read %d", n);

    /* An empty slot must NOT be invented. Getting this wrong turns a hole in
     * the grid into a clip with a zero-length loop, and a zero loop length
     * would make every phase calculation on it undefined. */
    printf("empty slots stay empty\n");
    for (int t = 0; t < CLIP_TRACKS; t++)
        for (int s = 0; s < CLIP_SLOTS; s++)
            if (!seen[t][s])
                CHECK(!rg.slots[t][s].exists,
                      "T%d c%d is empty in the file but was parsed as present",
                      t + 1, s + 1);

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
