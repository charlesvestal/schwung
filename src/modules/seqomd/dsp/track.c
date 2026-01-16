/*
 * SEQOMD DSP Plugin - Track Functions
 *
 * Track initialization and playback (trigger, advance).
 */

#include "seq_plugin.h"

void init_pattern(pattern_t *pattern) {
    pattern->loop_start = 0;
    pattern->loop_end = NUM_STEPS - 1;
    for (int i = 0; i < NUM_STEPS; i++) {
        for (int n = 0; n < MAX_NOTES_PER_STEP; n++) {
            pattern->steps[i].notes[n] = 0;
            pattern->steps[i].velocities[n] = DEFAULT_VELOCITY;
        }
        pattern->steps[i].num_notes = 0;
        pattern->steps[i].gate = DEFAULT_GATE;
        pattern->steps[i].cc1 = -1;  /* Not set */
        pattern->steps[i].cc2 = -1;  /* Not set */
        pattern->steps[i].probability = 100;    /* Always trigger */
        pattern->steps[i].condition_n = 0;      /* No condition */
        pattern->steps[i].condition_m = 0;
        pattern->steps[i].condition_not = 0;    /* Normal (not negated) */
        pattern->steps[i].ratchet = 1;          /* Single trigger (no ratchet) */
        pattern->steps[i].length = 1;           /* Single step length */
        /* Spark fields */
        pattern->steps[i].param_spark_n = 0;    /* Always apply CC locks */
        pattern->steps[i].param_spark_m = 0;
        pattern->steps[i].param_spark_not = 0;
        pattern->steps[i].comp_spark_n = 0;     /* Always apply ratchet/jump */
        pattern->steps[i].comp_spark_m = 0;
        pattern->steps[i].comp_spark_not = 0;
        pattern->steps[i].jump = -1;            /* No jump */
        pattern->steps[i].offset = 0;           /* No micro-timing offset */
        /* Arp per-step overrides */
        pattern->steps[i].arp_mode = -1;        /* Use track default */
        pattern->steps[i].arp_speed = -1;       /* Use track default */
        pattern->steps[i].arp_layer = ARP_LAYER_LAYER;  /* Default to layer */
        pattern->steps[i].arp_play_steps = -1;  /* Use track default */
        pattern->steps[i].arp_play_start = -1;  /* Use track default */
    }
}

void init_track(track_t *track, int channel) {
    memset(track, 0, sizeof(track_t));
    track->midi_channel = channel;
    track->length = NUM_STEPS;
    track->current_pattern = 0;
    track->current_step = 0;
    track->muted = 0;
    track->swing = 50;   /* Default swing (50 = no swing) */
    track->speed = 1.0;  /* Default speed */
    track->phase = 0.0;
    track->gate_phase = 0.0;
    track->num_last_notes = 0;
    track->note_on_active = 0;
    track->loop_count = 0;
    track->ratchet_count = 0;
    track->ratchet_total = 1;
    track->ratchet_phase = 0.0;
    track->note_length_total = 1;
    track->note_gate = DEFAULT_GATE;
    track->note_length_phase = 0.0;
    track->trigger_pending = 0;
    track->trigger_at_phase = 0.0;
    track->pending_step = 0;
    track->next_step_at = 1.0;  /* Default step length */
    /* Arpeggiator defaults */
    track->arp_mode = ARP_OFF;
    track->arp_speed = DEFAULT_ARP_SPEED;
    track->arp_octave = ARP_OCT_NONE;
    track->preview_velocity = DEFAULT_VELOCITY;
    /* Track CC defaults */
    track->cc1_default = 64;
    track->cc2_default = 64;
    track->cc1_steps_remaining = 0;
    track->cc2_steps_remaining = 0;
    /* Arp continuous mode */
    track->arp_continuous = 0;
    track->arp_pattern_idx = 0;
    track->arp_last_num_notes = 0;
    for (int i = 0; i < MAX_NOTES_PER_STEP; i++) {
        track->arp_last_notes[i] = 0;
    }
    /* Arp play steps */
    track->arp_play_steps = 1;  /* Default: all notes play */
    track->arp_play_start = 0;
    track->arp_skip_idx = 0;

    for (int i = 0; i < MAX_NOTES_PER_STEP; i++) {
        track->last_notes[i] = -1;
    }

    /* Initialize all patterns */
    for (int p = 0; p < NUM_PATTERNS; p++) {
        init_pattern(&track->patterns[p]);
    }
}

