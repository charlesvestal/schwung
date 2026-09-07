/*
 * The chain-knob acceleration curve, run rather than grepped.
 *
 * Three paths turn a chain knob — the relative CC decode and the absolute CC
 * in chain_midi.c, and knob_N_adjust in chain_host.c, which is the path the
 * device's own encoders use. Each carried its own copy of this curve. Two
 * copies survived to be compared and they nested their bounds differently:
 *
 *   chain_midi.c   if (elapsed <  SLOW) { if (elapsed <= FAST) MAX; else ratio }
 *   chain_host.c   if (elapsed <= FAST) MAX; else if (elapsed < SLOW) ratio
 *
 * Both happen to compute the same answer, which is exactly why nobody noticed
 * they had drifted apart. Neither TU can be compiled natively, so no test could
 * ever have said so — the arithmetic had to move somewhere testable first.
 *
 * The contract:
 *   1. a gap at or under FAST is the maximum multiplier
 *   2. a gap at or over SLOW is the minimum — a deliberate slow turn
 *   3. in between it is monotone non-increasing, and stays within the ends
 *   4. the FIRST message of a session is slow (no previous timestamp)
 *   5. the type caps: enums never accelerate on time, ints are limited
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain_internal.h"

/* ------------------------------------------------------------------ stubs */
void chain_log(const char *msg) { (void)msg; }
void parse_debug_log(const char *msg) { (void)msg; }
void v2_chain_log(chain_instance_t *inst, const char *msg) { (void)inst; (void)msg; }
void v2_synth_panic(chain_instance_t *inst) { (void)inst; }
void chain_mod_clear_source(void *ctx, const char *source_id) { (void)ctx; (void)source_id; }
int chain_mod_refresh_target_param_cache(chain_instance_t *inst, const char *t) {
    (void)inst; (void)t; return 0;
}
int chain_mod_is_target_active(chain_instance_t *inst, const char *t, const char *p) {
    (void)inst; (void)t; (void)p; return 0;
}
void chain_mod_update_base_from_set_param(chain_instance_t *inst, const char *t,
                                          const char *p, const char *v) {
    (void)inst; (void)t; (void)p; (void)v;
}
mod_target_state_t *chain_mod_find_target_entry(chain_instance_t *inst, const char *t,
                                                const char *p) {
    (void)inst; (void)t; (void)p; return NULL;
}
void chain_mod_apply_effective_value(chain_instance_t *inst, mod_target_state_t *e, int f) {
    (void)inst; (void)e; (void)f;
}

/* ---------------------------------------------------------------- harness */
static int failures = 0;
static void check(int cond, const char *what) {
    if (cond) printf("  ok  %s\n", what);
    else { printf("FAIL: %s\n", what); failures++; }
}

int main(void) {
    check(chain_knob_accel_for_gap(0) == KNOB_ACCEL_MAX_MULT,
          "an instantaneous gap is the maximum multiplier");
    check(chain_knob_accel_for_gap(KNOB_ACCEL_FAST_MS) == KNOB_ACCEL_MAX_MULT,
          "a gap exactly at FAST is still the maximum (the bound is inclusive)");
    check(chain_knob_accel_for_gap(KNOB_ACCEL_SLOW_MS) == KNOB_ACCEL_MIN_MULT,
          "a gap exactly at SLOW is the minimum (that bound is exclusive)");
    check(chain_knob_accel_for_gap(KNOB_ACCEL_SLOW_MS * 100) == KNOB_ACCEL_MIN_MULT,
          "and any longer gap stays at the minimum");

    /* Monotone across the whole ramp, and never outside its own ends. A curve
     * that dipped or overshot in the middle would feel like a knob that speeds
     * up as you slow down, which is the #404 symptom one octave quieter. */
    int prev = KNOB_ACCEL_MAX_MULT + 1;
    int monotone = 1, in_range = 1;
    for (uint64_t gap = 0; gap <= KNOB_ACCEL_SLOW_MS + 5; gap++) {
        int a = chain_knob_accel_for_gap(gap);
        if (a > prev) monotone = 0;
        if (a < KNOB_ACCEL_MIN_MULT || a > KNOB_ACCEL_MAX_MULT) in_range = 0;
        prev = a;
    }
    check(monotone, "the curve never speeds up as the gap grows");
    check(in_range, "and never leaves [MIN_MULT, MAX_MULT]");

    /* The first message of a session has no previous timestamp. Reading the
     * zero as an elapsed time would make it the FASTEST turn instead of the
     * slowest — a knob that jumps on first touch. */
    uint64_t last = 0;
    check(chain_knob_accel(&last) == KNOB_ACCEL_MIN_MULT,
          "the first turn of a session is slow, not maximal");
    check(last != 0, "and the timestamp is recorded for the next message");

    /* Two messages back to back are as fast as it gets. */
    int a2 = chain_knob_accel(&last);
    check(a2 == KNOB_ACCEL_MAX_MULT,
          "a message immediately after another is the maximum multiplier");

    /* The type caps. */
    check(chain_knob_accel_cap(KNOB_ACCEL_MAX_MULT, KNOB_TYPE_FLOAT) == KNOB_ACCEL_MAX_MULT,
          "a float keeps the full multiplier");
    check(chain_knob_accel_cap(KNOB_ACCEL_MAX_MULT, KNOB_TYPE_ENUM) == KNOB_ACCEL_ENUM_MULT,
          "an enum never accelerates on time");
    check(chain_knob_accel_cap(KNOB_ACCEL_MIN_MULT, KNOB_TYPE_ENUM) == KNOB_ACCEL_ENUM_MULT,
          "...whatever the gap was");
    check(chain_knob_accel_cap(KNOB_ACCEL_MAX_MULT, KNOB_TYPE_INT) == KNOB_ACCEL_MAX_MULT_INT,
          "an int is capped, not pinned");
    check(chain_knob_accel_cap(KNOB_ACCEL_MIN_MULT, KNOB_TYPE_INT) == KNOB_ACCEL_MIN_MULT,
          "...and a slow int turn is left alone");

    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall checks passed\n");
    return 0;
}
