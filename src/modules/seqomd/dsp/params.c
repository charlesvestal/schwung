/*
 * SEQOMD DSP Plugin - Parameter Handlers
 *
 * Get/set handlers for step, track, and transpose parameters.
 */

#include "seq_plugin.h"

/**
 * Handle step-level parameter setting.
 * Called for params like: track_T_step_S_note, track_T_step_S_vel, etc.
 */
void set_step_param(int track_idx, int step_idx, const char *param, const char *val) {
    step_t *s = &get_current_pattern(&g_tracks[track_idx])->steps[step_idx];

    /* Set single note (backward compat - clears other notes) */
    if (strcmp(param, "note") == 0) {
        int note = atoi(val);
        if (note == 0) {
            s->num_notes = 0;
            for (int n = 0; n < MAX_NOTES_PER_STEP; n++) {
                s->notes[n] = 0;
            }
        } else if (note >= 1 && note <= 127) {
            s->notes[0] = note;
            s->num_notes = 1;
            for (int n = 1; n < MAX_NOTES_PER_STEP; n++) {
                s->notes[n] = 0;
            }
        }
    }
    /* Add a note to the step (for chords) - format: "note" or "note,velocity" */
    else if (strcmp(param, "add_note") == 0) {
        int note = 0;
        int velocity = DEFAULT_VELOCITY;
        /* Parse "note,velocity" format */
        const char *comma = strchr(val, ',');
        if (comma) {
            note = atoi(val);
            velocity = atoi(comma + 1);
        } else {
            note = atoi(val);
        }
        if (note >= 1 && note <= 127) {
            if (velocity < 1) velocity = 1;
            if (velocity > 127) velocity = 127;
            int exists = 0;
            for (int n = 0; n < s->num_notes; n++) {
                if (s->notes[n] == note) {
                    /* Note exists - update its velocity */
                    s->velocities[n] = velocity;
                    exists = 1;
                    break;
                }
            }
            if (!exists && s->num_notes < MAX_NOTES_PER_STEP) {
                s->notes[s->num_notes] = note;
                s->velocities[s->num_notes] = velocity;
                s->num_notes++;
                if (g_chord_follow[track_idx]) {
                    g_scale_dirty = 1;
                }
            }
        }
    }
    /* Remove a note from the step */
    else if (strcmp(param, "remove_note") == 0) {
        int note = atoi(val);
        if (note >= 1 && note <= 127) {
            for (int n = 0; n < s->num_notes; n++) {
                if (s->notes[n] == note) {
                    for (int m = n; m < s->num_notes - 1; m++) {
                        s->notes[m] = s->notes[m + 1];
                        s->velocities[m] = s->velocities[m + 1];
                    }
                    s->notes[s->num_notes - 1] = 0;
                    s->velocities[s->num_notes - 1] = DEFAULT_VELOCITY;
                    s->num_notes--;
                    if (g_chord_follow[track_idx]) {
                        g_scale_dirty = 1;
                    }
                    break;
                }
            }
        }
    }
    /* Clear all notes, CCs, and parameters from step */
    else if (strcmp(param, "clear") == 0) {
        s->num_notes = 0;
        for (int n = 0; n < MAX_NOTES_PER_STEP; n++) {
            s->notes[n] = 0;
            s->velocities[n] = DEFAULT_VELOCITY;
        }
        s->cc1 = -1;
        s->cc2 = -1;
        s->probability = 100;
        s->condition_n = 0;
        s->condition_m = 0;
        s->condition_not = 0;
        s->ratchet = 1;
        s->length = 1;
        s->param_spark_n = 0;
        s->param_spark_m = 0;
        s->param_spark_not = 0;
        s->comp_spark_n = 0;
        s->comp_spark_m = 0;
        s->comp_spark_not = 0;
        s->jump = -1;
        s->offset = 0;
        s->arp_mode = -1;
        s->arp_speed = -1;
        s->arp_layer = ARP_LAYER_LAYER;
        if (g_chord_follow[track_idx]) {
            g_scale_dirty = 1;
        }
    }
    /* Set all note velocities to the same value (backward compat) */
    else if (strcmp(param, "vel") == 0) {
        int vel = atoi(val);
        if (vel >= 1 && vel <= 127) {
            for (int n = 0; n < s->num_notes; n++) {
                s->velocities[n] = vel;
            }
        }
    }
    /* Apply delta to all note velocities (for knob adjustment) */
    else if (strcmp(param, "velocity_delta") == 0) {
        int delta = atoi(val);
        for (int n = 0; n < s->num_notes; n++) {
            int new_vel = (int)s->velocities[n] + delta;
            if (new_vel < 1) new_vel = 1;
            if (new_vel > 127) new_vel = 127;
            s->velocities[n] = new_vel;
        }
    }
    else if (strcmp(param, "gate") == 0) {
        int gate = atoi(val);
        if (gate >= 1 && gate <= 100) {
            s->gate = gate;
        }
    }
    else if (strcmp(param, "cc1") == 0) {
        int cc_val = atoi(val);
        if (cc_val >= -1 && cc_val <= 127) {
            s->cc1 = cc_val;
        }
    }
    else if (strcmp(param, "cc2") == 0) {
        int cc_val = atoi(val);
        if (cc_val >= -1 && cc_val <= 127) {
            s->cc2 = cc_val;
        }
    }
    else if (strcmp(param, "probability") == 0) {
        int prob = atoi(val);
        if (prob >= 1 && prob <= 100) {
            s->probability = prob;
        }
    }
    else if (strcmp(param, "condition_n") == 0) {
        s->condition_n = atoi(val);
    }
    else if (strcmp(param, "condition_m") == 0) {
        s->condition_m = atoi(val);
    }
    else if (strcmp(param, "condition_not") == 0) {
        s->condition_not = atoi(val) ? 1 : 0;
    }
    else if (strcmp(param, "param_spark_n") == 0) {
        s->param_spark_n = atoi(val);
    }
    else if (strcmp(param, "param_spark_m") == 0) {
        s->param_spark_m = atoi(val);
    }
    else if (strcmp(param, "param_spark_not") == 0) {
        s->param_spark_not = atoi(val) ? 1 : 0;
    }
    else if (strcmp(param, "comp_spark_n") == 0) {
        s->comp_spark_n = atoi(val);
    }
    else if (strcmp(param, "comp_spark_m") == 0) {
        s->comp_spark_m = atoi(val);
    }
    else if (strcmp(param, "comp_spark_not") == 0) {
        s->comp_spark_not = atoi(val) ? 1 : 0;
    }
    else if (strcmp(param, "jump") == 0) {
        int jump = atoi(val);
        if (jump >= -1 && jump < NUM_STEPS) {
            s->jump = jump;
        }
    }
    else if (strcmp(param, "ratchet") == 0) {
        int ratch = atoi(val);
        /* Accept three ranges:
         * 1-8: Regular ratchet
         * 10-16: Velocity ramp up (2x-8x)
         * 20-26: Velocity ramp down (2x-8x)
         */
        if ((ratch >= 1 && ratch <= 8) || (ratch >= 10 && ratch <= 16) || (ratch >= 20 && ratch <= 26)) {
            s->ratchet = ratch;
        }
    }
    /* Set all note velocities to the same value (backward compat) */
    else if (strcmp(param, "velocity") == 0) {
        int vel = atoi(val);
        if (vel >= 1 && vel <= 127) {
            for (int n = 0; n < s->num_notes; n++) {
                s->velocities[n] = vel;
            }
        }
    }
    else if (strcmp(param, "length") == 0) {
        int len = atoi(val);
        if (len >= 1 && len <= 16) {
            s->length = len;
        }
    }
    else if (strcmp(param, "offset") == 0) {
        int off = atoi(val);
        if (off >= -24 && off <= 24) {
            s->offset = off;
        }
    }
    else if (strcmp(param, "arp_mode") == 0) {
        int mode = atoi(val);
        if (mode >= -1 && mode < NUM_ARP_MODES) {
            s->arp_mode = mode;
        }
    }
    else if (strcmp(param, "arp_speed") == 0) {
        int speed = atoi(val);
        if (speed >= -1 && speed < NUM_ARP_SPEEDS) {
            s->arp_speed = speed;
        }
    }
    else if (strcmp(param, "arp_layer") == 0) {
        int layer = atoi(val);
        if (layer >= 0 && layer < NUM_ARP_LAYERS) {
            s->arp_layer = layer;
        }
    }
}

