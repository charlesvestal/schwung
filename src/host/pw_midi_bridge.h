/*
 * pw_midi_bridge.h — Shared structures for PipeWire MIDI bridge
 *
 * The shim writes MIDI events from Move's SPI mailbox into these
 * ring buffers. A bridge daemon in the PipeWire chroot reads them
 * and creates ALSA sequencer virtual ports.
 *
 * Shared between: move_anything_shim.c (writer) and pw-midi-bridge (reader)
 */

#ifndef PW_MIDI_BRIDGE_H
#define PW_MIDI_BRIDGE_H

#include <stdint.h>

#define SHM_PW_MIDI_UI    "/move-pw-midi-ui"    /* MIDI_IN cable 0: pads, knobs, buttons */
#define SHM_PW_MIDI_OUT   "/move-pw-midi-out"   /* MIDI_OUT cable 0: track output */

/* Ring buffer holds 256 events (3 bytes each + 1 byte padding = 4 bytes per slot).
 * At ~344 ioctl/sec with max 64 packets/frame, this is ~1 second of buffer. */
#define PW_MIDI_RING_SIZE 256
#define PW_MIDI_RING_MASK (PW_MIDI_RING_SIZE - 1)

/* Each event is 3 bytes of MIDI (status, data1, data2) plus 1 byte length.
 * Length is 1-3: note on/off = 3, program change = 2, system realtime = 1. */
typedef struct pw_midi_event_t {
    uint8_t len;     /* Number of valid MIDI bytes (1-3), 0 = empty slot */
    uint8_t data[3]; /* Raw MIDI: [status, data1, data2] */
} pw_midi_event_t;

typedef struct pw_midi_ring_t {
    volatile uint32_t write_idx;  /* Writer increments after writing (wraps via mask) */
    volatile uint32_t read_idx;   /* Reader increments after reading (wraps via mask) */
    volatile uint32_t active;     /* 1 = shim is actively writing, 0 = inactive */
    uint32_t reserved;
    pw_midi_event_t events[PW_MIDI_RING_SIZE];
} pw_midi_ring_t;

#endif /* PW_MIDI_BRIDGE_H */
