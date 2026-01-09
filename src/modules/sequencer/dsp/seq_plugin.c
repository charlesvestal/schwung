/*
 * SEQOMD DSP Plugin
 *
 * 8-track sequencer with per-track timing, MIDI output, and master clock.
 * Inspired by OP-Z architecture.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Include plugin API */
#include "host/plugin_api_v1.h"

/* ============ Constants ============ */

#define NUM_TRACKS 8
#define NUM_STEPS 16
#define NUM_PATTERNS 30
#define MAX_NOTES_PER_STEP 4
#define MAX_SCHEDULED_NOTES 128

#define DEFAULT_VELOCITY 100
#define DEFAULT_GATE 50

/* Swing is applied as a delay to upbeat notes.
 * Swing value 50 = no swing, 67 = triplet feel.
 * The delay is calculated as: (swing - 50) / 100.0 * 0.5 steps */
#define SWING_MAX_DELAY 0.5

/* MIDI real-time messages */
#define MIDI_CLOCK      0xF8
#define MIDI_START      0xFA
#define MIDI_CONTINUE   0xFB
#define MIDI_STOP       0xFC

/* ============ Data Structures ============ */

/* Step data */
typedef struct {
    uint8_t notes[MAX_NOTES_PER_STEP];  /* Up to 4 notes per step (0 = empty slot) */
    uint8_t num_notes;                   /* Number of active notes */
    uint8_t velocity;                    /* 1-127 */
    uint8_t gate;                        /* Gate length as % of step (1-100) */
    int8_t cc1;                          /* CC1 value (-1 = not set, 0-127 = value) */
    int8_t cc2;                          /* CC2 value (-1 = not set, 0-127 = value) */
    uint8_t probability;                 /* 1-100% chance to trigger */
    int8_t condition_n;                  /* Trigger Spark: cycle length (0=none) */
    int8_t condition_m;                  /* Trigger Spark: which iteration to play (1 to N) */
    uint8_t condition_not;               /* Trigger Spark: negate condition */
    uint8_t ratchet;                     /* Number of sub-triggers (1, 2, 3, 4, 6, 8) */
    uint8_t length;                      /* Note length in steps (1-16) */
    /* Parameter Spark - when CC locks apply */
    int8_t param_spark_n;                /* 0=always, >0=every N loops */
    int8_t param_spark_m;                /* Which iteration (1 to N) */
    uint8_t param_spark_not;             /* Negate condition */
    /* Component Spark - when ratchet/jump apply */
    int8_t comp_spark_n;                 /* 0=always, >0=every N loops */
    int8_t comp_spark_m;                 /* Which iteration (1 to N) */
    uint8_t comp_spark_not;              /* Negate condition */
    int8_t jump;                         /* Jump target step (-1 = no jump, 0-15 = step) */
    int8_t offset;                       /* Micro-timing offset in ticks (-24 to +24, 48 ticks per step) */
} step_t;

/* Pattern data - contains steps and loop points */
typedef struct {
    step_t steps[NUM_STEPS];
    uint8_t loop_start;     /* Loop start step (0-15) */
    uint8_t loop_end;       /* Loop end step (0-15), wraps after this */
} pattern_t;

/* Track data */
typedef struct {
    pattern_t patterns[NUM_PATTERNS];  /* 8 patterns per track */
    uint8_t current_pattern;           /* Currently active pattern (0-7) */
    uint8_t midi_channel;   /* 0-15 */
    uint8_t length;         /* 1-64 steps (for now max 16) */
    uint8_t current_step;
    uint8_t muted;
    uint8_t swing;          /* Swing amount 0-100 (50 = no swing, 67 = triplet feel) */
    double speed;           /* Speed multiplier (0.25 to 4.0) */
    double phase;           /* Position within current step (0.0 to 1.0) for gate/ratchet */
    double gate_phase;      /* Gate timing */
    int8_t last_notes[MAX_NOTES_PER_STEP];  /* Last triggered notes (-1 = none) */
    uint8_t num_last_notes;                  /* Number of active notes */
    uint8_t note_on_active;
    uint32_t loop_count;    /* Number of times pattern has looped (for conditions) */
    /* Ratchet state */
    uint8_t ratchet_count;  /* Current ratchet sub-trigger index */
    uint8_t ratchet_total;  /* Total ratchets for current step */
    double ratchet_phase;   /* Phase accumulator for ratchet timing */
    /* Note length tracking */
    uint8_t note_length_total;   /* Total length of current note in steps */
    uint8_t note_gate;           /* Gate % of the note that triggered (stored at trigger time) */
    double note_length_phase;    /* Phase accumulator for note length */
    /* Pending note trigger (for micro-timing offset) */
    uint8_t trigger_pending;     /* 1 if a step trigger is pending */
    double trigger_at_phase;     /* Phase value when trigger should fire */
    uint8_t pending_step;        /* Which step is pending */
    double next_step_at;         /* Phase value when next step advance should happen */
} track_t;

