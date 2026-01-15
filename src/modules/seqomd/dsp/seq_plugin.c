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

#define NUM_TRACKS 16
#define NUM_STEPS 16
#define NUM_PATTERNS 16
#define MAX_NOTES_PER_STEP 7
#define MAX_SCHEDULED_NOTES 512  /* Increased from 128 for complex patterns with many overlapping notes */

#define DEFAULT_VELOCITY 100
#define DEFAULT_GATE 50

/* Transpose sequence constants */
#define MAX_TRANSPOSE_STEPS 16
#define MAX_TRANSPOSE_TOTAL_STEPS 4096  /* 16 steps × max 256 steps each */

/* Scale detection constants */
#define NUM_SCALE_TEMPLATES 15

/* Arpeggiator constants */
#define ARP_OFF           0
#define ARP_UP            1
#define ARP_DOWN          2
#define ARP_UP_DOWN       3   /* Includes endpoints twice */
#define ARP_DOWN_UP       4   /* Includes endpoints twice */
#define ARP_UP_AND_DOWN   5   /* Excludes repeated endpoints */
#define ARP_DOWN_AND_UP   6   /* Excludes repeated endpoints */
#define ARP_RANDOM        7
#define ARP_CHORD         8   /* Repeated chord hits */
#define ARP_OUTSIDE_IN    9   /* High/low alternating inward */
#define ARP_INSIDE_OUT    10  /* Middle outward alternating */
#define ARP_CONVERGE      11  /* Low/high pairs moving in */
#define ARP_DIVERGE       12  /* Middle expanding out */
#define ARP_THUMB         13  /* Bass note pedal */
#define ARP_PINKY         14  /* Top note pedal */
#define NUM_ARP_MODES     15

/* Arp speed: steps per arp note (musical note values, 16 steps = 1 bar)
 * Index: 0=1/32, 1=1/24, 2=1/16, 3=1/12, 4=1/8, 5=1/6, 6=1/4, 7=1/3, 8=1/2, 9=1/1 */
static const double ARP_STEP_RATES[] = {
    0.5,      /* 1/32 - 32nd notes (2 per step) */
    2.0/3.0,  /* 1/24 - triplet 16ths */
    1.0,      /* 1/16 - 16th notes (1 per step) */
    4.0/3.0,  /* 1/12 - triplet 8ths */
    2.0,      /* 1/8  - 8th notes */
    8.0/3.0,  /* 1/6  - triplet quarters */
    4.0,      /* 1/4  - quarter notes */
    16.0/3.0, /* 1/3  - triplet halves */
    8.0,      /* 1/2  - half notes */
    16.0      /* 1/1  - whole note */
};
#define NUM_ARP_SPEEDS    10
#define DEFAULT_ARP_SPEED 2  /* 1/16 = 1 note per step */

/* Arp octave options */
#define ARP_OCT_NONE      0
#define ARP_OCT_UP1       1
#define ARP_OCT_UP2       2
#define ARP_OCT_DOWN1     3
#define ARP_OCT_DOWN2     4
#define ARP_OCT_BOTH1     5
#define ARP_OCT_BOTH2     6
#define NUM_ARP_OCTAVES   7

/* Arp layer modes - step-only (no track default) */
#define ARP_LAYER_LAYER   0  /* Arps play over each other (default) */
#define ARP_LAYER_CUT     1  /* New step kills previous arp notes */
#define ARP_LAYER_LEGATO  2  /* Legato mode - smooth transition (not yet implemented, behaves like Layer) */
#define NUM_ARP_LAYERS    3

/* Max arp pattern length (4 notes * 3 octaves * 2 for ping-pong) */
#define MAX_ARP_PATTERN   64

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
    uint8_t notes[MAX_NOTES_PER_STEP];  /* Up to 7 notes per step (0 = empty slot) */
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
    /* Arpeggiator per-step overrides */
    int8_t arp_mode;                     /* -1=use track, 0+=override mode */
    int8_t arp_speed;                    /* -1=use track, 0+=override speed */
    uint8_t arp_layer;                   /* 0=Layer, 1=Cut, 2=Legato */
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
    /* Arpeggiator settings */
    uint8_t arp_mode;            /* 0=Off, 1=Up, 2=Down, etc. */
    uint8_t arp_speed;           /* 0=1/1, 1=1/2, 2=1/3, etc. (default 3=1/4) */
    uint8_t arp_octave;          /* 0=none, 1=+1, 2=+2, 3=-1, 4=-2, 5=±1, 6=±2 */
    /* Preview note velocity (for live pad audition) */
    uint8_t preview_velocity;    /* Velocity for next preview note (1-127) */
} track_t;

/* ============ Centralized Note Scheduler ============ */
/* All notes go through this scheduler which:
 * 1. Applies swing based on global beat position
 * 2. Handles note conflicts (same note+channel)
 * 3. Manages note-on and note-off timing
 */
typedef struct {
    uint8_t note;           /* Original untransposed note */
    uint8_t channel;
    uint8_t velocity;
    double on_phase;        /* Global phase when note-on should fire */
    double off_phase;       /* Global phase when note-off should fire */
    uint8_t on_sent;        /* Has note-on been sent? */
    uint8_t off_sent;       /* Has note-off been sent? */
    uint8_t active;         /* Is this slot in use? */
    uint8_t track_idx;      /* Track index for chord_follow lookup */
    int8_t sequence_transpose;  /* Sequence transpose value at schedule time */
    uint8_t sent_note;      /* Actual note sent (for note-off matching) */
} scheduled_note_t;

