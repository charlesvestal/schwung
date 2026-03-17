#ifndef SHADOW_CHORD_H
#define SHADOW_CHORD_H

#include <stdint.h>

/* Chord types (matches shadow_control_t.chord_mode values) */
#define CHORD_OFF     0
#define CHORD_MAJOR   1
#define CHORD_MINOR   2
#define CHORD_DIM     3
#define CHORD_AUG     4
#define CHORD_SUS2    5
#define CHORD_SUS4    6
#define CHORD_DOM7    7
#define CHORD_MIN7    8
#define CHORD_MAJ7    9
#define CHORD_POWER  10
#define CHORD_OCTAVE 11

#define CHORD_MAX_HARMONY   5    /* Max extra notes per root */
#define CHORD_MAX_ACTIVE   16    /* Max simultaneous roots tracked */
#define CHORD_INJECT_PER_TICK 4  /* Max cable-2 injections per frame */
#define CHORD_DEFER_FRAMES  2    /* Min frames after cable-0 before injecting cable-2 */

/* A pending injection event */
typedef struct {
    uint8_t packet[4];  /* USB-MIDI: [CIN|cable, status, d1, d2] */
} chord_pending_t;

/* An active root note with its generated harmony notes */
typedef struct {
    uint8_t root_note;
    uint8_t channel;
    uint8_t harmony[CHORD_MAX_HARMONY];
    uint8_t harmony_count;
    uint8_t active;
} chord_active_t;

/* Chord engine state */
typedef struct {
    chord_active_t active_notes[CHORD_MAX_ACTIVE];
    chord_pending_t pending[32];
    int pending_count;
    int defer_counter;
} chord_engine_t;

/* Initialize chord engine */
void chord_engine_init(chord_engine_t *engine);

/* Process a note from MIDI_OUT cable-0. Queues harmony notes for injection.
 * chord_type: one of CHORD_* constants. Returns number of harmony notes queued. */
int chord_engine_on_note(chord_engine_t *engine, uint8_t status, uint8_t note,
                         uint8_t velocity, uint8_t channel, int chord_type);

/* Drain pending queue into MIDI_IN buffer (cable-2).
 * Only injects if defer_counter >= CHORD_DEFER_FRAMES.
 * midi_in: pointer to MIDI_IN region of shadow_mailbox.
 * Returns number of events injected. */
int chord_engine_drain(chord_engine_t *engine, uint8_t *midi_in);

/* Call each frame with whether cable-0 notes were present in MIDI_IN.
 * Resets defer_counter when cable-0 activity detected. */
void chord_engine_tick(chord_engine_t *engine, int cable0_notes_present);

#endif /* SHADOW_CHORD_H */