/* Get current pattern for a track */
pattern_t* get_current_pattern(track_t *track) {
    return &track->patterns[track->current_pattern];
}

/* Check if step should trigger based on probability and conditions */
int should_step_trigger(step_t *step, track_t *track) {
    /* Check condition first */
    if (step->condition_n > 0) {
        /* Regular condition: play on iteration m of every n loops */
        /* loop_count is 0-indexed, condition_m is 1-indexed */
        int iteration = (track->loop_count % step->condition_n) + 1;
        int should_play = (iteration == step->condition_m);

        /* Negate if condition_not is set */
        if (step->condition_not) {
            should_play = !should_play;
        }

        if (!should_play) return 0;
    }

    /* Check probability (only if no condition or condition passed) */
    if (step->probability < 100) {
        if (!random_check(step->probability)) return 0;
    }

    return 1;
}

/* Check if a spark condition passes (param_spark or comp_spark) */
int check_spark_condition(int8_t spark_n, int8_t spark_m, uint8_t spark_not, track_t *track) {
    if (spark_n <= 0) {
        /* No condition - always passes */
        return 1;
    }
    /* Check iteration within loop cycle */
    int iteration = (track->loop_count % spark_n) + 1;
    int should_apply = (iteration == spark_m);

    /* Negate if spark_not is set */
    if (spark_not) {
        should_apply = !should_apply;
    }
    return should_apply;
}

/**
 * Check if two sorted note arrays are equal.
 * Used by arp continuous mode to detect if the same chord is triggering again.
 */
