/*
 * SEQOMD DSP Plugin - Transpose Sequence
 *
 * Transpose sequence management and playhead tracking.
 */

#include "seq_plugin.h"

/**
 * Rebuild the transpose lookup table from the sequence.
 * Called when transpose sequence is modified.
 */
void rebuild_transpose_lookup(void) {
    /* Calculate total steps */
    g_transpose_total_steps = 0;
    for (int i = 0; i < g_transpose_step_count; i++) {
        g_transpose_total_steps += g_transpose_sequence[i].duration;
    }

    if (g_transpose_total_steps == 0 || g_transpose_step_count == 0) {
        g_transpose_lookup_valid = 0;
        return;
    }

    /* Reallocate lookup table if needed */
    if (g_transpose_total_steps > g_transpose_lookup_size) {
        if (g_transpose_lookup) {
            free(g_transpose_lookup);
        }
        g_transpose_lookup = (int8_t *)malloc(g_transpose_total_steps);
        if (!g_transpose_lookup) {
            g_transpose_lookup_size = 0;
            g_transpose_lookup_valid = 0;
            return;
        }
        g_transpose_lookup_size = g_transpose_total_steps;
    }

    /* Build lookup table */
    uint32_t step = 0;
    for (int i = 0; i < g_transpose_step_count; i++) {
        int8_t transpose = g_transpose_sequence[i].transpose;
        uint16_t duration = g_transpose_sequence[i].duration;
        for (uint16_t d = 0; d < duration && step < g_transpose_total_steps; d++) {
            g_transpose_lookup[step++] = transpose;
        }
    }

    g_transpose_lookup_valid = 1;
}

/**
 * Check if a transpose step's condition passes based on its iteration count.
 * Returns 1 if condition passes, 0 otherwise.
 */
int check_transpose_condition(int step_index, transpose_step_t *step) {
    if (step->condition_n <= 0) {
        return 1;  /* No condition (n=0) always passes */
    }

    /* Calculate which iteration of the cycle we're in (1-indexed) */
    uint32_t step_iter = g_transpose_step_iteration[step_index];
    int iteration = (step_iter % step->condition_n) + 1;
    int should_apply = (iteration == step->condition_m);

    /* Apply NOT flag if set */
    if (step->condition_not) {
        should_apply = !should_apply;
    }

    return should_apply;
}

/**
 * Update the transpose virtual playhead (called every frame).
 * This ensures jumps execute even when no notes are triggering.
 */
void update_transpose_virtual_playhead(uint32_t step) {
    /* If transpose sequence is disabled or empty, nothing to do */
    if (!g_transpose_sequence_enabled) {
        return;
    }
    if (g_transpose_step_count == 0 || g_transpose_total_steps == 0) {
        return;
    }

    /* Initialize on first call - calculate which virtual step we should be at */
    if (g_transpose_first_call) {
        /* Calculate which virtual step corresponds to the current global step */
        uint32_t looped_step = step % g_transpose_total_steps;
        uint32_t accumulated = 0;
        g_transpose_virtual_step = 0;

        for (int i = 0; i < g_transpose_step_count; i++) {
            uint32_t next_accumulated = accumulated + g_transpose_sequence[i].duration;
            if (looped_step < next_accumulated) {
                /* This is the virtual step we should be in */
                g_transpose_virtual_step = i;
                g_transpose_virtual_entry_step = step - (looped_step - accumulated);
                break;
            }
            accumulated = next_accumulated;
        }

        g_transpose_first_call = 0;
        return;
    }

    /* Get current virtual step and its duration */
    transpose_step_t *current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    uint32_t duration_in_steps = current_virtual->duration;

    /* Check if we've been in this virtual step long enough to advance */
    uint32_t steps_in_current = step - g_transpose_virtual_entry_step;

    if (steps_in_current >= duration_in_steps) {
        /* Step finished playing - check for jump BEFORE advancing */
        if (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) {
            if (check_transpose_condition(g_transpose_virtual_step, current_virtual)) {
                /* Increment this step's iteration counter - we've evaluated this condition */
                g_transpose_step_iteration[g_transpose_virtual_step]++;

                /* Jump: go to target step */
                g_transpose_virtual_step = current_virtual->jump;
                g_transpose_virtual_entry_step = step;
                return;
            } else {
                /* Still increment - we evaluated the condition */
                g_transpose_step_iteration[g_transpose_virtual_step]++;
            }
        }

        /* No jump or condition failed - advance normally */
        int next_virtual = g_transpose_virtual_step + 1;

        /* Handle wraparound */
        if (next_virtual >= g_transpose_step_count) {
            next_virtual = 0;
        }

        g_transpose_virtual_step = next_virtual;
        g_transpose_virtual_entry_step = step;
    }
}

/**
 * Get transpose value for a given step position.
 * Now just returns the current transpose value without advancing the playhead.
 */
int8_t get_transpose_at_step(uint32_t step) {
    /* If transpose sequence is disabled, return 0 (no automation) */
    if (!g_transpose_sequence_enabled) {
        return 0;
    }
    if (g_transpose_step_count == 0 || g_transpose_total_steps == 0) {
        /* Fall back to legacy current_transpose when no sequence defined */
        return (int8_t)g_current_transpose;
    }

    /* Return transpose value of current virtual step */
    return g_transpose_sequence[g_transpose_virtual_step].transpose;
}

/**
 * Get the current transpose step index for a given step position.
 * Returns -1 if no sequence or invalid.
 */
int get_transpose_step_index(uint32_t step) {
    if (g_transpose_step_count == 0 || g_transpose_total_steps == 0) {
        return -1;
    }

    uint32_t looped_step = step % g_transpose_total_steps;
    uint32_t accumulated = 0;
    for (int i = 0; i < g_transpose_step_count; i++) {
        accumulated += g_transpose_sequence[i].duration;
        if (looped_step < accumulated) {
            return i;
        }
    }
    return g_transpose_step_count - 1;
}

/**
 * Clear the transpose sequence.
 */
void clear_transpose_sequence(void) {
    g_transpose_step_count = 0;
    g_transpose_total_steps = 0;
    g_transpose_lookup_valid = 0;
    memset(g_transpose_step_iteration, 0, sizeof(g_transpose_step_iteration));
    g_transpose_virtual_step = 0;
    g_transpose_virtual_entry_step = 0;
    g_transpose_first_call = 1;
    memset(g_transpose_sequence, 0, sizeof(g_transpose_sequence));
    /* Initialize jump to -1 (no jump) for all steps */
    for (int i = 0; i < MAX_TRANSPOSE_STEPS; i++) {
        g_transpose_sequence[i].jump = -1;
    }
}
