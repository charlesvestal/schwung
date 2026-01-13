/*
 * Test the exact user scenario:
 * "I have four steps. From three I jumped to one. It should never play four."
 *
 * Setup:
 *  Step 0 (user calls it "1"): transpose=0, duration=4
 *  Step 1 (user calls it "2"): transpose=5, duration=4
 *  Step 2 (user calls it "3"): transpose=7, duration=4, jump=0 (jump to "1")
 *  Step 3 (user calls it "4"): transpose=99, duration=4 - SHOULD NEVER PLAY
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
    if (!g_transpose_sequence_enabled) {
        return 0;
    }
    if (g_transpose_step_count == 0 || g_transpose_total_steps == 0) {
        return 0;
    }

    if (g_transpose_first_call) {
        g_transpose_virtual_step = 0;
        g_transpose_virtual_entry_step = step;
        g_transpose_first_call = 0;
    }

    transpose_step_t *current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    uint32_t duration_in_steps = current_virtual->duration;
    uint32_t steps_in_current = step - g_transpose_virtual_entry_step;

    if (steps_in_current >= duration_in_steps) {
        /* Check for jump */
        if (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) {
            if (check_transpose_condition(current_virtual)) {
                g_transpose_virtual_step = current_virtual->jump;
                g_transpose_virtual_entry_step = step;
                current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
                return current_virtual->transpose;
            }
        }

        /* Normal advance */
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

int main() {
    printf("User Scenario Test\n");
    printf("==================\n\n");
    printf("User description:\n");
    printf("  'I have four steps. From three I jumped to one.'\n");
    printf("  'It should never play four.'\n\n");

    /* Initialize all steps */
    memset(g_transpose_sequence, 0, sizeof(g_transpose_sequence));

    /* Set up 4 steps */
    g_transpose_step_count = 4;
    g_transpose_total_steps = 16;  /* 4 steps * 4 duration each */

    /* Step 0 (user calls it "step 1") */
    g_transpose_sequence[0].transpose = 0;
    g_transpose_sequence[0].duration = 4;
    g_transpose_sequence[0].jump = -1;  /* No jump */
    g_transpose_sequence[0].condition_n = 0;

    /* Step 1 (user calls it "step 2") */
    g_transpose_sequence[1].transpose = 5;
    g_transpose_sequence[1].duration = 4;
    g_transpose_sequence[1].jump = -1;  /* No jump */
    g_transpose_sequence[1].condition_n = 0;

    /* Step 2 (user calls it "step 3") - JUMPS BACK TO STEP 0 */
    g_transpose_sequence[2].transpose = 7;
    g_transpose_sequence[2].duration = 4;
    g_transpose_sequence[2].jump = 0;  /* Jump to step 0 (user's "step 1") */
    g_transpose_sequence[2].condition_n = 0;

    /* Step 3 (user calls it "step 4") - SHOULD NEVER BE PLAYED */
    g_transpose_sequence[3].transpose = 99;  /* Use 99 to easily spot if it plays */
    g_transpose_sequence[3].duration = 4;
    g_transpose_sequence[3].jump = -1;
    g_transpose_sequence[3].condition_n = 0;

    printf("Actual setup (0-indexed):\n");
    printf("  Step 0: transpose=0, duration=4, no jump\n");
    printf("  Step 1: transpose=5, duration=4, no jump\n");
    printf("  Step 2: transpose=7, duration=4, jump=0 (back to step 0)\n");
    printf("  Step 3: transpose=99, duration=4 (SHOULD NEVER PLAY)\n\n");

    printf("Expected behavior:\n");
    printf("  Steps 0-3:   transpose=0 (playing step 0)\n");
    printf("  Steps 4-7:   transpose=5 (playing step 1)\n");
    printf("  Steps 8-11:  transpose=7 (playing step 2)\n");
    printf("  Step 12:     JUMP back to step 0, transpose=0\n");
    printf("  Steps 12-15: transpose=0 (playing step 0 again)\n");
    printf("  ...loop continues...\n");
    printf("  Transpose 99 should NEVER appear!\n\n");

    printf("Actual behavior:\n");

    int found_99 = 0;
    int errors = 0;

    for (uint32_t i = 0; i < 24; i++) {
        int8_t t = get_transpose_at_step(i);

        /* Determine expected */
        int cycle_pos = i % 12;  /* After jump, cycle is 12 steps */
        int expected;
        if (i < 12) {
            /* First time through */
            if (cycle_pos < 4) expected = 0;
            else if (cycle_pos < 8) expected = 5;
            else expected = 7;
        } else {
            /* After jump - should loop 0->5->7->0... */
            cycle_pos = (i - 12) % 12;
            if (cycle_pos < 4) expected = 0;
            else if (cycle_pos < 8) expected = 5;
            else expected = 7;
        }

        char mark = ' ';
        if (t == 99) {
            mark = 'X';
            found_99 = 1;
        } else if (t != expected) {
            mark = '!';
            errors++;
        }

        printf("  [%c] Step %2u: transpose=%2d (expected %2d), virtual_step=%d\n",
               mark, i, t, expected, g_transpose_virtual_step);
    }

    printf("\n");
    if (found_99) {
        printf("✗ TEST FAILED: Step 3 (transpose=99) WAS PLAYED!\n");
        printf("  This is the bug the user reported.\n");
        return 1;
    } else if (errors > 0) {
        printf("✗ TEST FAILED: %d steps had incorrect transpose values\n", errors);
        return 1;
    } else {
        printf("✓ TEST PASSED: Jump worked correctly, step 3 never played\n");
        return 0;
    }
}