/* Transpose step - one entry in the transpose sequence */
typedef struct {
    int8_t transpose;       /* -24 to +24 semitones */
    uint16_t duration;      /* Duration in steps (1-256) */
    int8_t jump;            /* Jump target (-1 = no jump, 0-15 = target step) */
    int8_t condition_n;     /* 0=always, >0=every N loops */
    int8_t condition_m;     /* Which iteration (1 to N) */
    uint8_t condition_not;  /* Negate condition */
} transpose_step_t;

/* Scale template for scale detection */
typedef struct {
    const char *name;
    uint8_t notes[8];       /* Pitch classes, terminated by 255 */
    uint8_t note_count;
} scale_template_t;

/* Scale templates - ordered by preference (simpler scales first) */
static const scale_template_t g_scale_templates[NUM_SCALE_TEMPLATES] = {
    { "Minor Penta",    {0, 3, 5, 7, 10, 255, 255, 255}, 5 },
    { "Major Penta",    {0, 2, 4, 7, 9, 255, 255, 255}, 5 },
    { "Blues",          {0, 3, 5, 6, 7, 10, 255, 255}, 6 },
    { "Whole Tone",     {0, 2, 4, 6, 8, 10, 255, 255}, 6 },
    { "Major",          {0, 2, 4, 5, 7, 9, 11, 255}, 7 },
    { "Natural Minor",  {0, 2, 3, 5, 7, 8, 10, 255}, 7 },
    { "Dorian",         {0, 2, 3, 5, 7, 9, 10, 255}, 7 },
    { "Mixolydian",     {0, 2, 4, 5, 7, 9, 10, 255}, 7 },
    { "Phrygian",       {0, 1, 3, 5, 7, 8, 10, 255}, 7 },
    { "Lydian",         {0, 2, 4, 6, 7, 9, 11, 255}, 7 },
    { "Locrian",        {0, 1, 3, 5, 6, 8, 10, 255}, 7 },
    { "Harmonic Minor", {0, 2, 3, 5, 7, 8, 11, 255}, 7 },
    { "Melodic Minor",  {0, 2, 3, 5, 7, 9, 11, 255}, 7 },
    { "Diminished HW",  {0, 1, 3, 4, 6, 7, 9, 10}, 8 },
    { "Diminished WH",  {0, 2, 3, 5, 6, 8, 9, 11}, 8 }
};

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

/* Transpose/chord follow state */
static int g_chord_follow[NUM_TRACKS] = {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1};  /* Tracks 5-8, 13-16 follow by default */
static int g_current_transpose = 0;  /* Current transpose offset in semitones (legacy, kept for compatibility) */
static int g_live_transpose = 0;     /* Live transpose offset (-24 to +24) applied on top of sequence */
static uint32_t g_beat_count = 0;    /* Global beat counter for UI sync */

/* Transpose sequence - managed internally by DSP */
static transpose_step_t g_transpose_sequence[MAX_TRANSPOSE_STEPS];
static int g_transpose_step_count = 0;          /* Number of active steps */
static uint32_t g_transpose_total_steps = 0;    /* Sum of all durations */
static int8_t *g_transpose_lookup = NULL;       /* Pre-computed lookup table (dynamically allocated) */
static uint32_t g_transpose_lookup_size = 0;    /* Size of lookup table */
static int g_transpose_lookup_valid = 0;        /* Is lookup table valid? */
static int g_transpose_sequence_enabled = 1;    /* Enable/disable transpose sequence automation */
static uint32_t g_transpose_step_iteration[MAX_TRANSPOSE_STEPS];  /* Per-step iteration counter for conditions */
static int g_transpose_virtual_step = 0;        /* Virtual playhead for jumps (0 to step_count-1) */
static uint32_t g_transpose_virtual_entry_step = 0;  /* Beat position when we entered current virtual step */
static int g_transpose_first_call = 1;          /* First call flag for initialization */

/* Scale detection state */
static int8_t g_detected_scale_root = -1;       /* 0-11, or -1 if none */
static int8_t g_detected_scale_index = -1;      /* Index into g_scale_templates, or -1 */
static int g_scale_dirty = 1;                   /* Needs recalculation */

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

/* ============ Transpose Sequence Functions ============ */

/**
 * Rebuild the transpose lookup table from the sequence.
 * Called when transpose sequence is modified.
 */
