#ifndef LINK_AUDIO_H
#define LINK_AUDIO_H

#include <stdint.h>
#include <pthread.h>
#include <netinet/in.h>

/* ============================================================================
 * LINK AUDIO INTERCEPTION AND PUBLISHING
 * ============================================================================
 * Move firmware 2.0 sends per-track audio over UDP/IPv6 using the "chnnlsv"
 * protocol.  This header defines constants, ring buffer structures, and the
 * global state used by the sendto() hook, self-subscriber, and publisher.
 * ============================================================================ */

/* Protocol constants */
#define LINK_AUDIO_MAGIC        "chnnlsv"
#define LINK_AUDIO_MAGIC_LEN    7
#define LINK_AUDIO_VERSION      0x01
#define LINK_AUDIO_MSG_SESSION  1
#define LINK_AUDIO_MSG_PONG     3
#define LINK_AUDIO_MSG_REQUEST  4
#define LINK_AUDIO_MSG_STOP     5
#define LINK_AUDIO_MSG_AUDIO    6
#define LINK_AUDIO_HEADER_SIZE  74
#define LINK_AUDIO_PAYLOAD_SIZE 500
#define LINK_AUDIO_PACKET_SIZE  574
#define LINK_AUDIO_FRAMES_PER_PACKET 125

/* Channel limits: 5 Move (tracks 1-4 + Main) + 4 shadow slots */
#define LINK_AUDIO_MOVE_CHANNELS    5
#define LINK_AUDIO_SHADOW_CHANNELS  4
#define LINK_AUDIO_MAX_CHANNELS     (LINK_AUDIO_MOVE_CHANNELS + LINK_AUDIO_SHADOW_CHANNELS)

/* Lock-free SPSC ring buffer per channel.
 * 512 frames = ~11.6ms at 44100 Hz, absorbs 125-vs-128 frame mismatch.
 * Must be power-of-two for mask-based wrapping. */
#define LINK_AUDIO_RING_FRAMES  512
#define LINK_AUDIO_RING_SAMPLES (LINK_AUDIO_RING_FRAMES * 2)  /* stereo */
#define LINK_AUDIO_RING_MASK    (LINK_AUDIO_RING_SAMPLES - 1)

/* Publisher output ring: accumulates 128-frame render blocks, drains 125-frame packets */
#define LINK_AUDIO_PUB_RING_FRAMES  1024
#define LINK_AUDIO_PUB_RING_SAMPLES (LINK_AUDIO_PUB_RING_FRAMES * 2)
#define LINK_AUDIO_PUB_RING_MASK    (LINK_AUDIO_PUB_RING_SAMPLES - 1)

/* Shared memory for publisher (shim → link_subscriber) */
#define SHM_LINK_AUDIO_PUB  "/schwung-pub-audio"
#define LINK_AUDIO_PUB_BLOCK_FRAMES  128   /* matches FRAMES_PER_BLOCK */
#define LINK_AUDIO_PUB_BLOCK_SAMPLES (LINK_AUDIO_PUB_BLOCK_FRAMES * 2) /* stereo */

/* Per-slot ring in shared memory: 16 blocks deep = ~46ms buffer.
 * MUST stay larger than the consumer catch-up threshold in
 * link_audio_read_channel_shm (need * 12 = 3072 samples) — otherwise
 * Link Audio SDK bursts overrun the ring before catch-up trips and
 * the consumer reads partially-overwritten samples (sounds like
 * sample-rate corruption). */
#define LINK_AUDIO_PUB_SHM_BLOCKS   16
#define LINK_AUDIO_PUB_SHM_RING_SAMPLES (LINK_AUDIO_PUB_BLOCK_SAMPLES * LINK_AUDIO_PUB_SHM_BLOCKS)
#define LINK_AUDIO_PUB_SHM_RING_MASK   (LINK_AUDIO_PUB_SHM_RING_SAMPLES - 1)

