/*
 * EXACT user scenario test
 * 4 steps, step 3 jumps to step 1
 * Step sequence: 1→2→3→1→2→3 (step 4 never plays)
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
static int g_transpose_sequence_enabled = 1;
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

static void update_transpose_virtual_playhead(uint32_t step) {
    if (!g_transpose_sequence_enabled) return;
    if (g_transpose_step_count == 0) return;

    if (g_transpose_first_call) {
        g_transpose_virtual_step = 0;
        g_transpose_virtual_entry_step = step;
        g_transpose_first_call = 0;
        printf("    [INIT] virtual_step=0, entry_step=%u\n", step);
        return;
    }

    transpose_step_t *current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    uint32_t duration_in_steps = current_virtual->duration;
    uint32_t steps_in_current = step - g_transpose_virtual_entry_step;

    if (steps_in_current >= duration_in_steps) {
        printf("    [ADVANCE] step=%u, virtual_step=%d finished (duration=%u)\n",
               step, g_transpose_virtual_step, duration_in_steps);

        if (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) {
            printf("    [CHECK JUMP] jump=%d, step_count=%d, condition_n=%d\n",
                   current_virtual->jump, g_transpose_step_count, current_virtual->condition_n);

            if (check_transpose_condition(current_virtual)) {
                printf("    [JUMP EXECUTED] %d → %d\n", g_transpose_virtual_step, current_virtual->jump);
                g_transpose_virtual_step = current_virtual->jump;
                g_transpose_virtual_entry_step = step;
                return;
            } else {
                printf("    [JUMP BLOCKED] condition failed\n");
            }
        } else {
            printf("    [NO JUMP] jump=%d, step_count=%d\n",
                   current_virtual->jump, g_transpose_step_count);
        }

        int next_virtual = g_transpose_virtual_step + 1;
        if (next_virtual >= g_transpose_step_count) {
            printf("    [WRAP] %d → 0\n", g_transpose_virtual_step);
            next_virtual = 0;
            g_transpose_loop_count++;
        } else {
            printf("    [NORMAL ADVANCE] %d → %d\n", g_transpose_virtual_step, next_virtual);
        }
        g_transpose_virtual_step = next_virtual;
        g_transpose_virtual_entry_step = step;
    }
}

static int8_t get_transpose_at_step(void) {
    if (!g_transpose_sequence_enabled) return 0;
    if (g_transpose_step_count == 0) return 0;
    return g_transpose_sequence[g_transpose_virtual_step].transpose;
}

int main() {
    printf("EXACT User Scenario Test\n");
    printf("========================\n\n");

    /* User has 4 steps */
    g_transpose_step_count = 4;
    memset(g_transpose_sequence, 0, sizeof(g_transpose_sequence));

    /* Step 0 (UI "Step 1"): duration=4, no jump */
    g_transpose_sequence[0].transpose = 0;
    g_transpose_sequence[0].duration = 16;  /* 4 beats = 16 steps */
    g_transpose_sequence[0].jump = -1;
    g_transpose_sequence[0].condition_n = 0;
    g_transpose_sequence[0].condition_m = 0;
    g_transpose_sequence[0].condition_not = 0;

    /* Step 1 (UI "Step 2"): duration=4, no jump */
    g_transpose_sequence[1].transpose = 5;
    g_transpose_sequence[1].duration = 16;
    g_transpose_sequence[1].jump = -1;
    g_transpose_sequence[1].condition_n = 0;
    g_transpose_sequence[1].condition_m = 0;
    g_transpose_sequence[1].condition_not = 0;

    /* Step 2 (UI "Step 3"): duration=4, JUMP TO 0 */
    g_transpose_sequence[2].transpose = 7;
    g_transpose_sequence[2].duration = 16;
    g_transpose_sequence[2].jump = 0;  /* Jump to step 0 */
    g_transpose_sequence[2].condition_n = 0;  /* No condition (always jump) */
    g_transpose_sequence[2].condition_m = 0;
    g_transpose_sequence[2].condition_not = 0;

    /* Step 3 (UI "Step 4"): duration=4, no jump - SHOULD NEVER PLAY */
    g_transpose_sequence[3].transpose = 99;
    g_transpose_sequence[3].duration = 16;
    g_transpose_sequence[3].jump = -1;
    g_transpose_sequence[3].condition_n = 0;
    g_transpose_sequence[3].condition_m = 0;
    g_transpose_sequence[3].condition_not = 0;

    printf("Setup:\n");
    printf("  Step 0 (UI '1'): transpose=0, duration=16, jump=-1\n");
    printf("  Step 1 (UI '2'): transpose=5, duration=16, jump=-1\n");
    printf("  Step 2 (UI '3'): transpose=7, duration=16, jump=0 ← JUMPS TO STEP 0\n");
    printf("  Step 3 (UI '4'): transpose=99, duration=16 ← SHOULD NEVER PLAY\n\n");

    printf("Expected playhead: 0→1→2→(jump)0→1→2→(jump)0→...\n");
    printf("User sees on LEDs: 1→2→3→(jump)1→2→3→(jump)1→...\n");
    printf("Step 4 LED should NEVER light up!\n\n");

    printf("Simulating playback:\n\n");

    int found_99 = 0;
    int step_play_count[4] = {0, 0, 0, 0};
    int last_virtual = -1;

    for (uint32_t step = 0; step < 80; step++) {
        /* Only update on step boundaries */
        update_transpose_virtual_playhead(step);

        if (g_transpose_virtual_step != last_virtual) {
            step_play_count[g_transpose_virtual_step]++;
            last_virtual = g_transpose_virtual_step;
        }

        int8_t t = get_transpose_at_step();
        if (t == 99) found_99 = 1;

        /* Print every 16 steps (one full step duration) */
        if (step % 16 == 0) {
            printf("Step %2u: virtual_step=%d, transpose=%d\n",
                   step, g_transpose_virtual_step, t);
        }
    }

    printf("\n");
    printf("Play counts:\n");
    for (int i = 0; i < 4; i++) {
        printf("  Step %d (UI '%d'): %d times%s\n",
               i, i+1, step_play_count[i],
               i == 3 ? " ← SHOULD BE 0!" : "");
    }

    printf("\n");
    if (found_99) {
        printf("✗ FAIL: Step 3 (transpose=99) was played!\n");
        return 1;
    } else if (step_play_count[3] > 0) {
        printf("✗ FAIL: Step 3 was entered %d times (should be 0)\n", step_play_count[3]);
        return 1;
    } else {
        printf("✓ PASS: Jump worked! Step 3 never played.\n");
        return 0;
    }
}
