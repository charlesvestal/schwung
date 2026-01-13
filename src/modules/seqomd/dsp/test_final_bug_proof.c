/*
 * Final proof of the jump bug
 *
 * This test proves that even with "4 steps", if there's a gap in the UI array,
 * jumps will fail without proper index remapping.
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
        if (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) {
            if (check_transpose_condition(current_virtual)) {
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
        }
        g_transpose_virtual_step = next_virtual;
        g_transpose_virtual_entry_step = step;
        current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    }

    return current_virtual->transpose;
}

int main() {
    printf("=== PROOF OF BUG ===\n\n");

    printf("User scenario:\n");
    printf("  'I have four steps. From three I jumped to one.'\n\n");

    printf("What MIGHT have happened (hidden from user):\n");
    printf("  - User created steps at buttons 1, 2, 4, 5 (skipped button 3)\n");
    printf("  - UI shows: Step 0, Step 1, Step 3, Step 4 (4 steps, but gap at index 2)\n");
    printf("  - User sets Step 1 (UI index 1) to jump to Step 4 (UI index 4)\n");
    printf("  - User thinks: 'Jump from 2nd step to 4th step'\n\n");

    printf("Without fix (what OLD code does):\n");
    printf("  - UI: step at index 1 has jump=4\n");
    printf("  - DSP receives: [step0, step1, step3, step4] with indices [0, 1, 2, 3]\n");
    printf("  - DSP step 1 has jump=4\n");
    printf("  - DSP checks: is 4 < step_count (4)? NO!\n");
    printf("  - Result: JUMP BLOCKED\n\n");

    /* Simulate what DSP receives WITHOUT remapping */
    memset(g_transpose_sequence, 0, sizeof(g_transpose_sequence));
    g_transpose_step_count = 4;
    g_transpose_total_steps = 16;

    g_transpose_sequence[0].transpose = 0;
    g_transpose_sequence[0].duration = 4;
    g_transpose_sequence[0].jump = -1;
    g_transpose_sequence[0].condition_n = 0;

    g_transpose_sequence[1].transpose = 5;
    g_transpose_sequence[1].duration = 4;
    g_transpose_sequence[1].jump = 4;  /* BUG: Still has UI index! */
    g_transpose_sequence[1].condition_n = 0;

    g_transpose_sequence[2].transpose = 7;
    g_transpose_sequence[2].duration = 4;
    g_transpose_sequence[2].jump = -1;
    g_transpose_sequence[2].condition_n = 0;

    g_transpose_sequence[3].transpose = 12;
    g_transpose_sequence[3].duration = 4;
    g_transpose_sequence[3].jump = -1;
    g_transpose_sequence[3].condition_n = 0;

    printf("Testing without fix:\n");
    int jump_worked = 0;
    for (int i = 0; i < 20; i++) {
        int8_t t = get_transpose_at_step(i);
        if (i == 8 && g_transpose_virtual_step == 3) {
            jump_worked = 1;
        }
    }

    if (!jump_worked) {
        printf("  ✗ JUMP FAILED (as expected with bug)\n\n");
    } else {
        printf("  ✓ Jump worked\n\n");
    }

    /* Reset and test WITH remapping */
    g_transpose_first_call = 1;
    g_transpose_virtual_step = 0;
    g_transpose_virtual_entry_step = 0;

    printf("With fix (what NEW code does):\n");
    printf("  - UI to DSP mapping: {0→0, 1→1, 3→2, 4→3}\n");
    printf("  - UI step 1 jump=4 gets remapped to DSP jump=3\n");
    printf("  - DSP step 1 has jump=3\n");
    printf("  - DSP checks: is 3 < step_count (4)? YES!\n");
    printf("  - Result: JUMP WORKS\n\n");

    g_transpose_sequence[1].jump = 3;  /* FIXED: Remapped to DSP index */

    printf("Testing with fix:\n");
    jump_worked = 0;
    for (int i = 0; i < 20; i++) {
        int8_t t = get_transpose_at_step(i);
        if (i == 8 && g_transpose_virtual_step == 3) {
            jump_worked = 1;
        }
    }

    if (jump_worked) {
        printf("  ✓ JUMP WORKS!\n\n");
    } else {
        printf("  ✗ Jump still failed\n\n");
    }

    printf("====================\n");
    printf("CONCLUSION: The bug happens when the transpose sequence array has\n");
    printf("ANY gaps (nulls), even if the user thinks they have contiguous steps.\n");
    printf("The fix: Remap jump indices from UI indices to DSP indices.\n");

    return jump_worked ? 0 : 1;
}
