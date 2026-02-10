/*
 * Complex test for transpose jump scenarios
 *
 * Tests:
 * 1. Simple forward jump (0 -> 2)
 * 2. Backward jump (2 -> 0 creating a loop)
 * 3. Jump to self (infinite loop on one step)
 * 4. Jump with gaps in sequence
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

static void reset_state(void) {
    g_transpose_virtual_step = 0;
    g_transpose_virtual_entry_step = 0;
    g_transpose_first_call = 1;
    g_transpose_loop_count = 0;
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

void test_backward_jump(void) {
    printf("\n=== TEST 1: Backward Jump (Creating Loop) ===\n");
    printf("Setup:\n");
    printf("  Step 0: transpose=0, duration=2, jump=2\n");
    printf("  Step 1: transpose=5, duration=2, no jump\n");
    printf("  Step 2: transpose=7, duration=2, jump=0 (BACKWARD JUMP)\n\n");

    reset_state();
    memset(g_transpose_sequence, 0, sizeof(g_transpose_sequence));

    g_transpose_step_count = 3;
    g_transpose_sequence[0].transpose = 0;
    g_transpose_sequence[0].duration = 2;
    g_transpose_sequence[0].jump = 2;  /* Skip step 1 */
    g_transpose_sequence[0].condition_n = 0;

    g_transpose_sequence[1].transpose = 5;
    g_transpose_sequence[1].duration = 2;
    g_transpose_sequence[1].jump = -1;
    g_transpose_sequence[1].condition_n = 0;

    g_transpose_sequence[2].transpose = 7;
    g_transpose_sequence[2].duration = 2;
    g_transpose_sequence[2].jump = 0;  /* Jump back to step 0 */
    g_transpose_sequence[2].condition_n = 0;

    printf("Expected: 0,0 -> jump to 2 -> 7,7 -> jump to 0 -> 0,0 -> ...\n");
    printf("Actual: ");

    int errors = 0;
    for (int i = 0; i < 10; i++) {
        int8_t t = get_transpose_at_step(i);
        printf("%d", t);
        if (i < 9) printf(",");

        /* Check expectations */
        int expected;
        if (i < 2) expected = 0;
        else if (i < 4) expected = 7;
        else if (i < 6) expected = 0;
        else if (i < 8) expected = 7;
        else expected = 0;

        if (t != expected) errors++;
    }
    printf("\n");
    printf("Result: %s\n", errors == 0 ? "PASS" : "FAIL");
}

void test_skip_step(void) {
    printf("\n=== TEST 2: Skip Middle Step ===\n");
    printf("Setup:\n");
    printf("  Step 0: transpose=0, duration=2, jump=2 (skip step 1)\n");
    printf("  Step 1: transpose=99, duration=2, no jump (NEVER PLAYED)\n");
    printf("  Step 2: transpose=12, duration=2, no jump\n\n");

    reset_state();
    memset(g_transpose_sequence, 0, sizeof(g_transpose_sequence));

    g_transpose_step_count = 3;
    g_transpose_sequence[0].transpose = 0;
    g_transpose_sequence[0].duration = 2;
    g_transpose_sequence[0].jump = 2;
    g_transpose_sequence[0].condition_n = 0;

    g_transpose_sequence[1].transpose = 99;  /* Should never be played */
    g_transpose_sequence[1].duration = 2;
    g_transpose_sequence[1].jump = -1;
    g_transpose_sequence[1].condition_n = 0;

    g_transpose_sequence[2].transpose = 12;
    g_transpose_sequence[2].duration = 2;
    g_transpose_sequence[2].jump = -1;
    g_transpose_sequence[2].condition_n = 0;

    printf("Expected: 0,0 -> jump to 2 -> 12,12 -> wrap to 0 -> 0,0...\n");
    printf("Expected: transpose 99 should NEVER appear\n");
    printf("Actual: ");

    int errors = 0;
    int found_99 = 0;
    for (int i = 0; i < 10; i++) {
        int8_t t = get_transpose_at_step(i);
        printf("%d", t);
        if (i < 9) printf(",");

        if (t == 99) found_99 = 1;

        /* Check expectations */
        int expected;
        if (i < 2) expected = 0;
        else if (i < 4) expected = 12;
        else if (i < 6) expected = 0;
        else if (i < 8) expected = 12;
        else expected = 0;

        if (t != expected) errors++;
    }
    printf("\n");
    if (found_99) {
        printf("Result: FAIL - Step 1 (transpose=99) was incorrectly played!\n");
    } else {
        printf("Result: %s\n", errors == 0 ? "PASS" : "FAIL");
    }
}

