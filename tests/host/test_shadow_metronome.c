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

/* Establish a prior pulse position, then cross a boundary. Returns what the
 * crossing call returned. */
static int click_at(int mode, int move_on, int prev, int now, int bpb, int level)
{
    shadow_metronome_reset();
    fill(0);
    shadow_metronome_render(buf, FRAMES, mode, move_on, 1, prev, bpb, level);
    fill(0);
    return shadow_metronome_render(buf, FRAMES, mode, move_on, 1, now, bpb, level);
}

static void expect(int got, int want, const char *what)
{
    if (got != want) { printf("FAIL: %s: got %d, want %d\n", what, got, want); failures++; }
}

int main(void)
{
    /* ---- gating ---- */
    expect(click_at(SHADOW_METRONOME_OFF, 1, 23, 24, 4, 100), 0,
           "OFF never clicks, even with Move's metronome on");
    expect(click_at(SHADOW_METRONOME_FOLLOW, 0, 23, 24, 4, 100), 0,
           "FOLLOW is silent while Move's metronome is off");
    expect(click_at(SHADOW_METRONOME_FOLLOW, 1, 23, 24, 4, 100), 1,
           "FOLLOW clicks while Move's metronome is on");
    expect(click_at(SHADOW_METRONOME_ON, 0, 23, 24, 4, 100), 1,
           "ON ignores Move's metronome entirely");

    /* ---- stopped transport ---- */
    {
        shadow_metronome_reset();
        fill(0);
        shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 1, 23, 4, 100);
        fill(0);
        int r = shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 0, 24, 4, 100);
        expect(r, 0, "a stopped transport is silent");
        /* And the stop must have FORGOTTEN the position, so resuming at the
         * same pulse cannot replay the boundary it already passed. */
        fill(0);
        r = shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 1, 24, 4, 100);
        expect(r, 0, "resuming does not replay a boundary crossed before the stop");
    }

    /* ---- no prior position: switching on mid-bar must not click ---- */
    {
        shadow_metronome_reset();
        fill(0);
        int r = shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 1, 96, 4, 100);
        expect(r, 0, "the first call after a reset does not click, even on a boundary pulse");
    }

    /* ---- level 0 is silent ---- */
    expect(click_at(SHADOW_METRONOME_ON, 1, 23, 24, 4, 0), 0,
           "level 0 produces no audible click");

    /* ---- downbeat and offbeat are different sounds ---- */
    {
        int16_t down[N], beat[N];
        click_at(SHADOW_METRONOME_ON, 1, 95, 96, 4, 100);   /* pulse 96 = downbeat */
        memcpy(down, buf, sizeof(down));
        click_at(SHADOW_METRONOME_ON, 1, 23, 24, 4, 100);   /* pulse 24 = beat 1 */
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
        shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 1, 23, 4, 100);
        fill(1000);
        shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_ON, 1, 1, 24, 4, 100);
        /* A fresh trigger starts at phase 0, and sin(0) is 0 — so the very
         * first sample must come back as the untouched input. An overwrite
         * would zero it. */
        if (buf[0] != 1000 || buf[1] != 1000) {
            printf("FAIL: mixing must preserve the input; sample 0 came back %d,%d not 1000,1000\n",
                   buf[0], buf[1]);
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
        shadow_metronome_render(buf, FRAMES, SHADOW_METRONOME_OFF, 1, 1, 24, 4, 100);
        if (changed_from(1234)) {
            printf("FAIL: OFF must leave the buffer completely untouched\n");
            failures++;
        }
    }

    /* ---- degenerate arguments ---- */
    expect(shadow_metronome_render(NULL, FRAMES, SHADOW_METRONOME_ON, 1, 1, 24, 4, 100), 0,
           "a NULL buffer is refused");
    expect(shadow_metronome_render(buf, 0, SHADOW_METRONOME_ON, 1, 1, 24, 4, 100), 0,
           "a zero-length block is refused");

    if (failures) { printf("test_shadow_metronome: FAIL (%d)\n", failures); return 1; }
    printf("test_shadow_metronome: PASS\n");
    return 0;
}
