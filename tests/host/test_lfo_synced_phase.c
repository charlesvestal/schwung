#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "host/lfo_common.h"

static void fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }
static void near(double got, double want, const char *m) {
    if (fabs(got - want) > 1e-9) {
        fprintf(stderr, "FAIL: %s (got %f want %f)\n", m, got, want);
        exit(1);
    }
}

/* Find a division index by its beats value so the test doesn't hardcode
 * table order. */
static int div_index(float beats) {
    for (int i = 0; i < LFO_NUM_DIVISIONS; i++)
        if (fabsf(lfo_divisions[i].beats - beats) < 1e-6f) return i;
    fail("division not found in table");
    return -1;
}

int main(void) {
    int d1 = div_index(1.0f);   /* 1-beat cycle */
    int d4 = div_index(4.0f);   /* 1-bar cycle */

    near(lfo_synced_phase(0.0, d1), 0.0, "beat 0 -> phase 0");
    near(lfo_synced_phase(0.5, d1), 0.5, "half beat -> phase 0.5 on 1-beat div");
    near(lfo_synced_phase(7.0, d1), 0.0, "whole beats wrap to 0");
    near(lfo_synced_phase(6.0, d4), 0.5, "beat 6 -> phase 0.5 on 4-beat div");
    near(lfo_synced_phase(9.0, d4), 0.25, "beat 9 -> phase 0.25 on 4-beat div");
    /* Out-of-range division indexes clamp, never crash. */
    (void)lfo_synced_phase(1.0, -5);
    (void)lfo_synced_phase(1.0, LFO_NUM_DIVISIONS + 5);

    printf("PASS: test_lfo_synced_phase\n");
    return 0;
}
