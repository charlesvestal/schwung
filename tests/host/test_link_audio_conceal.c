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

/* ---- The frame gate: a stall on EVERY track at once ----------------------
 *
 * A frame-level simulation of the shim's rebuild block: four tracks, each
 * read through its own concealment state, and Move's native mix standing in
 * the mailbox. The tracks are a steady 4000 each and native carries the same
 * sum (16000), so any step in the output is the gate's doing. */

#define NTRACKS 4
#define TRACK_LEVEL 4000

/* 1 = the gate (fixed), 0 = the policy before it: rebuild whenever ANY track
 * returned "filled", native at full level otherwise. */
static int sim_max_step(int use_gate, const int *starved, int nframes, int *out_silent_frames)
{
    la_conceal_t tr[NTRACKS];
    la_rebuild_gate_t g;
    for (int t = 0; t < NTRACKS; t++) la_conceal_reset(&tr[t]);
    la_rebuild_gate_reset(&g);

    int16_t prev_lr[2] = {0, 0};
    int have_prev = 0, max_step = 0, silent = 0;
    for (int fr = 0; fr < nframes; fr++) {
        int16_t buf[NTRACKS][FRAMES * 2];
        int valid[NTRACKS], n_real = 0, n_conc = 0;
        for (int t = 0; t < NTRACKS; t++) {
            if (!starved[fr]) {
                for (int i = 0; i < FRAMES * 2; i++) buf[t][i] = TRACK_LEVEL;
                la_conceal_real(&tr[t], buf[t], FRAMES);
                valid[t] = 1; n_real++;
            } else {
                valid[t] = la_conceal_fill(&tr[t], buf[t], FRAMES);
                if (valid[t]) n_conc++;
            }
        }
        int16_t native[FRAMES * 2], out[FRAMES * 2];
        for (int i = 0; i < FRAMES * 2; i++) native[i] = NTRACKS * TRACK_LEVEL;

        int rebuild, ramp_in = 0, xfade = 0;
        if (use_gate) {
            int d = la_rebuild_gate(&g, n_real, n_conc);
            rebuild = la_gate_is_rebuild(d);
            ramp_in = (d == LA_GATE_FALLBACK_RAMP_IN);
            xfade   = (d == LA_GATE_REBUILD_XFADE);
        } else {
            rebuild = (n_real + n_conc) > 0;
        }
        if (rebuild) {
            for (int i = 0; i < FRAMES * 2; i++) {
                int32_t v = 0;
                for (int t = 0; t < NTRACKS; t++) if (valid[t]) v += buf[t][i];
                out[i] = (int16_t)v;
            }
            if (xfade) la_mix_ramp_out(out, native, FRAMES);
        } else {
            memcpy(out, native, sizeof(out));
            if (ramp_in) la_ramp_in(out, FRAMES);
        }

        int all_zero = 1;
        for (int i = 0; i < FRAMES * 2; i++) {
            if (out[i] != 0) all_zero = 0;
            int d;
            if (i >= 2) d = abs((int)out[i] - (int)out[i - 2]);   /* same channel */
            else if (have_prev) d = abs((int)out[i] - (int)prev_lr[i]);
            else continue;
            if (d > max_step) max_step = d;
        }
        silent += all_zero;
        prev_lr[0] = out[FRAMES * 2 - 2];
        prev_lr[1] = out[FRAMES * 2 - 1];
        have_prev = 1;
    }
    if (out_silent_frames) *out_silent_frames = silent;
    return max_step;
}

static void test_a_shared_stall_never_steps_and_never_plays_silence(void)
{
    /* real x4, then every track starved for 1..10 blocks, then real x4. */
    for (int len = 1; len <= 10; len++) {
        int starved[32] = {0};
        int n = 4 + len + 4;
        for (int i = 4; i < 4 + len; i++) starved[i] = 1;
        int silent = 0;
        int step = sim_max_step(1, starved, n, &silent);
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "a %d-block shared stall steps by %d (a click) with the gate", len, step);
        /* A linear ramp over 128 frames moves ~125 per sample at this level. */
        CHECK(step <= 300, msg);
        snprintf(msg, sizeof(msg),
                 "a %d-block shared stall played %d silent block(s) though Move's mix was there",
                 len, silent);
        CHECK(silent == 0, msg);
    }
}

static void test_the_old_policy_did_click_positive_control(void)
{
    /* Without the gate, a stall long enough to exhaust concealment ended in
     * silence and then a full-scale cut into native. If this ever stops
     * failing, the simulation above has stopped measuring anything. */
    int starved[32] = {0};
    for (int i = 4; i < 4 + 8; i++) starved[i] = 1;
    int silent = 0;
    int step = sim_max_step(0, starved, 16, &silent);
    CHECK(step >= NTRACKS * TRACK_LEVEL - 200,
          "positive control: the pre-gate policy should hard-cut into native");
    CHECK(silent >= 2, "positive control: the pre-gate policy should play concealed silence");
}

static void test_one_track_stalling_alone_keeps_the_rebuild(void)
{
    la_rebuild_gate_t g;
    la_rebuild_gate_reset(&g);
    CHECK(la_rebuild_gate(&g, 4, 0) == LA_GATE_REBUILD, "steady state is not a rebuild");
    for (int i = 0; i < 8; i++)
        CHECK(la_gate_is_rebuild(la_rebuild_gate(&g, 3, i < 4 ? 1 : 0)),
              "one starved track bounced the whole mix to native");
}

static void test_the_gate_edges(void)
{
    la_rebuild_gate_t g;
    la_rebuild_gate_reset(&g);
    /* Engage with nothing arrived yet: native as it is, no ramp from zero. */
    CHECK(la_rebuild_gate(&g, 0, 0) == LA_GATE_FALLBACK,
          "the first frame of an engage with no audio yet was not a plain fallback");
    CHECK(la_rebuild_gate(&g, 4, 0) == LA_GATE_REBUILD_XFADE,
          "the first rebuilt frame after a fallback does not crossfade native out");
    CHECK(la_rebuild_gate(&g, 4, 0) == LA_GATE_REBUILD, "the crossfade lasted more than a block");
    /* Shared stall: the mirror block rebuilds, the next ramps native in. */
    CHECK(la_rebuild_gate(&g, 0, 4) == LA_GATE_REBUILD,
          "the first concealed block of a shared stall did not keep the mirror");
    CHECK(la_rebuild_gate(&g, 0, 4) == LA_GATE_FALLBACK_RAMP_IN,
          "the second block of a shared stall did not ramp native in");
    CHECK(la_rebuild_gate(&g, 0, 4) == LA_GATE_FALLBACK, "the ramp-in lasted more than a block");
    CHECK(la_rebuild_gate(&g, 0, 0) == LA_GATE_FALLBACK, "concealment giving up changed the path");
    /* A real outage that was never concealed falls back at once, ramped. */
    la_rebuild_gate_reset(&g);
    la_rebuild_gate(&g, 4, 0);
    CHECK(la_rebuild_gate(&g, 0, 0) == LA_GATE_FALLBACK_RAMP_IN,
          "an unconcealed starve after a rebuild did not ramp native in");
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
    test_a_shared_stall_never_steps_and_never_plays_silence();
    test_the_old_policy_did_click_positive_control();
    test_one_track_stalling_alone_keeps_the_rebuild();
    test_the_gate_edges();
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("test_link_audio_conceal: all passed\n");
    return 0;
}
