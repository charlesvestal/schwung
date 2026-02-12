# Link Audio Interception and Publishing

## Context

Move firmware 2.0 beta publishes 5 Link Audio channels over UDP/IPv6 (4 per-track + Main mix). We reverse-engineered the wire format (see `LINK_AUDIO_WIRE_FORMAT.md` in the parent directory). This plan adds:

1. **sendto() hook** — intercept per-track audio at zero latency inside the shim
2. **Self-subscriber** — trigger audio transmission without Live connected
3. **Shadow audio publishing** — broadcast shadow slot audio as additional Link Audio channels visible in Live

This unlocks routing Move's native per-track audio through shadow FX (future Phase 2) and gives Live access to shadow slot audio.

## Architecture Overview

```
Move Audio Engine (tracks 1-4 + Main)
    │ sendto() with chnnlsv packets
    ▼
sendto() hook (intercepts, copies to ring buffers, passes through)
    │                                    │
    ▼                                    ▼
Per-track ring buffers              Original packet → Live
(5 channels, lock-free SPSC)
    │
    ▼ (future Phase 2: shadow FX processing)

Self-subscriber thread ──── ChannelRequest heartbeats ────→ Move's Link Audio
(triggers transmission without Live)

Shadow publish thread ──── chnnlsv audio packets ────→ Live
(shadow slot audio as "ME Shadow-1..4" channels)
```

## Part 1: sendto() Hook + Per-Channel Ring Buffers

**File:** `move-anything/src/move_anything_shim.c`

### 1a. New header: `src/host/link_audio.h`

Constants and structures for Link Audio interception:

```c
#define LINK_AUDIO_MAGIC "chnnlsv"
#define LINK_AUDIO_MSG_SESSION  1
#define LINK_AUDIO_MSG_REQUEST  3
#define LINK_AUDIO_MSG_AUDIO    6
#define LINK_AUDIO_HEADER_SIZE  74
#define LINK_AUDIO_PAYLOAD_SIZE 500
#define LINK_AUDIO_PACKET_SIZE  574
#define LINK_AUDIO_FRAMES_PER_PACKET 125
#define LINK_AUDIO_MAX_CHANNELS 9   /* 5 Move + 4 shadow */

/* Lock-free SPSC ring buffer per channel.
 * 512 frames = ~11.6ms at 44100 Hz, absorbs 125-vs-128 frame mismatch. */
#define LINK_AUDIO_RING_FRAMES  512
#define LINK_AUDIO_RING_SAMPLES (LINK_AUDIO_RING_FRAMES * 2)  /* stereo */
#define LINK_AUDIO_RING_MASK    (LINK_AUDIO_RING_SAMPLES - 1)

typedef struct {
    uint8_t  channel_id[8];
    char     name[32];
    int16_t  ring[LINK_AUDIO_RING_SAMPLES];
    volatile uint32_t write_pos;   /* sendto thread */
    volatile uint32_t read_pos;    /* ioctl thread */
    volatile uint32_t sequence;
    volatile int      active;
} link_audio_channel_t;

typedef struct {
    volatile int enabled;
    uint8_t peer_id[8];
    uint8_t session_id[8];
    int channel_count;
    link_audio_channel_t channels[LINK_AUDIO_MAX_CHANNELS];
    /* Self-subscriber state */
    volatile int subscriber_running;
    pthread_t subscriber_thread;
    int subscriber_socket_fd;
    struct sockaddr_in6 move_addr;  /* captured from sendto dest */
    uint16_t move_port;             /* Move's listening port */
    /* Publisher state */
    volatile int publisher_running;
    pthread_t publisher_thread;
    /* ... */
} link_audio_state_t;
```

### 1b. sendto() hook

Follow existing `connect()`/`send()` pattern (shim line ~1800-1878):

```c
static ssize_t (*real_sendto)(...) = NULL;

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    if (!real_sendto) real_sendto = dlsym(RTLD_NEXT, "sendto");
    if (!link_audio.enabled) return real_sendto(...);

    const uint8_t *p = buf;
    if (len >= 12 && memcmp(p, "chnnlsv", 7) == 0) {
        uint8_t msg_type = p[8];
        if (msg_type == 6 && len == 574)
            link_audio_intercept_audio(p);
        else if (msg_type == 1)
            link_audio_parse_session(p, len, sockfd, dest_addr, addrlen);
    }
    return real_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}
```

### 1c. Audio interception (runs on audio thread, must be fast)

- Extract ChannelID at offset 20, find matching channel in state
- Byte-swap 125 frames of BE int16 → LE int16, write to ring buffer
- Memory barrier before publishing write_pos
- ~250 int16 swaps per packet — negligible cost

### 1d. Session announcement parser

- Triggered on msg_type=1 packets in sendto hook
- Parse TLV: extract "sess" (session ID), "__pi" (peer name), "auca" (channel list)
- Populate `link_audio.channels[]` with channel IDs and names
- Capture `sockfd` and `dest_addr` for self-subscriber bootstrapping
- Also capture Move's source port via `getsockname(sockfd)` for subscriber targeting

### 1e. Ring buffer read (called from ioctl thread)

```c
int link_audio_read_channel(int idx, int16_t *out, int frames) {
    // Read up to `frames` stereo samples from ring
    // Return 1 if enough data, 0 if underrun (zero-fill)
    // Uses __sync_synchronize() memory barriers
}
```