/**
 * Handle step-level parameter getting.
 * Returns bytes written to buf, or -1 if param not found.
 */
int get_step_param(int track_idx, int step_idx, const char *param, char *buf, int buf_len) {
    step_t *s = &get_current_pattern(&g_tracks[track_idx])->steps[step_idx];

    if (strcmp(param, "note") == 0) {
        return snprintf(buf, buf_len, "%d", s->num_notes > 0 ? s->notes[0] : 0);
    }
    else if (strcmp(param, "notes") == 0) {
        if (s->num_notes == 0) {
            return snprintf(buf, buf_len, "");
        }
        int pos = 0;
        for (int n = 0; n < s->num_notes && pos < buf_len - 4; n++) {
            if (n > 0) {
                pos += snprintf(buf + pos, buf_len - pos, ",");
            }
            pos += snprintf(buf + pos, buf_len - pos, "%d", s->notes[n]);
        }
        return pos;
    }
    else if (strcmp(param, "num_notes") == 0) {
        return snprintf(buf, buf_len, "%d", s->num_notes);
    }
    else if (strcmp(param, "vel") == 0) {
        /* Return first note's velocity, or default if no notes */
        int vel = (s->num_notes > 0) ? s->velocities[0] : DEFAULT_VELOCITY;
        return snprintf(buf, buf_len, "%d", vel);
    }
    else if (strcmp(param, "gate") == 0) {
        return snprintf(buf, buf_len, "%d", s->gate);
    }
    else if (strcmp(param, "arp_mode") == 0) {
        return snprintf(buf, buf_len, "%d", s->arp_mode);
    }
    else if (strcmp(param, "arp_speed") == 0) {
        return snprintf(buf, buf_len, "%d", s->arp_speed);
    }
    else if (strcmp(param, "arp_layer") == 0) {
        return snprintf(buf, buf_len, "%d", s->arp_layer);
    }

    return -1;
}

