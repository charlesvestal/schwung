#include "shadow_chord.h"
#include "shadow_constants.h"
#include <string.h>

void chord_engine_init(chord_engine_t *engine) {
    memset(engine, 0, sizeof(*engine));
    engine->defer_counter = CHORD_DEFER_FRAMES;
}

static int chord_get_intervals(int chord_type, int intervals[], int max) {
    (void)max;
    switch (chord_type) {
        case CHORD_MAJOR:   intervals[0]=4; intervals[1]=7; return 2;
        case CHORD_MINOR:   intervals[0]=3; intervals[1]=7; return 2;
        case CHORD_DIM:     intervals[0]=3; intervals[1]=6; return 2;
        case CHORD_AUG:     intervals[0]=4; intervals[1]=8; return 2;
        case CHORD_SUS2:    intervals[0]=2; intervals[1]=7; return 2;
        case CHORD_SUS4:    intervals[0]=5; intervals[1]=7; return 2;
        case CHORD_DOM7:    intervals[0]=4; intervals[1]=7; intervals[2]=10; return 3;
        case CHORD_MIN7:    intervals[0]=3; intervals[1]=7; intervals[2]=10; return 3;
        case CHORD_MAJ7:    intervals[0]=4; intervals[1]=7; intervals[2]=11; return 3;
        case CHORD_POWER:   intervals[0]=7; return 1;
        case CHORD_OCTAVE:  intervals[0]=12; return 1;
        default: return 0;
    }
}

static void queue_packet(chord_engine_t *engine, uint8_t cin, uint8_t status,
                         uint8_t d1, uint8_t d2) {
    if (engine->pending_count >= 32) return;
    chord_pending_t *p = &engine->pending[engine->pending_count++];
    p->packet[0] = (2 << 4) | cin;  /* cable 2 */
    p->packet[1] = status;
    p->packet[2] = d1;
    p->packet[3] = d2;
}

int chord_engine_on_note(chord_engine_t *engine, uint8_t status, uint8_t note,
                         uint8_t velocity, uint8_t channel, int chord_type) {
    uint8_t type = status & 0xF0;
    int is_note_on = (type == 0x90 && velocity > 0);
    int is_note_off = (type == 0x80 || (type == 0x90 && velocity == 0));

    if (is_note_on) {
        int slot = -1;
        for (int i = 0; i < CHORD_MAX_ACTIVE; i++) {
            if (!engine->active_notes[i].active) { slot = i; break; }
        }
        if (slot < 0) return 0;

        int intervals[CHORD_MAX_HARMONY];
        int count = chord_get_intervals(chord_type, intervals, CHORD_MAX_HARMONY);
        if (count == 0) return 0;

        chord_active_t *a = &engine->active_notes[slot];
        a->root_note = note;
        a->channel = channel;
        a->harmony_count = 0;
        a->active = 1;

        for (int i = 0; i < count; i++) {
            int harm_note = note + intervals[i];
            if (harm_note > 127) continue;
            a->harmony[a->harmony_count++] = (uint8_t)harm_note;
            queue_packet(engine, 0x09, 0x90 | channel, (uint8_t)harm_note, velocity);
        }
        return a->harmony_count;
    }

    if (is_note_off) {
        int released = 0;
        for (int i = 0; i < CHORD_MAX_ACTIVE; i++) {
            chord_active_t *a = &engine->active_notes[i];
            if (a->active && a->root_note == note && a->channel == channel) {
                for (int h = 0; h < a->harmony_count; h++) {
                    queue_packet(engine, 0x08, 0x80 | channel, a->harmony[h], 0);
                    released++;
                }
                a->active = 0;
                break;
            }
        }
        return released;
    }

    return 0;
}

void chord_engine_tick(chord_engine_t *engine, int cable0_notes_present) {
    if (cable0_notes_present) {
        engine->defer_counter = 0;
    } else if (engine->defer_counter < CHORD_DEFER_FRAMES + 1) {
        engine->defer_counter++;
    }
}

int chord_engine_drain(chord_engine_t *engine, uint8_t *midi_in) {
    if (engine->pending_count == 0) return 0;
    if (engine->defer_counter < CHORD_DEFER_FRAMES) return 0;

    int injected = 0;
    int hw_offset = 0;
    int src_idx = 0;

    while (src_idx < engine->pending_count && injected < CHORD_INJECT_PER_TICK) {
        while (hw_offset < MIDI_SPI_MAX_BYTES) {
            if (midi_in[hw_offset] == 0 && midi_in[hw_offset+1] == 0 &&
                midi_in[hw_offset+2] == 0 && midi_in[hw_offset+3] == 0) {
                break;
            }
            hw_offset += 4;
        }
        if (hw_offset >= MIDI_SPI_MAX_BYTES) break;

        memcpy(&midi_in[hw_offset], engine->pending[src_idx].packet, 4);
        hw_offset += 4;
        src_idx++;
        injected++;
    }

    if (src_idx > 0 && src_idx < engine->pending_count) {
        memmove(&engine->pending[0], &engine->pending[src_idx],
                (engine->pending_count - src_idx) * sizeof(chord_pending_t));
    }
    engine->pending_count -= src_idx;

    return injected;
}
