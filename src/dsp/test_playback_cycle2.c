/*
 * Test transpose jumps with play/stop cycle - show jump execution
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

/* Global state */
static transpose_step_t g_transpose_sequence[MAX_TRANSPOSE_STEPS];
static int g_transpose_step_count = 0;
static uint32_t g_transpose_loop_count = 0;
static int g_transpose_virtual_step = 0;
static uint32_t g_transpose_virtual_entry_step = 0;
static int g_transpose_first_call = 1;

static int check_transpose_condition(transpose_step_t *step) {
    if (step->condition_n <= 0) return 1;
    int iteration = (g_transpose_loop_count % step->condition_n) + 1;
    int should_apply = (iteration == step->condition_m);
    if (step->condition_not) should_apply = !should_apply;
    return should_apply;
}

static int8_t get_transpose_at_step(uint32_t step) {
    if (g_transpose_step_count == 0) return 0;

    if (g_transpose_first_call) {
        g_transpose_virtual_step = 0;
        g_transpose_virtual_entry_step = step;
        g_transpose_first_call = 0;
    }

    transpose_step_t *current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    uint32_t duration_in_steps = current_virtual->duration;
    uint32_t steps_in_current = step - g_transpose_virtual_entry_step;

    if (steps_in_current >= duration_in_steps) {
        /* Check for jump BEFORE advancing */
        if (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) {
            if (check_transpose_condition(current_virtual)) {
                printf("    *** JUMP: %d -> %d ***\n",
                       g_transpose_virtual_step, current_virtual->jump);
                g_transpose_virtual_step = current_virtual->jump;
                g_transpose_virtual_entry_step = step;
                current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
                return current_virtual->transpose;
            }
        }

        /* Advance normally */
        int next_virtual = g_transpose_virtual_step + 1;
        if (next_virtual >= g_transpose_step_count) {
            next_virtual = 0;
            g_transpose_loop_count++;
        }

        g_transpose_virtual_step = next_virtual;
        g_transpose_virtual_entry_step = step;
        current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    }

    return current_virtual->transpose;
}

void start_playback() {
    printf("\n*** START PLAYBACK ***\n");
    g_transpose_virtual_step = 0;
    g_transpose_virtual_entry_step = 0;
    g_transpose_loop_count = 0;
    g_transpose_first_call = 1;
}

int main() {
    printf("=== Transpose Jump Test: User Scenario ===\n");
    printf("4 transpose steps, step 2 jumps to step 1\n\n");

    /* Initialize */
    for (int i = 0; i < MAX_TRANSPOSE_STEPS; i++) {
        g_transpose_sequence[i].jump = -1;
    }

    g_transpose_step_count = 4;

    /* Steps 0-3: transpose +1, +2, +3, +4, each 16 steps (1 bar) */
    for (int i = 0; i < 4; i++) {
        g_transpose_sequence[i].transpose = i + 1;
        g_transpose_sequence[i].duration = 16;
        g_transpose_sequence[i].jump = -1;
        g_transpose_sequence[i].condition_n = 0;
        g_transpose_sequence[i].condition_m = 0;
        g_transpose_sequence[i].condition_not = 0;
    }

    /* Step 2 jumps to step 1 */
    g_transpose_sequence[2].jump = 1;

    printf("Sequence:\n");
    for (int i = 0; i < 4; i++) {
        printf("  Step %d: transpose=%+d, duration=%d, jump=%d\n",
               i, g_transpose_sequence[i].transpose,
               g_transpose_sequence[i].duration,
               g_transpose_sequence[i].jump);
    }

    printf("\nExpected: 0 → 1 → 2 → JUMP(1) → 1 → 2 → JUMP(1) → ...\n");
    printf("(Steps 0 and 3 should never play after first cycle)\n");

    start_playback();

    /* Simulate 5 bars of playback */
    int step_play_count[4] = {0, 0, 0, 0};
    int last_virtual = -1;

    for (uint32_t step = 0; step < 80; step++) {
        int8_t transpose = get_transpose_at_step(step);

        if (g_transpose_virtual_step != last_virtual) {
            printf("Step %2u: Virtual=%d, Transpose=%+d\n",
                   step, g_transpose_virtual_step, transpose);
            step_play_count[g_transpose_virtual_step]++;
            last_virtual = g_transpose_virtual_step;
        }
    }

    printf("\nPlay counts:\n");
    for (int i = 0; i < 4; i++) {
        printf("  Step %d: %d times%s\n", i, step_play_count[i],
               (i == 0) ? " (first cycle only)" :
               (i == 3) ? " (NEVER - correct!)" : "");
    }

    if (step_play_count[3] == 0 && step_play_count[1] > 1 && step_play_count[2] > 1) {
        printf("\n✓ TEST PASSED: Step 2 jumps to step 1, skipping steps 0 and 3!\n");
    } else {
        printf("\n✗ TEST FAILED\n");
    }

    return 0;
}
