/*
 * shadow_midi_inject_writer.h — single source of truth for the
 * /schwung-midi-inject SHM ring (producers + the single consumer).
 *
 * The ring is multi-producer / single-consumer. Producers can live in
 * different processes (shim, schwung_host) and call concurrently; the
 * one consumer is the shim's SPI-thread drain. Without coordination they
 * race on slot contents and the drain reads garbage — a cable=0/CIN=0
 * packet reaching Move's MIDI_IN reads as "misc function" and Move
 * firmware aborts (SIGABRT).
 *
 * This is Dmitry Vyukov's bounded MPSC queue: an array of slots, each
 * with a sequence number `seq` and a 4-byte packet. Properties that
 * matter here (see the long note in shadow_constants.h):
 *   - the consumer never resets cursors or memsets the buffer, so it
 *     cannot race a cross-process producer mid-write;
 *   - producers never wait on each other's commit (no FIFO-commit spin),
 *     so a producer preempted between claim and publish cannot stall the
 *     SPI-thread producer on the realtime path;
 *   - every cursor/seq access is atomic — no volatile-vs-atomic race.
 *
 * Memory ordering (the standard Vyukov pairing):
 *   - enqueue_pos / read_pos advance RELAXED (they only arbitrate slot
 *     ownership; the data handoff rides on seq).
 *   - producer publishes with a RELEASE store on slot.seq; consumer's
 *     ACQUIRE load of slot.seq pairs with it, making the packet write
 *     visible before the consumer reads it.
 *   - consumer frees a slot with a RELEASE store on slot.seq; the next
 *     lap's producer ACQUIRE-loads it before reusing the slot.
 *
 * Header-only: small, hot, self-contained (depends only on
 * shadow_constants.h and GCC __atomic builtins).
 */

#ifndef SHADOW_MIDI_INJECT_WRITER_H
#define SHADOW_MIDI_INJECT_WRITER_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "shadow_constants.h"

/* shadow_midi_inject_init — one-time initialization by the SHM creator
 * (the shim), BEFORE any producer maps the segment. Zero-fill is not a
 * valid initial state: each slot's seq must equal its index. Safe to call
 * on a freshly created (or re-created) segment when no producer is live. */
static inline void
shadow_midi_inject_init(shadow_midi_inject_t *shm)
{
    __atomic_store_n(&shm->enqueue_pos, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&shm->read_pos, 0u, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < SHADOW_MIDI_INJECT_SLOTS; i++) {
        shm->slots[i].pkt[0] = shm->slots[i].pkt[1] =
            shm->slots[i].pkt[2] = shm->slots[i].pkt[3] = 0;
        __atomic_store_n(&shm->slots[i].seq, i, __ATOMIC_RELAXED);
    }
}

/* shadow_midi_inject_push — enqueue one 4-byte USB-MIDI packet.
 *
 * Safe to call concurrently from any thread/process that has the SHM
 * mapped read-write. Wait-free except for a bounded CAS-retry under
 * producer contention (retries at most once per concurrent producer —
 * never waits on another producer to publish).
 *
 * Returns:
 *    0 — enqueued; the drain will deliver it on a subsequent pass.
 *   -1 — ring full (consumer hasn't freed this slot's previous lap yet).
 *        Caller may retry after a frame.
 */
static inline int
shadow_midi_inject_push(shadow_midi_inject_t *shm, const uint8_t pkt[4])
{
    shadow_midi_inject_slot_t *slot;
    uint32_t pos = __atomic_load_n(&shm->enqueue_pos, __ATOMIC_RELAXED);
    for (;;) {
        slot = &shm->slots[pos & SHADOW_MIDI_INJECT_MASK];
        uint32_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        int32_t diff = (int32_t)(seq - pos);
        if (diff == 0) {
            /* Slot is free for this lap. Try to claim `pos`. Weak CAS:
             * on failure `pos` is refreshed to the current enqueue_pos. */
            if (__atomic_compare_exchange_n(&shm->enqueue_pos, &pos, pos + 1,
                                            true /* weak */,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                break;  /* we own `slot` at `pos` */
            }
            /* CAS failed → pos reloaded; recompute slot and retry. */
        } else if (diff < 0) {
            /* Slot still holds an unconsumed packet from a prior lap. The
             * ring is full at this position → drop, let the caller retry. */
            return -1;
        } else {
            /* diff > 0: another producer already claimed this pos. Refresh
             * and retry against the current head. */
            pos = __atomic_load_n(&shm->enqueue_pos, __ATOMIC_RELAXED);
        }
    }

    memcpy(slot->pkt, pkt, 4);
    /* Publish: RELEASE makes the packet write visible to the consumer's
     * ACQUIRE load of seq. */
    __atomic_store_n(&slot->seq, pos + 1, __ATOMIC_RELEASE);
    return 0;
}

/* shadow_midi_inject_peek — copy the next ready packet into out[4] WITHOUT
 * consuming it. Single-consumer only (the drain). Returns 1 if a packet was
 * copied, 0 if none is ready yet (empty, or the head slot's producer is
 * still mid-write). The packet stays stable until _pop(), so the consumer
 * may decline to consume it this pass (it will reappear next pass). */
static inline int
shadow_midi_inject_peek(shadow_midi_inject_t *shm, uint8_t out[4])
{
    uint32_t pos = __atomic_load_n(&shm->read_pos, __ATOMIC_RELAXED);
    shadow_midi_inject_slot_t *slot = &shm->slots[pos & SHADOW_MIDI_INJECT_MASK];
    uint32_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
    if ((int32_t)(seq - (pos + 1)) != 0) {
        return 0;  /* not published yet → nothing ready at the head */
    }
    memcpy(out, slot->pkt, 4);
    return 1;
}

/* shadow_midi_inject_pop — consume the packet previously observed by _peek,
 * freeing its slot for a future lap. Single-consumer only. Call exactly once
 * per successful _peek that you decide to consume. */
static inline void
shadow_midi_inject_pop(shadow_midi_inject_t *shm)
{
    uint32_t pos = __atomic_load_n(&shm->read_pos, __ATOMIC_RELAXED);
    shadow_midi_inject_slot_t *slot = &shm->slots[pos & SHADOW_MIDI_INJECT_MASK];
    /* Free the slot for the producer that will claim pos + SLOTS. RELEASE
     * pairs with that producer's ACQUIRE load of seq. */
    __atomic_store_n(&slot->seq, pos + SHADOW_MIDI_INJECT_SLOTS, __ATOMIC_RELEASE);
    __atomic_store_n(&shm->read_pos, pos + 1, __ATOMIC_RELAXED);
}

#endif /* SHADOW_MIDI_INJECT_WRITER_H */