/* ============ Centralized Note Scheduler ============ */
/* All notes go through this scheduler which:
 * 1. Applies swing based on global beat position
 * 2. Handles note conflicts (same note+channel)
 * 3. Manages note-on and note-off timing
 */
typedef struct {
    uint8_t note;
    uint8_t channel;
    uint8_t velocity;
    double on_phase;        /* Global phase when note-on should fire */
    double off_phase;       /* Global phase when note-off should fire */
    uint8_t on_sent;        /* Has note-on been sent? */
    uint8_t off_sent;       /* Has note-off been sent? */
    uint8_t active;         /* Is this slot in use? */
} scheduled_note_t;

/* ============ Plugin State ============ */

static const host_api_v1_t *g_host = NULL;
static plugin_api_v1_t g_plugin_api;

/* Tracks */
static track_t g_tracks[NUM_TRACKS];

/* Centralized note scheduler */
static scheduled_note_t g_scheduled_notes[MAX_SCHEDULED_NOTES];

/* Global playback state */
static int g_bpm = 120;
static int g_playing = 0;
static int g_send_clock = 1;
static double g_clock_phase = 0.0;
static double g_global_phase = 0.0;  /* Master clock for all timing */

/* ============ Helpers ============ */

/* Simple PRNG for probability (xorshift32) */
static uint32_t g_random_state = 1;

static uint32_t random_next(void) {
    uint32_t x = g_random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_random_state = x;
    return x;
}

/* Returns true with probability (percent/100) */
static int random_check(int percent) {
    if (percent >= 100) return 1;
    if (percent <= 0) return 0;
    return (random_next() % 100) < (uint32_t)percent;
}

static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        g_host->log(msg);
    }
}

static void send_note_on(int note, int velocity, int channel) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = {
        0x29,                           /* Cable 2, CIN 0x9 (Note On) */
        (uint8_t)(0x90 | (channel & 0x0F)),
        (uint8_t)(note & 0x7F),
        (uint8_t)(velocity & 0x7F)
    };
    g_host->midi_send_external(msg, 4);
}

static void send_note_off(int note, int channel) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = {
        0x28,                           /* Cable 2, CIN 0x8 (Note Off) */
        (uint8_t)(0x80 | (channel & 0x0F)),
        (uint8_t)(note & 0x7F),
        0x00
    };
    g_host->midi_send_external(msg, 4);
}

static void send_cc(int cc, int value, int channel) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = {
        0x2B,                           /* Cable 2, CIN 0xB (Control Change) */
        (uint8_t)(0xB0 | (channel & 0x0F)),
        (uint8_t)(cc & 0x7F),
        (uint8_t)(value & 0x7F)
    };
    g_host->midi_send_external(msg, 4);
}

static void send_midi_clock(void) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = { 0x2F, MIDI_CLOCK, 0x00, 0x00 };
    g_host->midi_send_external(msg, 4);
}

static void send_midi_start(void) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = { 0x2F, MIDI_START, 0x00, 0x00 };
    g_host->midi_send_external(msg, 4);
    plugin_log("MIDI Start");
}

static void send_midi_stop(void) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = { 0x2F, MIDI_STOP, 0x00, 0x00 };
    g_host->midi_send_external(msg, 4);
    plugin_log("MIDI Stop");
}

/* ============ Centralized Note Scheduler Functions ============ */

/**
 * Calculate swing delay based on global phase.
 * Swing is applied to "upbeat" positions (odd global beats).
 * Returns the delay in steps (0.0 to SWING_MAX_DELAY).
 */
