/* Unit test: Link Audio starve concealment and track-depth alignment.
 *
 * Both exist because the obvious fix -- a deeper reserve -- is latency on
 * Move's tracks, and was rejected. So the properties pinned here are the ones
 * that make them free: concealment never steps (no click at either edge) and
 * gives up so a real outage still falls back; alignment only ever REMOVES
 * depth, only after it has been sustained, and blends instead of jumping.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link_audio_conceal.h"

#define FRAMES 128

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
} while (0)

/* A loud ramp, so a step anywhere is large and visible. */
static void ramp_block(int16_t *b, int16_t start, int16_t step)
{
    for (int f = 0; f < FRAMES; f++) {
        b[f * 2]     = (int16_t)(start + f * step);
        b[f * 2 + 1] = (int16_t)(-(start + f * step));
    }
}

static int max_step_across(const int16_t *a, const int16_t *b)
{
    /* |first frame of b - last frame of a|, both channels */
    int dl = abs((int)b[0] - (int)a[(FRAMES - 1) * 2]);
    int dr = abs((int)b[1] - (int)a[(FRAMES - 1) * 2 + 1]);
    return dl > dr ? dl : dr;
}

static void test_a_starve_with_nothing_heard_is_a_starve(void)
{
    la_conceal_t c;
    la_conceal_reset(&c);
    int16_t out[FRAMES * 2];
    CHECK(la_conceal_fill(&c, out, FRAMES) == 0,
          "concealed with no previous block -- startup must still fall back");
}

static void test_the_concealment_joins_what_was_heard_without_a_step(void)
{
    la_conceal_t c;
    la_conceal_reset(&c);
    int16_t real[FRAMES * 2], out[FRAMES * 2];
    ramp_block(real, 10000, 40);            /* ends near 15080 */
    la_conceal_real(&c, real, FRAMES);

    CHECK(la_conceal_fill(&c, out, FRAMES) == 1, "a single starve was not concealed");
    CHECK(max_step_across(real, out) == 0,
          "the concealed block does not start where the heard audio ended");
    CHECK(abs(out[(FRAMES - 1) * 2]) < 200 && abs(out[(FRAMES - 1) * 2 + 1]) < 200,
          "the concealed block does not fade to ~zero");
}

static void test_the_recovery_fades_in_from_zero(void)
{
    la_conceal_t c;
    la_conceal_reset(&c);
    int16_t real[FRAMES * 2], out[FRAMES * 2], next[FRAMES * 2];
    ramp_block(real, 10000, 40);
    la_conceal_real(&c, real, FRAMES);
    la_conceal_fill(&c, out, FRAMES);

    ramp_block(next, 20000, 10);            /* loud, unrelated */
    la_conceal_real(&c, next, FRAMES);
    CHECK(max_step_across(out, next) < 300,
          "the first real block after a concealment starts with a step");
    CHECK(next[(FRAMES - 1) * 2] == (int16_t)(20000 + (FRAMES - 1) * 10),
          "the fade-in did not reach full level by the end of the block");

    /* ...and only that one block is faded. */
    int16_t again[FRAMES * 2];
    ramp_block(again, 20000, 10);
    la_conceal_real(&c, again, FRAMES);
    CHECK(again[0] == 20000, "a second real block was faded too");
}

static void test_a_long_stall_gives_up_so_the_fallback_still_runs(void)
{
    la_conceal_t c;
    la_conceal_reset(&c);
    int16_t real[FRAMES * 2], out[FRAMES * 2];
    ramp_block(real, 1000, 1);
    la_conceal_real(&c, real, FRAMES);

    for (int i = 0; i < LA_CONCEAL_MAX_BLOCKS; i++)
        CHECK(la_conceal_fill(&c, out, FRAMES) == 1, "a short stall was not concealed");
    int silent = 1;
    for (int i = 0; i < FRAMES * 2; i++) if (out[i]) silent = 0;
    CHECK(silent, "blocks after the first concealment are not silence");
    CHECK(la_conceal_fill(&c, out, FRAMES) == 0,
          "a stall past the limit was concealed -- the all-starve fallback would never run");

    /* A real block resets the run. */
    la_conceal_real(&c, real, FRAMES);
    CHECK(la_conceal_fill(&c, out, FRAMES) == 1, "the run did not reset after real audio");
}