/**
 * Handle track-level parameter setting.
 * Called for params like: track_T_channel, track_T_mute, etc.
 * Also dispatches to set_step_param for step_S_* params.
 */
void set_track_param(int track_idx, const char *param, const char *val) {
    track_t *track = &g_tracks[track_idx];

    if (strcmp(param, "channel") == 0) {
        int ch = atoi(val);
        if (ch >= 0 && ch <= 15) {
            track->midi_channel = ch;
        }
    }
    else if (strcmp(param, "mute") == 0) {
        track->muted = atoi(val) ? 1 : 0;
    }
    else if (strcmp(param, "length") == 0) {
        int len = atoi(val);
        if (len >= 1 && len <= NUM_STEPS) {
            track->length = len;
        }
    }
    else if (strcmp(param, "speed") == 0) {
        double spd = atof(val);
        if (spd >= 0.1 && spd <= 8.0) {
            track->speed = spd;
        }
    }
    else if (strcmp(param, "swing") == 0) {
        int sw = atoi(val);
        if (sw >= 0 && sw <= 100) {
            track->swing = sw;
        }
    }
    else if (strcmp(param, "chord_follow") == 0) {
        g_chord_follow[track_idx] = atoi(val) ? 1 : 0;
        g_scale_dirty = 1;
    }
    else if (strcmp(param, "arp_mode") == 0) {
        int mode = atoi(val);
        if (mode >= 0 && mode < NUM_ARP_MODES) {
            track->arp_mode = mode;
        }
    }
    else if (strcmp(param, "arp_speed") == 0) {
        int speed = atoi(val);
        if (speed >= 0 && speed < NUM_ARP_SPEEDS) {
            track->arp_speed = speed;
        }
    }
    else if (strcmp(param, "arp_octave") == 0) {
        int oct = atoi(val);
        if (oct >= 0 && oct < NUM_ARP_OCTAVES) {
            track->arp_octave = oct;
        }
    }
    else if (strcmp(param, "arp_continuous") == 0) {
        track->arp_continuous = atoi(val) ? 1 : 0;
        /* Reset pattern index when toggling continuous mode */
        track->arp_pattern_idx = 0;
        track->arp_last_num_notes = 0;
    }
    else if (strcmp(param, "loop_start") == 0) {
        int start = atoi(val);
        if (start >= 0 && start < NUM_STEPS) {
            get_current_pattern(track)->loop_start = start;
        }
    }
    else if (strcmp(param, "loop_end") == 0) {
        int end = atoi(val);
        if (end >= 0 && end < NUM_STEPS) {
            get_current_pattern(track)->loop_end = end;
        }
    }
    else if (strcmp(param, "pattern") == 0) {
        int pat = atoi(val);
        if (pat >= 0 && pat < NUM_PATTERNS) {
            track->current_pattern = pat;
        }
    }
    else if (strcmp(param, "preview_velocity") == 0) {
        int vel = atoi(val);
        if (vel >= 1 && vel <= 127) {
            track->preview_velocity = vel;
        }
    }
    else if (strcmp(param, "preview_note") == 0) {
        int note = atoi(val);
        if (note > 0 && note <= 127) {
            send_note_on(note, track->preview_velocity, track->midi_channel);
        }
    }
    else if (strcmp(param, "preview_note_off") == 0) {
        int note = atoi(val);
        if (note > 0 && note <= 127) {
            send_note_off(note, track->midi_channel);
        }
    }
    else if (strcmp(param, "cc1_default") == 0) {
        int cc = atoi(val);
        if (cc >= 0 && cc <= 127) {
            track->cc1_default = cc;
        }
    }
    else if (strcmp(param, "cc2_default") == 0) {
        int cc = atoi(val);
        if (cc >= 0 && cc <= 127) {
            track->cc2_default = cc;
        }
    }
    /* Step-level params: step_S_param */
    else if (strncmp(param, "step_", 5) == 0) {
        int step = atoi(param + 5);
        if (step >= 0 && step < NUM_STEPS) {
            const char *step_param = strchr(param + 5, '_');
            if (step_param) {
                set_step_param(track_idx, step, step_param + 1, val);
            }
        }
    }
}

