/*
 * test_audio_live.c -- the mirror's audio ring (audio_live_shm.h): a push
 * that wraps lands contiguously for the reader, the gain un-scales and clamps,
 * and write_pos counts every frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include "audio_live_shm.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    audio_live_shm_t *s = malloc(sizeof *s);
    audio_live_init(s);
    int16_t blk[128 * 2], out[4096 * 2];
    uint64_t f = 0;
    /* fill past one wrap with a ramp that encodes the frame index */
    for (int b = 0; b < (AUDIO_LIVE_FRAMES / 128) + 7; b++) {
        for (int i = 0; i < 128; i++) { blk[i * 2] = (int16_t)(f & 0x7FFF); blk[i * 2 + 1] = (int16_t)-(int16_t)(f & 0x7FFF); f++; }
        audio_live_push(s, blk, 128, 1.0f);
    }
    uint64_t wp = __atomic_load_n(&s->write_pos, __ATOMIC_ACQUIRE);
    CHECK(wp == f, "write_pos %llu, pushed %llu", (unsigned long long)wp, (unsigned long long)f);
    /* the newest 3000 frames straddle the wrap point */
    uint64_t from = wp - 3000;
    int n = audio_live_read(s, from, wp, out);
    int bad = 0;
    for (int i = 0; i < n; i++)
        if (out[i * 2] != (int16_t)((from + i) & 0x7FFF) || out[i * 2 + 1] != (int16_t)-(int16_t)((from + i) & 0x7FFF)) bad++;
    CHECK(n == 3000 && bad == 0, "read across the wrap: %d frames, %d wrong", n, bad);

    /* gain: 1/mv un-scaling, clamped at full scale */
    audio_live_init(s);
    for (int i = 0; i < 128; i++) { blk[i * 2] = 1000; blk[i * 2 + 1] = -20000; }
    audio_live_push(s, blk, 128, 4.0f);
    audio_live_read(s, 0, 128, out);
    CHECK(out[0] == 4000 && out[1] == -32768 && out[254] == 4000, "gain/clamp: %d %d", out[0], out[1]);
    CHECK(s->sample_rate == 44100 && s->version == AUDIO_LIVE_VERSION, "header");

    free(s);
    if (fails) { printf("test_audio_live: %d FAILED\n", fails); return 1; }
    printf("test_audio_live: all passed\n");
    return 0;
}
