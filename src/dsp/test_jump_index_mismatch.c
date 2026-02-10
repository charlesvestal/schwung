/*
 * Test for jump index mismatch bug
 *
 * Scenario: User creates 3 steps, then deletes step 1
 * UI shows: Step 0, Step 2
 * But step 0 still has jump=2 (referencing UI step 2)
 * DSP receives: 2 steps with indices 0, 1
 * DSP step 0 has jump=2, but only 2 steps exist (indices 0, 1)
 * So jump=2 is out of bounds (not < step_count=2)
 * Result: Jump never fires!
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
    if (step->condition_n <= 0) return 1;
    int iteration = (g_transpose_loop_count % step->condition_n) + 1;
    int should_apply = (iteration == step->condition_m);
    if (step->condition_not) should_apply = !should_apply;
    return should_apply;
}

static int8_t get_transpose_at_step(uint32_t step) {
    if (!g_transpose_sequence_enabled) return 0;
    if (g_transpose_step_count == 0 || g_transpose_total_steps == 0) return 0;

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
        printf("    Step %u: Checking jump: jump=%d, step_count=%d, condition: %d < %d? %s\n",
               step, current_virtual->jump, g_transpose_step_count,
               current_virtual->jump, g_transpose_step_count,
               (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) ? "YES" : "NO");

        if (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) {
            if (check_transpose_condition(current_virtual)) {
                printf("    -> JUMP FIRES to step %d\n", current_virtual->jump);
                g_transpose_virtual_step = current_virtual->jump;
                g_transpose_virtual_entry_step = step;
                current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
                return current_virtual->transpose;
            }
        } else {
            printf("    -> JUMP BLOCKED: %d is not < %d\n",
                   current_virtual->jump, g_transpose_step_count);
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
    printf("Jump Index Mismatch Bug Test\n");
    printf("=============================\n\n");

    printf("Scenario:\n");
    printf("  User creates steps 0, 1, 2\n");
    printf("  Step 0 has jump=2 (jump to step 2)\n");
    printf("  User deletes step 1\n");
    printf("  UI now shows: Step 0, Step 2\n");
    printf("  But Step 0 still has jump=2\n\n");

    printf("What DSP receives (after syncTransposeSequenceToDSP):\n");
    printf("  DSP index 0: (from UI step 0) jump=2\n");
    printf("  DSP index 1: (from UI step 2)\n");
    printf("  step_count = 2\n\n");

    printf("BUG: DSP checks if jump=2 < step_count=2\n");
    printf("     2 < 2 is FALSE, so jump is blocked!\n\n");

    /* Simulate what DSP receives */
    memset(g_transpose_sequence, 0, sizeof(g_transpose_sequence));
    g_transpose_step_count = 2;  /* Only 2 steps after deletion */
    g_transpose_total_steps = 8;

    /* DSP index 0 (was UI step 0) */
    g_transpose_sequence[0].transpose = 0;
    g_transpose_sequence[0].duration = 4;
    g_transpose_sequence[0].jump = 2;  /* STILL REFERENCES UI STEP 2, which is now DSP index 1! */
    g_transpose_sequence[0].condition_n = 0;

    /* DSP index 1 (was UI step 2) */
    g_transpose_sequence[1].transpose = 12;
    g_transpose_sequence[1].duration = 4;
    g_transpose_sequence[1].jump = -1;
    g_transpose_sequence[1].condition_n = 0;

    printf("Testing playback:\n\n");

    for (uint32_t i = 0; i < 12; i++) {
        int8_t t = get_transpose_at_step(i);
        printf("  Step %2u: transpose=%d, virtual_step=%d\n", i, t, g_transpose_virtual_step);
    }

    printf("\n");
    printf("✗ BUG CONFIRMED: Jump never fires because jump=2 is not < step_count=2\n");
    printf("\nThis is why jumps don't work on the device!\n");
    printf("\nThe fix: Jump indices must be remapped when syncing to DSP.\n");

    return 1;
}
