/* display_pull.h — answer the XMOS's display slice request.
 *
 * The OLED is a PULL protocol (docs/SPI_PROTOCOL.md, "Index handshake"): each
 * frame the XMOS names the slice it wants, 1..6, in the RX display status word,
 * and the writer echoes that index in the TX status word beside that slice's
 * bytes. Move does exactly this -- every SPI snapshot taken on hardware shows
 * its TX status equal to the RX request -- and so do boot-select.c and the
 * JACK bridge.
 *
 * shadow_swap_display() did not. It free-ran its own 0..6 counter, one step per
 * ioctl, and sent whatever slice the counter named. That only lands in the
 * right place while the XMOS happens to be asking for the same slice, and
 * measured on 1.5.0 it frequently was not: 126 snapshots showed the request
 * falling back to 1 or 2 while the counter was at 3, 4 or 5. A slice delivered
 * against the wrong request is a band of the panel drawn in the wrong place --
 * the "wrapped" screen -- and it came and went because the two sequences drift
 * in and out of step.
 *
 * Header-only and I/O-free: runs on the SPI callback, and tests/host drives it
 * directly (test_display_pull.c).
 */
#ifndef DISPLAY_PULL_H
#define DISPLAY_PULL_H

#include <stdint.h>
#include <string.h>

#define DISPLAY_PULL_FRAME_BYTES  1024
#define DISPLAY_PULL_SLICE_BYTES  172
#define DISPLAY_PULL_SLICES       6
/* Byte offsets inside the 4096-byte SPI mailbox (schwung_spi_lib.h names the
 * same numbers SCHWUNG_OFF_OUT_DISP_STAT / _DATA / SCHWUNG_OFF_IN_DISP_STAT). */
#define DISPLAY_PULL_TX_STAT      80
#define DISPLAY_PULL_TX_DATA      84
#define DISPLAY_PULL_RX_STAT      (2048 + 248)

typedef struct {
    uint8_t frame[DISPLAY_PULL_FRAME_BYTES];
    int     valid;   /* frame holds a latch from THIS shadow-display session */
} display_pull_t;

/* Forget the latch. Call whenever the shadow display stops owning the panel,
 * so a return does not open with slices of a frame from the last session. */
static inline void display_pull_reset(display_pull_t *dp)
{
    dp->valid = 0;
}

/* Bytes carried by 1-based slice `idx`: 172 for 1..5, 164 for the last. */
static inline int display_pull_slice_len(uint32_t idx)
{
    return (idx == DISPLAY_PULL_SLICES)
        ? DISPLAY_PULL_FRAME_BYTES - (DISPLAY_PULL_SLICES - 1) * DISPLAY_PULL_SLICE_BYTES
        : DISPLAY_PULL_SLICE_BYTES;
}

/*
 * Serve this frame's request from `src` into `mailbox`.
 *
 * The panel frame is LATCHED when slice 1 is requested and held for 2..6, so
 * every slice of one panel frame comes from one render (the tearing fix from
 * #213, now keyed to the XMOS's frame start instead of our own counter). With
 * no latch yet -- first frame after a reset -- it latches on whatever slice is
 * asked for rather than sending an empty buffer.
 *
 * A request outside 1..6 is answered with status 0 and an empty slice, never
 * left alone: Move has already written ITS slice there this frame, and leaving
 * it is how Move's screen would bleed through.
 *
 * Returns the index served, or 0.
 */
static inline uint32_t display_pull_serve(display_pull_t *dp, uint8_t *mailbox,
                                          const uint8_t *src)
{
    uint32_t idx;
    memcpy(&idx, mailbox + DISPLAY_PULL_RX_STAT, sizeof(idx));

    if (idx < 1 || idx > DISPLAY_PULL_SLICES) {
        uint32_t zero = 0;
        memcpy(mailbox + DISPLAY_PULL_TX_STAT, &zero, sizeof(zero));
        memset(mailbox + DISPLAY_PULL_TX_DATA, 0, DISPLAY_PULL_SLICE_BYTES);
        return 0;
    }

    if (idx == 1 || !dp->valid) {
        memcpy(dp->frame, src, DISPLAY_PULL_FRAME_BYTES);
        dp->valid = 1;
    }

    int off = (int)(idx - 1) * DISPLAY_PULL_SLICE_BYTES;
    int len = display_pull_slice_len(idx);
    memcpy(mailbox + DISPLAY_PULL_TX_STAT, &idx, sizeof(idx));
    memcpy(mailbox + DISPLAY_PULL_TX_DATA, dp->frame + off, (size_t)len);
    if (len < DISPLAY_PULL_SLICE_BYTES)
        memset(mailbox + DISPLAY_PULL_TX_DATA + len, 0,
               (size_t)(DISPLAY_PULL_SLICE_BYTES - len));
    return idx;
}

#endif /* DISPLAY_PULL_H */
