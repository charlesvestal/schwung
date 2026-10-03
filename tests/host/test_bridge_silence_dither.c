/* bridge_silence_dither.h: a silent bridge block must not reach Move as
 * all zeros (Move counts that as an input dropout), and a block carrying any
 * audio must pass through bit-exact. */
#include <stdio.h>
#include <string.h>
#include "bridge_silence_dither.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    int16_t b[256], prev[256];
    uint32_t rng = 1;

    /* Silence: dithered, within +-1 LSB, and no longer all zero. */
    memset(b, 0, sizeof b);
    CHECK(bridge_dither_if_silent(b, 256, &rng) == 1);
    int nonzero = 0, out_of_range = 0;
    for (int i = 0; i < 256; i++) {
        if (b[i]) nonzero++;
        if (b[i] < -1 || b[i] > 1) out_of_range++;
    }
    CHECK(nonzero > 0);
    CHECK(out_of_range == 0);

    /* Consecutive silent blocks differ (a repeated block is not a fix). */
    memcpy(prev, b, sizeof b);
    memset(b, 0, sizeof b);
    CHECK(bridge_dither_if_silent(b, 256, &rng) == 1);
    CHECK(memcmp(prev, b, sizeof b) != 0);

    /* Audio -- even one quiet sample -- is never touched. */
    memset(b, 0, sizeof b);
    b[200] = -3;
    memcpy(prev, b, sizeof b);
    CHECK(bridge_dither_if_silent(b, 256, &rng) == 0);
    CHECK(memcmp(prev, b, sizeof b) == 0);
    for (int i = 0; i < 256; i++) b[i] = (int16_t)(i * 97 - 12000);
    memcpy(prev, b, sizeof b);
    CHECK(bridge_dither_if_silent(b, 256, &rng) == 0);
    CHECK(memcmp(prev, b, sizeof b) == 0);

    if (failures) { printf("test_bridge_silence_dither: %d failure(s)\n", failures); return 1; }
    printf("test_bridge_silence_dither: PASS\n");
    return 0;
}
