/*
 * Debug test for transpose jumps
 * Logs every call to get_transpose_at_step to see what's happening
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define MAX_TRANSPOSE_STEPS 16

typedef struct {
    int8_t transpose;
    uint16_t duration;
    int8_t jump;
    int8_t condition_n;
    int8_t condition_m;
    uint8_t condition_not;
} transpose_step_t;

/* Global state */
static transpose_step_t g_transpose_sequence[MAX_TRANSPOSE_STEPS];
static int g_transpose_step_count = 0;
static uint32_t g_transpose_loop_count = 0;
static int g_transpose_virtual_step = 0;
static uint32_t g_transpose_virtual_entry_step = 0;
static int g_transpose_first_call = 1;

static int check_transpose_condition(transpose_step_t *step) {
    printf("    check_condition: n=%d m=%d not=%d loop_count=%u\n",
           step->condition_n, step->condition_m, step->condition_not, g_transpose_loop_count);

    if (step->condition_n <= 0) {
        printf("    -> ALWAYS (n<=0)\n");
        return 1;
    }
    int iteration = (g_transpose_loop_count % step->condition_n) + 1;
    int should_apply = (iteration == step->condition_m);
    if (step->condition_not) {
        should_apply = !should_apply;
    }
    printf("    -> iteration=%d, should_apply=%d\n", iteration, should_apply);
    return should_apply;
}

static int8_t get_transpose_at_step(uint32_t step) {
    printf("  [get_transpose_at_step(step=%u)]\n", step);

    if (g_transpose_step_count == 0) {
        printf("    -> step_count=0, return 0\n");
        return 0;
    }

    if (g_transpose_first_call) {
        printf("    FIRST CALL: init virtual_step=0, entry_step=%u\n", step);
        g_transpose_virtual_step = 0;
        g_transpose_virtual_entry_step = step;
        g_transpose_first_call = 0;
    }

    transpose_step_t *current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    uint32_t duration_in_steps = current_virtual->duration;

    uint32_t steps_in_current = step - g_transpose_virtual_entry_step;

    printf("    virtual_step=%d, entry_step=%u, steps_in_current=%u, duration=%u\n",
           g_transpose_virtual_step, g_transpose_virtual_entry_step, steps_in_current, duration_in_steps);

    if (steps_in_current >= duration_in_steps) {
        printf("    DURATION EXPIRED (steps_in_current=%u >= duration=%u)\n", steps_in_current, duration_in_steps);

        /* Check for jump BEFORE advancing */
        if (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) {
            printf("    Checking JUMP: jump=%d\n", current_virtual->jump);
            if (check_transpose_condition(current_virtual)) {
                printf("    *** JUMP EXECUTED: %d -> %d (at global step %u) ***\n",
                       g_transpose_virtual_step, current_virtual->jump, step);
                g_transpose_virtual_step = current_virtual->jump;
                g_transpose_virtual_entry_step = step;
                current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
                return current_virtual->transpose;
            } else {
                printf("    Jump condition FAILED\n");
            }
        } else {
            printf("    No jump (jump=%d, step_count=%d)\n", current_virtual->jump, g_transpose_step_count);
        }

        /* No jump - advance normally */
        int next_virtual = g_transpose_virtual_step + 1;
        if (next_virtual >= g_transpose_step_count) {
            next_virtual = 0;
            g_transpose_loop_count++;
            printf("    WRAP: loop_count=%u\n", g_transpose_loop_count);
        }

        printf("    ADVANCE: %d -> %d\n", g_transpose_virtual_step, next_virtual);
        g_transpose_virtual_step = next_virtual;
        g_transpose_virtual_entry_step = step;
        current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    }

    printf("    -> transpose=%+d (virtual_step=%d)\n", current_virtual->transpose, g_transpose_virtual_step);
    return current_virtual->transpose;
}

int main() {
    printf("=== DEBUG: Transpose Jump Test (Set 23) ===\n\n");

    /* Initialize jumps to -1 */
    for (int i = 0; i < MAX_TRANSPOSE_STEPS; i++) {
        g_transpose_sequence[i].jump = -1;
    }

    /* Set up sequence from set 23 (durations already in steps) */
    g_transpose_step_count = 4;

    /* Step 0: +3, 16 steps, no jump */
    g_transpose_sequence[0].transpose = 3;
    g_transpose_sequence[0].duration = 16;
    g_transpose_sequence[0].jump = -1;
    g_transpose_sequence[0].condition_n = 0;
    g_transpose_sequence[0].condition_m = 0;
    g_transpose_sequence[0].condition_not = 0;

    /* Step 1: +1, 16 steps, JUMP TO 0 (always) */
    g_transpose_sequence[1].transpose = 1;
    g_transpose_sequence[1].duration = 16;
    g_transpose_sequence[1].jump = 0;  /* JUMP! */
    g_transpose_sequence[1].condition_n = 0;  /* Always */
    g_transpose_sequence[1].condition_m = 0;
    g_transpose_sequence[1].condition_not = 0;

    /* Step 2: +7, 4 steps, no jump */
    g_transpose_sequence[2].transpose = 7;
    g_transpose_sequence[2].duration = 4;
    g_transpose_sequence[2].jump = -1;
    g_transpose_sequence[2].condition_n = 0;
    g_transpose_sequence[2].condition_m = 0;
    g_transpose_sequence[2].condition_not = 0;

    /* Step 3: +5, 16 steps, no jump */
    g_transpose_sequence[3].transpose = 5;
    g_transpose_sequence[3].duration = 16;
    g_transpose_sequence[3].jump = -1;
    g_transpose_sequence[3].condition_n = 0;
    g_transpose_sequence[3].condition_m = 0;
    g_transpose_sequence[3].condition_not = 0;

    printf("Sequence:\n");
    for (int i = 0; i < g_transpose_step_count; i++) {
        printf("  Step %d: transpose=%+d, duration=%d steps, jump=%d\n",
               i, g_transpose_sequence[i].transpose,
               g_transpose_sequence[i].duration,
               g_transpose_sequence[i].jump);
    }
    printf("\n");

    /* Simulate calling get_transpose_at_step every 4 steps (like track loop of 1 with 1 note per beat) */
    printf("Simulating playback (calling every 4 steps):\n\n");

    for (uint32_t step = 0; step < 128; step += 4) {
        printf("STEP %u:\n", step);
        int8_t transpose = get_transpose_at_step(step);
        printf("\n");
    }

    return 0;
}
