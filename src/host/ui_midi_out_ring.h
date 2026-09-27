#ifndef UI_MIDI_OUT_RING_H
#define UI_MIDI_OUT_RING_H

/*
 * SPSC discipline for /schwung-midi-out (shadow_midi_out_t).
 *
 * THE CONSUMER USED TO WRITE THE PRODUCER'S REGION, and that is the bug this
 * file exists to make impossible.
 *
 * shadow_inject_ui_midi_out() (the shim, SPI callback) did:
 *
 *     int snapshot_len = midi_out_shm->write_idx;
 *     memcpy(local_buf, midi_out_shm->buffer, copy_len);
 *     __sync_synchronize();
 *     midi_out_shm->write_idx = 0;
 *     memset(midi_out_shm->buffer, 0, SHADOW_MIDI_OUT_BUFFER_SIZE);
 *
 * while js_shadow_midi_send() (shadow_ui, a SEPARATE PROCESS) appended at
 * write_idx and returned JS_TRUE. A packet written between the `snapshot_len`
 * read and the memset is erased, and its index discarded with `write_idx = 0`
 * — after the sender has already been told it was sent.
 *
 * That loss is invisible to every counter on either side: JS counts it as
 * sent, the shim never sees it. One lost packet inside a SysEx run corrupts
 * the whole message, which the E16 renders as a garbled screen. The shim
 * drains ~344x/sec and JS writes bursts of up to 34 packets at ~60 Hz, so the
 * window is small and hit regularly under load — which is why the fault
 * presented as "garbles when playing and turning knobs fast" and survived
 * every pacing change.
 *
 * The old code's own comment ("Copy before resetting to avoid a race where the
 * JS process writes new data between our reset and memcpy") shows the race was
 * considered. Reordering the copy and the reset only closes the half where the
 * reset comes first; the memset reopens it.
 *
 * The rule here, and there is no exception to it:
 *
 *   - the PRODUCER (shadow_ui) owns `write_idx` and the bytes at and after it.
 *     It never reads or writes `read_idx`.
 *   - the CONSUMER (the shim) owns `read_idx`. It never writes `write_idx` and
 *     NEVER writes the buffer — no memset, not even of bytes it has consumed.
 *
 * Each side reading a stale copy of the other's index is safe by construction:
 * a stale (smaller) read_idx makes the producer conservative about free space,
 * and a stale (smaller) write_idx makes the consumer take less this frame. A
 * torn read cannot happen — both are naturally aligned uint16 on ARM64.
 *
 * INDICES ARE FREE-RUNNING BYTE COUNTS, not offsets. They wrap at 2^16, and
 * that is exact rather than approximate: 65536 is a whole multiple of
 * SHADOW_MIDI_OUT_BUFFER_SIZE, so (uint16)(write_idx - read_idx) is the true
 * distance across the wrap. The buffer position is `idx & (SIZE - 1)`.
 *
 * Capacity is one packet short of the buffer, because a full ring and an empty
 * one are both `write_idx == read_idx` and there is no third index to tell
 * them apart. Spending 4 bytes is cheaper than a flag that has to be written
 * by whichever side moved last.
 */

#include <stdint.h>
#include <string.h>

#include "shadow_constants.h"

/* The masking above is only a masking if the size is a power of two, and the
 * wrap arithmetic is only exact if 2^16 divides it. Both hold at 4096; neither
 * is obvious from the constant, and a "round" resize to, say, 3000 would break
 * the distance calculation silently — the ring would appear to work and lose a
 * message per wrap. */
_Static_assert((SHADOW_MIDI_OUT_BUFFER_SIZE &
                (SHADOW_MIDI_OUT_BUFFER_SIZE - 1)) == 0,
               "SHADOW_MIDI_OUT_BUFFER_SIZE must be a power of two");
_Static_assert(SHADOW_MIDI_OUT_BUFFER_SIZE <= 65536 &&
                   (65536 % SHADOW_MIDI_OUT_BUFFER_SIZE) == 0,
               "uint16 indices must wrap on a whole multiple of the buffer");
_Static_assert((SHADOW_MIDI_OUT_BUFFER_SIZE % 4) == 0,
               "a 4-byte packet must never straddle the wrap");

#define UI_MIDI_OUT_MASK     (SHADOW_MIDI_OUT_BUFFER_SIZE - 1)
/* One packet reserved so full != empty. */
#define UI_MIDI_OUT_CAPACITY (SHADOW_MIDI_OUT_BUFFER_SIZE - 4)

/* Bytes queued and not yet consumed. Safe from either side. */
static inline uint16_t ui_midi_out_used(const shadow_midi_out_t *m)
{
    return (uint16_t)((uint16_t)m->write_idx - (uint16_t)m->read_idx);
}

/* Bytes the producer may still append. Producer side only. */
static inline uint16_t ui_midi_out_free(const shadow_midi_out_t *m)
{
    return (uint16_t)(UI_MIDI_OUT_CAPACITY - ui_midi_out_used(m));
}

/*
 * PRODUCER: append `len` bytes, or none of them. Returns 1 on success.
 *
 * All-or-nothing is not a nicety here. A USB-MIDI SysEx is a RUN of packets
 * the device assembles into one message, so a prefix landing and a tail being
 * refused is a truncated message on the wire — the same garbled screen a lost
 * packet gives. The caller's `false` already means "still owed, retry".
 */
static inline int ui_midi_out_push(shadow_midi_out_t *m,
                                   const uint8_t *src, uint16_t len)
{
    if (len == 0) return 1;
    if (len > ui_midi_out_free(m)) return 0;

    uint16_t w = (uint16_t)m->write_idx;
    uint16_t pos = (uint16_t)(w & UI_MIDI_OUT_MASK);
    uint16_t first = (uint16_t)(SHADOW_MIDI_OUT_BUFFER_SIZE - pos);
    if (first > len) first = len;

    memcpy(&m->buffer[pos], src, first);
    if (len > first)
        memcpy(&m->buffer[0], src + first, (size_t)(len - first));

    /* Publish the bytes before the index that advertises them. */
    __sync_synchronize();
    m->write_idx = (uint16_t)(w + len);
    return 1;
}

/*
 * CONSUMER: copy the `len` bytes at read_idx into `dst`. Does not consume —
 * see ui_midi_out_commit. Splitting copy from commit is what lets the caller
 * DEFER a snapshot whole: if it cannot place the whole run this frame it
 * simply does not commit, and takes the same bytes again on a later one.
 */
static inline void ui_midi_out_copy(const shadow_midi_out_t *m,
                                    uint8_t *dst, uint16_t len)
{
    if (len == 0) return;
    uint16_t pos = (uint16_t)((uint16_t)m->read_idx & UI_MIDI_OUT_MASK);
    uint16_t first = (uint16_t)(SHADOW_MIDI_OUT_BUFFER_SIZE - pos);
    if (first > len) first = len;

    memcpy(dst, &m->buffer[pos], first);
    if (len > first)
        memcpy(dst + first, &m->buffer[0], (size_t)(len - first));
}

/* CONSUMER: release `len` bytes. The ONLY write the consumer ever makes to
 * the segment. Barrier first so the copy above is complete before the
 * producer can see the space as free. */
static inline void ui_midi_out_commit(shadow_midi_out_t *m, uint16_t len)
{
    __sync_synchronize();
    m->read_idx = (uint16_t)((uint16_t)m->read_idx + len);
}

#endif /* UI_MIDI_OUT_RING_H */
