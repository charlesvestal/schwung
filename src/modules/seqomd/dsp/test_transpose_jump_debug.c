/*
 * Debug version - shows exactly what's happening with jump logic
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define MAX_TRANSPOSE_STEPS 16

typedef struct {
    int8_t transpose;
    uint16_t duration;
    int8_t jump;
    int8_t condition_n;
    int8_t condition_m;
    uint8_t condition_not;
} transpose_step_t;

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
        return 1;
    }
    int iteration = (g_transpose_loop_count % step->condition_n) + 1;
    int should_apply = (iteration == step->condition_m);
    if (step->condition_not) {
        should_apply = !should_apply;
    }
    return should_apply;
}

static int8_t get_transpose_at_step(uint32_t step) {
    printf("  [get_transpose_at_step] step=%u\n", step);

    if (!g_transpose_sequence_enabled) {
        printf("    -> sequence disabled, return 0\n");
        return 0;
    }
    if (g_transpose_step_count == 0 || g_transpose_total_steps == 0) {
        printf("    -> no steps defined, return 0\n");
        return 0;
    }

    if (g_transpose_first_call) {
        printf("    -> first call, init virtual_step=0, entry_step=%u\n", step);
        g_transpose_virtual_step = 0;
        g_transpose_virtual_entry_step = step;
        g_transpose_first_call = 0;
    }

    transpose_step_t *current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    uint32_t duration_in_steps = current_virtual->duration;
    uint32_t steps_in_current = step - g_transpose_virtual_entry_step;

    printf("    current virtual_step=%d, entry_step=%u, duration=%u\n",
           g_transpose_virtual_step, g_transpose_virtual_entry_step, duration_in_steps);
    printf("    steps_in_current=%u, checking if >= duration (%u)\n",
           steps_in_current, duration_in_steps);

    if (steps_in_current >= duration_in_steps) {
        printf("    -> YES, time to advance/jump\n");

        /* Check for jump */
        printf("    checking jump: current->jump=%d, step_count=%d\n",
               current_virtual->jump, g_transpose_step_count);

        if (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) {
            printf("    -> jump target valid, checking condition\n");
            int cond = check_transpose_condition(current_virtual);
            printf("    -> condition result=%d\n", cond);

            if (cond) {
                printf("    -> JUMPING to step %d\n", current_virtual->jump);
                g_transpose_virtual_step = current_virtual->jump;
                g_transpose_virtual_entry_step = step;
                current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
                printf("    -> after jump: virtual_step=%d, transpose=%d\n",
                       g_transpose_virtual_step, current_virtual->transpose);
                return current_virtual->transpose;
            }
        } else {
            printf("    -> no valid jump\n");
        }

        /* Normal advance */
        int next_virtual = g_transpose_virtual_step + 1;
        printf("    advancing normally to %d\n", next_virtual);

        if (next_virtual >= g_transpose_step_count) {
            printf("    -> wrapping to 0, incrementing loop_count\n");
            next_virtual = 0;
            g_transpose_loop_count++;
        }

        g_transpose_virtual_step = next_virtual;
        g_transpose_virtual_entry_step = step;
        current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
        printf("    -> after advance: virtual_step=%d, transpose=%d\n",
               g_transpose_virtual_step, current_virtual->transpose);
    } else {
        printf("    -> NO, continue current step\n");
    }

    printf("    -> returning transpose=%d\n\n", current_virtual->transpose);
    return current_virtual->transpose;
}

int main() {
    printf("Debug Test: Simple Jump Scenario\n");
    printf("=================================\n\n");

    /* Simple case: 2 steps, first should jump to second */
    g_transpose_step_count = 2;
    g_transpose_sequence[0].transpose = 0;
    g_transpose_sequence[0].duration = 4;
    g_transpose_sequence[0].jump = 1;
    g_transpose_sequence[0].condition_n = 0;
    g_transpose_sequence[0].condition_m = 0;
    g_transpose_sequence[0].condition_not = 0;

    g_transpose_sequence[1].transpose = 12;
    g_transpose_sequence[1].duration = 4;
    g_transpose_sequence[1].jump = -1;
    g_transpose_sequence[1].condition_n = 0;
    g_transpose_sequence[1].condition_m = 0;
    g_transpose_sequence[1].condition_not = 0;

    printf("Sequence:\n");
    printf("  Step 0: transpose=0, duration=4, jump=1\n");
    printf("  Step 1: transpose=12, duration=4, jump=-1\n\n");

    printf("Expected:\n");
    printf("  Steps 0-3: transpose=0\n");
    printf("  Step 4: JUMP to step 1, transpose=12\n");
    printf("  Steps 5-7: transpose=12\n\n");

    printf("Calling get_transpose_at_step for steps 0-7:\n\n");

    for (uint32_t i = 0; i <= 7; i++) {
        printf("Step %u:\n", i);
        int8_t t = get_transpose_at_step(i);
        printf("  Result: transpose=%d\n\n", t);
    }

    return 0;
}