static void test_alignment_removes_only_sustained_excess_depth(void)
{
    /* Measured: track 2 at ~2580 samples, the rest at ~1450. */
    uint32_t avail[4] = { 1494, 2582, 1448, 1442 };
    int active[4] = { 1, 1, 1, 1 };
    uint32_t run[4] = { 0 };

    uint32_t skip = 0;
    for (int i = 0; i < LA_ALIGN_SUSTAIN_READS - 1; i++)
        skip |= la_align_skip(avail, active, 4, 1, &run[1]);
    CHECK(skip == 0, "aligned before the excess was sustained");
    skip = la_align_skip(avail, active, 4, 1, &run[1]);
    CHECK(skip == 2582 - 1442, "did not skip the deep track to the shallowest");
    CHECK((skip & 1) == 0, "skip is not a whole stereo frame");

    /* The shallow tracks are never moved, whatever happens. */
    for (int s = 0; s < 4; s++) {
        if (s == 1) continue;
        uint32_t any = 0;
        for (int i = 0; i < 2 * LA_ALIGN_SUSTAIN_READS; i++)
            any |= la_align_skip(avail, active, 4, s, &run[s]);
        CHECK(any == 0, "a track within a block of the shallowest was skipped");
    }
}

static void test_alignment_ignores_jitter_and_resets_on_a_dip(void)
{
    uint32_t avail[4] = { 1400, 1400 + LA_ALIGN_SLACK_SAMPLES, 1400, 1400 };
    int active[4] = { 1, 1, 1, 1 };
    uint32_t run = 0, skip = 0;
    for (int i = 0; i < 3 * LA_ALIGN_SUSTAIN_READS; i++)
        skip |= la_align_skip(avail, active, 4, 1, &run);
    CHECK(skip == 0, "a one-block difference was treated as misalignment");

    avail[1] = 3000;
    for (int i = 0; i < LA_ALIGN_SUSTAIN_READS - 1; i++)
        la_align_skip(avail, active, 4, 1, &run);
    avail[1] = 1400;                         /* dips back for one read */
    la_align_skip(avail, active, 4, 1, &run);
    avail[1] = 3000;
    CHECK(la_align_skip(avail, active, 4, 1, &run) == 0,
          "the sustain counter survived a dip");
}

static void test_alignment_needs_a_peer_and_skips_inactive_slots(void)
{
    uint32_t avail[4] = { 9000, 1000, 1000, 1000 };
    int one[4] = { 1, 0, 0, 0 };
    uint32_t run = 0, skip = 0;
    for (int i = 0; i < 2 * LA_ALIGN_SUSTAIN_READS; i++)
        skip |= la_align_skip(avail, one, 4, 0, &run);
    CHECK(skip == 0, "a lone track was aligned against inactive slots");
}

static void test_the_skip_crossfade_is_continuous_at_both_ends(void)
{
    int16_t from[FRAMES * 2], to[FRAMES * 2], out[FRAMES * 2];
    ramp_block(from, 5000, 20);
    ramp_block(to, -8000, 20);
    la_align_crossfade(from, to, out, FRAMES);
    CHECK(abs(out[0] - from[0]) < 200, "crossfade does not start at the old position");
    CHECK(out[(FRAMES - 1) * 2] == to[(FRAMES - 1) * 2],
          "crossfade does not end at the new position");
}

int main(void)
{
    test_a_starve_with_nothing_heard_is_a_starve();
    test_the_concealment_joins_what_was_heard_without_a_step();
    test_the_recovery_fades_in_from_zero();
    test_a_long_stall_gives_up_so_the_fallback_still_runs();
    test_alignment_removes_only_sustained_excess_depth();
    test_alignment_ignores_jitter_and_resets_on_a_dip();
    test_alignment_needs_a_peer_and_skips_inactive_slots();
    test_the_skip_crossfade_is_continuous_at_both_ends();
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("test_link_audio_conceal: all passed\n");
    return 0;
}