static int notes_equal_sorted(uint8_t *a, int a_count, uint8_t *b, int b_count) {
    if (a_count != b_count) return 0;
    for (int i = 0; i < a_count; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/**
 * Schedule notes for a step via the centralized scheduler.
 * This handles swing, ratchets, arp, note conflicts, and transpose automatically.
 *
 * @param track       Track data
 * @param track_idx   Track index (for chord_follow check)
 * @param step        Step data
 * @param base_phase  Global phase when this step starts
 * @param use_arp     Whether to use arp scheduling (1=use arp, 0=don't)
 * @param use_ratchet Whether to use step's ratchet value (1=use ratchet, 0=force single trigger)
 */
void schedule_step_notes(track_t *track, int track_idx, step_t *step, double base_phase, int use_arp, int use_ratchet) {
    int note_length = step->length > 0 ? step->length : 1;
    int gate = step->gate > 0 ? step->gate : DEFAULT_GATE;

    /* Clamp note length to not extend past the loop end.
     * This prevents arp/notes from overlapping when the track loops back. */
    pattern_t *pattern = get_current_pattern(track);
    int remaining_steps = pattern->loop_end - track->current_step + 1;
    if (note_length > remaining_steps) {
        note_length = remaining_steps;
    }

    /* Get sequence transpose for this track (will be applied at send time).
     * We only store the sequence transpose here; live transpose is checked at send time
     * so it can respond immediately when the user changes it. */
    uint32_t global_step = (uint32_t)g_global_phase;
    int8_t sequence_transpose = 0;
    if (g_chord_follow[track_idx]) {
        sequence_transpose = get_transpose_at_step(global_step);
    }

    if (use_arp && step->num_notes >= 1) {
        /* Arpeggiator scheduling - ignore ratchet when arp is active */

        /* Resolve arp settings (step override or track default) */
        int arp_mode = step->arp_mode >= 0 ? step->arp_mode : track->arp_mode;
        int arp_speed = step->arp_speed >= 0 ? step->arp_speed : track->arp_speed;
        int arp_octave = track->arp_octave;  /* Octave is track-only, no step override */

        /* Resolve play steps settings (step override or track default) */
        int play_steps = step->arp_play_steps >= 0 ? step->arp_play_steps : track->arp_play_steps;
        int play_start = step->arp_play_start >= 0 ? step->arp_play_start : track->arp_play_start;
        if (play_steps < 1) play_steps = 1;  /* Clamp to minimum */
        int play_steps_len = get_play_steps_length((uint8_t)play_steps);

        /* Generate arp pattern */
        uint8_t arp_pattern[MAX_ARP_PATTERN];
        int pattern_len = generate_arp_pattern(step->notes, step->num_notes,
                                                arp_mode, arp_octave, arp_pattern, MAX_ARP_PATTERN);

        if (pattern_len == 0) return;

        /* Continuous mode: check if same notes are triggering again.
         * If so, continue from where we left off in the pattern.
         * Random mode always restarts (no meaningful continuation). */
        int start_idx = 0;
        int skip_start_idx = 0;  /* For play steps pattern */
        if (track->arp_continuous && arp_mode != ARP_RANDOM) {
            /* Sort current notes for comparison */
            uint8_t sorted_notes[MAX_NOTES_PER_STEP];
            for (int i = 0; i < step->num_notes && i < MAX_NOTES_PER_STEP; i++) {
                sorted_notes[i] = step->notes[i];
            }
            sort_notes(sorted_notes, step->num_notes);

            /* Check if same notes as last time */
            if (notes_equal_sorted(sorted_notes, step->num_notes,
                                   track->arp_last_notes, track->arp_last_num_notes)) {
                /* Continue from stored position */
                start_idx = track->arp_pattern_idx % pattern_len;
                skip_start_idx = track->arp_skip_idx % play_steps_len;
            }

            /* Store current notes (sorted) for next comparison */
            for (int i = 0; i < step->num_notes && i < MAX_NOTES_PER_STEP; i++) {
                track->arp_last_notes[i] = sorted_notes[i];
            }
            track->arp_last_num_notes = step->num_notes;
        }

        /* Calculate arp timing using musical note values
         * ARP_STEP_RATES[speed] = steps per arp note (in global phase)
         * e.g., 1/32 = 0.5 (2 notes per step), 1/4 = 4.0 (1 note per 4 steps)
         *
         * Arp speed is tempo-relative, so it stays constant regardless of track speed.
         * But the total duration the arp plays scales with track speed:
         * at 0.5x speed, a 16-step note spans 32 global steps, so more arp notes play. */
        double speed_scale = 1.0 / track->speed;
        double effective_length = note_length * speed_scale;  /* Length in global steps */
        double steps_per_note = ARP_STEP_RATES[arp_speed];
        int total_arp_notes = (int)(effective_length / steps_per_note + 0.5);
        if (total_arp_notes < 1) total_arp_notes = 1;
        double note_duration = steps_per_note;  /* Each arp note is the musical note value */

        /* Handle ARP_CHORD mode - all notes together at each arp position */
        if (arp_mode == ARP_CHORD) {
            int skip_idx = skip_start_idx;
            for (int i = 0; i < total_arp_notes; i++) {
                double note_phase = base_phase + (i * note_duration);

                /* Check play steps pattern: should this arp position be played? */
                int bit_index = (play_start + skip_idx) % play_steps_len;
                int should_play = (play_steps >> bit_index) & 1;
                skip_idx++;

                if (!should_play) continue;  /* Skip this arp position */

                /* Play all notes as chord */
                for (int n = 0; n < step->num_notes && n < MAX_NOTES_PER_STEP; n++) {
                    if (step->notes[n] > 0) {
                        schedule_note(
                            step->notes[n],  /* Original note - transpose applied at send time */
                            step->velocities[n],  /* Per-note velocity */
                            track->midi_channel,
                            track->swing,
                            note_phase,
                            note_duration,
                            gate,
                            track_idx,
                            sequence_transpose
                        );
                    }
                }
            }
            /* For chord mode, continuous doesn't really apply but update indices anyway */
            if (track->arp_continuous) {
                track->arp_pattern_idx = (start_idx + total_arp_notes) % pattern_len;
                track->arp_skip_idx = (skip_start_idx + total_arp_notes) % play_steps_len;
            }
        } else {
            /* Normal arp: cycle through pattern with play steps skip logic */
            int pattern_idx = start_idx;
            int skip_idx = skip_start_idx;
            for (int i = 0; i < total_arp_notes; i++) {
                double note_phase = base_phase + (i * note_duration);

                /* Check play steps pattern: should this arp position be played? */
                int bit_index = (play_start + skip_idx) % play_steps_len;
                int should_play = (play_steps >> bit_index) & 1;
                skip_idx++;

                if (!should_play) {
                    /* Skip this arp position but still advance the note pattern */
                    pattern_idx = (pattern_idx + 1) % pattern_len;
                    continue;
                }

                int note_value = arp_pattern[pattern_idx];
                pattern_idx = (pattern_idx + 1) % pattern_len;

                /* Cycle through velocities based on source note index */
                int vel_idx = (step->num_notes > 0) ? (i % step->num_notes) : 0;
                uint8_t velocity = (step->num_notes > 0) ? step->velocities[vel_idx] : DEFAULT_VELOCITY;

                schedule_note(
                    note_value,  /* Original note from arp pattern - transpose applied at send time */
                    velocity,    /* Per-note velocity, cycling through source notes */
                    track->midi_channel,
                    track->swing,
                    note_phase,
                    note_duration,
                    gate,
                    track_idx,
                    sequence_transpose
                );
            }
            /* Store ending indices for continuous mode */
            if (track->arp_continuous) {
                track->arp_pattern_idx = pattern_idx;
                track->arp_skip_idx = skip_idx % play_steps_len;
            }
        }
    } else {
        /* Standard ratchet scheduling (no arp, or single note) */

        /* Decode ratchet mode and count from parameter value:
         * 1-8: Regular ratchet (1x-8x)
         * 10-16: Velocity Ramp Up (2x-8x) - count = value - 8
         * 20-26: Velocity Ramp Down (2x-8x) - count = value - 18
         */
        int ratchet_value = (use_ratchet && step->ratchet > 0) ? step->ratchet : 1;
        int ratchet_count = 1;
        int ratchet_mode = 0;  /* 0=regular, 1=ramp_up, 2=ramp_down */

        if (ratchet_value >= 20) {
            ratchet_mode = 2;  /* ramp_down */
            ratchet_count = ratchet_value - 18;
        } else if (ratchet_value >= 10) {
            ratchet_mode = 1;  /* ramp_up */
            ratchet_count = ratchet_value - 8;
        } else {
            ratchet_mode = 0;  /* regular */
            ratchet_count = ratchet_value;
        }

        /* Scale by track speed: at 0.5x speed, each track step takes 2 global steps */
        double speed_scale = 1.0 / track->speed;

        /* For ratchets, divide the NOTE LENGTH into equal parts (not just one step) */
        double ratchet_step = ((double)note_length / ratchet_count) * speed_scale;
        /* Each ratchet note gets equal length */
        double ratchet_length = ((double)note_length / ratchet_count) * speed_scale;

        for (int r = 0; r < ratchet_count; r++) {
            double note_on_phase = base_phase + (r * ratchet_step);

            /* Calculate velocity scale factor for this ratchet based on mode */
            int vel_numerator = ratchet_count;  /* Default: full velocity (factor = 1) */
            if (ratchet_mode == 1) {
                /* Ramp Up: velocity increases from low to target */
                vel_numerator = r + 1;
            } else if (ratchet_mode == 2) {
                /* Ramp Down: velocity decreases from target to low */
                vel_numerator = ratchet_count - r;
            }

            /* Schedule each note in the step with its per-note velocity */
            for (int n = 0; n < step->num_notes && n < MAX_NOTES_PER_STEP; n++) {
                if (step->notes[n] > 0) {
                    /* Apply ratchet velocity scaling to per-note velocity */
                    uint8_t note_velocity = step->velocities[n];
                    if (ratchet_mode != 0) {
                        note_velocity = (uint8_t)((vel_numerator * step->velocities[n]) / ratchet_count);
                        if (note_velocity < 1) note_velocity = 1;
                    }
                    schedule_note(
                        step->notes[n],  /* Original note - transpose applied at send time */
                        note_velocity,   /* Per-note velocity with ratchet scaling */
                        track->midi_channel,
                        track->swing,
                        note_on_phase,
                        ratchet_length,
                        gate,
                        track_idx,
                        sequence_transpose
                    );
                }
            }
        }
    }
}

void trigger_track_step(track_t *track, int track_idx, double step_start_phase) {
    pattern_t *pattern = get_current_pattern(track);
    step_t *step = &pattern->steps[track->current_step];

    /* Skip if muted */
    if (track->muted) return;

    /* Check param_spark - should CC locks apply this loop? */
    int param_spark_pass = check_spark_condition(
        step->param_spark_n, step->param_spark_m, step->param_spark_not, track);

    /* CC handling: step CC overrides track default for step->length duration.
     * Only send CC when: (1) step has CC value, or (2) override just expired.
     * Track whether we were in override state to detect expiration. */

    int cc1_num = 20 + (track_idx * 2);
    int cc2_num = cc1_num + 1;

    /* Check if override is expiring this step (was 1, will become 0) */
    int cc1_expiring = (track->cc1_steps_remaining == 1);
    int cc2_expiring = (track->cc2_steps_remaining == 1);

    /* Decrement remaining counters */
    if (track->cc1_steps_remaining > 0) track->cc1_steps_remaining--;
    if (track->cc2_steps_remaining > 0) track->cc2_steps_remaining--;

    if (param_spark_pass) {
        /* CC1: step override sets new value for step->length duration */
        if (step->cc1 >= 0) {
            send_cc(cc1_num, step->cc1, track->midi_channel);
            track->cc1_steps_remaining = step->length;
        } else if (cc1_expiring) {
            /* Override just expired, restore track default */
            send_cc(cc1_num, track->cc1_default, track->midi_channel);
        }

        /* CC2: step override sets new value for step->length duration */
        if (step->cc2 >= 0) {
            send_cc(cc2_num, step->cc2, track->midi_channel);
            track->cc2_steps_remaining = step->length;
        } else if (cc2_expiring) {
            /* Override just expired, restore track default */
            send_cc(cc2_num, track->cc2_default, track->midi_channel);
        }
    }

    /* Check comp_spark early - needed for both notes and jumps */
    int comp_spark_pass = check_spark_condition(
        step->comp_spark_n, step->comp_spark_m, step->comp_spark_not, track);

    /* Handle note scheduling if step has notes */
    if (step->num_notes > 0) {
        /* Check if this step should trigger (probability + conditions / trigger spark) */
        if (should_step_trigger(step, track)) {
            /* Apply micro-timing offset */
            double offset_phase = (double)step->offset / 48.0;
            double note_phase = step_start_phase + offset_phase;

            /* Determine if arp is active (step override or track default) */
            int arp_mode = step->arp_mode >= 0 ? step->arp_mode : track->arp_mode;
            int use_arp = (arp_mode > ARP_OFF) && (step->num_notes >= 1);

            /* Handle arp layer mode - Cut cancels previous notes before scheduling new ones.
             * This applies to both arp and non-arp steps (a non-arp step can cut a running arp) */
            if (step->arp_layer == ARP_LAYER_CUT) {
                cut_channel_notes(track->midi_channel);
            }

            /* Schedule notes - arp takes priority over ratchet when active */
            if (use_arp) {
                /* Arp is active - use arp scheduling (ignores ratchet) */
                schedule_step_notes(track, track_idx, step, note_phase, 1, 0);
            } else if (comp_spark_pass && step->ratchet > 1) {
                /* No arp, use ratchets */
                schedule_step_notes(track, track_idx, step, note_phase, 0, 1);
            } else {
                /* Single trigger (no arp, no ratchet) */
                schedule_step_notes(track, track_idx, step, note_phase, 0, 0);
            }
        }
    }

    /* Handle jump (only if comp_spark passes) - works on empty steps too */
    if (comp_spark_pass && step->jump >= 0 && step->jump < NUM_STEPS) {
        /* Jump to target step on next advance */
        /* We set current_step to jump-1 because advance_track will increment it */
        /* But we need to be careful about loop boundaries */
        if (step->jump <= pattern->loop_end && step->jump >= pattern->loop_start) {
            /* Jump is within current loop range - will be incremented by advance */
            track->current_step = step->jump - 1;
            if (track->current_step < pattern->loop_start) {
                track->current_step = pattern->loop_end;  /* Wrap to end */
            }
        }
    }
}

/**
 * Advance a track to the next step and schedule its notes.
 * Step duration is now fixed at 1.0 - swing is applied as a delay on notes,
 * not as a duration change on steps.
 */
void advance_track(track_t *track, int track_idx) {
    /* Advance step, respecting loop points from current pattern */
    pattern_t *pattern = get_current_pattern(track);

    if (track->current_step >= pattern->loop_end) {
        track->current_step = pattern->loop_start;
        track->loop_count++;  /* Increment loop count when pattern loops */
    } else {
        track->current_step++;
    }

    /* Calculate the global phase when this step starts.
     * This is used by the scheduler to apply swing based on global position. */
    double step_start_phase = g_global_phase;

    /* Trigger the step - this schedules notes via the centralized scheduler */
    trigger_track_step(track, track_idx, step_start_phase);

    /* Fixed step duration - swing is handled as note delay, not step duration */
    track->next_step_at = 1.0;
}