/* Shared memory layout for one publisher slot */
typedef struct {
    int16_t  ring[LINK_AUDIO_PUB_SHM_RING_SAMPLES];  /* stereo int16 ring buffer */
    volatile uint32_t write_pos;   /* updated by shim (producer), sample index */
    volatile uint32_t read_pos;    /* updated by subscriber (consumer), sample index */
    volatile int      active;      /* slot has an active chain instance */
} link_audio_pub_slot_t;

/* Publisher slot count: 4 per-track + 1 master mix */
#define LINK_AUDIO_PUB_SLOT_COUNT   (LINK_AUDIO_SHADOW_CHANNELS + 1)
#define LINK_AUDIO_PUB_MASTER_IDX   LINK_AUDIO_SHADOW_CHANNELS  /* index 4 */

/* Top-level shared memory structure */
typedef struct {
    volatile uint32_t magic;       /* 0x4C415042 = "LAPB" - Link Audio Pub Buffer */
    volatile uint32_t version;     /* structure version, currently 1 */
    volatile int      num_slots;   /* number of active slots */
    link_audio_pub_slot_t slots[LINK_AUDIO_PUB_SLOT_COUNT]; /* 0-3: per-track, 4: master */
} link_audio_pub_shm_t;

#define LINK_AUDIO_PUB_SHM_MAGIC   0x4C415042
#define LINK_AUDIO_PUB_SHM_VERSION 1

/* Timing */
#define LINK_AUDIO_SESSION_INTERVAL_MS     1000

/* Per-channel state with SPSC ring buffer */
typedef struct {
    uint8_t  channel_id[8];     /* 8-byte channel identifier from session */
    char     name[32];          /* Human-readable name (e.g. "1-MIDI", "Main") */
    int16_t  ring[LINK_AUDIO_RING_SAMPLES];
    volatile uint32_t write_pos;   /* updated by sendto thread (producer) */
    volatile uint32_t read_pos;    /* updated by consumer (ioctl or publisher) */
    volatile uint32_t sequence;    /* packet sequence counter */
    volatile int      active;      /* channel discovered and receiving data */
    volatile int16_t  peak;        /* peak absolute sample since last stats reset */
    volatile uint32_t pkt_count;   /* packets received since last stats reset */
} link_audio_channel_t;

/* Publisher per-channel output ring (for 128→125 repacketing) */
typedef struct {
    int16_t  ring[LINK_AUDIO_PUB_RING_SAMPLES];
    volatile uint32_t write_pos;
    volatile uint32_t read_pos;
    uint32_t sequence;          /* outgoing packet sequence */
    volatile int subscribed;    /* Live is requesting this channel */
    uint8_t  channel_id[8];    /* our generated channel ID */
    char     name[32];         /* e.g. "Shadow-1" */
} link_audio_pub_channel_t;

/* Global Link Audio state */
typedef struct {
    volatile int enabled;          /* Feature toggle from config */

    /* Move's identity (parsed from session announcements) */
    uint8_t move_peer_id[8];
    uint8_t session_id[8];
    volatile int session_parsed;   /* Set once we've parsed a session announcement */

    /* Move channels (intercepted via sendto hook) */
    volatile int move_channel_count;
    link_audio_channel_t channels[LINK_AUDIO_MOVE_CHANNELS];

    /* Network state captured from sendto hook */
    int move_socket_fd;                /* fd Move uses for sendto */
    struct sockaddr_in6 move_addr;     /* destination address from sendto (Live's addr) */
    struct sockaddr_in6 move_local_addr; /* Move's own local address (from getsockname) */
    socklen_t move_addrlen;
    volatile int addr_captured;        /* set once we have Move's network info */

    /* Publisher thread (sends shadow audio to Live) */
    volatile int publisher_running;
    pthread_t publisher_thread;
    int publisher_socket_fd;
    uint8_t publisher_peer_id[8];      /* our publisher peer ID */
    uint8_t publisher_session_id[8];   /* our session ID */
    link_audio_pub_channel_t pub_channels[LINK_AUDIO_SHADOW_CHANNELS];
    volatile int publisher_tick;       /* set by ioctl thread to wake publisher */

    /* Debug/stats */
    volatile uint32_t packets_intercepted;
    volatile uint32_t packets_published;
    volatile uint32_t underruns;
    volatile uint32_t overruns;   /* ring buffer overflow (producer too far ahead) */
} link_audio_state_t;

