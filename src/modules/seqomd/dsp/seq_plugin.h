/*
 * SEQOMD DSP Plugin - Header
 *
 * 8-track sequencer with per-track timing, MIDI output, and master clock.
 * Inspired by OP-Z architecture.
 */

#ifndef SEQ_PLUGIN_H
#define SEQ_PLUGIN_H

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
extern const double ARP_STEP_RATES[];
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
    uint8_t notes[MAX_NOTES_PER_STEP];       /* Up to 7 notes per step (0 = empty slot) */
    uint8_t velocities[MAX_NOTES_PER_STEP];  /* Per-note velocity (1-127), parallel to notes */
    uint8_t num_notes;                       /* Number of active notes */
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

/* Centralized Note Scheduler */
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

/* ============ Global State (extern declarations) ============ */

extern const host_api_v1_t *g_host;

/* Tracks */
extern track_t g_tracks[NUM_TRACKS];

/* Centralized note scheduler */
extern scheduled_note_t g_scheduled_notes[MAX_SCHEDULED_NOTES];

/* Global playback state */
extern int g_bpm;
extern int g_playing;
extern int g_send_clock;
extern double g_clock_phase;
extern double g_global_phase;  /* Master clock for all timing */

/* Transpose/chord follow state */
extern int g_chord_follow[NUM_TRACKS];
extern int g_current_transpose;  /* Current transpose offset in semitones (legacy) */
extern int g_live_transpose;     /* Live transpose offset (-24 to +24) */
extern uint32_t g_beat_count;    /* Global beat counter for UI sync */

/* Transpose sequence state */
extern transpose_step_t g_transpose_sequence[MAX_TRANSPOSE_STEPS];
extern int g_transpose_step_count;
extern uint32_t g_transpose_total_steps;
extern int8_t *g_transpose_lookup;
extern uint32_t g_transpose_lookup_size;
extern int g_transpose_lookup_valid;
extern int g_transpose_sequence_enabled;
extern uint32_t g_transpose_step_iteration[MAX_TRANSPOSE_STEPS];
extern int g_transpose_virtual_step;
extern uint32_t g_transpose_virtual_entry_step;
extern int g_transpose_first_call;

/* Scale detection state */
extern int8_t g_detected_scale_root;
extern int8_t g_detected_scale_index;
extern int g_scale_dirty;

/* Scale templates */
extern const scale_template_t g_scale_templates[NUM_SCALE_TEMPLATES];

/* Random state */
extern uint32_t g_random_state;

/* ============ Function Prototypes ============ */

/* helpers.c / seq_plugin.c */
uint32_t random_next(void);
int random_check(int percent);
void plugin_log(const char *msg);

/* midi.c */
void send_note_on(int note, int velocity, int channel);
void send_note_off(int note, int channel);
void send_cc(int cc, int value, int channel);
void send_midi_clock(void);
void send_midi_start(void);
void send_midi_stop(void);

/* scheduler.c */
double calculate_swing_delay(int swing, double global_phase);
int find_conflicting_note(uint8_t note, uint8_t channel);
int find_free_slot(void);
void schedule_note(uint8_t note, uint8_t velocity, uint8_t channel,
                   int swing, double on_phase, double length, int gate,
                   uint8_t track_idx, int8_t sequence_transpose);
void process_scheduled_notes(void);
void clear_scheduled_notes(void);
void cut_channel_notes(uint8_t channel);
void all_notes_off(void);

/* transpose.c */
void rebuild_transpose_lookup(void);
void update_transpose_virtual_playhead(uint32_t step);
int8_t get_transpose_at_step(uint32_t step);
int get_transpose_step_index(uint32_t step);
void clear_transpose_sequence(void);
int check_transpose_condition(int step_index, transpose_step_t *step);

/* scale.c */
int popcount16(uint16_t x);
uint16_t collect_pitch_classes(void);
int score_scale(uint16_t pitch_mask, int scale_idx, int root);
void detect_scale(void);

/* arpeggiator.c */
void sort_notes(uint8_t *notes, int count);
void shuffle_notes(uint8_t *notes, int count);
int generate_arp_pattern(uint8_t *notes, int num_notes, int arp_mode,
                         int arp_octave, uint8_t *out_pattern, int max_len);

/* track.c */
void init_pattern(pattern_t *pattern);
void init_track(track_t *track, int channel);
pattern_t* get_current_pattern(track_t *track);
int should_step_trigger(step_t *step, track_t *track);
int check_spark_condition(int8_t spark_n, int8_t spark_m, uint8_t spark_not, track_t *track);
void schedule_step_notes(track_t *track, int track_idx, step_t *step, double base_phase, int use_arp, int use_ratchet);
void trigger_track_step(track_t *track, int track_idx, double step_start_phase);
void advance_track(track_t *track, int track_idx);

/* params.c */
void set_step_param(int track_idx, int step_idx, const char *param, const char *val);
int get_step_param(int track_idx, int step_idx, const char *param, char *buf, int buf_len);
void set_track_param(int track_idx, const char *param, const char *val);
int get_track_param(int track_idx, const char *param, char *buf, int buf_len);
void set_transpose_param(const char *key, const char *val);
int get_transpose_param(const char *key, char *buf, int buf_len);

#endif /* SEQ_PLUGIN_H */
