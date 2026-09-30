/* mix_soft_clip.h: identity below the knee, monotonic and bounded above it,
 * continuous in value and slope at the knee, and an int32 accumulation that
 * gives the true total where a per-add int16 clamp does not. */
#include <stdio.h>
#include <stdlib.h>
#include "mix_soft_clip.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* 1. Identity everywhere a signal fits today: bit-for-bit, both signs. */
    for (int32_t v = -MIX_SOFT_CLIP_KNEE; v <= MIX_SOFT_CLIP_KNEE; v++)
        if (mix_soft_clip(v) != v) { CHECK(0, "identity broken at %d -> %d", v, mix_soft_clip(v)); break; }

    /* 2. Monotonic and bounded over the whole range a 4-slot + 2-return sum
     *    can reach, and symmetric. */
    int16_t prev = mix_soft_clip(MIX_SOFT_CLIP_KNEE);
    for (int32_t v = MIX_SOFT_CLIP_KNEE + 1; v <= 8 * 32768; v++) {
        int16_t y = mix_soft_clip(v);
        if (y < prev) { CHECK(0, "not monotonic at %d (%d < %d)", v, y, prev); break; }
        if (y > 32767) { CHECK(0, "over full scale at %d", v); break; }
        if (mix_soft_clip(-v) != -y) { CHECK(0, "asymmetric at %d: %d vs %d", v, y, mix_soft_clip(-v)); break; }
        prev = y;
    }
    CHECK(mix_soft_clip(INT32_MAX) == 32767, "INT32_MAX -> %d", mix_soft_clip(INT32_MAX));
    CHECK(mix_soft_clip(INT32_MIN) == -32767, "INT32_MIN -> %d", mix_soft_clip(INT32_MIN));

    /* 3. No corner at the knee: the first steps above it move by ~1 per unit,
     *    as the identity below it does. A hard clamp at the knee would be 0. */
    int32_t k = MIX_SOFT_CLIP_KNEE;
    int d = mix_soft_clip(k + 100) - mix_soft_clip(k);
    CHECK(d >= 98 && d <= 100, "slope at knee: +100 in -> +%d out", d);

    /* 4. Order independence: an over that comes back. The old per-add clamp
     *    kept the clipped intermediate. */
    int16_t parts[3] = { 30000, 20000, -25000 };      /* true total 25000 */
    int16_t clamped = 0;
    int32_t acc = 0;
    for (int i = 0; i < 3; i++) {
        int32_t m = (int32_t)clamped + parts[i];
        clamped = (int16_t)(m > 32767 ? 32767 : m < -32768 ? -32768 : m);
        acc += parts[i];
    }
    CHECK(clamped != 25000, "fixture no longer shows the per-add clamp defect (%d)", clamped);
    CHECK(mix_soft_clip(acc) == 25000, "int32 sum -> %d, want 25000", mix_soft_clip(acc));

    /* 5. Block form matches the scalar. */
    int32_t src[6] = { 0, 1000, -29204, 29205, 40000, -90000 };
    int16_t dst[6];
    mix_soft_clip_block(src, dst, 6);
    for (int i = 0; i < 6; i++)
        CHECK(dst[i] == mix_soft_clip(src[i]), "block[%d] %d != %d", i, dst[i], mix_soft_clip(src[i]));

    if (fails) { printf("test_mix_soft_clip: %d failure(s)\n", fails); return 1; }
    printf("test_mix_soft_clip: PASS\n");
    return 0;
}