**Block size mismatch (125 vs 128)**: The ring buffer naturally absorbs this. sendto writes 125 frames at ~353 Hz. ioctl reads 128 frames at ~344 Hz. Both produce ~44100 samples/sec. The ring level fluctuates by a few frames — well within the 512-frame buffer.

## Part 2: Self-Subscriber

**File:** `move-anything/src/move_anything_shim.c`

### How it works

Move only sends audio when a peer subscribes via ChannelRequest (msg_type=3) heartbeats. The self-subscriber creates its own IPv6 UDP socket and poses as a separate peer.

### Implementation

- **Starts after** first session announcement is parsed (we need Move's address/port)
- Creates IPv6 UDP socket, binds to same link-local interface
- Generates a unique PeerID (random 8 bytes, distinct from Move's)
- Every ~500ms, sends 36-byte ChannelRequest packets for all 5 Move channels
- Packet format: common header (msg_type=3) + subscriber PeerID + "__ht" heartbeat TLV
- Sends to Move's listening address (captured from session announcements)

### Bootstrap sequence

1. Link enabled in settings → `link_audio.enabled = 1`
2. Move sends session announcements periodically (even without subscribers)
3. sendto hook captures session → parses channels, captures Move's address
4. Self-subscriber thread starts → sends ChannelRequests
5. Move starts sending audio → sendto hook captures per-track audio into ring buffers

## Part 3: Shadow Audio Publishing

**File:** `move-anything/src/move_anything_shim.c`

### Architecture decision: Separate peer

Publish shadow audio as a **separate Link Audio peer** (not modifying Move's packets):
- Peer name: "ME" (or "Move Everything")
- Up to 4 channels: "Shadow-1" through "Shadow-4"
- Own PeerID, own socket, own session announcements
- If shim crashes, Move's Link Audio continues unaffected
- Live sees two peers: "Move" (5 channels) + "ME" (4 channels)

### Per-slot audio capture

Add per-slot output buffers in `shadow_inprocess_mix_audio()`:

```c
static int16_t shadow_slot_capture[SHADOW_CHAIN_INSTANCES][FRAMES_PER_BLOCK * 2];
```

After each slot's `render_block()`, copy to capture buffer before mixing.

### Publisher thread

- Creates own IPv6 UDP socket
- Sends session announcements every ~1 second (msg_type=1, "auca" with shadow channels)
- Listens for incoming ChannelRequest messages from Live (non-blocking recvfrom)
- When a channel has subscribers, sends audio packets at ~353/sec
- Reads from per-slot capture buffers
- Converts LE int16 → BE int16, constructs 574-byte packets
- 128→125 frame conversion: use small output ring buffer per shadow channel

### Timing

Publisher thread wakes on each ioctl tick (~344 Hz) via atomic flag set by the ioctl hook. On each wake, it sends one 125-frame packet per subscribed channel (accumulating 3 extra frames from each 128-frame render block, draining them periodically).

## File Changes

### New files
- `src/host/link_audio.h` — protocol constants, ring buffer structs, state struct

### Modified files
- `src/move_anything_shim.c` — sendto() hook, state init, ring buffer ops, subscriber thread, publisher thread, per-slot capture in mix path
- `src/host/shadow_constants.h` — (if needed) add SHM segment for sharing Link Audio channel list with shadow UI

### Not modified (this phase)
- `chain_host.c` — no FX routing changes yet
- `shadow_ui.js` — no UI changes yet (Phase 2)

## Implementation Order

1. Add `link_audio.h` with constants and structures
2. Add `link_audio_state_t` to shim, init in `init_shadow_shm()`
3. Add sendto() hook with session parser (log-only first, verify detection)
4. Add per-channel ring buffers and audio interception
5. Add `link_audio_read_channel()` — verify data by logging sample stats
6. Add self-subscriber thread
7. Add per-slot capture buffers in `shadow_inprocess_mix_audio()`
8. Add publisher thread (session announcements + audio packets)
9. Feature toggle in shadow config JSON

## Verification

1. **Build**: `cd move-anything && ./scripts/build.sh` — shim compiles with new sendto hook
2. **Deploy**: `./scripts/install.sh local --skip-modules --skip-confirmation`
3. **Enable Link** on Move in Settings
4. **Without Live**: Check logs for "Link Audio: session parsed, N channels discovered" and "Link Audio: subscriber started, audio flowing"
5. **With Live**: Open Live → Link Audio sources → should see "Move" (5ch) + "ME" (4ch)
6. **Audio verification**: Play audio on Move tracks → verify per-track ring buffers have non-zero data (add debug logging that prints RMS per channel periodically)
7. **Shadow publishing**: Load shadow patches → Live should receive shadow audio on "ME" channels

## Future: Phase 2 — Move Track FX Routing

Not in this plan. Design options for later:
- **Per-slot Move Audio FX**: each shadow slot gets an optional parallel FX chain for its corresponding Move track. Slot configuration page shows "Move Audio FX" alongside the existing synth FX.
- **8 separate outs**: 4 shadow DSP+FX + 4 Move track+FX, all publishable as Link Audio channels
- Requires extending chain host or adding FX processing directly in the shim

## Wire Format Reference

See `/Volumes/ExtFS/charlesvestal/github/move-everything-parent/LINK_AUDIO_WIRE_FORMAT.md` for the complete observed wire format including:
- Audio Data packet structure (msg_type=6, 574 bytes)
- Session Announcement TLV format (msg_type=1)
- Channel Request format (msg_type=3, 36 bytes)
- Network details (IPv6 link-local, multicast discovery on 224.76.78.75:20808)
- Bandwidth estimates (~202 KB/sec per channel)
