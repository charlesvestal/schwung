/* Unit tests for bus_mix.h — the per-voice routing and mixing arithmetic.
 *
 * Header-only and pure, so it runs natively on the dev machine. The chain
 * translation unit that calls it cannot be built here, which is exactly how
 * arithmetic like this ends up shipped untested. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bus_mix.h"

#define N 8   /* samples per test buffer; frames*2 in the real path */

static void test_aliasing_is_the_summing_mechanism(void) {
    int16_t main_buf[N], b0[N], b1[N];
    int16_t *bus_buf[2] = { b0, b1 };
    /* voices: 0 and 1 -> bus 0, 2 -> bus 1, 3 -> main */
    int8_t voice_bus[4] = { 0, 0, 1, BUS_MIX_MAIN };
    int16_t *voice_out[4];

    bus_mix_build_table(voice_out, 4, voice_bus, main_buf, bus_buf, 2);

    assert(voice_out[0] == b0);
    assert(voice_out[1] == b0);          /* SAME pointer: HH and OH share a bus */
    assert(voice_out[2] == b1);
    assert(voice_out[3] == main_buf);
    printf("  aliasing: ok\n");
}

static void test_unassigned_and_unallocated_fall_to_main(void) {
    int16_t main_buf[N], b0[N];
    int16_t *bus_buf[2] = { b0, NULL };   /* bus 1 not allocated yet */
    int8_t voice_bus[3] = { BUS_MIX_MAIN, 1, 7 };  /* 7 is out of range */
    int16_t *voice_out[3];

    bus_mix_build_table(voice_out, 3, voice_bus, main_buf, bus_buf, 2);

    assert(voice_out[0] == main_buf);
    assert(voice_out[1] == main_buf);    /* lazy allocation must not crash */
    assert(voice_out[2] == main_buf);    /* out of range is not a trap */
    printf("  fallbacks: ok\n");
}

static void test_active_mask_names_the_clear_set(void) {
    int16_t b0[N], b2[N];
    int16_t *bus_buf[4] = { b0, NULL, b2, NULL };
    int8_t voice_bus[5] = { 0, 0, 2, BUS_MIX_MAIN, 2 };
    uint32_t mask = 0;
    int n = bus_mix_active_mask(voice_bus, 5, 4, bus_buf, &mask);
    assert(n == 2);
    assert(mask == ((1u << 0) | (1u << 2)));
    printf("  active mask: ok\n");
}

static void test_active_mask_agrees_with_build_table(void) {
    /* bus 1 is a voice's target but has not been allocated yet. The mask
     * must NOT claim it: build_table sent that voice's audio to main_buf,
     * so a caller that memsets every masked buffer would NULL-deref bus 1
     * on the SPI callback. */
    int16_t main_buf[N], b0[N];
    int16_t *bus_buf[2] = { b0, NULL };   /* bus 1 unallocated */
    int8_t voice_bus[3] = { 0, 1, BUS_MIX_MAIN };
    int16_t *voice_out[3];
    uint32_t mask = 0;

    bus_mix_build_table(voice_out, 3, voice_bus, main_buf, bus_buf, 2);
    int n = bus_mix_active_mask(voice_bus, 3, 2, bus_buf, &mask);

    assert(voice_out[1] == main_buf);     /* fell back: bus 1 not allocated */
    assert(!(mask & (1u << 1)));          /* so the mask must not name it */
    assert(mask == (1u << 0));
    assert(n == 1);
    printf("  active mask agrees with build_table: ok\n");
}

static void test_accumulate_saturates(void) {
    int16_t dst[4] = { 32000,  -32000, 0,  100 };
    int16_t src[4] = {  2000,   -2000, 0, -100 };
    bus_mix_accumulate(dst, src, 4);
    assert(dst[0] == 32767);    /* clamped, not wrapped to a negative */
    assert(dst[1] == -32768);
    assert(dst[2] == 0);
    assert(dst[3] == 0);
    printf("  accumulate saturates: ok\n");
}

static void test_send_level_endpoints(void) {
    int16_t dst[3] = { 5, 6, 7 };
    const int16_t before[3] = { 5, 6, 7 };
    int16_t src[3] = { 1000, -1000, 32767 };

    bus_mix_send(dst, src, 3, 0);
    assert(memcmp(dst, before, sizeof(before)) == 0);   /* 0 is a true no-op */

    int16_t dst2[3] = { 0, 0, 0 };
    bus_mix_send(dst2, src, 3, BUS_MIX_SEND_LEVEL_MAX);
    assert(dst2[0] == 1000 && dst2[1] == -1000 && dst2[2] == 32767);  /* unity */

    int16_t dst3[3] = { 0, 0, 0 };
    bus_mix_send(dst3, src, 3, 64);
    assert(dst3[0] > 400 && dst3[0] < 600);            /* roughly half */

    /* A negative level must be a no-op too, not a phase-inverted send —
     * without the <= 0 guard this would SUBTRACT src from dst. */
    int16_t dst4[3] = { 5, 6, 7 };
    bus_mix_send(dst4, src, 3, -64);
    assert(memcmp(dst4, before, sizeof(before)) == 0);

    /* Above-max must clamp to unity, not scale past it. */
    int16_t dstA[3] = { 0, 0, 0 };
    int16_t dstB[3] = { 0, 0, 0 };
    bus_mix_send(dstA, src, 3, 200);
    bus_mix_send(dstB, src, 3, BUS_MIX_SEND_LEVEL_MAX);
    assert(memcmp(dstA, dstB, sizeof(dstA)) == 0);

    printf("  send endpoints: ok\n");
}

static void test_bus_sum_equals_voice_sum(void) {
    /* The exact-sum property: routing voices through buses and summing back
     * must equal summing the voices directly, absent clamping. */
    int16_t main_buf[N] = {0}, b0[N] = {0}, b1[N] = {0};
    int16_t *bus_buf[2] = { b0, b1 };
    int8_t voice_bus[3] = { 0, 1, BUS_MIX_MAIN };
    int16_t *voice_out[3];
    bus_mix_build_table(voice_out, 3, voice_bus, main_buf, bus_buf, 2);

    int16_t v[3][N];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < N; j++) v[i][j] = (int16_t)(100 * (i + 1) + j);

    for (int i = 0; i < 3; i++) bus_mix_accumulate(voice_out[i], v[i], N);
    bus_mix_accumulate(main_buf, b0, N);
    bus_mix_accumulate(main_buf, b1, N);

    for (int j = 0; j < N; j++)
        assert(main_buf[j] == (int16_t)(v[0][j] + v[1][j] + v[2][j]));
    printf("  exact sum: ok\n");
}

int main(void) {
    printf("test_bus_mix:\n");
    test_aliasing_is_the_summing_mechanism();
    test_unassigned_and_unallocated_fall_to_main();
    test_active_mask_names_the_clear_set();
    test_active_mask_agrees_with_build_table();
    test_accumulate_saturates();
    test_send_level_endpoints();
    test_bus_sum_equals_voice_sum();
    printf("PASS\n");
    return 0;
}