/**
 * Handle track-level parameter getting.
 * Returns bytes written to buf, or -1 if param not found.
 */
int get_track_param(int track_idx, const char *param, char *buf, int buf_len) {
    track_t *track = &g_tracks[track_idx];

    if (strcmp(param, "channel") == 0) {
        return snprintf(buf, buf_len, "%d", track->midi_channel);
    }
    else if (strcmp(param, "mute") == 0) {
        return snprintf(buf, buf_len, "%d", track->muted);
    }
    else if (strcmp(param, "length") == 0) {
        return snprintf(buf, buf_len, "%d", track->length);
    }
    else if (strcmp(param, "speed") == 0) {
        return snprintf(buf, buf_len, "%.4f", track->speed);
    }
    else if (strcmp(param, "swing") == 0) {
        return snprintf(buf, buf_len, "%d", track->swing);
    }
    else if (strcmp(param, "loop_start") == 0) {
        return snprintf(buf, buf_len, "%d", get_current_pattern(track)->loop_start);
    }
    else if (strcmp(param, "loop_end") == 0) {
        return snprintf(buf, buf_len, "%d", get_current_pattern(track)->loop_end);
    }
    else if (strcmp(param, "pattern") == 0) {
        return snprintf(buf, buf_len, "%d", track->current_pattern);
    }
    else if (strcmp(param, "current_step") == 0) {
        return snprintf(buf, buf_len, "%d", track->current_step);
    }
    else if (strcmp(param, "arp_mode") == 0) {
        return snprintf(buf, buf_len, "%d", track->arp_mode);
    }
    else if (strcmp(param, "arp_speed") == 0) {
        return snprintf(buf, buf_len, "%d", track->arp_speed);
    }
    else if (strcmp(param, "arp_octave") == 0) {
        return snprintf(buf, buf_len, "%d", track->arp_octave);
    }
    else if (strcmp(param, "arp_continuous") == 0) {
        return snprintf(buf, buf_len, "%d", track->arp_continuous);
    }
    /* Step-level params: step_S_param */
    else if (strncmp(param, "step_", 5) == 0) {
        int step = atoi(param + 5);
        if (step >= 0 && step < NUM_STEPS) {
            const char *step_param = strchr(param + 5, '_');
            if (step_param) {
                return get_step_param(track_idx, step, step_param + 1, buf, buf_len);
            }
        }
    }

    return -1;
}