static double calculate_swing_delay(int swing, double global_phase) {
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
static int find_conflicting_note(uint8_t note, uint8_t channel) {
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
static int find_free_slot(void) {
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
 *
 * @param note      MIDI note number
 * @param velocity  Note velocity
 * @param channel   MIDI channel
 * @param swing     Swing amount (50 = no swing)
 * @param on_phase  Global phase when note should start (before swing)
 * @param length    Note length in steps
 * @param gate      Gate percentage (1-100)
 */
static void schedule_note(uint8_t note, uint8_t velocity, uint8_t channel,
                          int swing, double on_phase, double length, int gate) {
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
                    send_note_off(conflict->note, conflict->channel);
                    conflict->off_sent = 1;
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
}

/**
 * Process all scheduled notes - send note-on/off at the right time.
 * Called every sample from render_block.
 */
static void process_scheduled_notes(void) {
    for (int i = 0; i < MAX_SCHEDULED_NOTES; i++) {
        scheduled_note_t *sn = &g_scheduled_notes[i];
        if (!sn->active) continue;

        /* Send note-on at the scheduled time */
        if (!sn->on_sent && g_global_phase >= sn->on_phase) {
            send_note_on(sn->note, sn->velocity, sn->channel);
            sn->on_sent = 1;
        }

        /* Send note-off at the scheduled time */
        if (sn->on_sent && !sn->off_sent && g_global_phase >= sn->off_phase) {
            send_note_off(sn->note, sn->channel);
            sn->off_sent = 1;
            sn->active = 0;  /* Free the slot */
        }
    }
}

/**
 * Clear all scheduled notes and send note-off for any active notes.
 */
static void clear_scheduled_notes(void) {
    for (int i = 0; i < MAX_SCHEDULED_NOTES; i++) {
        scheduled_note_t *sn = &g_scheduled_notes[i];
        if (sn->active && sn->on_sent && !sn->off_sent) {
            send_note_off(sn->note, sn->channel);
        }
        sn->active = 0;
        sn->on_sent = 0;
        sn->off_sent = 0;
    }
}

/* Send note-off for all active notes */
static void all_notes_off(void) {
    /* Clear all scheduled notes - this sends note-off for any active notes */
    clear_scheduled_notes();
}

/* ============ Track Functions ============ */

static void init_pattern(pattern_t *pattern) {
    pattern->loop_start = 0;
    pattern->loop_end = NUM_STEPS - 1;
    for (int i = 0; i < NUM_STEPS; i++) {
        for (int n = 0; n < MAX_NOTES_PER_STEP; n++) {
            pattern->steps[i].notes[n] = 0;
        }
        pattern->steps[i].num_notes = 0;
        pattern->steps[i].velocity = DEFAULT_VELOCITY;
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
    }
}

static void init_track(track_t *track, int channel) {
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

    for (int i = 0; i < MAX_NOTES_PER_STEP; i++) {
        track->last_notes[i] = -1;
    }

    /* Initialize all patterns */
    for (int p = 0; p < NUM_PATTERNS; p++) {
        init_pattern(&track->patterns[p]);
    }
}

/* Get current pattern for a track */
static inline pattern_t* get_current_pattern(track_t *track) {
    return &track->patterns[track->current_pattern];
}

/* Check if step should trigger based on probability and conditions */
static int should_step_trigger(step_t *step, track_t *track) {
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
static int check_spark_condition(int8_t spark_n, int8_t spark_m, uint8_t spark_not, track_t *track) {
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
 * Schedule notes for a step via the centralized scheduler.
 * This handles swing, ratchets, and note conflicts automatically.
 *
 * @param track      Track data
 * @param step       Step data
 * @param base_phase Global phase when this step starts
 */
static void schedule_step_notes(track_t *track, step_t *step, double base_phase) {
    int note_length = step->length > 0 ? step->length : 1;
    int gate = step->gate > 0 ? step->gate : DEFAULT_GATE;
    int ratchet_count = step->ratchet > 0 ? step->ratchet : 1;

    /* For ratchets, divide the step into equal parts */
    double ratchet_step = 1.0 / ratchet_count;
    /* Ratchet note length is proportional to gate but divided by ratchet count */
    double ratchet_length = (double)note_length / ratchet_count;

    for (int r = 0; r < ratchet_count; r++) {
        double note_on_phase = base_phase + (r * ratchet_step);

        /* Schedule each note in the step */
        for (int n = 0; n < step->num_notes && n < MAX_NOTES_PER_STEP; n++) {
            if (step->notes[n] > 0) {
                schedule_note(
                    step->notes[n],
                    step->velocity,
                    track->midi_channel,
                    track->swing,
                    note_on_phase,
                    ratchet_length,
                    gate
                );
            }
        }
    }
}

static void trigger_track_step(track_t *track, int track_idx, double step_start_phase) {
    pattern_t *pattern = get_current_pattern(track);
    step_t *step = &pattern->steps[track->current_step];

    /* Skip if muted */
    if (track->muted) return;

    /* Check param_spark - should CC locks apply this loop? */
    int param_spark_pass = check_spark_condition(
        step->param_spark_n, step->param_spark_m, step->param_spark_not, track);

    /* Send CC values if set AND param_spark passes */
    /* Note: CCs are sent immediately, not scheduled (they don't need swing) */
    if (param_spark_pass) {
        if (step->cc1 >= 0) {
            int cc = 20 + (track_idx * 2);
            send_cc(cc, step->cc1, track->midi_channel);
        }
        if (step->cc2 >= 0) {
            int cc = 20 + (track_idx * 2) + 1;
            send_cc(cc, step->cc2, track->midi_channel);
        }
    }

    /* Skip notes if step has none */
    if (step->num_notes == 0) return;

    /* Check if this step should trigger (probability + conditions / trigger spark) */
    if (!should_step_trigger(step, track)) return;

    /* Check comp_spark - should ratchet apply this loop? */
    int comp_spark_pass = check_spark_condition(
        step->comp_spark_n, step->comp_spark_m, step->comp_spark_not, track);

    /* Apply micro-timing offset */
    double offset_phase = (double)step->offset / 48.0;
    double note_phase = step_start_phase + offset_phase;

    /* Schedule notes (with ratchets if comp_spark passes) */
    if (comp_spark_pass && step->ratchet > 1) {
        /* Schedule with ratchets */
        schedule_step_notes(track, step, note_phase);
    } else {
        /* Schedule single trigger (no ratchet) */
        int note_length = step->length > 0 ? step->length : 1;
        int gate = step->gate > 0 ? step->gate : DEFAULT_GATE;

        for (int n = 0; n < step->num_notes && n < MAX_NOTES_PER_STEP; n++) {
            if (step->notes[n] > 0) {
                schedule_note(
                    step->notes[n],
                    step->velocity,
                    track->midi_channel,
                    track->swing,
                    note_phase,
                    note_length,
                    gate
                );
            }
        }
    }

    /* Handle jump (only if comp_spark passes) */
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
static void advance_track(track_t *track, int track_idx) {
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

/* ============ Plugin Callbacks ============ */

static int plugin_on_load(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "SEQOMD loading from: %s", module_dir);
    plugin_log(msg);

    /* Initialize all tracks with default MIDI channels */
    for (int t = 0; t < NUM_TRACKS; t++) {
        init_track(&g_tracks[t], t);  /* Track 0 = ch 0, etc. */
    }

    /* Clear note scheduler */
    memset(g_scheduled_notes, 0, sizeof(g_scheduled_notes));

    /* Parse BPM from defaults if provided */
    if (json_defaults) {
        const char *pos = strstr(json_defaults, "\"bpm\"");
        if (pos) {
            pos = strchr(pos, ':');
            if (pos) {
                g_bpm = atoi(pos + 1);
                if (g_bpm < 20) g_bpm = 20;
                if (g_bpm > 300) g_bpm = 300;
            }
        }
    }

    snprintf(msg, sizeof(msg), "SEQOMD ready: %d tracks, BPM: %d", NUM_TRACKS, g_bpm);
    plugin_log(msg);

    return 0;
}

static void plugin_on_unload(void) {
    plugin_log("SEQOMD unloading");
    all_notes_off();
}

static void plugin_on_midi(const uint8_t *msg, int len, int source) {
    /* Currently no MIDI input handling - Move is master */
    (void)msg;
    (void)len;
    (void)source;
}

static void plugin_set_param(const char *key, const char *val) {
    if (strcmp(key, "bpm") == 0) {
        int new_bpm = atoi(val);
        if (new_bpm >= 20 && new_bpm <= 300) {
            g_bpm = new_bpm;
        }
    }
    else if (strcmp(key, "playing") == 0) {
        int new_playing = atoi(val);
        if (new_playing && !g_playing) {
            /* Starting playback - clear scheduler and reset all tracks */
            clear_scheduled_notes();

            for (int t = 0; t < NUM_TRACKS; t++) {
                g_tracks[t].current_step = get_current_pattern(&g_tracks[t])->loop_start;
                g_tracks[t].phase = 0.0;
                g_tracks[t].loop_count = 0;
                g_tracks[t].next_step_at = 1.0;
            }
            g_clock_phase = 0.0;
            g_global_phase = 0.0;

            /* Seed PRNG with a bit of entropy */
            g_random_state = 12345;

            if (g_send_clock) {
                send_midi_start();
                send_midi_clock();
            }

            /* Schedule first step on all tracks via centralized scheduler */
            for (int t = 0; t < NUM_TRACKS; t++) {
                trigger_track_step(&g_tracks[t], t, 0.0);
            }
        } else if (!new_playing && g_playing) {
            /* Stopping playback */
            all_notes_off();
            if (g_send_clock) {
                send_midi_stop();
            }
        }
        g_playing = new_playing;
    }
    else if (strcmp(key, "send_clock") == 0) {
        g_send_clock = atoi(val);
    }
    /* Send CC externally: send_cc_CHANNEL_CC = VALUE */
    else if (strncmp(key, "send_cc_", 8) == 0) {
        /* Parse: send_cc_15_1 for channel 15, CC 1 */
        int channel = atoi(key + 8);
        const char *cc_part = strchr(key + 8, '_');
        if (cc_part) {
            int cc = atoi(cc_part + 1);
            int value = atoi(val);
            if (channel >= 0 && channel <= 15 && cc >= 0 && cc <= 127) {
                send_cc(cc, value, channel);
            }
        }
    }
    /* Track-specific parameters: track_T_step_S_note, track_T_step_S_vel, etc. */
    else if (strncmp(key, "track_", 6) == 0) {
        int track = atoi(key + 6);
        if (track >= 0 && track < NUM_TRACKS) {
            const char *param = strchr(key + 6, '_');
            if (param) {
                param++;

                /* Track-level params: track_T_channel, track_T_mute, track_T_length */
                if (strcmp(param, "channel") == 0) {
                    int ch = atoi(val);
                    if (ch >= 0 && ch <= 15) {
                        g_tracks[track].midi_channel = ch;
                    }
                }
                else if (strcmp(param, "mute") == 0) {
                    g_tracks[track].muted = atoi(val) ? 1 : 0;
                }
                else if (strcmp(param, "length") == 0) {
                    int len = atoi(val);
                    if (len >= 1 && len <= NUM_STEPS) {
                        g_tracks[track].length = len;
                    }
                }
                else if (strcmp(param, "speed") == 0) {
                    double spd = atof(val);
                    if (spd >= 0.1 && spd <= 8.0) {
                        g_tracks[track].speed = spd;
                    }
                }
                else if (strcmp(param, "swing") == 0) {
                    int sw = atoi(val);
                    if (sw >= 0 && sw <= 100) {
                        g_tracks[track].swing = sw;
                    }
                }
                else if (strcmp(param, "loop_start") == 0) {
                    int start = atoi(val);
                    if (start >= 0 && start < NUM_STEPS) {
                        get_current_pattern(&g_tracks[track])->loop_start = start;
                    }
                }
                else if (strcmp(param, "loop_end") == 0) {
                    int end = atoi(val);
                    if (end >= 0 && end < NUM_STEPS) {
                        get_current_pattern(&g_tracks[track])->loop_end = end;
                    }
                }
                else if (strcmp(param, "pattern") == 0) {
                    int pat = atoi(val);
                    if (pat >= 0 && pat < NUM_PATTERNS) {
                        g_tracks[track].current_pattern = pat;
                    }
                }
                /* Preview note - play a note immediately for auditioning */
                else if (strcmp(param, "preview_note") == 0) {
                    int note = atoi(val);
                    if (note > 0 && note <= 127) {
                        send_note_on(note, DEFAULT_VELOCITY, g_tracks[track].midi_channel);
                    }
                }
                else if (strcmp(param, "preview_note_off") == 0) {
                    int note = atoi(val);
                    if (note > 0 && note <= 127) {
                        send_note_off(note, g_tracks[track].midi_channel);
                    }
                }
                /* Step-level params: track_T_step_S_note, etc. */
                else if (strncmp(param, "step_", 5) == 0) {
                    int step = atoi(param + 5);
                    if (step >= 0 && step < NUM_STEPS) {
                        const char *step_param = strchr(param + 5, '_');
                        if (step_param) {
                            step_param++;
                            /* Set single note (backward compat - clears other notes) */
                            if (strcmp(step_param, "note") == 0) {
                                int note = atoi(val);
                                step_t *s = &get_current_pattern(&g_tracks[track])->steps[step];
                                if (note == 0) {
                                    /* Clear all notes */
                                    s->num_notes = 0;
                                    for (int n = 0; n < MAX_NOTES_PER_STEP; n++) {
                                        s->notes[n] = 0;
                                    }
                                } else if (note >= 1 && note <= 127) {
                                    /* Set as single note */
                                    s->notes[0] = note;
                                    s->num_notes = 1;
                                    for (int n = 1; n < MAX_NOTES_PER_STEP; n++) {
                                        s->notes[n] = 0;
                                    }
                                }
                            }
                            /* Add a note to the step (for chords) */
                            else if (strcmp(step_param, "add_note") == 0) {
                                int note = atoi(val);
                                if (note >= 1 && note <= 127) {
                                    step_t *s = &get_current_pattern(&g_tracks[track])->steps[step];
                                    /* Check if note already exists */
                                    int exists = 0;
                                    for (int n = 0; n < s->num_notes; n++) {
                                        if (s->notes[n] == note) {
                                            exists = 1;
                                            break;
                                        }
                                    }
                                    /* Add if not exists and room available */
                                    if (!exists && s->num_notes < MAX_NOTES_PER_STEP) {
                                        s->notes[s->num_notes] = note;
                                        s->num_notes++;
                                    }
                                }
                            }
                            /* Remove a note from the step */
                            else if (strcmp(step_param, "remove_note") == 0) {
                                int note = atoi(val);
                                if (note >= 1 && note <= 127) {
                                    step_t *s = &get_current_pattern(&g_tracks[track])->steps[step];
                                    for (int n = 0; n < s->num_notes; n++) {
                                        if (s->notes[n] == note) {
                                            /* Shift remaining notes down */
                                            for (int m = n; m < s->num_notes - 1; m++) {
                                                s->notes[m] = s->notes[m + 1];
                                            }
                                            s->notes[s->num_notes - 1] = 0;
                                            s->num_notes--;
                                            break;
                                        }
                                    }
                                }
                            }
                            /* Clear all notes, CCs, and parameters from step */
                            else if (strcmp(step_param, "clear") == 0) {
                                step_t *s = &get_current_pattern(&g_tracks[track])->steps[step];
                                s->num_notes = 0;
                                for (int n = 0; n < MAX_NOTES_PER_STEP; n++) {
                                    s->notes[n] = 0;
                                }
                                s->cc1 = -1;
                                s->cc2 = -1;
                                s->probability = 100;
                                s->condition_n = 0;
                                s->condition_m = 0;
                                s->condition_not = 0;
                                s->ratchet = 1;
                                s->length = 1;
                                /* Clear spark fields */
                                s->param_spark_n = 0;
                                s->param_spark_m = 0;
                                s->param_spark_not = 0;
                                s->comp_spark_n = 0;
                                s->comp_spark_m = 0;
                                s->comp_spark_not = 0;
                                s->jump = -1;
                                s->offset = 0;
                            }
                            else if (strcmp(step_param, "vel") == 0) {
                                int vel = atoi(val);
                                if (vel >= 1 && vel <= 127) {
                                    get_current_pattern(&g_tracks[track])->steps[step].velocity = vel;
                                }
                            }
                            else if (strcmp(step_param, "gate") == 0) {
                                int gate = atoi(val);
                                if (gate >= 1 && gate <= 100) {
                                    get_current_pattern(&g_tracks[track])->steps[step].gate = gate;
                                }
                            }
                            /* Per-step CC values */
                            else if (strcmp(step_param, "cc1") == 0) {
                                int cc_val = atoi(val);
                                if (cc_val >= -1 && cc_val <= 127) {
                                    get_current_pattern(&g_tracks[track])->steps[step].cc1 = cc_val;
                                }
                            }
                            else if (strcmp(step_param, "cc2") == 0) {
                                int cc_val = atoi(val);
                                if (cc_val >= -1 && cc_val <= 127) {
                                    get_current_pattern(&g_tracks[track])->steps[step].cc2 = cc_val;
                                }
                            }
                            /* Probability */
                            else if (strcmp(step_param, "probability") == 0) {
                                int prob = atoi(val);
                                if (prob >= 1 && prob <= 100) {
                                    get_current_pattern(&g_tracks[track])->steps[step].probability = prob;
                                }
                            }
                            /* Condition parameters */
                            else if (strcmp(step_param, "condition_n") == 0) {
                                int n = atoi(val);
                                get_current_pattern(&g_tracks[track])->steps[step].condition_n = n;
                            }
                            else if (strcmp(step_param, "condition_m") == 0) {
                                int m = atoi(val);
                                get_current_pattern(&g_tracks[track])->steps[step].condition_m = m;
                            }
                            else if (strcmp(step_param, "condition_not") == 0) {
                                int not_flag = atoi(val);
                                get_current_pattern(&g_tracks[track])->steps[step].condition_not = not_flag ? 1 : 0;
                            }
                            /* Parameter Spark (when CC locks apply) */
                            else if (strcmp(step_param, "param_spark_n") == 0) {
                                int n = atoi(val);
                                get_current_pattern(&g_tracks[track])->steps[step].param_spark_n = n;
                            }
                            else if (strcmp(step_param, "param_spark_m") == 0) {
                                int m = atoi(val);
                                get_current_pattern(&g_tracks[track])->steps[step].param_spark_m = m;
                            }
                            else if (strcmp(step_param, "param_spark_not") == 0) {
                                int not_flag = atoi(val);
                                get_current_pattern(&g_tracks[track])->steps[step].param_spark_not = not_flag ? 1 : 0;
                            }
                            /* Component Spark (when ratchet/jump apply) */
                            else if (strcmp(step_param, "comp_spark_n") == 0) {
                                int n = atoi(val);
                                get_current_pattern(&g_tracks[track])->steps[step].comp_spark_n = n;
                            }
                            else if (strcmp(step_param, "comp_spark_m") == 0) {
                                int m = atoi(val);
                                get_current_pattern(&g_tracks[track])->steps[step].comp_spark_m = m;
                            }
                            else if (strcmp(step_param, "comp_spark_not") == 0) {
                                int not_flag = atoi(val);
                                get_current_pattern(&g_tracks[track])->steps[step].comp_spark_not = not_flag ? 1 : 0;
                            }
                            /* Jump target */
                            else if (strcmp(step_param, "jump") == 0) {
                                int jump = atoi(val);
                                if (jump >= -1 && jump < NUM_STEPS) {
                                    get_current_pattern(&g_tracks[track])->steps[step].jump = jump;
                                }
                            }
                            /* Ratchet */
                            else if (strcmp(step_param, "ratchet") == 0) {
                                int ratch = atoi(val);
                                if (ratch >= 1 && ratch <= 8) {
                                    get_current_pattern(&g_tracks[track])->steps[step].ratchet = ratch;
                                }
                            }
                            /* Note length in steps */
                            else if (strcmp(step_param, "length") == 0) {
                                int len = atoi(val);
                                if (len >= 1 && len <= 16) {
                                    get_current_pattern(&g_tracks[track])->steps[step].length = len;
                                }
                            }
                            /* Micro-timing offset */
                            else if (strcmp(step_param, "offset") == 0) {
                                int off = atoi(val);
                                if (off >= -24 && off <= 24) {
                                    get_current_pattern(&g_tracks[track])->steps[step].offset = off;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    /* Legacy single-track params for backward compatibility */
    else if (strncmp(key, "step_", 5) == 0) {
        int step = atoi(key + 5);
        if (step >= 0 && step < NUM_STEPS) {
            const char *param = strchr(key + 5, '_');
            if (param) {
                param++;
                /* Apply to track 0 for backward compat */
                if (strcmp(param, "note") == 0) {
                    int note = atoi(val);
                    if (note >= 0 && note <= 127) {
                        /* Backward compat: clear step and set single note */
                        pattern_t *pat = get_current_pattern(&g_tracks[0]);
                        pat->steps[step].num_notes = 0;
                        if (note > 0) {
                            pat->steps[step].notes[0] = (uint8_t)note;
                            pat->steps[step].num_notes = 1;
                        }
                    }
                }
            }
        }
    }
}

static int plugin_get_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "bpm") == 0) {
        return snprintf(buf, buf_len, "%d", g_bpm);
    }
    else if (strcmp(key, "playing") == 0) {
        return snprintf(buf, buf_len, "%d", g_playing);
    }
    else if (strcmp(key, "send_clock") == 0) {
        return snprintf(buf, buf_len, "%d", g_send_clock);
    }
    else if (strcmp(key, "num_tracks") == 0) {
        return snprintf(buf, buf_len, "%d", NUM_TRACKS);
    }
    /* Track params */
    else if (strncmp(key, "track_", 6) == 0) {
        int track = atoi(key + 6);
        if (track >= 0 && track < NUM_TRACKS) {
            const char *param = strchr(key + 6, '_');
            if (param) {
                param++;
                if (strcmp(param, "channel") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].midi_channel);
                }
                else if (strcmp(param, "mute") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].muted);
                }
                else if (strcmp(param, "length") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].length);
                }
                else if (strcmp(param, "speed") == 0) {
                    return snprintf(buf, buf_len, "%.4f", g_tracks[track].speed);
                }
                else if (strcmp(param, "swing") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].swing);
                }
                else if (strcmp(param, "loop_start") == 0) {
                    return snprintf(buf, buf_len, "%d", get_current_pattern(&g_tracks[track])->loop_start);
                }
                else if (strcmp(param, "loop_end") == 0) {
                    return snprintf(buf, buf_len, "%d", get_current_pattern(&g_tracks[track])->loop_end);
                }
                else if (strcmp(param, "pattern") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].current_pattern);
                }
                else if (strcmp(param, "current_step") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].current_step);
                }
                else if (strncmp(param, "step_", 5) == 0) {
                    int step = atoi(param + 5);
                    if (step >= 0 && step < NUM_STEPS) {
                        const char *step_param = strchr(param + 5, '_');
                        if (step_param) {
                            step_param++;
                            /* Return first note (backward compat) */
                            if (strcmp(step_param, "note") == 0) {
                                step_t *s = &get_current_pattern(&g_tracks[track])->steps[step];
                                return snprintf(buf, buf_len, "%d", s->num_notes > 0 ? s->notes[0] : 0);
                            }
                            /* Return all notes as comma-separated */
                            else if (strcmp(step_param, "notes") == 0) {
                                step_t *s = &get_current_pattern(&g_tracks[track])->steps[step];
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
                            else if (strcmp(step_param, "num_notes") == 0) {
                                return snprintf(buf, buf_len, "%d", get_current_pattern(&g_tracks[track])->steps[step].num_notes);
                            }
                            else if (strcmp(step_param, "vel") == 0) {
                                return snprintf(buf, buf_len, "%d", get_current_pattern(&g_tracks[track])->steps[step].velocity);
                            }
                            else if (strcmp(step_param, "gate") == 0) {
                                return snprintf(buf, buf_len, "%d", get_current_pattern(&g_tracks[track])->steps[step].gate);
                            }
                        }
                    }
                }
            }
        }
    }
    /* Legacy: current_step returns track 0 */
    else if (strcmp(key, "current_step") == 0) {
        return snprintf(buf, buf_len, "%d", g_tracks[0].current_step);
    }

    return -1;
}

