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
#define LINK_AUDIO_MSG_REQUEST  3
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
    int move_channel_count;
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
} link_audio_state_t;

#endif /* LINK_AUDIO_H */
