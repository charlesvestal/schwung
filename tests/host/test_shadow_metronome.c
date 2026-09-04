/* The click generator's gating and mixing.
 *
 * metronome_click.h covers the arithmetic. What is left here is the part that
 * decides WHETHER to click, and that is where a silent wrong answer lives: a
 * FOLLOW that ignores Move's state clicks over a user who turned the metronome
 * off, and an overwrite-instead-of-mix silences the music under every click.
 */
#include <stdio.h>
#include <string.h>
#include "shadow_metronome.h"

#define FRAMES 128
#define N (FRAMES * 2)

static int failures = 0;
static int16_t buf[N];

static void fill(int16_t v) { for (int i = 0; i < N; i++) buf[i] = v; }
static int changed_from(int16_t v) {
    for (int i = 0; i < N; i++) if (buf[i] != v) return 1;
    return 0;
}

/* Establish a prior pulse position, cross a boundary, then render enough
 * blocks for the Link-Audio-compensated click to actually land.
 *
 * The crossing block itself is now SILENT by design: the click is scheduled
 * METRONOME_LA_COMP_FRAMES ahead so it meets Move's audio rather than leading
 * it. Returns 1 if a click sounded within the compensation window.
 *
 * `buf` is left holding the block the click landed in, so callers can inspect
 * it. */
#define BLOCKS_TO_LAND (METRONOME_LA_COMP_FRAMES / FRAMES + 2)

static int click_at(int mode, int move_on, int prev, int now, int bpb, int level)
{
    shadow_metronome_reset();
    fill(0);
    shadow_metronome_render(buf, FRAMES, mode, move_on, 1, prev, bpb, level);
    fill(0);
    if (shadow_metronome_render(buf, FRAMES, mode, move_on, 1, now, bpb, level)) {
        printf("FAIL: the crossing block must be silent — the click is held back "
               "to meet Move's Link Audio\n");
        failures++;
    }
    for (int b = 0; b < BLOCKS_TO_LAND; b++) {
        fill(0);
        if (shadow_metronome_render(buf, FRAMES, mode, move_on, 1, now, bpb, level))
            return 1;
    }
    return 0;
}

static void expect(int got, int want, const char *what)
{
    if (got != want) { printf("FAIL: %s: got %d, want %d\n", what, got, want); failures++; }
}

