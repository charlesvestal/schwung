/*
 * Simple test to demonstrate transpose jump bug
 *
 * Test setup:
 * - Step 0: transpose=0, duration=4 steps, jump to step 1
 * - Step 1: transpose=12, duration=4 steps, no jump
 *
 * Expected behavior:
 * - Steps 0-3: transpose=0 (playing step 0)
 * - Step 4: jump to step 1, transpose=12
 * - Steps 5-7: transpose=12 (playing step 1)
 * - Step 8: wrap to step 0, transpose=0
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define MAX_TRANSPOSE_STEPS 16

typedef struct {
    int8_t transpose;       /* -24 to +24 semitones */
    uint16_t duration;      /* Duration in steps (1-256) */
    int8_t jump;            /* Jump target (-1 = no jump, 0-15 = target step) */
    int8_t condition_n;     /* 0=always, >0=every N loops */
    int8_t condition_m;     /* Which iteration (1 to N) */
    uint8_t condition_not;  /* Negate condition */
} transpose_step_t;

/* Global state */
static transpose_step_t g_transpose_sequence[MAX_TRANSPOSE_STEPS];
static int g_transpose_step_count = 0;
static uint32_t g_transpose_total_steps = 0;
static int g_transpose_sequence_enabled = 1;
static uint32_t g_transpose_loop_count = 0;
static int g_transpose_virtual_step = 0;
static uint32_t g_transpose_virtual_entry_step = 0;
static int g_transpose_first_call = 1;

static int check_transpose_condition(transpose_step_t *step) {
    if (step->condition_n <= 0) {
        return 1;  /* No condition (n=0) always passes */
    }

    /* Calculate which iteration of the loop cycle we're in (1-indexed) */
    int iteration = (g_transpose_loop_count % step->condition_n) + 1;
    int should_apply = (iteration == step->condition_m);

    /* Apply NOT flag if set */
    if (step->condition_not) {
        should_apply = !should_apply;
    }

    return should_apply;
}

static int8_t get_transpose_at_step(uint32_t step) {
    /* If transpose sequence is disabled, return 0 (no automation) */
    if (!g_transpose_sequence_enabled) {
        return 0;
    }
    if (g_transpose_step_count == 0 || g_transpose_total_steps == 0) {
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
    uint32_t duration_in_steps = current_virtual->duration;

    /* Check if we've been in this virtual step long enough to advance */
    uint32_t steps_in_current = step - g_transpose_virtual_entry_step;

    if (steps_in_current >= duration_in_steps) {
        /* Step finished playing - check for jump BEFORE advancing */
        if (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) {
            if (check_transpose_condition(current_virtual)) {
                /* Jump: go to target step instead of advancing normally */
                g_transpose_virtual_step = current_virtual->jump;
                g_transpose_virtual_entry_step = step;
                current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
                /* Don't advance, don't increment loop counter - jump took us somewhere */
                return current_virtual->transpose;
            }
        }

        /* No jump or condition failed - advance normally */
        int next_virtual = g_transpose_virtual_step + 1;

        /* Handle wraparound */
        if (next_virtual >= g_transpose_step_count) {
            next_virtual = 0;
            g_transpose_loop_count++;  /* Increment loop counter on wrap */
        }

        g_transpose_virtual_step = next_virtual;
        g_transpose_virtual_entry_step = step;
        current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    }

    /* Return transpose value of current virtual step */
    return current_virtual->transpose;
}

int main() {
    printf("Testing transpose jump functionality\n");
    printf("=====================================\n\n");

    /* Setup: 2 steps */
    g_transpose_step_count = 2;
    g_transpose_total_steps = 8;  /* 4 + 4 */

    /* Step 0: transpose=0, duration=4, jump to step 1, no condition */
    g_transpose_sequence[0].transpose = 0;
    g_transpose_sequence[0].duration = 4;
    g_transpose_sequence[0].jump = 1;
    g_transpose_sequence[0].condition_n = 0;  /* No condition */
    g_transpose_sequence[0].condition_m = 0;
    g_transpose_sequence[0].condition_not = 0;

    /* Step 1: transpose=12, duration=4, no jump */
    g_transpose_sequence[1].transpose = 12;
    g_transpose_sequence[1].duration = 4;
    g_transpose_sequence[1].jump = -1;  /* No jump */
    g_transpose_sequence[1].condition_n = 0;
    g_transpose_sequence[1].condition_m = 0;
    g_transpose_sequence[1].condition_not = 0;

    printf("Sequence setup:\n");
    printf("  Step 0: transpose=0, duration=4, jump=1 (jump to step 1)\n");
    printf("  Step 1: transpose=12, duration=4, no jump\n\n");

    printf("Expected behavior:\n");
    printf("  Steps 0-3: transpose=0 (virtual step 0)\n");
    printf("  Step 4: JUMP to step 1, transpose=12\n");
    printf("  Steps 5-7: transpose=12 (virtual step 1)\n");
    printf("  Step 8: wrap to step 0, transpose=0\n\n");

    printf("Actual behavior:\n");
    int error_count = 0;

    for (uint32_t step = 0; step < 12; step++) {
        int8_t transpose = get_transpose_at_step(step);
        int expected_transpose;
        int expected_virtual_step;

        /* Determine expected values */
        if (step < 4) {
            expected_transpose = 0;
            expected_virtual_step = 0;
        } else if (step < 8) {
            /* After jump at step 4, we should be at virtual step 1 */
            expected_transpose = 12;
            expected_virtual_step = 1;
        } else {
            /* After wrap at step 8, we should be back at virtual step 0 */
            expected_transpose = 0;
            expected_virtual_step = 0;
        }

        char status = ' ';
        if (transpose != expected_transpose || g_transpose_virtual_step != expected_virtual_step) {
            status = 'X';
            error_count++;
        } else {
            status = 'O';
        }

        printf("  [%c] Step %2u: transpose=%2d (expected %2d), virtual_step=%d (expected %d), loop_count=%u\n",
               status, step, transpose, expected_transpose,
               g_transpose_virtual_step, expected_virtual_step,
               g_transpose_loop_count);
    }

    printf("\n");
    if (error_count == 0) {
        printf("✓ TEST PASSED: All steps produced expected transpose values\n");
        return 0;
    } else {
        printf("✗ TEST FAILED: %d steps produced incorrect transpose values\n", error_count);
        printf("\nBUG DETECTED: Jump logic is not working correctly!\n");
        return 1;
    }
}