static void plugin_render_block(int16_t *out_interleaved_lr, int frames) {
    /* Safety check */
    if (!out_interleaved_lr || frames <= 0) {
        return;
    }

    /* Output silence - sequencer doesn't generate audio */
    memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));

    if (!g_playing || !g_host) {
        return;
    }

    /* Phase increments (drift-free timing) */
    double step_inc = (double)(g_bpm * 4) / (double)(MOVE_SAMPLE_RATE * 60);
    double clock_inc = (double)(g_bpm * 24) / (double)(MOVE_SAMPLE_RATE * 60);

    for (int i = 0; i < frames; i++) {
        g_clock_phase += clock_inc;
        g_global_phase += step_inc;

        /* Send MIDI clock at 24 PPQN */
        if (g_send_clock && g_clock_phase >= 1.0) {
            g_clock_phase -= 1.0;
            send_midi_clock();
        }

        /* Process scheduled notes - handles note-on/off timing for ALL tracks */
        process_scheduled_notes();

        /* Process each track - advance steps and schedule notes */
        for (int t = 0; t < NUM_TRACKS; t++) {
            track_t *track = &g_tracks[t];

            /* Per-track phase increment */
            double track_step_inc = step_inc * track->speed;
            track->phase += track_step_inc;

            /* Check step advance (fixed 1.0 step duration - swing is in note delay) */
            if (track->phase >= track->next_step_at) {
                track->phase -= track->next_step_at;
                advance_track(track, t);
            }
        }
    }
}

/* ============ Plugin Entry Point ============ */

plugin_api_v1_t* move_plugin_init_v1(const host_api_v1_t *host) {
    g_host = host;

    /* Verify API version */
    if (host->api_version != MOVE_PLUGIN_API_VERSION) {
        char msg[128];
        snprintf(msg, sizeof(msg), "API version mismatch: host=%d, plugin=%d",
                 host->api_version, MOVE_PLUGIN_API_VERSION);
        if (host->log) host->log(msg);
        return NULL;
    }

    /* Initialize plugin API struct */
    memset(&g_plugin_api, 0, sizeof(g_plugin_api));
    g_plugin_api.api_version = MOVE_PLUGIN_API_VERSION;
    g_plugin_api.on_load = plugin_on_load;
    g_plugin_api.on_unload = plugin_on_unload;
    g_plugin_api.on_midi = plugin_on_midi;
    g_plugin_api.set_param = plugin_set_param;
    g_plugin_api.get_param = plugin_get_param;
    g_plugin_api.render_block = plugin_render_block;

    plugin_log("SEQOMD initialized");

    return &g_plugin_api;
}
