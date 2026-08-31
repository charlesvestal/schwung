/*
 * The shadow_ui -> MIDI_OUT carry: what could not be placed this frame.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS
 *
 * shadow_inject_ui_midi_out() used to snapshot the shadow_ui SHM buffer, reset
 * write_idx, MEMSET THE SOURCE, and only then start placing packets into the
 * 80-byte MIDI_OUT region — stopping at the first frame boundary it could not
 * fit. Everything past that point in the snapshot was gone: not delayed, not
 * retried, not counted, not logged. The comment on ext_midi_ring_drain named
 * it in passing ("The loss downstream is shadow_inject_ui_midi_out's, which
 * memsets its source before copying") and nothing acted on it.
 *
 * MIDI_OUT holds 20 packets, and the drain ran once per shadow_ui flush rather
 * than once per SPI frame, so the real ceiling was ~20 packets per 60 Hz tick
 * = 3600 SysEx data bytes/s. DIN MIDI is 3125 bytes/s. The whole outbound
 * budget sat 15% above the wire rate with no backpressure and no margin, which
 * is why short control messages "work more or less perfectly" while any bulk
 * dump loses whole packets from the middle — the same multiple-of-3 signature
 * as #358, from the opposite direction.
 *
 * A 158-byte SysEx message is 53 packets. It cannot fit in one frame and never
 * could; the bug was never the frame size, it was that the remainder had
 * nowhere to live.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS DOES
 *
 * It gives the remainder somewhere to live. Packets that do not fit stay here,
 * in order, and go out on the next SPI frame — a DELAY instead of a loss, the
 * same contract ext_midi_ring_drain already honours for the DSP path (it does
 * not advance `tail` when the region fills). That alone lifts the ceiling from
 * ~20 packets per 60 Hz tick to ~20 per 344 Hz frame: 6x clear of DIN rate
 * instead of 15% above it.
 *
 * It is header-only and pure — no allocation, no I/O, no locks, no globals —
 * for the same reason as fx_midi_filter.h and ext_midi_ring.h: the only caller
 * is the SPI callback, which cannot be built on the dev machine, and filtering
 * like this is exactly what ends up shipped untested. tests/host compiles and
 * RUNS it natively.
 *
 * ---------------------------------------------------------------------------
 * ORDER IS THE WHOLE POINT
 *
 * A SysEx message is a run of packets that only means anything in sequence.
 * Appends go to the tail, drains take from the head, and a partial drain
 * shifts the remainder down rather than leaving a hole. Reordering a SysEx run
 * is worse than dropping it: the receiver assembles a well-framed message out
 * of shuffled data and has no way to know.
 */
#ifndef UI_MIDI_OUT_CARRY_H
#define UI_MIDI_OUT_CARRY_H

#include <stdint.h>
#include <string.h>

#include "shadow_constants.h"

/* 256 packets. Sized against the message that exposed the bug rather than
 * against the mailbox: a 158-byte SysEx is 53 packets, so this holds ~4.8 of
 * them back-to-back. Matching SHADOW_UI_MIDI_BYTES/4 on the inbound side is
 * deliberate — a tool that can be SENT a burst of that size can answer one. */
#define UI_MIDI_CARRY_PACKETS 256
#define UI_MIDI_CARRY_BYTES   (UI_MIDI_CARRY_PACKETS * 4)

/* The carry must hold whatever one flush of the SHM buffer can deliver.
 *
 * Found on hardware: a 632-byte SysEx (212 packets) came back REFUSED because
 * the SHM buffer was 128 packets, so the two numbers were already disagreeing
 * about how big a single send may be. Enlarging only one of them moves the
 * failure rather than fixing it — make the SHM side bigger and the surplus is
 * DROPPED at the carry instead of REFUSED at the send, which converts a return
 * value the caller can act on into a silent loss, the exact thing this file
 * exists to remove. Keep them equal. */