/* ============================================================================
 * Move → shim audio shared memory (written by link-subscriber sidecar,
 * read by shim). Replaces the sendto()-hook + in-process channel rings.
 * ============================================================================ */

#define SHM_LINK_AUDIO_IN  "/schwung-link-in"

/* Use the same block size as the pub side for symmetry. */
#define LINK_AUDIO_IN_BLOCK_FRAMES   LINK_AUDIO_PUB_BLOCK_FRAMES
#define LINK_AUDIO_IN_BLOCK_SAMPLES  LINK_AUDIO_PUB_BLOCK_SAMPLES

/*
 * THE IN RING IS SIZED FOR MOVE'S JITTER, NOT FOR SYMMETRY WITH THE PUB RING.
 *
 * It used to be `LINK_AUDIO_PUB_SHM_BLOCKS` (16 blocks = 4096 stereo samples =
 * 46 ms), inherited from the publish side because the two sit next to each
 * other in this header. The two directions do not have the same problem. We
 * write the pub ring on a metronome — one block per SPI frame. Move writes
 * this one in bursts.
 *
 * Measured on hardware 2026-08-27 with the sidecar's own delivery telemetry
 * (`cb slot=N max_gap=... max_burst_run=...`), under a load that made it
 * misbehave — osirus in a slot, drum kit on the Move track:
 *
 *     max_gap        = 92 ms      Move publishes nothing at all
 *     max_burst_run  = 30 blocks  then 30 callbacks back to back (~85 ms)
 *     avail max      = 13128 stereo samples = 149 ms
 *
 * All four channels stalled within 5 ms of each other, so it is one shared
 * stall in Move's publisher rather than per-channel jitter.
 *
 * A 46 ms ring cannot absorb a 92 ms stall — it is empty less than halfway
 * through — and it cannot hold the 85 ms burst that follows, so the producer
 * laps the consumer (`would_overrun` was ~51 per 5 s window). The result was
 * ~16% of frames with no Move audio at all.
 *
 * 64 blocks = 16384 stereo samples = 8192 frames = 186 ms, which is ~2x the
 * worst observed stall and ~1.25x the worst observed `avail`. Cost is 4 slots
 * x 32 KB = 128 KB of SHM, up from 32 KB.
 *
 * This does NOT add steady-state latency. The consumer sets the pace: it takes
 * one block per SPI frame whatever the ring holds, so mean `avail` stays where
 * Move's delivery puts it (~16 ms measured). A deeper ring only changes what
 * happens during a burst — absorbed instead of discarded.
 *
 * MUST stay a power of two: LINK_AUDIO_IN_RING_MASK is used as `& mask` by
 * both the sidecar's writer and the shim's reader.
 */
#define LINK_AUDIO_IN_RING_BLOCKS    64
#define LINK_AUDIO_IN_RING_SAMPLES   (LINK_AUDIO_IN_BLOCK_SAMPLES * LINK_AUDIO_IN_RING_BLOCKS)
#define LINK_AUDIO_IN_RING_MASK      (LINK_AUDIO_IN_RING_SAMPLES - 1)

/* The mask is only a valid substitute for a modulo if the ring is a power of
 * two, and this is the kind of constant somebody rounds to 50 later. */
#if (LINK_AUDIO_IN_RING_SAMPLES & LINK_AUDIO_IN_RING_MASK) != 0
#error "LINK_AUDIO_IN_RING_SAMPLES must be a power of two (see LINK_AUDIO_IN_RING_MASK)"
#endif

