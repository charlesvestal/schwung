/* link_audio_reorder.h -- put Move's Link Audio packets back in order.
 *
 * Link Audio is UDP, and UDP does not promise order. Each buffer carries a
 * sequence number (BufferHandle::Info::count), and the sidecar used to ignore
 * it and write buffers in ARRIVAL order. Two packets that swap in flight then
 * play swapped: 2.8 ms of the next packet early, 2.8 ms of the previous one
 * late, with a hard edge at each seam -- a click on that one track.
 *
 * Measured 2026-10-04 from a Skipback stem (track 1, 125-frame packets): the
 * block before the click matched the pattern shifted +125 samples, the click
 * block -125, the block after 0, against three different passes of the loop.
 * It only happened with Move->Schwung on, because only then does Move's audio
 * reach the speakers through Link Audio.
 *
 * The repair is a ONE-PACKET hold: a packet that arrives exactly one ahead of
 * the expected sequence number is parked; if the missing one arrives next, the
 * two go out in order. Anything else ends the hold (the missing packet is
 * counted as lost). In-order delivery passes straight through, so this costs
 * no latency at all unless a reorder actually happens, and then one packet
 * (~2.8 ms), which the ring's reserve already covers.
 *
 * Pure and allocation-free: it runs in Link's audio callback. The emit
 * function is called synchronously, 0, 1 or 2 times per push.
 */
#ifndef LINK_AUDIO_REORDER_H
#define LINK_AUDIO_REORDER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Largest packet that can be held, in frames (stereo). Move sends 125. A
 * larger packet is never held -- it passes through in arrival order. */
#define LA_REORDER_HOLD_FRAMES 512
/* A sequence number this far behind or ahead is a restart (a new stream),
 * not a reorder or a loss: resynchronise rather than count it. */
#define LA_REORDER_RESYNC_SPAN 64

typedef void (*la_reorder_emit_fn)(void *ctx, const int16_t *samples, size_t frames);

typedef struct {
    int      started;
    uint64_t expected;          /* next sequence number to emit */
    int      held;              /* a packet is parked */
    uint64_t held_count;
    size_t   held_frames;
    int16_t  held_samples[LA_REORDER_HOLD_FRAMES * 2];
    /* Telemetry, read and reset by the reporter. */
    uint32_t reordered;         /* swaps repaired */
    uint32_t lost;              /* packets that never arrived */
    uint32_t late;              /* arrived after we moved on: dropped */
    uint32_t resyncs;
} la_reorder_t;

static inline void la_reorder_reset(la_reorder_t *r)
{
    r->started = 0;
    r->held = 0;
}

static inline void la_reorder_emit_held(la_reorder_t *r, la_reorder_emit_fn emit, void *ctx)
{
    emit(ctx, r->held_samples, r->held_frames);
    r->expected = r->held_count + 1;
    r->held = 0;
}

static inline void la_reorder_push(la_reorder_t *r, uint64_t count,
                                   const int16_t *samples, size_t frames,
                                   la_reorder_emit_fn emit, void *ctx)
{
    if (!r->started) {
        r->started = 1;
        emit(ctx, samples, frames);
        r->expected = count + 1;
        return;
    }
    const int64_t d = (int64_t)(count - r->expected);

    if (r->held) {
        if (d == 0) {                           /* the missing one: in order now */
            emit(ctx, samples, frames);
            la_reorder_emit_held(r, emit, ctx);
            r->reordered++;
            return;
        }
        if (d < 0 && d > -LA_REORDER_RESYNC_SPAN) {   /* a stale duplicate */
            r->late++;
            return;
        }
        r->lost++;                              /* the missing one is gone */
        la_reorder_emit_held(r, emit, ctx);
        /* fall through: judge `count` against the new expectation */
    }

    const int64_t e = (int64_t)(count - r->expected);
    if (e == 0) {
        emit(ctx, samples, frames);
        r->expected = count + 1;
    } else if (e == 1 && frames <= LA_REORDER_HOLD_FRAMES) {
        memcpy(r->held_samples, samples, frames * 2 * sizeof(int16_t));
        r->held_frames = frames;
        r->held_count = count;
        r->held = 1;
    } else if (e < 0 && e > -LA_REORDER_RESYNC_SPAN) {
        r->late++;                              /* already played past it */
    } else if (e > 0 && e < LA_REORDER_RESYNC_SPAN) {
        r->lost += (uint32_t)e;                 /* expected .. count-1 never came */
        emit(ctx, samples, frames);
        r->expected = count + 1;
    } else {                                    /* a new stream */
        r->resyncs++;
        emit(ctx, samples, frames);
        r->expected = count + 1;
    }
}

#endif /* LINK_AUDIO_REORDER_H */