/**
 * Handle transpose sequence parameter setting.
 */
void set_transpose_param(const char *key, const char *val) {
    if (strcmp(key, "transpose_clear") == 0) {
        clear_transpose_sequence();
    }
    else if (strcmp(key, "transpose_sequence_enabled") == 0) {
        g_transpose_sequence_enabled = atoi(val) ? 1 : 0;
    }
    else if (strcmp(key, "transpose_step_count") == 0) {
        int count = atoi(val);
        if (count >= 0 && count <= MAX_TRANSPOSE_STEPS) {
            g_transpose_step_count = count;
            rebuild_transpose_lookup();
            printf("[TRANSPOSE] Set step_count = %d, total_steps = %u\n",
                   count, g_transpose_total_steps);

            /* Log current jump values */
            for (int i = 0; i < count; i++) {
                if (g_transpose_sequence[i].jump >= 0) {
                    printf("[TRANSPOSE] Step %d has jump = %d\n",
                           i, g_transpose_sequence[i].jump);
                }
            }
        }
    }
    else if (strncmp(key, "transpose_step_", 15) == 0) {
        int step_idx = atoi(key + 15);
        if (step_idx >= 0 && step_idx < MAX_TRANSPOSE_STEPS) {
            const char *param = strchr(key + 15, '_');
            if (param) {
                param++;
                if (strcmp(param, "transpose") == 0) {
                    int t = atoi(val);
                    if (t >= -24 && t <= 24) {
                        g_transpose_sequence[step_idx].transpose = t;
                        if (step_idx >= g_transpose_step_count) {
                            g_transpose_step_count = step_idx + 1;
                        }
                        rebuild_transpose_lookup();
                    }
                }
                else if (strcmp(param, "duration") == 0) {
                    int d = atoi(val);
                    if (d >= 1 && d <= 256) {
                        g_transpose_sequence[step_idx].duration = d;
                        rebuild_transpose_lookup();
                    }
                }
                else if (strcmp(param, "jump") == 0) {
                    int j = atoi(val);
                    if (j >= -1 && j < MAX_TRANSPOSE_STEPS) {
                        g_transpose_sequence[step_idx].jump = j;
                        printf("[TRANSPOSE] Set step %d jump = %d\n", step_idx, j);
                    }
                }
                else if (strcmp(param, "condition_n") == 0) {
                    int n = atoi(val);
                    if (n >= 0 && n <= 127) {
                        g_transpose_sequence[step_idx].condition_n = n;
                    }
                }
                else if (strcmp(param, "condition_m") == 0) {
                    int m = atoi(val);
                    if (m >= 0 && m <= 127) {
                        g_transpose_sequence[step_idx].condition_m = m;
                    }
                }
                else if (strcmp(param, "condition_not") == 0) {
                    g_transpose_sequence[step_idx].condition_not = (strcmp(val, "1") == 0) ? 1 : 0;
                }
            }
        }
    }
}

/**
 * Handle transpose sequence parameter getting.
 * Returns bytes written to buf, or -1 if param not found.
 */
int get_transpose_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "current_transpose") == 0) {
        uint32_t global_step = (uint32_t)g_global_phase;
        int8_t transpose = get_transpose_at_step(global_step);
        return snprintf(buf, buf_len, "%d", transpose);
    }
    else if (strcmp(key, "current_transpose_step") == 0) {
        /* Return virtual step position (after jumps), not real time-based position */
        return snprintf(buf, buf_len, "%d", g_transpose_virtual_step);
    }
    else if (strcmp(key, "transpose_sequence_enabled") == 0) {
        return snprintf(buf, buf_len, "%d", g_transpose_sequence_enabled);
    }
    else if (strcmp(key, "transpose_step_count") == 0) {
        return snprintf(buf, buf_len, "%d", g_transpose_step_count);
    }
    else if (strcmp(key, "transpose_total_steps") == 0) {
        return snprintf(buf, buf_len, "%u", g_transpose_total_steps);
    }

    return -1;
}
