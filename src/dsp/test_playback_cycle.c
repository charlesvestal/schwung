/*
 * Test transpose jumps with play/stop cycle
 * Simulates: play, advance, stop, modify, play again
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
                printf("  [JUMP] %d -> %d at step %u\n",
                       g_transpose_virtual_step, current_virtual->jump, step);
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

/* Simulate playback start */
void start_playback() {
    printf("*** PLAYBACK START - Reset virtual playhead ***\n");
    g_transpose_virtual_step = 0;
    g_transpose_virtual_entry_step = 0;
    g_transpose_loop_count = 0;
    g_transpose_first_call = 1;
}

int main() {
    printf("=== Test: Play/Stop/Modify/Play Cycle ===\n\n");

    /* Initialize jumps to -1 */
    for (int i = 0; i < MAX_TRANSPOSE_STEPS; i++) {
        g_transpose_sequence[i].jump = -1;
    }

    /* Set up 4 transpose steps, no jumps initially */
    g_transpose_step_count = 4;

    for (int i = 0; i < 4; i++) {
        g_transpose_sequence[i].transpose = i + 1;  /* +1, +2, +3, +4 */
        g_transpose_sequence[i].duration = 16;      /* 1 bar each */
        g_transpose_sequence[i].jump = -1;          /* No jump */
        g_transpose_sequence[i].condition_n = 0;
        g_transpose_sequence[i].condition_m = 0;
        g_transpose_sequence[i].condition_not = 0;
    }

    printf("Sequence: 4 steps, 16 steps each, no jumps\n\n");

    /* First playback session - play 2 bars */
    printf("=== Session 1: Play 2 bars ===\n");
    start_playback();

    for (uint32_t step = 0; step < 32; step += 4) {
        int8_t transpose = get_transpose_at_step(step);
        printf("Step %u: virtual_step=%d, transpose=%+d\n",
               step, g_transpose_virtual_step, transpose);
    }

    printf("\n*** STOP PLAYBACK ***\n");
    printf("(virtual_step=%d, entry_step=%u remain in memory)\n\n",
           g_transpose_virtual_step, g_transpose_virtual_entry_step);

    /* Modify: Set step 2 to jump to step 1 */
    printf("=== User modifies: Step 2 jumps to Step 1 ===\n\n");
    g_transpose_sequence[2].jump = 1;

    /* Second playback session - WITHOUT reset (OLD BUG) */
    printf("=== Session 2 WITHOUT RESET (OLD BUG): ===\n");
    printf("virtual_step=%d, entry_step=%u (NOT reset!)\n",
           g_transpose_virtual_step, g_transpose_virtual_entry_step);

    /* Don't call start_playback() to simulate the bug */
    g_transpose_first_call = 1;  /* But first_call gets set */

    for (uint32_t step = 0; step < 64; step += 4) {
        int8_t transpose = get_transpose_at_step(step);
        printf("Step %u: virtual_step=%d, transpose=%+d\n",
               step, g_transpose_virtual_step, transpose);
        if (step == 28) {
            printf("... (continuing shows broken behavior)\n");
            break;
        }
    }

    printf("\n*** STOP PLAYBACK ***\n\n");

    /* Third playback session - WITH reset (FIXED) */
    printf("=== Session 3 WITH RESET (FIXED): ===\n");
    start_playback();

    for (uint32_t step = 0; step < 64; step += 4) {
        int8_t transpose = get_transpose_at_step(step);
        printf("Step %u: virtual_step=%d, transpose=%+d\n",
               step, g_transpose_virtual_step, transpose);
        if (step == 44) {
            printf("... (continuing shows correct loop)\n");
            break;
        }
    }

    return 0;
}
