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

static void test_bus_past_the_mask_width_is_excluded(void) {
    /* The mask is a uint32_t. A caller claiming more buses than it can name
     * must not shift past the word — that is UB, not a wrong answer — so a
     * bus at or past BUS_MIX_MAX_BUSES is excluded exactly as an unallocated
     * one is. Unreachable from today's SLOT_BUSES; pinned so it stays that
     * way if the cap is ever raised. */
    int16_t main_buf[N], far_buf[N];
    int16_t *bus_buf[40] = { 0 };
    bus_buf[33] = far_buf;
    int8_t voice_bus[1] = { 33 };
    int16_t *voice_out[1];
    uint32_t mask = 0;

    bus_mix_build_table(voice_out, 1, voice_bus, main_buf, bus_buf, 40);
    int n = bus_mix_active_mask(voice_bus, 1, 40, bus_buf, &mask);

    assert(voice_out[0] == main_buf);
    assert(mask == 0);
    assert(n == 0);
    printf("  past mask width excluded: ok\n");
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


/* ==========================================================================
 * THE SOLO PARTITION — per-voice sends
 * ========================================================================== */

#define NV 6
#define STRIDE 8   /* the pool's per-voice capacity, BUS_BUF_SAMPLES in the real path */

static void test_solo_partition_is_exactly_nonzero_send(void) {
    /* One rule, stated once: a voice is solo-buffered iff any of its per-voice
     * send levels is above zero. Not "is on a bus", not "the slot has sends" —
     * those are different questions with different answers. */
    const int8_t send[NV][2] = {
        { 0, 0 },     /* 0: silent on both -> aliased */
        { 1, 0 },     /* 1: the smallest audible ask still buys a buffer */
        { 0, 40 },    /* 2: send B alone counts */
        { 0, 0 },     /* 3 */
        { 127, 127 }, /* 4 */
        { 0, 0 },     /* 5 */
    };
    uint32_t mask = 0;
    int n = bus_mix_solo_mask(&send[0][0], NV, 2, &mask);
    assert(n == 3);
    assert(mask == ((1u << 1) | (1u << 2) | (1u << 4)));

    /* Per-voice agreement with the singular form, over the whole table. */
    for (int i = 0; i < NV; i++)
        assert(!!bus_mix_voice_is_solo(&send[0][0], NV, 2, i) ==
               !!(mask & (1u << i)));
    printf("  solo partition: ok\n");
}

static void test_solo_partition_is_stable(void) {
    /* Asked twice about the same table, the answer must be the same table —
     * the partition decides which pointer a voice renders through and which
     * buffer its send is taken from, and those are read at three different
     * points in one frame. */
    const int8_t send[NV][2] = {
        { 0, 0 }, { 9, 0 }, { 0, 0 }, { 0, 3 }, { 0, 0 }, { 0, 0 },
    };
    uint32_t a = 0, b = 0;
    int na = bus_mix_solo_mask(&send[0][0], NV, 2, &a);
    int nb = bus_mix_solo_mask(&send[0][0], NV, 2, &b);
    assert(a == b && na == nb);

    /* A NULL table, or no sends at all, is the empty partition — never a
     * read of a table that is not there. */
    uint32_t z = 0xdeadbeefu;
    assert(bus_mix_solo_mask(NULL, NV, 2, &z) == 0 && z == 0);
    z = 0xdeadbeefu;
    assert(bus_mix_solo_mask(&send[0][0], NV, 0, &z) == 0 && z == 0);
    printf("  solo partition stable: ok\n");
}

static void test_negative_send_is_not_a_solo(void) {
    /* bus_mix_send treats a negative level as a no-op (it would otherwise
     * phase-invert). The partition must agree, or a voice buys a buffer, a
     * clear and a fold-back to send nothing at all. */
    const int8_t send[2][2] = { { -5, 0 }, { 0, -1 } };
    uint32_t mask = 0xffffu;
    assert(bus_mix_solo_mask(&send[0][0], 2, 2, &mask) == 0);
    assert(mask == 0);
    printf("  negative send is not a solo: ok\n");
}

static void test_voice_past_the_mask_width_is_excluded(void) {
    /* Same rule as the bus form above, for the same reason: the mask is a
     * uint32_t and 1u << 33 is UB, not a wrong answer. The excluded voice
     * falls back to plain routing, so it is heard and only its send is lost. */
    int8_t send[40][2];
    memset(send, 0, sizeof(send));
    send[33][0] = 100;
    uint32_t mask = 0;
    assert(bus_mix_solo_mask(&send[0][0], 40, 2, &mask) == 0);
    assert(mask == 0);
    /* The SINGULAR form has to refuse it too, not merely never be asked: it is
     * exported, and a caller reaching for "is this one solo" without building a
     * mask must get the same answer the mask would have given. */
    assert(bus_mix_voice_is_solo(&send[0][0], 40, 2, 33) == 0);
    assert(bus_mix_voice_is_solo(&send[0][0], 40, 2, -1) == 0);
    assert(bus_mix_voice_is_solo(&send[0][0], 40, 2, 40) == 0);

    int16_t main_buf[STRIDE], pool[40 * STRIDE];
    int16_t *voice_out[40];
    bus_mix_build_table_split(voice_out, 40, NULL, main_buf, NULL, 0,
                              mask, pool, STRIDE);
    assert(voice_out[33] == main_buf);
    printf("  voice past mask width excluded: ok\n");
}

static void test_sparse_case_is_byte_identical(void) {
    /*
     * THE PROPERTY THAT KEEPS BUSES CHEAP. With no per-voice send anywhere,
     * the split build must produce the same table, pointer for pointer, that
     * the pre-sends build did — same aliasing, same fallbacks, and not one
     * pool slot touched.
     *
     * Both tables are built by the SAME loop today, which is the point: the
     * assertion is what makes that a requirement rather than an accident, and
     * it fails the moment someone gives the split path its own routing.
     */
    int16_t main_buf[STRIDE], b0[STRIDE], b2[STRIDE];
    int16_t *bus_buf[4] = { b0, NULL, b2, NULL };
    int8_t voice_bus[NV] = { 0, 0, 2, BUS_MIX_MAIN, 1 /* unallocated */, 9 /* out of range */ };
    int8_t send[NV][2];
    memset(send, 0, sizeof(send));

    uint32_t mask = 0xffffu;
    assert(bus_mix_solo_mask(&send[0][0], NV, 2, &mask) == 0);
    assert(mask == 0);

    int16_t pool[NV * STRIDE];
    const int16_t canary = 0x5a5a;
    for (int i = 0; i < NV * STRIDE; i++) pool[i] = canary;

    int16_t *before[NV], *after[NV];
    bus_mix_build_table(before, NV, voice_bus, main_buf, bus_buf, 4);
    bus_mix_build_table_split(after, NV, voice_bus, main_buf, bus_buf, 4,
                              mask, pool, STRIDE);
    assert(memcmp(before, after, sizeof(before)) == 0);

    /* No entry points into the pool, so nothing renders there and the caller
     * clears nothing: the pool is untouched memory the frame never pays for. */
    for (int i = 0; i < NV; i++)
        for (int j = 0; j < NV * STRIDE; j++)
            assert(after[i] != &pool[j]);
    for (int i = 0; i < NV * STRIDE; i++) assert(pool[i] == canary);

    /* And the active mask — the clear set — must not move either. */
    uint32_t m1 = 0, m2 = 0;
    int n1 = bus_mix_active_mask(voice_bus, NV, 4, bus_buf, &m1);
    int n2 = bus_mix_active_mask(voice_bus, NV, 4, bus_buf, &m2);
    assert(m1 == m2 && n1 == n2 && m1 == ((1u << 0) | (1u << 2)));
    printf("  sparse case byte-identical: ok\n");
}

static void test_solo_voices_get_their_own_slot_by_index(void) {
    /* The pool is indexed by VOICE INDEX and never compacted: voice 4 renders
     * at pool + 4*stride whether or not voices 0..3 are solo. A packed pool
     * would need a second index carried to the send tap and to the fold-back,
     * and an index that means two things is this branch's worst bug. */
    int16_t main_buf[STRIDE], b0[STRIDE];
    int16_t *bus_buf[2] = { b0, NULL };
    int8_t voice_bus[NV] = { 0, 0, BUS_MIX_MAIN, BUS_MIX_MAIN, 0, BUS_MIX_MAIN };
    int8_t send[NV][2];
    memset(send, 0, sizeof(send));
    send[1][0] = 20;   /* on bus 0, with a send */
    send[4][1] = 90;   /* also on bus 0, DIFFERENT send */
    send[3][0] = 5;    /* on main, with a send */

    uint32_t mask = 0;
    assert(bus_mix_solo_mask(&send[0][0], NV, 2, &mask) == 3);

    int16_t pool[NV * STRIDE];
    int16_t *voice_out[NV];
    bus_mix_build_table_split(voice_out, NV, voice_bus, main_buf, bus_buf, 2,
                              mask, pool, STRIDE);

    assert(voice_out[1] == pool + 1 * STRIDE);
    assert(voice_out[3] == pool + 3 * STRIDE);
    assert(voice_out[4] == pool + 4 * STRIDE);
    /* Distinct, which is the entire point: two voices in ONE bus with
     * different send levels must not share a pointer. */
    assert(voice_out[1] != voice_out[4]);
    /* Everyone else is routed exactly as before, aliasing included. */
    assert(voice_out[0] == b0 && voice_out[2] == main_buf && voice_out[5] == main_buf);

    /* Their DESTINATION is unchanged, and that is what puts them through the
     * bus insert after the fold-back. */
    assert(bus_mix_voice_dest(1, voice_bus, main_buf, bus_buf, 2) == b0);
    assert(bus_mix_voice_dest(4, voice_bus, main_buf, bus_buf, 2) == b0);
    assert(bus_mix_voice_dest(3, voice_bus, main_buf, bus_buf, 2) == main_buf);

    /* And bus 0 is STILL in the clear set even though only voice 0 renders
     * into it directly — the other two are accumulated in afterwards. */
    uint32_t active = 0;
    assert(bus_mix_active_mask(voice_bus, NV, 2, bus_buf, &active) == 1);
    assert(active == (1u << 0));
    printf("  solo slots by index: ok\n");
}

static void test_solo_mask_without_a_pool_is_ignored(void) {
    /* A mask computed without a buffer to back it must degrade to today's
     * routing, not to a store through NULL on the SPI callback. */
    int16_t main_buf[STRIDE];
    int8_t voice_bus[2] = { BUS_MIX_MAIN, BUS_MIX_MAIN };
    int16_t *voice_out[2];
    bus_mix_build_table_split(voice_out, 2, voice_bus, main_buf, NULL, 0,
                              0x3u, NULL, STRIDE);
    assert(voice_out[0] == main_buf && voice_out[1] == main_buf);

    int16_t pool[2 * STRIDE];
    bus_mix_build_table_split(voice_out, 2, voice_bus, main_buf, NULL, 0,
                              0x3u, pool, 0);
    assert(voice_out[0] == main_buf && voice_out[1] == main_buf);
    printf("  solo mask without a pool: ok\n");
}

static void test_two_voices_one_bus_send_and_insert(void) {
    /*
     * The whole feature end to end, in the order v2_render_block runs it:
     * render into the solo slots, take each voice's send at its OWN level,
     * fold both back into the shared bus buffer, and let the bus's insert see
     * the sum. Both halves of the acceptance criterion in one arithmetic.
     */
    int16_t main_buf[STRIDE] = {0}, b0[STRIDE] = {0};
    int16_t *bus_buf[1] = { b0 };
    int8_t voice_bus[2] = { 0, 0 };          /* SAME bus */
    int8_t send[2][2] = { { 127, 0 }, { 0, 127 } };   /* different sends */

    uint32_t mask = 0;
    assert(bus_mix_solo_mask(&send[0][0], 2, 2, &mask) == 2);

    int16_t pool[2 * STRIDE];
    memset(pool, 0, sizeof(pool));
    int16_t *voice_out[2];
    bus_mix_build_table_split(voice_out, 2, voice_bus, main_buf, bus_buf, 1,
                              mask, pool, STRIDE);

    /* The "module" accumulates into what it was handed. */
    for (int j = 0; j < STRIDE; j++) { voice_out[0][j] += 100; voice_out[1][j] += 30; }

    int16_t accumA[STRIDE] = {0}, accumB[STRIDE] = {0};
    int16_t *accum[2] = { accumA, accumB };
    for (int i = 0; i < 2; i++) {
        if (!(mask & (1u << i))) continue;
        for (int s = 0; s < 2; s++)
            bus_mix_send(accum[s], pool + i * STRIDE, STRIDE, send[i][s]);
        bus_mix_accumulate(bus_mix_voice_dest(i, voice_bus, main_buf, bus_buf, 1),
                           pool + i * STRIDE, STRIDE);
    }

    for (int j = 0; j < STRIDE; j++) {
        assert(accumA[j] == 100);   /* voice 0 only, at its own level */
        assert(accumB[j] == 30);    /* voice 1 only */
        assert(b0[j] == 130);       /* BOTH reached the bus, and its insert */
        assert(main_buf[j] == 0);   /* nothing leaked past the bus */
    }
    printf("  two voices one bus: ok\n");
}

int main(void) {
    printf("test_bus_mix:\n");
    test_aliasing_is_the_summing_mechanism();
    test_unassigned_and_unallocated_fall_to_main();
    test_active_mask_names_the_clear_set();
    test_active_mask_agrees_with_build_table();
    test_bus_past_the_mask_width_is_excluded();
    test_accumulate_saturates();
    test_send_level_endpoints();
    test_bus_sum_equals_voice_sum();
    test_solo_partition_is_exactly_nonzero_send();
    test_solo_partition_is_stable();
    test_negative_send_is_not_a_solo();
    test_voice_past_the_mask_width_is_excluded();
    test_sparse_case_is_byte_identical();
    test_solo_voices_get_their_own_slot_by_index();
    test_solo_mask_without_a_pool_is_ignored();
    test_two_voices_one_bus_send_and_insert();
    printf("PASS\n");
    return 0;
}
