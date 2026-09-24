/*
 * e16_mirror_shm.h -- what the E16 is showing, for the web mirror.
 *
 * Written by shadow_ui (the E16 surface), read by display_server, which
 * streams it at /stream-e16 beside Move's own screen. It is what we BELIEVE
 * the E16 shows: the screen bytes it has been sent (the display's belief,
 * lastSentBuf) and the last ring sent for each knob. A row the device dropped
 * looks right here until the surface repairs it.
 *
 * Torn-read protection is a sequence counter: the writer makes it ODD before
 * writing and EVEN after; a reader copies only between two equal, even reads.
 * `last_update_ms` (CLOCK_MONOTONIC) is refreshed at least every second while
 * the surface is live, so a reader can tell a stopped writer from a still
 * screen.
 */
#ifndef E16_MIRROR_SHM_H
#define E16_MIRROR_SHM_H

#include <stdint.h>

#define E16_MIRROR_SHM_NAME   "/schwung-e16-live"
#define E16_MIRROR_SHM_PATH   "/dev/shm/schwung-e16-live"
#define E16_MIRROR_MAGIC      "E16MIR1"
#define E16_MIRROR_FRAME_SIZE 1024        /* 128 x 64, SSD1306 pages, as Move's */
#define E16_MIRROR_RINGS      16
#define E16_MIRROR_RING_BYTES 6           /* r, g, b, amount hi, amount lo, bipolar */
#define E16_MIRROR_STALE_MS   3000

/* Every field sits on its natural alignment, so this is not `packed`: the
 * sequence counter is read atomically, which a packed member forbids. The
 * layout is pinned below instead -- two processes map it. */
typedef struct {
    char magic[8];
    uint32_t version;              /* 1 */
    uint32_t seq;                  /* odd while writing */
    uint64_t last_update_ms;       /* CLOCK_MONOTONIC */
    uint8_t active;                /* the surface is on and the device present */
    uint8_t has_frame;             /* 0: nothing drawn yet (or text mode) */
    uint8_t reserved[6];
    uint8_t frame[E16_MIRROR_FRAME_SIZE];
    uint8_t rings[E16_MIRROR_RINGS * E16_MIRROR_RING_BYTES];
} e16_mirror_shm_t;

#include <stddef.h>
_Static_assert(offsetof(e16_mirror_shm_t, seq) == 12, "e16 mirror layout");
_Static_assert(offsetof(e16_mirror_shm_t, last_update_ms) == 16, "e16 mirror layout");
_Static_assert(offsetof(e16_mirror_shm_t, frame) == 32, "e16 mirror layout");
_Static_assert(sizeof(e16_mirror_shm_t) == 32 + E16_MIRROR_FRAME_SIZE +
               E16_MIRROR_RINGS * E16_MIRROR_RING_BYTES, "e16 mirror layout");

#endif /* E16_MIRROR_SHM_H */
