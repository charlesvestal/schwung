/*
 * Test transpose jump behavior
 *
 * Simulates the sequence from set 23:
 * Step 0: +3 transpose, 4 beats (1 bar), no jump
 * Step 1: +1 transpose, 4 beats (1 bar), JUMP TO STEP 0 (always)
 * Step 2: +7 transpose, 1 beat, no jump
 * Step 3: +5 transpose, 4 beats (1 bar), no jump
 *
 * Expected: Steps 0 and 1 loop forever, steps 2 and 3 never reached.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define MAX_TRANSPOSE_STEPS 16

typedef struct {
    int8_t transpose;
    uint16_t duration;      /* Duration in steps (beats * 4) */
    int8_t jump;
    int8_t condition_n;
    int8_t condition_m;
    uint8_t condition_not;
} transpose_step_t;

/* Global state (mimicking seq_plugin.c) */
static transpose_step_t g_transpose_sequence[MAX_TRANSPOSE_STEPS];
static int g_transpose_step_count = 0;
static uint32_t g_transpose_loop_count = 0;
static int g_transpose_virtual_step = 0;
static uint32_t g_transpose_virtual_entry_step = 0;
static int g_transpose_first_call = 1;

static int check_transpose_condition(transpose_step_t *step) {
    if (step->condition_n <= 0) {
        return 1;  /* No condition - always passes */
    }
    int iteration = (g_transpose_loop_count % step->condition_n) + 1;
    int should_apply = (iteration == step->condition_m);
    if (step->condition_not) {
        should_apply = !should_apply;
    }
    return should_apply;
}

static int8_t get_transpose_at_step(uint32_t step) {
    if (g_transpose_step_count == 0) {
        return 0;
    }

    /* Initialize on first call */
    if (g_transpose_first_call) {
        g_transpose_virtual_step = 0;
        g_transpose_virtual_entry_step = step;
        g_transpose_first_call = 0;
    }

    /* Get current virtual step and its duration */
    transpose_step_t *current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    uint32_t duration_in_steps = current_virtual->duration;  /* Already in steps */

    /* Check if we've been in this virtual step long enough to advance */
    uint32_t steps_in_current = step - g_transpose_virtual_entry_step;

    if (steps_in_current >= duration_in_steps) {
        /* Step finished playing - check for jump BEFORE advancing */
        if (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) {
            if (check_transpose_condition(current_virtual)) {
                /* Jump: go to target step instead of advancing normally */
                printf("  [JUMP] Step %d jumped to step %d\n",
                       g_transpose_virtual_step, current_virtual->jump);
                g_transpose_virtual_step = current_virtual->jump;
                g_transpose_virtual_entry_step = step;
                current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
                return current_virtual->transpose;
            }
        }

        /* No jump or condition failed - advance normally */
        int next_virtual = g_transpose_virtual_step + 1;

        /* Handle wraparound */
        if (next_virtual >= g_transpose_step_count) {
            next_virtual = 0;
            g_transpose_loop_count++;
            printf("  [WRAP] Sequence wrapped, loop_count=%u\n", g_transpose_loop_count);
        }

        g_transpose_virtual_step = next_virtual;
        g_transpose_virtual_entry_step = step;
        current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    }

    return current_virtual->transpose;
}

int main() {
    printf("=== Testing Transpose Jump Behavior ===\n\n");

    /* Set up sequence from set 23 */
    g_transpose_step_count = 4;

    /* Step 0: +3 transpose, 1 bar (16 steps), no jump */
    g_transpose_sequence[0].transpose = 3;
    g_transpose_sequence[0].duration = 16;  /* 4 beats * 4 = 16 steps */
    g_transpose_sequence[0].jump = -1;
    g_transpose_sequence[0].condition_n = 0;
    g_transpose_sequence[0].condition_m = 0;
    g_transpose_sequence[0].condition_not = 0;

    /* Step 1: +1 transpose, 1 bar (16 steps), JUMP TO 0 */
    g_transpose_sequence[1].transpose = 1;
    g_transpose_sequence[1].duration = 16;
    g_transpose_sequence[1].jump = 0;  /* JUMP BACK TO STEP 0 */
    g_transpose_sequence[1].condition_n = 0;  /* Always */
    g_transpose_sequence[1].condition_m = 0;
    g_transpose_sequence[1].condition_not = 0;

    /* Step 2: +7 transpose, 1 beat (4 steps), no jump */
    g_transpose_sequence[2].transpose = 7;
    g_transpose_sequence[2].duration = 4;
    g_transpose_sequence[2].jump = -1;
    g_transpose_sequence[2].condition_n = 0;
    g_transpose_sequence[2].condition_m = 0;
    g_transpose_sequence[2].condition_not = 0;

    /* Step 3: +5 transpose, 1 bar (16 steps), no jump */
    g_transpose_sequence[3].transpose = 5;
    g_transpose_sequence[3].duration = 16;
    g_transpose_sequence[3].jump = -1;
    g_transpose_sequence[3].condition_n = 0;
    g_transpose_sequence[3].condition_m = 0;
    g_transpose_sequence[3].condition_not = 0;

    printf("Sequence setup:\n");
    printf("  Step 0: transpose +3, 16 steps (1 bar), no jump\n");
    printf("  Step 1: transpose +1, 16 steps (1 bar), JUMP TO 0\n");
    printf("  Step 2: transpose +7, 4 steps (1 beat), no jump\n");
    printf("  Step 3: transpose +5, 16 steps (1 bar), no jump\n");
    printf("\n");

    printf("Expected: Steps 0 and 1 loop forever, steps 2 and 3 never play.\n\n");

    /* Simulate playback for 8 bars (128 steps = 32 beats = 8 bars) */
    printf("Simulating 8 bars of playback:\n\n");

    int step_count[4] = {0, 0, 0, 0};  /* Count how many times each step plays */
    int last_virtual_step = -1;
    int last_transpose = 999;

    for (uint32_t step = 0; step < 128; step++) {
        int8_t transpose = get_transpose_at_step(step);

        /* Print when step or transpose changes */
        if (g_transpose_virtual_step != last_virtual_step || transpose != last_transpose) {
            float bar = step / 16.0f;
            printf("Step %3u (Bar %.1f): Virtual Step %d, Transpose %+d\n",
                   step, bar, g_transpose_virtual_step, transpose);

            step_count[g_transpose_virtual_step]++;
            last_virtual_step = g_transpose_virtual_step;
            last_transpose = transpose;
        }
    }

    printf("\n=== Results ===\n");
    printf("Step play counts:\n");
    for (int i = 0; i < 4; i++) {
        printf("  Step %d: %d times%s\n", i, step_count[i],
               (i >= 2 && step_count[i] == 0) ? " (NEVER PLAYED - CORRECT!)" : "");
    }

    printf("\n");
    if (step_count[0] > 0 && step_count[1] > 0 && step_count[2] == 0 && step_count[3] == 0) {
        printf("✓ TEST PASSED: Infinite loop between steps 0 and 1, steps 2-3 never reached!\n");
        return 0;
    } else {
        printf("✗ TEST FAILED: Unexpected behavior!\n");
        return 1;
    }
}
