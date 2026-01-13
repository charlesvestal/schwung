/*
 * Verification test for jump fix
 * Tests that jumps work when update function is called independently
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

/* NEW: Separate update function (called every frame) */
static void update_transpose_virtual_playhead(uint32_t step) {
    if (!g_transpose_sequence_enabled) return;
    if (g_transpose_step_count == 0) return;

    if (g_transpose_first_call) {
        g_transpose_virtual_step = 0;
        g_transpose_virtual_entry_step = step;
        g_transpose_first_call = 0;
        return;
    }

    transpose_step_t *current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    uint32_t duration_in_steps = current_virtual->duration;
    uint32_t steps_in_current = step - g_transpose_virtual_entry_step;

    if (steps_in_current >= duration_in_steps) {
        if (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) {
            if (check_transpose_condition(current_virtual)) {
                g_transpose_virtual_step = current_virtual->jump;
                g_transpose_virtual_entry_step = step;
                return;
            }
        }

        int next_virtual = g_transpose_virtual_step + 1;
        if (next_virtual >= g_transpose_step_count) {
            next_virtual = 0;
            g_transpose_loop_count++;
        }
        g_transpose_virtual_step = next_virtual;
        g_transpose_virtual_entry_step = step;
    }
}

/* NEW: Just returns current value (no advancing) */
static int8_t get_transpose_at_step(void) {
    if (!g_transpose_sequence_enabled) return 0;
    if (g_transpose_step_count == 0) return 0;
    return g_transpose_sequence[g_transpose_virtual_step].transpose;
}

int main() {
    printf("Jump Fix Verification Test\n");
    printf("===========================\n\n");

    /* Setup: 4 steps, step 2 jumps to step 0 */
    memset(g_transpose_sequence, 0, sizeof(g_transpose_sequence));
    g_transpose_step_count = 4;

    g_transpose_sequence[0].transpose = 0;
    g_transpose_sequence[0].duration = 4;
    g_transpose_sequence[0].jump = -1;
    g_transpose_sequence[0].condition_n = 0;

    g_transpose_sequence[1].transpose = 5;
    g_transpose_sequence[1].duration = 4;
    g_transpose_sequence[1].jump = -1;
    g_transpose_sequence[1].condition_n = 0;

    g_transpose_sequence[2].transpose = 7;
    g_transpose_sequence[2].duration = 4;
    g_transpose_sequence[2].jump = 0;  /* Jump back to step 0 */
    g_transpose_sequence[2].condition_n = 0;

    g_transpose_sequence[3].transpose = 99;  /* Should never play */
    g_transpose_sequence[3].duration = 4;
    g_transpose_sequence[3].jump = -1;
    g_transpose_sequence[3].condition_n = 0;

    printf("Setup:\n");
    printf("  Step 0: transpose=0, duration=4\n");
    printf("  Step 1: transpose=5, duration=4\n");
    printf("  Step 2: transpose=7, duration=4, jump=0\n");
    printf("  Step 3: transpose=99 (SHOULD NEVER PLAY)\n\n");

    printf("Simulating continuous playback (update called every step):\n\n");

    int found_99 = 0;
    int errors = 0;

    for (uint32_t step = 0; step < 24; step++) {
        /* Update playhead (simulates main audio loop) */
        update_transpose_virtual_playhead(step);

        /* Get current transpose value */
        int8_t t = get_transpose_at_step();

        /* Determine expected */
        int cycle_pos = step % 12;
        int expected;
        if (cycle_pos < 4) expected = 0;
        else if (cycle_pos < 8) expected = 5;
        else expected = 7;

        char mark = ' ';
        if (t == 99) {
            mark = 'X';
            found_99 = 1;
        } else if (t != expected) {
            mark = '!';
            errors++;
        }

        printf("  [%c] Step %2u: transpose=%2d (expected %2d), virtual_step=%d\n",
               mark, step, t, expected, g_transpose_virtual_step);
    }

    printf("\n");
    if (found_99) {
        printf("✗ TEST FAILED: Step 3 (transpose=99) WAS PLAYED!\n");
        return 1;
    } else if (errors > 0) {
        printf("✗ TEST FAILED: %d steps had incorrect transpose values\n", errors);
        return 1;
    } else {
        printf("✓ TEST PASSED: Jump worked correctly!\n");
        printf("\nThe fix works - the playhead now advances independently,\n");
        printf("so jumps execute even when no notes are playing.\n");
        return 0;
    }
}