_Static_assert(UI_MIDI_CARRY_BYTES == SHADOW_MIDI_OUT_BUFFER_SIZE,
               "carry must match SHADOW_MIDI_OUT_BUFFER_SIZE: a flush the SHM "
               "buffer accepts has to fit downstream, or a refusal becomes a drop");

/* Stop accepting new work from the SHM buffer while the carry is at least this
 * full. See ui_midi_carry_wants_more() for why this is backpressure and not
 * just a threshold. */
#define UI_MIDI_CARRY_HIGH_WATER (UI_MIDI_CARRY_BYTES / 2)

typedef struct {
    uint8_t buf[UI_MIDI_CARRY_BYTES];
    int len;        /* bytes currently held, always a multiple of 4 */
    int drops;      /* packets refused because the carry was full */
} ui_midi_carry_t;

static inline void ui_midi_carry_reset(ui_midi_carry_t *c)
{
    if (!c) return;
    c->len = 0;
    c->drops = 0;
}

/*
 * Should the caller take another snapshot of the shadow_ui SHM buffer?
 *
 * THIS IS THE BACKPRESSURE, and it works by NOT reading. Leaving the SHM
 * buffer alone lets it fill, and a full SHM buffer is the one condition
 * js_shadow_midi_send() already reports to JS as a `false` return — a signal
 * that exists, is documented ("Report the failure so the caller can decline to
 * cache it"), and until now could only ever fire on a single 128-packet flush.
 * Refusing to drain upstream is what connects it to the real constraint
 * downstream, so a module that paces on the return value paces on the mailbox.
 *
 * Draining unconditionally and dropping at the far end would keep the same
 * silent-loss shape this file exists to remove, one buffer further along.
 */
static inline int ui_midi_carry_wants_more(const ui_midi_carry_t *c)
{
    return c && c->len < UI_MIDI_CARRY_HIGH_WATER;
}

/*
 * Append one 4-byte packet. Returns 1 when queued, 0 when the carry is full.
 *
 * Drop-oldest is NOT an option here: the head of the carry is the head of a
 * SysEx message that is already partly on the wire. Refusing the newest packet
 * truncates one message; dropping the oldest corrupts one that the receiver
 * has already begun to assemble.
 */
static inline int ui_midi_carry_push(ui_midi_carry_t *c, const uint8_t pkt[4])
{
    if (!c || !pkt) return 0;
    if (c->len + 4 > UI_MIDI_CARRY_BYTES) { c->drops++; return 0; }
    memcpy(&c->buf[c->len], pkt, 4);
    c->len += 4;
    return 1;
}

/*
 * Place as many leading packets as fit into free slots of a MIDI_OUT region,
 * then shift the remainder down. Returns packets placed.
 *
 * "Free" means all four bytes zero — the same test every other writer into
 * this region uses, because Move's own output and the LED flush share it.
 * Placement stops at the first packet that does not fit; it does not skip
 * ahead to find a smaller gap, because that would reorder the run.
 */
static inline int ui_midi_carry_drain(ui_midi_carry_t *c, uint8_t *midi_out,
                                      int region_bytes)
{
    if (!c || !midi_out || c->len <= 0 || region_bytes < 4) return 0;

    int placed = 0;
    int slot = 0;
    int read = 0;

    while (read < c->len) {
        while (slot + 4 <= region_bytes &&
               (midi_out[slot] || midi_out[slot + 1] ||
                midi_out[slot + 2] || midi_out[slot + 3])) {
            slot += 4;
        }
        if (slot + 4 > region_bytes) break;  /* full this frame — retry next */

        memcpy(&midi_out[slot], &c->buf[read], 4);
        slot += 4;
        read += 4;
        placed++;
    }

    if (read > 0) {
        int remain = c->len - read;
        if (remain > 0) memmove(c->buf, &c->buf[read], (size_t)remain);
        c->len = remain;
    }
    return placed;
}

#endif /* UI_MIDI_OUT_CARRY_H */