int main(void)
{
    /* ---- gating ---- */
    expect(click_at(SHADOW_METRONOME_OFF, 1, 24, 25, 4, 100), 0,
           "OFF never clicks, even with Move's metronome on");
    expect(click_at(SHADOW_METRONOME_FOLLOW, 0, 24, 25, 4, 100), 0,
           "FOLLOW is silent while Move's metronome is off");
    expect(click_at(SHADOW_METRONOME_FOLLOW, 1, 24, 25, 4, 100), 1,
           "FOLLOW clicks while Move's metronome is on");
    expect(click_at(SHADOW_METRONOME_ON, 0, 24, 25, 4, 100), 1,
           "ON ignores Move's metronome entirely");

    /* ---- stopped transport ---- */
    {
        shadow_metronome_reset();
        fill(0);
        shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 1, 24, 4, 100);
        fill(0);
        int r = shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 0, 25, 4, 100);
        expect(r, 0, "a stopped transport is silent");
        /* And the stop must have FORGOTTEN the position, so resuming at the
         * same pulse cannot replay the boundary it already passed. */
        fill(0);
        r = shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 1, 25, 4, 100);
        expect(r, 0, "resuming does not replay a boundary crossed before the stop");
    }

    /* ---- no prior position: switching on mid-bar must not click ---- */
    {
        shadow_metronome_reset();
        fill(0);
        int r = shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 1, 97, 4, 100);
        expect(r, 0, "the first call after a reset does not click, even on a boundary pulse");
    }

    /* ---- level 0 is silent ---- */
    expect(click_at(SHADOW_METRONOME_ON, 1, 24, 25, 4, 0), 0,
           "level 0 produces no audible click");

    /* ---- downbeat and offbeat are different sounds ---- */
    {
        int16_t down[N], beat[N];
        click_at(SHADOW_METRONOME_ON, 1, 96, 97, 4, 100);   /* pulse 97 = downbeat */
        memcpy(down, buf, sizeof(down));
        click_at(SHADOW_METRONOME_ON, 1, 24, 25, 4, 100);   /* pulse 25 = beat 1 */
        memcpy(beat, buf, sizeof(beat));
        if (memcmp(down, beat, sizeof(down)) == 0) {
            printf("FAIL: the downbeat must not sound identical to an offbeat\n");
            failures++;
        }
    }

    /* ---- it MIXES, it does not overwrite ---- */
    {
        shadow_metronome_reset();
        fill(1000);
        shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 1, 24, 4, 100);
        shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 1, 25, 4, 100);
        /* Run out the compensation delay, refilling so the landing block is
         * clean input. */
        for (int b = 0; b < BLOCKS_TO_LAND; b++) {
            fill(1000);
            if (shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 1, 25, 4, 100)) break;
        }
        /* A fresh trigger starts at phase 0, and sin(0) is 0 — so the sample
         * the click starts on must come back as the untouched input. An
         * overwrite would zero it. The trigger lands at offset
         * METRONOME_LA_COMP_FRAMES % FRAMES within its block. */
        {
            int k = (METRONOME_LA_COMP_FRAMES % FRAMES) * 2;
            if (buf[k] != 1000 || buf[k+1] != 1000) {
                printf("FAIL: mixing must preserve the input at the trigger sample; "
                       "got %d,%d not 1000,1000\n", buf[k], buf[k+1]);
                failures++;
            }
        }
        if (0) {
            failures++;
        }
        if (!changed_from(1000)) {
            printf("FAIL: the click never reached the buffer\n");
            failures++;
        }
    }

    /* ---- an untouched block really is untouched ---- */
    {
        shadow_metronome_reset();
        fill(1234);
        shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_OFF, 1, 1, 25, 4, 100);
        if (changed_from(1234)) {
            printf("FAIL: OFF must leave the buffer completely untouched\n");
            failures++;
        }
    }

    /* ---- the Link Audio compensation delay, measured in frames ----
     *
     * The whole point of the fix: measured on hardware, the click led Move's
     * audio by a constant 19.6 ms on top of the one-pulse phase error. If this
     * delay silently became 0, the click would go back to leading the music
     * and every other assertion here would still pass.
     */
    {
        shadow_metronome_reset();
        fill(0);
        shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 1, 24, 4, 100);
        int elapsed = -1;
        for (int b = 0; b < 40; b++) {
            fill(0);
            if (shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 1, 25, 4, 100)) {
                /* first non-zero sample in this block = the trigger offset */
                int k = -1;
                for (int i = 0; i < N; i++) if (buf[i] != 0) { k = i/2; break; }
                elapsed = b*FRAMES + (k < 0 ? 0 : k);
                break;
            }
        }
        /* sin(0)=0, so the first AUDIBLE sample is one after the trigger. */
        if (elapsed < 0 || elapsed - 1 != METRONOME_LA_COMP_FRAMES) {
            printf("FAIL: the click must be held back %d frames to meet Move's audio, "
                   "measured %d\n", METRONOME_LA_COMP_FRAMES, elapsed - 1);
            failures++;
        }
    }

    /* ---- degenerate arguments ---- */
    expect(shadow_metronome_render(NULL, FRAMES, SHADOW_METRONOME_ON, 1, 1, 25, 4, 100), 0,
           "a NULL buffer is refused");
    expect(shadow_metronome_render(buf, 0, SHADOW_METRONOME_ON, 1, 1, 25, 4, 100), 0,
           "a zero-length block is refused");

    if (failures) { printf("test_shadow_metronome: FAIL (%d)\n", failures); return 1; }
    printf("test_shadow_metronome: PASS\n");
    return 0;
}
