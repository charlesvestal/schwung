/*
 * test_audio_live.c -- the mirror's audio ring (audio_live_shm.h): a push
 * that wraps lands contiguously for the reader, the gain un-scales and clamps,
 * and write_pos counts every frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    /* The lap check: a reader that copied [from, to) and then found the
     * writer had moved on by most of a ring must drop the overwritten head. */
    audio_live_init(s);
    for (int b = 0; b < 8; b++) audio_live_push(s, blk, 128, 1.0f);       /* wp = 1024 */
    CHECK(audio_live_lapped(s, 0) == 0, "an unlapped read reported loss");
    /* reader copied [1024 - 512, 1024) and was preempted... */
    uint64_t rfrom = 1024 - 512;
    /* ...while the writer pushed nearly a whole ring */
    for (int b = 0; b < (AUDIO_LIVE_FRAMES / 128) - 10; b++) audio_live_push(s, blk, 128, 1.0f);
    uint64_t wp2 = __atomic_load_n(&s->write_pos, __ATOMIC_ACQUIRE);
    uint64_t lost = audio_live_lapped(s, rfrom);
    uint64_t want = wp2 + AUDIO_LIVE_PUSH_MAX - AUDIO_LIVE_FRAMES - rfrom;
    CHECK(lost == want && lost > 0 && lost <= 512,
          "lapped read: lost %llu, want %llu", (unsigned long long)lost, (unsigned long long)want);
    /* a reader lapped by more than its whole span loses all of it */
    for (int b = 0; b < 16; b++) audio_live_push(s, blk, 128, 1.0f);
    CHECK(audio_live_lapped(s, rfrom) >= 512, "a fully lapped read kept frames");
    /* the display server's own use: the lap check must follow the copy */
    {
        FILE *fp = fopen("../../src/host/display_server.c", "r");
        if (!fp) fp = fopen("src/host/display_server.c", "r");
        CHECK(fp != NULL, "cannot open display_server.c");
        if (fp) {
            static char src[1 << 20];
            size_t len = fread(src, 1, sizeof src - 1, fp); src[len] = 0; fclose(fp);
            const char *rd = strstr(src, "audio_live_read(audio_ptr");
            const char *lp = strstr(src, "audio_live_lapped(audio_ptr");
            CHECK(rd && lp && lp > rd, "display_server does not lap-check after audio_live_read");
        }
    }

    free(s);
    if (fails) { printf("test_audio_live: %d FAILED\n", fails); return 1; }
    printf("test_audio_live: all passed\n");
    return 0;
}
