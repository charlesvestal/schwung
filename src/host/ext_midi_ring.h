/* ext_midi_ring.h — the ROUTE_EXTERNAL packet ring, as pure state.
 *
 * Packets queued here are drained into Move's 80-byte MIDI_OUT region once per
 * audio block and go out of USB-A. It has TWO producers:
 *
 *   EXT_SRC_OVERTAKE   an overtake DSP's midi_send_external
 *   EXT_SRC_CHAIN      chain knob CC out (a chain slot answering a controller)
 *
 * Both run on the SPI callback, so the plain head/tail bumps are safe — but
 * that is a property of the callers, not of this file, and a producer added on
 * a worker thread would break it.
 *
 * WHY THE SOURCE TAG EXISTS. When an overtake module unloads, its queued
 * packets must be discarded: otherwise the next load ships up to a ringful of
 * a dead module's events to USB-A across the first few blocks. That discard
 * used to reset head/tail, which was correct while the overtake DSP was the
 * only producer and stopped being correct the moment the chain became one —
 * the chain outlives the overtake module, and a swallowed chain packet is
 * worse than a dropped one, because knob_emit_cc_out only records a value as
 * delivered on a non-zero push and will not retry a value it thinks it sent.
 * So a discard blanks packets BY SOURCE and leaves the cursors alone.
 *
 * Pure: no allocation, no I/O, no locks, no clock. SPI-callback-safe, and
 * host-testable without a device (tests/host/test_ext_midi_ring.c).
 */

#ifndef EXT_MIDI_RING_H
#define EXT_MIDI_RING_H

#include <stdint.h>
#include <string.h>

#define EXT_MIDI_RING_PACKETS 64          /* 64 slots x 4 bytes USB-MIDI = 256 B */

/* Source tags. 0 is reserved for "discarded — skip on drain", so a real
 * producer must never be 0. */
#define EXT_SRC_NONE     0
#define EXT_SRC_OVERTAKE 1
#define EXT_SRC_CHAIN    2

typedef struct {
    uint8_t pkt[EXT_MIDI_RING_PACKETS][4];
    uint8_t src[EXT_MIDI_RING_PACKETS];   /* EXT_SRC_*; EXT_SRC_NONE = skip */
    volatile uint32_t head;               /* producers write */
    volatile uint32_t tail;               /* consumer writes */
    volatile int drops;                   /* ring-full count, reported off-RT */
} ext_midi_ring_t;

static inline void ext_midi_ring_init(ext_midi_ring_t *r) {
    if (r) memset((void *)r, 0, sizeof(*r));
}

/* Enqueue. Drop-newest on full. Returns len when queued, 0 when dropped —
 * and 0 MUST be treated as "not sent", never as "sent". */
static inline int ext_midi_ring_push(ext_midi_ring_t *r, const uint8_t *msg,
                                     int len, uint8_t src) {
    if (!r || !msg || len < 4) return 0;

    uint32_t head = r->head;
    uint32_t tail = r->tail;
    __sync_synchronize();  /* acquire */
    if ((uint32_t)(head - tail) >= EXT_MIDI_RING_PACKETS) {
        r->drops++;
        return 0;
    }
    uint32_t idx = head % EXT_MIDI_RING_PACKETS;
    memcpy(r->pkt[idx], msg, 4);
    r->src[idx] = src;
    __sync_synchronize();  /* release — packet visible before the head bump */
    r->head = head + 1;
    return len;
}

/* Blank every queued packet from one producer, in place.
 *
 * Deliberately does NOT touch head/tail: the other producer's packets are
 * interleaved with this one's, and there is no cursor position that separates
 * them. The drain steps over EXT_SRC_NONE without spending a mailbox slot. */
static inline void ext_midi_ring_discard_source(ext_midi_ring_t *r, uint8_t src) {
    if (!r || src == EXT_SRC_NONE) return;
    uint32_t tail = r->tail;
    uint32_t head = r->head;
    __sync_synchronize();
    for (uint32_t i = tail; i != head; i++) {
        uint32_t idx = i % EXT_MIDI_RING_PACKETS;
        if (r->src[idx] != src) continue;
        r->src[idx] = EXT_SRC_NONE;
        memset(r->pkt[idx], 0, 4);
    }
    __sync_synchronize();
}

/* Drain into a MIDI_OUT region of `region_bytes` bytes, filling only 4-byte
 * slots that are entirely zero. Returns packets copied.
 *
 * Packets that do not fit this block STAY IN THE RING and are retried next
 * block — this is a delay, not a loss. (The loss downstream is
 * shadow_inject_ui_midi_out's, which memsets its source before copying.) */
static inline int ext_midi_ring_drain(ext_midi_ring_t *r, uint8_t *midi_out,
                                      int region_bytes) {
    if (!r || !midi_out || region_bytes < 4) return 0;

    uint32_t tail = r->tail;
    uint32_t head = r->head;
    __sync_synchronize();  /* acquire — see packet data the producer published */
    if (tail == head) return 0;

    int copied = 0;
    int slot = 0;
    while (tail != head) {
        uint32_t idx = tail % EXT_MIDI_RING_PACKETS;
        /* Discarded in place by ext_midi_ring_discard_source. Consume the ring
         * position, spend no mailbox slot. */
        if (r->src[idx] == EXT_SRC_NONE) { tail++; continue; }

        while (slot + 4 <= region_bytes &&
               (midi_out[slot] || midi_out[slot+1] ||
                midi_out[slot+2] || midi_out[slot+3])) {
            slot += 4;
        }
        if (slot + 4 > region_bytes) break;  /* full this block — retry next */

        memcpy(&midi_out[slot], r->pkt[idx], 4);
        slot += 4;
        tail++;
        copied++;
    }
    __sync_synchronize();
    r->tail = tail;
    return copied;
}

#endif /* EXT_MIDI_RING_H */
