/*
 * ui_midi_ring.h -- the shim -> shadow_ui MIDI channel, IN ARRIVAL ORDER.
 *
 * The segment is an array of 4-byte USB-MIDI slots; byte 0 (the CIN/cable
 * head) is the gate: 0 = empty. That layout is unchanged -- two binaries map
 * it and a resize is a layout change -- only the ORDER discipline is new.
 *
 * WHAT WAS WRONG. The producer put each packet in the LOWEST free slot and
 * the consumer drained slots in INDEX order. The consumer frees low slots
 * while later ones still hold older packets, so the next frame's packets land
 * low and are read FIRST: delivery reordered whenever a burst straddled a
 * drain. One short message survives that; a SysEx does not -- its bytes arrive
 * shuffled and the reassembler aborts. Measured on hardware 2026-09-24: the
 * E16 ACKed 38 of 38 OLED CLEARs on the wire and not one reached JS (the lone
 * 3-packet ENTER ack always did, which hid it). The 6.9% QY-70 bulk-dump loss
 * in #358 is the same channel.
 *
 * NOW. Both sides walk the array as a RING with their own cursor: the producer
 * writes at `wr` and advances; the consumer reads at `rd` and advances. Order
 * is arrival order by construction.
 *
 * RESYNC. The cursors live in two processes, and shadow_ui can restart (it
 * clears the segment on attach) while the shim keeps its `wr`. So a consumer
 * that finds its slot EMPTY scans forward, circularly, to the first FULL slot
 * and continues from there. The producer writes contiguously from `wr`, so the
 * first full slot after an empty one is the START of its oldest run -- order
 * is kept even across a desync. In steady state rd == wr and the scan finds
 * nothing, which is the ordinary "caught up".
 *
 * RT: the producer is the SPI callback -- one load, three stores, one release,
 * no loop. A full slot at `wr` means the consumer is a whole ring behind: the
 * packet is dropped and COUNTED by the caller, never written over an unread
 * one (that would reorder again, one ring later).
 */
#ifndef UI_MIDI_RING_H
#define UI_MIDI_RING_H

#include <stdint.h>

/* Producer. Returns 1 if placed, 0 if the ring is full at the cursor. */
static inline int ui_midi_ring_put(uint8_t *ring, int bytes, int *wr,
                                   uint8_t head, uint8_t status, uint8_t d1, uint8_t d2)
{
    int i = *wr;
    if (__atomic_load_n(&ring[i], __ATOMIC_ACQUIRE) != 0) return 0;
    ring[i + 1] = status;
    ring[i + 2] = d1;
    ring[i + 3] = d2;
    __atomic_store_n(&ring[i], head, __ATOMIC_RELEASE);
    *wr = (i + 4) % bytes;
    return 1;
}

/* Consumer. Byte offset of the next packet IN ORDER, or -1 if none. The
 * caller reads it, clears byte 0 (release), then calls ui_midi_ring_advance. */
static inline int ui_midi_ring_next(const uint8_t *ring, int bytes, int *rd)
{
    if (__atomic_load_n(&ring[*rd], __ATOMIC_ACQUIRE) != 0) return *rd;
    /* Empty at the cursor: caught up, or desynced from a producer that kept
     * its cursor across our restart. Resync to the start of its run. */
    for (int k = 4; k < bytes; k += 4) {
        int i = (*rd + k) % bytes;
        if (__atomic_load_n(&ring[i], __ATOMIC_ACQUIRE) != 0) {
            /* RACE: the producer may have filled OUR slot and then this one
             * while we scanned. It writes in order, so having acquired slot
             * i, a fill of slot *rd before it is visible now -- and it is
             * the older packet. */
            if (__atomic_load_n(&ring[*rd], __ATOMIC_ACQUIRE) != 0) return *rd;
            *rd = i;
            return i;
        }
    }
    return -1;
}

static inline void ui_midi_ring_advance(int *rd, int bytes) { *rd = (*rd + 4) % bytes; }

#endif /* UI_MIDI_RING_H */
