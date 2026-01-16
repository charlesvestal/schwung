/*
 * SEQOMD DSP Plugin - Note Scheduler
 *
 * Centralized note scheduling with swing, note conflicts, and note-on/off timing.
 */

#include "seq_plugin.h"

/**
 * Calculate swing delay based on global phase.
 * Swing is applied to "upbeat" positions (odd global beats).
 * Returns the delay in steps (0.0 to SWING_MAX_DELAY).
 */
double calculate_swing_delay(int swing, double global_phase) {
    if (swing <= 50) return 0.0;  /* No swing */

    /* Check if this is an upbeat (odd beat number) */
    int global_beat = (int)global_phase;
    int is_upbeat = global_beat & 1;

    if (!is_upbeat) return 0.0;  /* Downbeats don't swing */

    /* Calculate delay: swing 50 = 0, swing 100 = 0.5 steps delay */
    double swing_amount = (swing - 50) / 100.0;  /* 0.0 to 0.5 */
    return swing_amount * SWING_MAX_DELAY;
}

/**
 * Find an existing scheduled note with the same note+channel that's still active.
 * Returns the index, or -1 if not found.
 */
int find_conflicting_note(uint8_t note, uint8_t channel) {
    for (int i = 0; i < MAX_SCHEDULED_NOTES; i++) {
        if (g_scheduled_notes[i].active &&
            g_scheduled_notes[i].note == note &&
            g_scheduled_notes[i].channel == channel &&
            !g_scheduled_notes[i].off_sent) {
            return i;
        }
    }
    return -1;
}

/**
 * Find a free slot in the scheduler.
 * Returns the index, or -1 if full.
 */
int find_free_slot(void) {
    for (int i = 0; i < MAX_SCHEDULED_NOTES; i++) {
        if (!g_scheduled_notes[i].active) {
            return i;
        }
    }
    return -1;
}

/**
 * Schedule a note to be played.
 * Handles swing application and note conflicts automatically.
 * Transpose is applied at send time, not schedule time, to support live transpose.
 *
 * @param note      Original (untransposed) MIDI note number
 * @param velocity  Note velocity
 * @param channel   MIDI channel
 * @param swing     Swing amount (50 = no swing)
 * @param on_phase  Global phase when note should start (before swing)
 * @param length    Note length in steps
 * @param gate      Gate percentage (1-100)
 * @param track_idx Track index (for chord_follow lookup at send time)
 * @param sequence_transpose  Sequence transpose value at schedule time
 */
void schedule_note(uint8_t note, uint8_t velocity, uint8_t channel,
                   int swing, double on_phase, double length, int gate,
                   uint8_t track_idx, int8_t sequence_transpose) {
    /* Apply swing delay based on global phase */
    double swing_delay = calculate_swing_delay(swing, on_phase);
    double swung_on_phase = on_phase + swing_delay;

    /* Calculate note-off time: length adjusted by gate percentage */
    double gate_mult = gate / 100.0;
    double note_duration = length * gate_mult;
    double off_phase = swung_on_phase + note_duration;

    /* Check for conflicting note (same note+channel already playing) */
    int conflict_idx = find_conflicting_note(note, channel);
    if (conflict_idx >= 0) {
        scheduled_note_t *conflict = &g_scheduled_notes[conflict_idx];
        /* If the new note starts before the old note ends, truncate the old note */
        if (swung_on_phase < conflict->off_phase) {
            /* Schedule the old note to end just before the new one starts */
            double early_off = swung_on_phase - 0.001;  /* Tiny gap to avoid overlap */
            if (early_off > g_global_phase) {
                conflict->off_phase = early_off;
            } else {
                /* Old note should end now - send immediate note-off if on was sent */
                if (conflict->on_sent && !conflict->off_sent) {
                    send_note_off(conflict->sent_note, conflict->channel);
                    conflict->off_sent = 1;
                    conflict->active = 0;  /* Free the slot to prevent leak */
                }
            }
        }
    }

    /* Find a free slot */
    int slot = find_free_slot();
    if (slot < 0) {
        /* Scheduler full - skip note (shouldn't happen with reasonable settings) */
        return;
    }

    /* Schedule the note */
    scheduled_note_t *sn = &g_scheduled_notes[slot];
    sn->note = note;
    sn->channel = channel;
    sn->velocity = velocity;
    sn->on_phase = swung_on_phase;
    sn->off_phase = off_phase;
    sn->on_sent = 0;
    sn->off_sent = 0;
    sn->active = 1;
    sn->track_idx = track_idx;
    sn->sequence_transpose = sequence_transpose;
    sn->sent_note = 0;  /* Will be set when note-on is sent */
}

/**
 * Process all scheduled notes - send note-on/off at the right time.
 * Called once per block (~2.9ms at 128 samples/block).
 * Transpose is applied at send time to support immediate live transpose.
 */
void process_scheduled_notes(void) {
    for (int i = 0; i < MAX_SCHEDULED_NOTES; i++) {
        scheduled_note_t *sn = &g_scheduled_notes[i];
        if (!sn->active) continue;

        /* Send note-on at the scheduled time */
        if (!sn->on_sent && g_global_phase >= sn->on_phase) {
            /* Apply transpose at send time (not schedule time) */
            int final_note = sn->note;
            if (g_chord_follow[sn->track_idx]) {
                /* Live transpose takes precedence over sequence transpose.
                 * Sequence transpose is looked up now (at send time) to support
                 * long notes/arps that span multiple transpose steps. */
                int transpose = (g_live_transpose != 0)
                    ? g_live_transpose
                    : get_transpose_at_step((uint32_t)g_global_phase);
                final_note = sn->note + transpose;
                if (final_note < 0) final_note = 0;
                if (final_note > 127) final_note = 127;
            }

            send_note_on(final_note, sn->velocity, sn->channel);
            sn->sent_note = (uint8_t)final_note;  /* Remember for note-off */
            sn->on_sent = 1;
        }

        /* Send note-off at the scheduled time */
        if (sn->on_sent && !sn->off_sent && g_global_phase >= sn->off_phase) {
            send_note_off(sn->sent_note, sn->channel);
            sn->off_sent = 1;
            sn->active = 0;  /* Free the slot */
        }
    }
}

/**
 * Clear all scheduled notes and send note-off for any active notes.
 */
void clear_scheduled_notes(void) {
    for (int i = 0; i < MAX_SCHEDULED_NOTES; i++) {
        scheduled_note_t *sn = &g_scheduled_notes[i];
        if (sn->active && sn->on_sent && !sn->off_sent) {
            send_note_off(sn->sent_note, sn->channel);
        }
        sn->active = 0;
        sn->on_sent = 0;
        sn->off_sent = 0;
    }
}

/**
 * Clear scheduled notes for a specific channel (Cut mode).
 * Sends note-off for any currently playing notes and cancels pending notes.
 */
void cut_channel_notes(uint8_t channel) {
    for (int i = 0; i < MAX_SCHEDULED_NOTES; i++) {
        scheduled_note_t *sn = &g_scheduled_notes[i];
        if (sn->active && sn->channel == channel) {
            /* Send note-off for any note that has started but not ended */
            if (sn->on_sent && !sn->off_sent) {
                send_note_off(sn->sent_note, sn->channel);
            }
            /* Cancel the slot */
            sn->active = 0;
            sn->on_sent = 0;
            sn->off_sent = 0;
        }
    }
}

/* Send note-off for all active notes */
void all_notes_off(void) {
    /* Clear all scheduled notes - this sends note-off for any active notes */
    clear_scheduled_notes();
}