/*
 * Catch-up threshold: how far the producer may get ahead before the reader
 * gives up on the backlog and jumps to the newest block, dropping whatever
 * sat between. Every catch-up is an audible discontinuity.
 *
 * DERIVED from the ring, never written as a literal, because the two have to
 * move together: a threshold above the ring can never fire (the producer laps
 * us first and the data is already corrupt), and one far below the burst size
 * discards the very refill that was about to cover the next stall. The old
 * value was a bare `need * 12` at the call site, which is how it came to be
 * calibrated for a 30 ms burst and left there when bursts reached 85 ms.
 *
 * 3/4 of the ring: high enough to absorb Move's measured burst, low enough
 * that a runaway producer is still caught before it laps the reader.
 */
#define LINK_AUDIO_IN_CATCHUP_SAMPLES \
    (LINK_AUDIO_IN_RING_SAMPLES - (LINK_AUDIO_IN_RING_SAMPLES / 4))

#if LINK_AUDIO_IN_CATCHUP_SAMPLES >= LINK_AUDIO_IN_RING_SAMPLES
#error "catch-up threshold must sit inside the ring, or it can never fire"
#endif

/* Slots 0-3: per-track audio from Move. Slot 4 (Main) is reserved but
 * unsubscribed by link_subscriber to keep Move's Audio Worker threads free
 * from a publish we don't consume. The shim rebuilds Move's output from the
 * per-track slots under rebuild_from_la. */
#define LINK_AUDIO_IN_SLOT_COUNT   LINK_AUDIO_MOVE_CHANNELS
#define LINK_AUDIO_IN_MAIN_IDX     (LINK_AUDIO_MOVE_CHANNELS - 1)

typedef struct {
    int16_t  ring[LINK_AUDIO_IN_RING_SAMPLES];  /* stereo interleaved */
    volatile uint32_t write_pos;                /* sidecar (producer) */
    volatile uint32_t read_pos;                 /* shim (consumer) */
    volatile int      active;                   /* 1 once first packet received */
    char     name[32];                          /* "1-MIDI", "Main", … */

    /* Drop / jitter counters (v2). Monotonically increasing, reset by the
     * background logger when it reads them. Writers use relaxed atomics —
     * torn reads are harmless (pure telemetry). */
    volatile uint32_t starve_count;             /* reader: avail < need */
    volatile uint32_t catchup_count;            /* reader: avail > need*4 jump fired */
    volatile uint32_t catchup_samples_dropped;  /* reader: samples skipped by jump */
    volatile uint32_t max_avail_seen;           /* reader: peak pending samples */
    volatile uint32_t produced_count;           /* writer: source-callback invocations */
    volatile uint32_t would_overrun_count;      /* writer: ring lapped read_pos */
    volatile uint32_t max_frames_seen;          /* writer: peak num_frames per cb */
    uint32_t          _stats_pad[1];            /* keep 8-byte alignment */
} link_audio_in_slot_t;

/*
 * `magic` and `version` MUST stay the first two fields.
 *
 * The shim maps `sizeof(link_audio_in_shm_t)` from a segment the sidecar
 * owns. If a sidecar from an older build is still running, that segment is
 * SMALLER than the mapping, and touching the tail of an undersized mapping is
 * SIGBUS, not a read of zeroes. Reading offsets 0 and 4 is always safe (page
 * zero exists either way), so the version check below can reject the segment
 * and munmap before anything reaches `slots[]`. Move these fields to the end
 * and a sidecar/shim version skew becomes a crash loop instead of a warning.
 */
typedef struct {
    volatile uint32_t magic;    /* 0x4C41494E = "LAIN" */
    volatile uint32_t version;
    link_audio_in_slot_t slots[LINK_AUDIO_IN_SLOT_COUNT];
} link_audio_in_shm_t;

#define LINK_AUDIO_IN_SHM_MAGIC   0x4C41494E

/*
 * BUMP THIS WHENEVER THE LAYOUT OR THE RING SIZE CHANGES.
 *
 * 3: LINK_AUDIO_IN_RING_BLOCKS 16 -> 64 (46 ms -> 186 ms). The struct grew
 *    from 32 KB to 128 KB, so a v2 segment left behind by an old sidecar is
 *    not merely stale, it is too short for the new mapping.
 * 2: earlier layout.
 */
#define LINK_AUDIO_IN_SHM_VERSION 3

#endif /* LINK_AUDIO_H */
