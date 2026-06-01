/*
 * shadow_midi_inject_writer.h — single source of truth for writing into
 * the /schwung-midi-inject SHM ring.
 *
 * The inject ring is multi-producer / single-consumer. Producers can
 * be in different processes (shim, schwung_host) and may call
 * concurrently. Without coordination they race on the cursor or
 * leave reserved-but-unwritten slots that the drain reads as garbage —
 * the latter manifests as Move firmware SIGABRT (see shadow_midi.c:553).
 *
 * This helper encodes the lock-free MPSC pattern once:
 *   1. CAS-reserve a 4-byte slot at `alloc_idx`
 *   2. memcpy our 4 bytes into the reserved slot
 *   3. Spin (bounded) until prior producers have advanced `commit_idx`
 *      to our slot start (FIFO commit order)
 *   4. Atomically advance `commit_idx` past our slot, then bump `ready`
 *
 * The drain reads `commit_idx` (not `alloc_idx`), so steps 1–3 are
 * invisible to it; only step 4 publishes our packet.
 *
 * Memory ordering:
 *   - alloc_idx CAS: RELAXED — we only need atomic agreement on which
 *     slot belongs to whom, no data dependency.
 *   - commit_idx load (in spin): ACQUIRE — pairs with prior producer's
 *     RELEASE store to commit_idx, ensuring their buffer write is
 *     visible to us if we ever needed to read it (we don't, but the
 *     pairing keeps semantics tight).
 *   - commit_idx store: RELEASE — pairs with the drain's ACQUIRE load,
 *     publishing our buffer write to the drain.
 *   - ready fetch_add: RELEASE — pairs with drain's read of `ready`
 *     to detect "something new arrived".
 *
 * Header-only on purpose: the helper is small, hot enough that we want
 * inlining at every callsite, and self-contained (depends only on
 * shadow_constants.h and stdatomic-style GCC builtins).
 */

#ifndef SHADOW_MIDI_INJECT_WRITER_H
#define SHADOW_MIDI_INJECT_WRITER_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "shadow_constants.h"

/* Bound on the FIFO-commit spin. In realistic conditions this never
 * triggers — concurrent-writer rate across all four producers is well
 * under 100/sec, drain runs every ~3ms, so the chance of finding a
 * prior producer mid-write is ~10^-4 per push. When it does fire, a
 * tight loop of atomic loads is nanoseconds per iteration; 100k caps
 * worst-case at ~1ms of CPU, after which we give up and return -2
 * rather than hang the caller. */
#define SHADOW_MIDI_INJECT_SPIN_LIMIT 100000

/* shadow_midi_inject_push — push one 4-byte USB-MIDI packet into the ring.
 *
 * Safe to call concurrently from any thread/process that has the SHM
 * mapped read-write. Wait-free in the no-contention case; bounded spin
 * (~1 ms cap) in the rare case a prior producer is mid-write.
 *
 * Returns:
 *    0 — committed; drain will see this packet on its next pass.
 *   -1 — buffer full (drain hasn't run / can't keep up). Caller may retry
 *        after a frame.
 *   -2 — gave up waiting for a prior stranded producer's commit. Our
 *        4 bytes are physically in the buffer but were never published
 *        via commit_idx; they will be wiped on the next drain reset.
 *        Callers that care should log a one-shot warning and skip the
 *        packet (or retry).
 */
static inline int
shadow_midi_inject_push(shadow_midi_inject_t *shm, const uint8_t pkt[4])
{
    /* 1. CAS-reserve a slot. Loop body retries only if another producer
     *    advanced alloc_idx between our load and our compare-exchange —
     *    not a busy-wait, just a value refresh. */
    uint8_t my_slot;
    do {
        my_slot = __atomic_load_n(&shm->alloc_idx, __ATOMIC_RELAXED);
        if ((unsigned)my_slot + 4u >= SHADOW_MIDI_INJECT_BUFFER_SIZE) {
            return -1;  /* buffer full */
        }
    } while (!__atomic_compare_exchange_n(
        &shm->alloc_idx, &my_slot, (uint8_t)(my_slot + 4),
        false /* strong */,
        __ATOMIC_RELAXED, __ATOMIC_RELAXED));

    /* 2. We now own [my_slot, my_slot + 4). No other producer will
     *    touch these bytes; safe to memcpy without further locks. */
    memcpy(&shm->buffer[my_slot], pkt, 4);

    /* 3. Wait for prior producers to commit so commits land in FIFO
     *    order. The acquire load pairs with each prior producer's
     *    release store on commit_idx. Bounded by SPIN_LIMIT — see
     *    macro comment for rationale. */
    int spin_iters = 0;
    while (__atomic_load_n(&shm->commit_idx, __ATOMIC_ACQUIRE) != my_slot) {
        if (++spin_iters > SHADOW_MIDI_INJECT_SPIN_LIMIT) {
            return -2;  /* prior producer stranded */
        }
    }

    /* 4. Publish. The release store on commit_idx makes our memcpy
     *    visible to the drain's acquire load. The ready bump signals
     *    "something new arrived" so drain doesn't need to poll
     *    commit_idx every frame when nothing is happening. */
    __atomic_store_n(&shm->commit_idx, (uint8_t)(my_slot + 4),
                     __ATOMIC_RELEASE);
    __atomic_fetch_add(&shm->ready, 1, __ATOMIC_RELEASE);
    return 0;
}

#endif /* SHADOW_MIDI_INJECT_WRITER_H */