void test_realistic_scenario(void) {
    printf("\n=== TEST 3: Realistic Master View Scenario ===\n");
    printf("Setup (typical user case):\n");
    printf("  Step 0: transpose=0, duration=16, jump=1\n");
    printf("  Step 1: transpose=7, duration=16, no jump\n");
    printf("  (User expects: play step 0 for 16 steps, then jump to step 1)\n\n");

    reset_state();
    memset(g_transpose_sequence, 0, sizeof(g_transpose_sequence));

    g_transpose_step_count = 2;
    g_transpose_sequence[0].transpose = 0;
    g_transpose_sequence[0].duration = 16;
    g_transpose_sequence[0].jump = 1;
    g_transpose_sequence[0].condition_n = 0;

    g_transpose_sequence[1].transpose = 7;
    g_transpose_sequence[1].duration = 16;
    g_transpose_sequence[1].jump = -1;
    g_transpose_sequence[1].condition_n = 0;

    printf("Checking key transition points:\n");

    int errors = 0;

    /* Test steps 0-15 should be transpose=0 */
    printf("  Steps 0-15 (first sequence step): ");
    int all_zero = 1;
    for (int i = 0; i < 16; i++) {
        int8_t t = get_transpose_at_step(i);
        if (t != 0) {
            all_zero = 0;
            errors++;
        }
    }
    printf("%s (virtual_step=%d)\n", all_zero ? "OK (all 0)" : "FAIL", g_transpose_virtual_step);

    /* Test step 16 should jump to step 1 (transpose=7) */
    int8_t t16 = get_transpose_at_step(16);
    printf("  Step 16 (after jump): transpose=%d, virtual_step=%d ", t16, g_transpose_virtual_step);
    if (t16 == 7 && g_transpose_virtual_step == 1) {
        printf("OK\n");
    } else {
        printf("FAIL (expected transpose=7, virtual_step=1)\n");
        errors++;
    }

    /* Test steps 17-31 should continue as transpose=7 */
    printf("  Steps 17-31 (second sequence step): ");
    int all_seven = 1;
    for (int i = 17; i < 32; i++) {
        int8_t t = get_transpose_at_step(i);
        if (t != 7) {
            all_seven = 0;
            errors++;
        }
    }
    printf("%s (virtual_step=%d)\n", all_seven ? "OK (all 7)" : "FAIL", g_transpose_virtual_step);

    /* Test step 32 should wrap back to step 0 */
    int8_t t32 = get_transpose_at_step(32);
    printf("  Step 32 (after wrap): transpose=%d, virtual_step=%d ", t32, g_transpose_virtual_step);
    if (t32 == 0 && g_transpose_virtual_step == 0) {
        printf("OK\n");
    } else {
        printf("FAIL (expected transpose=0, virtual_step=0)\n");
        errors++;
    }

    printf("\nResult: %s\n", errors == 0 ? "PASS" : "FAIL");
}

int main() {
    printf("Transpose Jump Comprehensive Test Suite\n");
    printf("========================================\n");

    test_backward_jump();
    test_skip_step();
    test_realistic_scenario();

    printf("\n========================================\n");
    printf("All tests completed.\n");
    printf("If all tests PASS, the DSP logic is correct.\n");
    printf("If tests FAIL, there's a bug in the jump implementation.\n");

    return 0;
}
