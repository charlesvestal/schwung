/*
 * Test transpose jump behavior - REALISTIC VERSION
 * Simulates the ACTUAL parameter flow from JS to DSP
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define MAX_TRANSPOSE_STEPS 16

typedef struct {
    int8_t transpose;
    uint16_t duration;      /* Duration stored as sent by JS (already in steps!) */
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
    if (g_transpose_step_count == 0) {
        return 0;
    }

    if (g_transpose_first_call) {
        g_transpose_virtual_step = 0;
        g_transpose_virtual_entry_step = step;
        g_transpose_first_call = 0;
    }

    transpose_step_t *current_virtual = &g_transpose_sequence[g_transpose_virtual_step];

    /* CRITICAL: Duration is ALREADY in steps (JS converted beats*4) */
    uint32_t duration_in_steps = current_virtual->duration * 4;  /* BUG? Double conversion? */

    uint32_t steps_in_current = step - g_transpose_virtual_entry_step;

    if (steps_in_current >= duration_in_steps) {
        /* Check for jump BEFORE advancing */
        if (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) {
            if (check_transpose_condition(current_virtual)) {
                printf("  [JUMP] Step %d -> Step %d (at global step %u)\n",
                       g_transpose_virtual_step, current_virtual->jump, step);
                g_transpose_virtual_step = current_virtual->jump;
                g_transpose_virtual_entry_step = step;
                current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
                return current_virtual->transpose;
            }
        }

        int next_virtual = g_transpose_virtual_step + 1;
        if (next_virtual >= g_transpose_step_count) {
            next_virtual = 0;
            g_transpose_loop_count++;
            printf("  [WRAP] Loop count=%u (at global step %u)\n", g_transpose_loop_count, step);
        }

        g_transpose_virtual_step = next_virtual;
        g_transpose_virtual_entry_step = step;
        current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    }

    return current_virtual->transpose;
}

/* Simulate set_param like real DSP */
void set_param(const char *key, const char *val) {
    if (strncmp(key, "transpose_step_", 15) == 0) {
        int step_idx = atoi(key + 15);
        const char *param = strchr(key + 15, '_');
        if (param) {
            param++;
            if (strcmp(param, "transpose") == 0) {
                g_transpose_sequence[step_idx].transpose = atoi(val);
            }
            else if (strcmp(param, "duration") == 0) {
                g_transpose_sequence[step_idx].duration = atoi(val);
                printf("Set step %d duration to %d steps\n", step_idx, atoi(val));
            }
            else if (strcmp(param, "jump") == 0) {
                g_transpose_sequence[step_idx].jump = atoi(val);
                printf("Set step %d jump to %d\n", step_idx, atoi(val));
            }
            else if (strcmp(param, "condition_n") == 0) {
                g_transpose_sequence[step_idx].condition_n = atoi(val);
            }
            else if (strcmp(param, "condition_m") == 0) {
                g_transpose_sequence[step_idx].condition_m = atoi(val);
            }
            else if (strcmp(param, "condition_not") == 0) {
                g_transpose_sequence[step_idx].condition_not = (strcmp(val, "1") == 0);
            }
        }
    }
    else if (strcmp(key, "transpose_step_count") == 0) {
        g_transpose_step_count = atoi(val);
        printf("Set step count to %d\n", g_transpose_step_count);
    }
}

int main() {
    printf("=== REALISTIC Transpose Jump Test ===\n");
    printf("Simulating ACTUAL parameter flow from JS->DSP\n\n");

    /* Initialize like clear_transpose_sequence */
    for (int i = 0; i < MAX_TRANSPOSE_STEPS; i++) {
        g_transpose_sequence[i].jump = -1;
    }

    /* Simulate JS syncTransposeSequenceToDSP() for set 23 */
    printf("--- Simulating JS parameter sync ---\n");

    /* Step 0: transpose=3, duration=4 beats -> 16 steps */
    set_param("transpose_step_0_transpose", "3");
    set_param("transpose_step_0_duration", "16");  /* JS already converted: 4*4=16 */
    set_param("transpose_step_0_jump", "-1");
    set_param("transpose_step_0_condition_n", "0");
    set_param("transpose_step_0_condition_m", "0");
    set_param("transpose_step_0_condition_not", "0");

    /* Step 1: transpose=1, duration=4 beats -> 16 steps, JUMP TO 0 */
    set_param("transpose_step_1_transpose", "1");
    set_param("transpose_step_1_duration", "16");
    set_param("transpose_step_1_jump", "0");  /* JUMP! */
    set_param("transpose_step_1_condition_n", "0");
    set_param("transpose_step_1_condition_m", "0");
    set_param("transpose_step_1_condition_not", "0");

    /* Step 2: transpose=7, duration=1 beat -> 4 steps */
    set_param("transpose_step_2_transpose", "7");
    set_param("transpose_step_2_duration", "4");  /* JS: 1*4=4 */
    set_param("transpose_step_2_jump", "-1");
    set_param("transpose_step_2_condition_n", "0");
    set_param("transpose_step_2_condition_m", "0");
    set_param("transpose_step_2_condition_not", "0");

    /* Step 3: transpose=5, duration=4 beats -> 16 steps */
    set_param("transpose_step_3_transpose", "5");
    set_param("transpose_step_3_duration", "16");
    set_param("transpose_step_3_jump", "-1");
    set_param("transpose_step_3_condition_n", "0");
    set_param("transpose_step_3_condition_m", "0");
    set_param("transpose_step_3_condition_not", "0");

    set_param("transpose_step_count", "4");

    printf("\n--- Sequence loaded ---\n");
    for (int i = 0; i < g_transpose_step_count; i++) {
        printf("Step %d: transpose=%+d, duration=%d steps, jump=%d\n",
               i, g_transpose_sequence[i].transpose,
               g_transpose_sequence[i].duration,
               g_transpose_sequence[i].jump);
    }

    printf("\n--- Simulating playback (8 bars = 128 steps) ---\n\n");

    int step_entries[4] = {0, 0, 0, 0};
    int last_virtual = -1;

    for (uint32_t step = 0; step < 128; step++) {
        int8_t transpose = get_transpose_at_step(step);

        if (g_transpose_virtual_step != last_virtual) {
            printf("Step %3u (Bar %.1f): Virtual Step %d, Transpose %+d\n",
                   step, step/16.0f, g_transpose_virtual_step, transpose);
            step_entries[g_transpose_virtual_step]++;
            last_virtual = g_transpose_virtual_step;
        }
    }

    printf("\n=== Results ===\n");
    for (int i = 0; i < 4; i++) {
        printf("Step %d: entered %d times%s\n", i, step_entries[i],
               (i >= 2 && step_entries[i] == 0) ? " ← SHOULD BE 0!" :
               (i >= 2 && step_entries[i] > 0) ? " ← BUG: Should never play!" : "");
    }

    if (step_entries[0] > 0 && step_entries[1] > 0 && step_entries[2] == 0 && step_entries[3] == 0) {
        printf("\n✓ PASS: Infinite loop 0↔1\n");
        return 0;
    } else {
        printf("\n✗ FAIL: Steps 2 and 3 played!\n");
        return 1;
    }
}