static void rebuild_transpose_lookup(void) {
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

/* Forward declaration for condition checking */
static int check_transpose_condition(int step_index, transpose_step_t *step);

/**
 * Update the transpose virtual playhead (called every frame).
 * This ensures jumps execute even when no notes are triggering.
 */
static void update_transpose_virtual_playhead(uint32_t step) {
    /* If transpose sequence is disabled or empty, nothing to do */
    if (!g_transpose_sequence_enabled) {
        static int logged_disabled = 0;
        if (!logged_disabled) {
            printf("[TRANSPOSE] Sequence DISABLED\n");
            logged_disabled = 1;
        }
        return;
    }
    if (g_transpose_step_count == 0 || g_transpose_total_steps == 0) {
        static int logged_empty = 0;
        if (!logged_empty) {
            printf("[TRANSPOSE] Empty sequence: step_count=%d, total_steps=%u\n",
                   g_transpose_step_count, g_transpose_total_steps);
            logged_empty = 1;
        }
        return;
    }

    /* Initialize on first call - calculate which virtual step we should be at */
    if (g_transpose_first_call) {
        printf("[TRANSPOSE] INIT at global_step=%u, step_count=%d, total_steps=%u\n",
               step, g_transpose_step_count, g_transpose_total_steps);
        for (int i = 0; i < g_transpose_step_count; i++) {
            printf("[TRANSPOSE] Step %d: transpose=%d, duration=%u, jump=%d, cond_n=%d, cond_m=%d, cond_not=%d\n",
                   i, g_transpose_sequence[i].transpose, g_transpose_sequence[i].duration,
                   g_transpose_sequence[i].jump, g_transpose_sequence[i].condition_n,
                   g_transpose_sequence[i].condition_m, g_transpose_sequence[i].condition_not);
        }

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
                printf("[TRANSPOSE] Starting at virtual_step=%d, entry_step=%u (looped_step=%u, step_offset=%u)\n",
                       g_transpose_virtual_step, g_transpose_virtual_entry_step, looped_step, looped_step - accumulated);
                break;
            }
            accumulated = next_accumulated;
        }

        g_transpose_first_call = 0;
        return;
    }

    /* Get current virtual step and its duration */
    transpose_step_t *current_virtual = &g_transpose_sequence[g_transpose_virtual_step];
    uint32_t duration_in_steps = current_virtual->duration;  /* Already in steps (JS converts beats*4) */

    /* Check if we've been in this virtual step long enough to advance */
    uint32_t steps_in_current = step - g_transpose_virtual_entry_step;

    if (steps_in_current >= duration_in_steps) {
        printf("[TRANSPOSE] Step %u: virtual_step=%d finished (duration=%u)\n",
               step, g_transpose_virtual_step, duration_in_steps);

        /* Step finished playing - check for jump BEFORE advancing */
        if (current_virtual->jump >= 0 && current_virtual->jump < g_transpose_step_count) {
            printf("[TRANSPOSE] Checking jump: jump=%d, step_count=%d, cond_n=%d\n",
                   current_virtual->jump, g_transpose_step_count, current_virtual->condition_n);

            if (check_transpose_condition(g_transpose_virtual_step, current_virtual)) {
                printf("[TRANSPOSE] JUMP EXECUTED: %d -> %d\n",
                       g_transpose_virtual_step, current_virtual->jump);

                /* Increment this step's iteration counter - we've evaluated this condition */
                g_transpose_step_iteration[g_transpose_virtual_step]++;
                printf("[TRANSPOSE] Step %d iteration count incremented to %u\n",
                       g_transpose_virtual_step, g_transpose_step_iteration[g_transpose_virtual_step]);

                /* Jump: go to target step */
                g_transpose_virtual_step = current_virtual->jump;
                g_transpose_virtual_entry_step = step;
                return;
            } else {
                printf("[TRANSPOSE] Jump condition FAILED\n");

                /* Still increment - we evaluated the condition */
                g_transpose_step_iteration[g_transpose_virtual_step]++;
                printf("[TRANSPOSE] Step %d iteration count incremented to %u\n",
                       g_transpose_virtual_step, g_transpose_step_iteration[g_transpose_virtual_step]);
            }
        } else {
            printf("[TRANSPOSE] No jump: jump=%d, step_count=%d\n",
                   current_virtual->jump, g_transpose_step_count);
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
static int8_t get_transpose_at_step(uint32_t step) {
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
static int get_transpose_step_index(uint32_t step) {
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
static void clear_transpose_sequence(void) {
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

/**
 * Check if a transpose step's condition passes based on its iteration count.
 * Returns 1 if condition passes, 0 otherwise.
 */
static int check_transpose_condition(int step_index, transpose_step_t *step) {
    if (step->condition_n <= 0) {
        printf("[CONDITION] Step %d: No condition set (n=%d), always passes\n", step_index, step->condition_n);
        return 1;  /* No condition (n=0) always passes */
    }

    /* Calculate which iteration of the cycle we're in (1-indexed) */
    uint32_t step_iter = g_transpose_step_iteration[step_index];
    int iteration = (step_iter % step->condition_n) + 1;
    int should_apply = (iteration == step->condition_m);

    printf("[CONDITION] Step %d: iteration_count=%u, n=%d, m=%d, iteration=%d, match=%d, NOT=%d\n",
           step_index, step_iter, step->condition_n, step->condition_m,
           iteration, should_apply, step->condition_not);

    /* Apply NOT flag if set */
    if (step->condition_not) {
        should_apply = !should_apply;
    }

    printf("[CONDITION] Step %d final result: %s\n", step_index, should_apply ? "PASS" : "FAIL");
    return should_apply;
}

/* ============ Scale Detection Functions ============ */

/**
 * Count set bits in a 16-bit value (popcount).
 */
static int popcount16(uint16_t x) {
    int count = 0;
    while (x) {
        count += x & 1;
        x >>= 1;
    }
    return count;
}

/**
 * Collect all pitch classes from chord-follow tracks.
 * Returns a 12-bit mask where bit N = pitch class N is present.
 * Scans ALL patterns (not just current) to match JS behavior.
 */
static uint16_t collect_pitch_classes(void) {
    uint16_t mask = 0;

    for (int t = 0; t < NUM_TRACKS; t++) {
        if (!g_chord_follow[t]) continue;

        /* Scan all patterns for this track */
        for (int p = 0; p < NUM_PATTERNS; p++) {
            pattern_t *pattern = &g_tracks[t].patterns[p];
            for (int s = 0; s < NUM_STEPS; s++) {
                step_t *step = &pattern->steps[s];
                for (int n = 0; n < step->num_notes; n++) {
                    if (step->notes[n] > 0) {
                        int pitch_class = step->notes[n] % 12;
                        mask |= (1 << pitch_class);
                    }
                }
            }
        }
    }

    return mask;
}

/**
 * Score how well pitch classes fit a scale template at a given root.
 * Returns score * 1000 for integer comparison (higher = better).
 */
static int score_scale(uint16_t pitch_mask, int scale_idx, int root) {
    if (pitch_mask == 0) return 0;

    /* Build scale mask for this root */
    uint16_t scale_mask = 0;
    for (int i = 0; i < g_scale_templates[scale_idx].note_count; i++) {
        int pc = (g_scale_templates[scale_idx].notes[i] + root) % 12;
        scale_mask |= (1 << pc);
    }

    /* Count notes in scale */
    int in_scale = popcount16(pitch_mask & scale_mask);
    int total = popcount16(pitch_mask);

    if (total == 0) return 0;

    /* Score: fit ratio * 1000 + small bonus for simpler scales */
    int fit_score = (in_scale * 1000) / total;
    int size_bonus = 100 / g_scale_templates[scale_idx].note_count;

    return fit_score + size_bonus;
}

/**
 * Detect the best-fitting scale from chord-follow track notes.
 * Updates g_detected_scale_root and g_detected_scale_index.
 */
static void detect_scale(void) {
    uint16_t pitch_mask = collect_pitch_classes();

    if (pitch_mask == 0) {
        g_detected_scale_root = -1;
        g_detected_scale_index = -1;
        g_scale_dirty = 0;
        return;
    }

    int best_score = -1;
    int best_root = 0;
    int best_scale = 0;

    for (int root = 0; root < 12; root++) {
        for (int scale = 0; scale < NUM_SCALE_TEMPLATES; scale++) {
            int score = score_scale(pitch_mask, scale, root);
            if (score > best_score) {
                best_score = score;
                best_root = root;
                best_scale = scale;
            }
        }
    }

    g_detected_scale_root = best_root;
    g_detected_scale_index = best_scale;
    g_scale_dirty = 0;
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
static void schedule_note(uint8_t note, uint8_t velocity, uint8_t channel,
                          int swing, double on_phase, double length, int gate,
                          uint8_t track_idx, int8_t sequence_transpose) {
    /* Apply swing delay based on global phase */
    double swing_delay = calculate_swing_delay(swing, on_phase);
    double swung_on_phase = on_phase + swing_delay;

    /* Calculate note-off time: length adjusted by gate percentage */
    double gate_mult = gate / 100.0;
    double note_duration = length * gate_mult;
    double off_phase = swung_on_phase + note_duration;

    /* Debug: log scheduling for testing */
    if (note >= 60 && note <= 67) {
        printf("[SCHEDULE] Note %d on_phase=%.2f off_phase=%.2f (global_phase=%.2f)\n",
               note, swung_on_phase, off_phase, g_global_phase);
    }

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
 * Called every sample from render_block.
 * Transpose is applied at send time to support immediate live transpose.
 */
static void process_scheduled_notes(void) {
    for (int i = 0; i < MAX_SCHEDULED_NOTES; i++) {
        scheduled_note_t *sn = &g_scheduled_notes[i];
        if (!sn->active) continue;

        /* Send note-on at the scheduled time */
        if (!sn->on_sent && g_global_phase >= sn->on_phase) {
            /* Apply transpose at send time (not schedule time) */
            int final_note = sn->note;
            if (g_chord_follow[sn->track_idx]) {
                /* Live transpose takes precedence over sequence transpose */
                int transpose = (g_live_transpose != 0)
                    ? g_live_transpose
                    : sn->sequence_transpose;
                final_note = sn->note + transpose;
                if (final_note < 0) final_note = 0;
                if (final_note > 127) final_note = 127;
            }

            if (sn->note >= 60 && sn->note <= 67) {
                printf("[SEND] Note %d (orig=%d) at phase=%.2f (on_phase=%.2f)\n",
                       final_note, sn->note, g_global_phase, sn->on_phase);
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
static void clear_scheduled_notes(void) {
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
static void cut_channel_notes(uint8_t channel) {
    int cancelled_count = 0;
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
            cancelled_count++;
        }
    }
    if (cancelled_count > 0) {
        printf("[CUT] Cancelled %d notes on channel %d at phase=%.2f\n",
               cancelled_count, channel, g_global_phase);
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
        /* Arp per-step overrides */
        pattern->steps[i].arp_mode = -1;        /* Use track default */
        pattern->steps[i].arp_speed = -1;       /* Use track default */
        pattern->steps[i].arp_layer = ARP_LAYER_LAYER;  /* Default to layer */
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
    /* Arpeggiator defaults */
    track->arp_mode = ARP_OFF;
    track->arp_speed = DEFAULT_ARP_SPEED;
    track->arp_octave = ARP_OCT_NONE;
    track->preview_velocity = DEFAULT_VELOCITY;

    for (int i = 0; i < MAX_NOTES_PER_STEP; i++) {
        track->last_notes[i] = -1;
    }

    /* Initialize all patterns */
    for (int p = 0; p < NUM_PATTERNS; p++) {
        init_pattern(&track->patterns[p]);
    }
}

/* ============ Arpeggiator Pattern Generation ============ */

/* Helper: sort notes by pitch (insertion sort, small arrays) */
static void sort_notes(uint8_t *notes, int count) {
    for (int i = 1; i < count; i++) {
        uint8_t key = notes[i];
        int j = i - 1;
        while (j >= 0 && notes[j] > key) {
            notes[j + 1] = notes[j];
            j--;
        }
        notes[j + 1] = key;
    }
}

/* Helper: shuffle array (Fisher-Yates) */
static void shuffle_notes(uint8_t *notes, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = random_check(100) * i / 100;  /* Simple random index */
        if (j > i) j = i;
        uint8_t tmp = notes[i];
        notes[i] = notes[j];
        notes[j] = tmp;
    }
}

/**
 * Generate arp pattern for given notes.
 * Notes are sorted by pitch, then arranged according to arp_mode.
 * Octave extension is applied if arp_octave > 0.
 * Returns pattern length.
 */
static int generate_arp_pattern(uint8_t *notes, int num_notes, int arp_mode,
                                 int arp_octave, uint8_t *out_pattern, int max_len) {
    if (num_notes <= 0 || num_notes > MAX_NOTES_PER_STEP) return 0;

    /* Copy and sort notes by pitch */
    uint8_t sorted[MAX_NOTES_PER_STEP];
    for (int i = 0; i < num_notes; i++) {
        sorted[i] = notes[i];
    }
    sort_notes(sorted, num_notes);

    int len = 0;

    /* Generate base pattern based on mode */
    switch (arp_mode) {
        case ARP_UP:
            for (int i = 0; i < num_notes && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_DOWN:
            for (int i = num_notes - 1; i >= 0 && len < max_len; i--) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_UP_DOWN:
            /* Up then down, includes endpoints twice: C-E-G-E */
            for (int i = 0; i < num_notes && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            for (int i = num_notes - 2; i > 0 && len < max_len; i--) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_DOWN_UP:
            /* Down then up, includes endpoints twice: G-E-C-E */
            for (int i = num_notes - 1; i >= 0 && len < max_len; i--) {
                out_pattern[len++] = sorted[i];
            }
            for (int i = 1; i < num_notes - 1 && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_UP_AND_DOWN:
            /* Up then down, no repeated endpoints: C-E-G-G-E-C */
            for (int i = 0; i < num_notes && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            for (int i = num_notes - 1; i >= 0 && len < max_len; i--) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_DOWN_AND_UP:
            /* Down then up, no repeated endpoints: G-E-C-C-E-G */
            for (int i = num_notes - 1; i >= 0 && len < max_len; i--) {
                out_pattern[len++] = sorted[i];
            }
            for (int i = 0; i < num_notes && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_RANDOM:
            /* Shuffle the sorted notes */
            for (int i = 0; i < num_notes; i++) {
                out_pattern[i] = sorted[i];
            }
            shuffle_notes(out_pattern, num_notes);
            len = num_notes;
            break;

        case ARP_CHORD:
            /* All notes at once - just return first note, caller handles chord */
            /* For scheduling purposes, we treat this as playing all notes at each position */
            for (int i = 0; i < num_notes && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_OUTSIDE_IN:
            /* High/low alternating inward: G-C-E (for C-E-G) */
            for (int i = 0; i < (num_notes + 1) / 2 && len < max_len; i++) {
                if (len < max_len) out_pattern[len++] = sorted[num_notes - 1 - i];
                if (i != num_notes - 1 - i && len < max_len) {
                    out_pattern[len++] = sorted[i];
                }
            }
            break;

        case ARP_INSIDE_OUT:
            /* Middle outward alternating */
            {
                int mid = num_notes / 2;
                if (len < max_len) out_pattern[len++] = sorted[mid];
                for (int i = 1; i <= mid && len < max_len; i++) {
                    if (mid + i < num_notes && len < max_len) {
                        out_pattern[len++] = sorted[mid + i];
                    }
                    if (mid - i >= 0 && len < max_len) {
                        out_pattern[len++] = sorted[mid - i];
                    }
                }
            }
            break;

        case ARP_CONVERGE:
            /* Low/high pairs moving in: C-G-E (for C-E-G) */
            for (int i = 0; i < (num_notes + 1) / 2 && len < max_len; i++) {
                if (len < max_len) out_pattern[len++] = sorted[i];
                if (i != num_notes - 1 - i && len < max_len) {
                    out_pattern[len++] = sorted[num_notes - 1 - i];
                }
            }
            break;

        case ARP_DIVERGE:
            /* Middle expanding out (same as inside_out for notes) */
            {
                int mid = num_notes / 2;
                if (len < max_len) out_pattern[len++] = sorted[mid];
                for (int i = 1; i <= mid && len < max_len; i++) {
                    if (mid + i < num_notes && len < max_len) {
                        out_pattern[len++] = sorted[mid + i];
                    }
                    if (mid - i >= 0 && len < max_len) {
                        out_pattern[len++] = sorted[mid - i];
                    }
                }
            }
            break;

        case ARP_THUMB:
            /* Bass note pedal: C-C-E-C-G */
            if (len < max_len) out_pattern[len++] = sorted[0];
            for (int i = 1; i < num_notes && len < max_len; i++) {
                if (len < max_len) out_pattern[len++] = sorted[0];
                if (len < max_len) out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_PINKY:
            /* Top note pedal: G-G-E-G-C */
            if (len < max_len) out_pattern[len++] = sorted[num_notes - 1];
            for (int i = num_notes - 2; i >= 0 && len < max_len; i--) {
                if (len < max_len) out_pattern[len++] = sorted[num_notes - 1];
                if (len < max_len) out_pattern[len++] = sorted[i];
            }
            break;

        default:
            /* Default to up */
            for (int i = 0; i < num_notes && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            break;
    }

    /* Apply octave extension */
    if (arp_octave != ARP_OCT_NONE && len > 0) {
        int base_len = len;
        uint8_t base_pattern[MAX_ARP_PATTERN];
        for (int i = 0; i < base_len; i++) {
            base_pattern[i] = out_pattern[i];
        }

        len = 0;  /* Reset and rebuild with octaves */

        switch (arp_octave) {
            case ARP_OCT_UP1:
                /* Base, then +12 */
                for (int i = 0; i < base_len && len < max_len; i++) {
                    out_pattern[len++] = base_pattern[i];
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] + 12;
                    if (note <= 127) out_pattern[len++] = note;
                }
                break;

            case ARP_OCT_UP2:
                /* Base, +12, +24 */
                for (int i = 0; i < base_len && len < max_len; i++) {
                    out_pattern[len++] = base_pattern[i];
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] + 12;
                    if (note <= 127) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] + 24;
                    if (note <= 127) out_pattern[len++] = note;
                }
                break;

            case ARP_OCT_DOWN1:
                /* -12, then base */
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] - 12;
                    if (note >= 0) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    out_pattern[len++] = base_pattern[i];
                }
                break;

            case ARP_OCT_DOWN2:
                /* -24, -12, base */
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] - 24;
                    if (note >= 0) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] - 12;
                    if (note >= 0) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    out_pattern[len++] = base_pattern[i];
                }
                break;

            case ARP_OCT_BOTH1:
                /* -12, base, +12 */
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] - 12;
                    if (note >= 0) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    out_pattern[len++] = base_pattern[i];
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] + 12;
                    if (note <= 127) out_pattern[len++] = note;
                }
                break;

            case ARP_OCT_BOTH2:
                /* -24, -12, base, +12, +24 */
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] - 24;
                    if (note >= 0) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] - 12;
                    if (note >= 0) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    out_pattern[len++] = base_pattern[i];
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] + 12;
                    if (note <= 127) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] + 24;
                    if (note <= 127) out_pattern[len++] = note;
                }
                break;
        }
    }

    return len;
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
 * This handles swing, ratchets, arp, note conflicts, and transpose automatically.
 *
 * @param track       Track data
 * @param track_idx   Track index (for chord_follow check)
 * @param step        Step data
 * @param base_phase  Global phase when this step starts
 * @param use_arp     Whether to use arp scheduling (1=use arp, 0=don't)
 * @param use_ratchet Whether to use step's ratchet value (1=use ratchet, 0=force single trigger)
 */
static void schedule_step_notes(track_t *track, int track_idx, step_t *step, double base_phase, int use_arp, int use_ratchet) {
    int note_length = step->length > 0 ? step->length : 1;
    int gate = step->gate > 0 ? step->gate : DEFAULT_GATE;

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

        /* Generate arp pattern */
        uint8_t arp_pattern[MAX_ARP_PATTERN];
        int pattern_len = generate_arp_pattern(step->notes, step->num_notes,
                                                arp_mode, arp_octave, arp_pattern, MAX_ARP_PATTERN);

        if (pattern_len == 0) return;

        /* Calculate arp timing using musical note values
         * ARP_STEP_RATES[speed] = steps per arp note
         * e.g., 1/32 = 0.5 (2 notes per step), 1/4 = 4.0 (1 note per 4 steps) */
        double steps_per_note = ARP_STEP_RATES[arp_speed];
        int total_arp_notes = (int)(note_length / steps_per_note + 0.5);
        if (total_arp_notes < 1) total_arp_notes = 1;
        double note_duration = (double)note_length / total_arp_notes;

        /* Handle ARP_CHORD mode - all notes together at each arp position */
        if (arp_mode == ARP_CHORD) {
            for (int i = 0; i < total_arp_notes; i++) {
                double note_phase = base_phase + (i * note_duration);

                /* Play all notes as chord */
                for (int n = 0; n < step->num_notes && n < MAX_NOTES_PER_STEP; n++) {
                    if (step->notes[n] > 0) {
                        schedule_note(
                            step->notes[n],  /* Original note - transpose applied at send time */
                            step->velocity,
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
        } else {
            /* Normal arp: cycle through pattern */
            for (int i = 0; i < total_arp_notes; i++) {
                double note_phase = base_phase + (i * note_duration);
                int pattern_idx = i % pattern_len;
                int note_value = arp_pattern[pattern_idx];

                schedule_note(
                    note_value,  /* Original note from arp pattern - transpose applied at send time */
                    step->velocity,
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

        /* For ratchets, divide the NOTE LENGTH into equal parts (not just one step) */
        double ratchet_step = (double)note_length / ratchet_count;
        /* Each ratchet note gets equal length */
        double ratchet_length = (double)note_length / ratchet_count;

        for (int r = 0; r < ratchet_count; r++) {
            double note_on_phase = base_phase + (r * ratchet_step);

            /* Calculate velocity for this ratchet based on mode */
            uint8_t ratchet_velocity = step->velocity;

            if (ratchet_mode == 1) {
                /* Ramp Up: velocity increases from low to target */
                /* First note starts at velocity/count, last note is full velocity */
                ratchet_velocity = (uint8_t)(((r + 1) * step->velocity) / ratchet_count);
                if (ratchet_velocity < 1) ratchet_velocity = 1;
            } else if (ratchet_mode == 2) {
                /* Ramp Down: velocity decreases from target to low */
                /* First note is full velocity, last note is velocity/count */
                ratchet_velocity = (uint8_t)(((ratchet_count - r) * step->velocity) / ratchet_count);
                if (ratchet_velocity < 1) ratchet_velocity = 1;
            }

            /* Schedule each note in the step */
            for (int n = 0; n < step->num_notes && n < MAX_NOTES_PER_STEP; n++) {
                if (step->notes[n] > 0) {
                    schedule_note(
                        step->notes[n],  /* Original note - transpose applied at send time */
                        ratchet_velocity,
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

/* ============ Parameter Sub-handlers ============ */

/**
 * Handle step-level parameter setting.
 * Called for params like: track_T_step_S_note, track_T_step_S_vel, etc.
 */
static void set_step_param(int track_idx, int step_idx, const char *param, const char *val) {
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
    /* Add a note to the step (for chords) */
    else if (strcmp(param, "add_note") == 0) {
        int note = atoi(val);
        if (note >= 1 && note <= 127) {
            int exists = 0;
            for (int n = 0; n < s->num_notes; n++) {
                if (s->notes[n] == note) {
                    exists = 1;
                    break;
                }
            }
            if (!exists && s->num_notes < MAX_NOTES_PER_STEP) {
                s->notes[s->num_notes] = note;
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
                    }
                    s->notes[s->num_notes - 1] = 0;
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
    else if (strcmp(param, "vel") == 0) {
        int vel = atoi(val);
        if (vel >= 1 && vel <= 127) {
            s->velocity = vel;
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
    else if (strcmp(param, "velocity") == 0) {
        int vel = atoi(val);
        if (vel >= 1 && vel <= 127) {
            s->velocity = vel;
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
static int get_step_param(int track_idx, int step_idx, const char *param, char *buf, int buf_len) {
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
        return snprintf(buf, buf_len, "%d", s->velocity);
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
static void set_track_param(int track_idx, const char *param, const char *val) {
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
static int get_track_param(int track_idx, const char *param, char *buf, int buf_len) {
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
static void set_transpose_param(const char *key, const char *val) {
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
static int get_transpose_param(const char *key, char *buf, int buf_len) {
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

    /* Free transpose lookup table */
    if (g_transpose_lookup) {
        free(g_transpose_lookup);
        g_transpose_lookup = NULL;
        g_transpose_lookup_size = 0;
    }
}

static void plugin_on_midi(const uint8_t *msg, int len, int source) {
    /* Debug: Log CC button presses to identify buttons */
    if (len >= 3) {
        uint8_t status = msg[0];
        uint8_t data1 = msg[1];
        uint8_t data2 = msg[2];

        /* Log CC messages (0xB0-0xBF) when pressed (velocity > 0) */
        if ((status & 0xF0) == 0xB0 && data2 > 0) {
            printf("[DSP] CC Button: %d (0x%02X) vel=%d source=%d\n",
                   data1, data1, data2, source);
        }
    }

    /* Currently no other MIDI input handling - Move is master */
    (void)source;
}

static void plugin_set_param(const char *key, const char *val) {
    /* Global params */
    if (strcmp(key, "bpm") == 0) {
        int new_bpm = atoi(val);
        if (new_bpm >= 20 && new_bpm <= 300) {
            g_bpm = new_bpm;
            printf("[TEST] ===== BPM SET TO %d =====\n", new_bpm);
        }
    }
    else if (strcmp(key, "playing") == 0) {
        int new_playing = atoi(val);
        if (new_playing && !g_playing) {
            printf("[TEST] ===== PLAYBACK STARTING =====\n");
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
            g_beat_count = 0;
            g_random_state = 12345;
            /* Reset transpose virtual playhead and per-step iteration counters */
            g_transpose_virtual_step = 0;
            g_transpose_virtual_entry_step = 0;
            memset(g_transpose_step_iteration, 0, sizeof(g_transpose_step_iteration));
            g_transpose_first_call = 1;
            if (g_send_clock) {
                send_midi_start();
                send_midi_clock();
            }
            for (int t = 0; t < NUM_TRACKS; t++) {
                trigger_track_step(&g_tracks[t], t, 0.0);
            }
        } else if (!new_playing && g_playing) {
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
    else if (strcmp(key, "current_transpose") == 0) {
        g_current_transpose = atoi(val);
    }
    else if (strcmp(key, "live_transpose") == 0) {
        int val_int = atoi(val);
        /* Clamp to -24..+24 range */
        if (val_int < -24) val_int = -24;
        if (val_int > 24) val_int = 24;
        g_live_transpose = val_int;
    }
    /* Transpose sequence params */
    else if (strncmp(key, "transpose_", 10) == 0) {
        set_transpose_param(key, val);
    }
    /* Send CC externally: send_cc_CHANNEL_CC = VALUE */
    else if (strncmp(key, "send_cc_", 8) == 0) {
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
    /* Track params */
    else if (strncmp(key, "track_", 6) == 0) {
        int track = atoi(key + 6);
        if (track >= 0 && track < NUM_TRACKS) {
            const char *param = strchr(key + 6, '_');
            if (param) {
                set_track_param(track, param + 1, val);
            }
        }
    }
    /* Legacy single-track params for backward compatibility */
    else if (strncmp(key, "step_", 5) == 0) {
        int step = atoi(key + 5);
        if (step >= 0 && step < NUM_STEPS) {
            const char *param = strchr(key + 5, '_');
            if (param && strcmp(param + 1, "note") == 0) {
                int note = atoi(val);
                if (note >= 0 && note <= 127) {
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

static int plugin_get_param(const char *key, char *buf, int buf_len) {
    /* Global params */
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
    else if (strcmp(key, "beat_count") == 0) {
        return snprintf(buf, buf_len, "%u", g_beat_count);
    }
    /* Transpose params */
    else if (strcmp(key, "current_transpose") == 0 ||
             strcmp(key, "current_transpose_step") == 0 ||
             strcmp(key, "transpose_sequence_enabled") == 0 ||
             strcmp(key, "transpose_step_count") == 0 ||
             strcmp(key, "transpose_total_steps") == 0) {
        return get_transpose_param(key, buf, buf_len);
    }
    else if (strcmp(key, "live_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", g_live_transpose);
    }
    /* Scale detection params */
    else if (strcmp(key, "detected_scale_root") == 0) {
        if (g_scale_dirty) detect_scale();
        return snprintf(buf, buf_len, "%d", g_detected_scale_root);
    }
    else if (strcmp(key, "detected_scale_name") == 0) {
        if (g_scale_dirty) detect_scale();
        if (g_detected_scale_index >= 0 && g_detected_scale_index < NUM_SCALE_TEMPLATES) {
            return snprintf(buf, buf_len, "%s", g_scale_templates[g_detected_scale_index].name);
        }
        return snprintf(buf, buf_len, "None");
    }
    /* Track params */
    else if (strncmp(key, "track_", 6) == 0) {
        int track = atoi(key + 6);
        if (track >= 0 && track < NUM_TRACKS) {
            const char *param = strchr(key + 6, '_');
            if (param) {
                return get_track_param(track, param + 1, buf, buf_len);
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
    static int render_call_count = 0;

    /* Safety check */
    if (!out_interleaved_lr || frames <= 0) {
        return;
    }

    /* Output silence - sequencer doesn't generate audio */
    memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));

    if (!g_playing || !g_host) {
        return;
    }

    /* Log every 100 render calls when playing */
    render_call_count++;
    if (render_call_count % 100 == 0) {
        printf("[TEST] Render called %d times, g_playing=%d\n", render_call_count, g_playing);
    }

    /* Phase increments (drift-free timing) */
    double step_inc = (double)(g_bpm * 4) / (double)(MOVE_SAMPLE_RATE * 60);
    double clock_inc = (double)(g_bpm * 24) / (double)(MOVE_SAMPLE_RATE * 60);

    /* Track previous global phase for beat detection */
    double prev_global_phase = g_global_phase;

    for (int i = 0; i < frames; i++) {
        g_clock_phase += clock_inc;
        g_global_phase += step_inc;

        /* Track beat count (1 beat = 4 steps) for transpose sequence sync */
        /* Increment when we cross a 4-step boundary */
        uint32_t prev_beat = (uint32_t)(prev_global_phase / 4.0);
        uint32_t curr_beat = (uint32_t)(g_global_phase / 4.0);
        if (curr_beat > prev_beat) {
            g_beat_count = curr_beat;
        }

        /* Update transpose virtual playhead when we cross a step boundary */
        uint32_t prev_step = (uint32_t)prev_global_phase;
        uint32_t curr_step = (uint32_t)g_global_phase;
        if (curr_step > prev_step) {
            update_transpose_virtual_playhead(curr_step);
        }

        prev_global_phase = g_global_phase;

        /* Send MIDI clock at 24 PPQN */
        if (g_send_clock && g_clock_phase >= 1.0) {
            g_clock_phase -= 1.0;
            send_midi_clock();
        }

        /* Process each track FIRST - advance steps and schedule notes (including Cut)
         * IMPORTANT: This must happen before process_scheduled_notes() so that
         * Cut mode can cancel notes before they are sent */
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

        /* Process scheduled notes - handles note-on/off timing for ALL tracks
         * This happens AFTER track advancement so Cut mode can prevent notes from being sent */
        process_scheduled_notes();
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
