# Link Audio Wire Format (Observed)

Captured from Move firmware 2.0 beta with Link Audio enabled, streaming to Ableton Live 12.4 beta.
Protocol prefix: `chnnlsv` — likely "channel server" v1.

## Setup

To enable per-track audio processing on Move 2.0+ devices:

1. **Enable Link on Move**: Settings > Link (toggle on)
2. **Enable Link Audio feature**: Set `"link_audio_enabled": true` in `/data/UserData/move-anything/config/features.json` (the installer configures this)
3. **USB connection is sufficient** — WiFi is not required. The Link SDK discovers peers over any non-loopback network interface, including Move's USB gadget ethernet (`usb0`)

Once enabled, the standalone `link-subscriber` binary joins Move's Link session and subscribes to all 5 audio channels. This triggers Move to stream per-track audio, which the shim's `sendto()` hook intercepts into ring buffers for shadow FX processing.

## Overview

- Transport: UDP over **IPv6 link-local** (between devices) or **IPv4 loopback** (same device)
- All packets prefixed with magic `chnnlsv` (7 bytes) + version byte `0x01`
- Move sends audio at ~353 packets/sec per channel
- Max packet size: 574 bytes
- Audio: 125 stereo frames of big-endian int16 per packet (125 x 2ch x 2bytes = 500 bytes payload)
- 353 x 125 = 44,125 ~ 44,100 Hz (confirmed via embedded sample rate field)

## Architecture

```
link-subscriber (standalone C++17 binary, Ableton Link SDK)
    | Joins Link session as "ME-Sub" peer
    | Discovers Move's channels via SDK callbacks
    | Creates LinkAudioSource per channel (sends ChannelRequests)
    | Dummy LinkAudioSink triggers PeerAnnouncement with channels
    v
Move Audio Engine
    | Sees subscriber in mPeerSendHandlers -> starts streaming
    | sendto() with chnnlsv packets (IPv4 loopback for same-device)
    v
sendto() hook in shim (LD_PRELOAD)
    | Intercepts audio -> per-channel SPSC ring buffers (BE->LE swap)
    v
Shadow FX chain slots
    | chain_set_inject_audio() routes per-track audio through FX
```

### Key Implementation Detail: Dummy Sink

Move's chnnlsv handler requires subscribers to appear in `mPeerSendHandlers`, which is populated from PeerAnnouncements containing channels (Sinks). A subscriber with no Sinks sends announcements with 0 channels, causing `forPeer()` to return `nullopt` and audio to be silently dropped.

The fix: create a `LinkAudioSink` ("ME-Sub-Ack") that we never use for audio. This causes the subscriber's PeerAnnouncements to include a channel, which populates `mPeerSendHandlers` for our peer ID.

## Move's Channels

Move announces **5 Link Audio channels** via session messages:

| Channel | Example Name | Notes |
|---------|-------------|-------|
| Track 1 | `1-MIDI` | Name changes with track type |
| Track 2 | `2-MIDI` | e.g. becomes `2-Audio` for audio tracks |
| Track 3 | `3-MIDI` | |
| Track 4 | `4-Audio` | |
| Main | `Main` | Stereo mix of all tracks |

Channel names are **dynamic** — they update when you switch a track between MIDI and Audio types in Move's UI.

Channels are **demand-driven**: audio is only transmitted when at least one peer subscribes by sending ChannelRequest messages. With no subscribers, zero network bandwidth is used.

## Packet Types

| MsgType | Name | Size | Description |
|---------|------|------|-------------|
| 1 | Session Announcement | ~168 bytes | Peer info + channel list, sent periodically |
| 3 | Pong/Heartbeat | 36 bytes | Subscription keepalive from receiver |
| 4 | Channel Request | 28 bytes | Subscribe to a specific channel |
| 5 | Stop Channel Request | 28 bytes | Unsubscribe from a channel |
| 6 | Audio Data | 574 bytes | One block of audio samples |

## Audio Data Packet (msg_type=6)

574 bytes total = 74-byte header + 500-byte audio payload.

### Common Header (12 bytes)

```
Offset  Size  Type     Field
------  ----  ----     -----
 0       7    char[7]  Magic: "chnnlsv"
 7       1    u8       Version: 0x01
 8       1    u8       MsgType: 0x06 (audio data)
 9       1    u8       Flags: 0x00
10       2    u16 BE   Reserved: 0x0000
```

### Identity Block (24 bytes)

```
Offset  Size  Type     Field
------  ----  ----     -----
12       8    u8[8]    PeerID - sender's peer identifier
20       8    u8[8]    ChannelID - identifies which track/channel
28       8    u8[8]    SourcePeerID - originator (same as PeerID for direct send)
```

### Audio Metadata (38 bytes)

```
Offset  Size  Type     Field
------  ----  ----     -----
36       4    u32 BE   Unknown (always 1, possibly stream version)
40       4    u32 BE   Reserved (always 0)
44       4    u32 BE   Sequence number (per-channel, incrementing)
48       2    u16 BE   NumFrames: 125
50       2    u16 BE   Padding: 0
52       8    u64 BE   Timestamp (diff = 414,842,880 per block; units TBD)
60       4    u32 BE   Audio descriptor (always 6)
64       3    u8[3]    Unknown constant (0xd5, 0x11, 0x01)
67       4    u32 BE   SampleRate: 44100
71       1    u8       NumChannels: 2 (stereo)
72       2    u16 BE   PayloadSize: 500 (bytes of audio data)
```

### Audio Payload (500 bytes)

```
Offset  Size  Type       Field
------  ----  ----       -----
74     500    int16 BE   125 stereo frames, interleaved [L0,R0,L1,R1,...,L124,R124]
```

Samples are signed 16-bit big-endian integers, same range as standard PCM16 (-32768 to +32767).

## Session Announcement (msg_type=1)

Sent periodically by each peer. TLV-encoded after the common header + PeerID:

```
Offset  Field
------  -----
 0-11   Common header (magic, version, msg_type=1, flags, reserved)
12-19   PeerID
20+     TLV entries:

Tag     Field
----    -----
"sess"  Session ID (8 bytes) - shared by all peers in a Link session
"__pi"  Peer Info: u32 name_len + UTF-8 name (e.g. "Move", "Live")
"auca"  Audio Channel Announcements:
          u32 num_channels
          For each channel:
            u32 name_len
            char[name_len] name (e.g. "1-MIDI", "Main")
            u8[8] channel_id
"__ht"  Heartbeat timestamp (8 bytes)
```

### TLV Format

Each entry: `tag(4 bytes ASCII) + length(u32 BE) + data(length bytes)`

### Example: Move's Session Announcement

```
sess: 396c295a3b28776e (session ID)
__pi: name="Move"
auca: 5 channels
  "1-MIDI"  -> 3b684021727e4076
  "2-MIDI"  -> 646579414b723729
  "3-MIDI"  -> 4d563741223c6657
  "4-Audio" -> 52313f27594f7838
  "Main"    -> 6a214d3167715335
__ht: 00064a9d949749e5
```

## Network Details

### Link Peer Discovery (`_asdp_v1` protocol)

Link Audio requires an active Link session. Move only starts the chnnlsv audio
protocol when it sees at least one Link peer. Peer discovery uses a **separate
protocol** from chnnlsv:

- Protocol: `_asdp_v1` (Ableton Session Discovery Protocol v1)
- Transport: UDP multicast
- **IPv4**: `224.76.78.75:20808`
- **IPv6**: `ff12::8080` port `20808` (link-local scope, requires interface scope_id)
- Move binds 3 sockets to port 20808 (one per interface: lo, wlan0, usb0)

#### ALIVE Message Format (93 bytes)

Peers send ALIVE heartbeats every few seconds to the multicast group. When Move
sees an ALIVE message from another peer, it considers the Link session active and
enables Link Audio.

```
Offset  Size  Type       Field
------  ----  ----       -----
 0       8    char[8]    Protocol header: "_asdp_v1"
 8       1    u8         Message type: 0x01 (ALIVE), 0x02 (RESPONSE)
 9       1    u8         TTL: seconds before peer is considered dead (e.g. 60)
10       2    u16 BE     Session group ID: 0x0000
12       8    u8[8]      Node ID: unique 8-byte peer identifier

--- Payload entries (key + size + value) ---

20       4    u32 BE     Key: 'tmln' (0x746d6c6e) - Timeline
24       4    u32 BE     Size: 24
28       8    i64 BE     Tempo: microseconds per beat (500000 = 120 BPM)
36       8    i64 BE     Beat origin (micro-beats, 1e6 per beat)
44       8    i64 BE     Time origin (microseconds)

52       4    u32 BE     Key: 'sess' (0x73657373) - Session membership
56       4    u32 BE     Size: 8
60       8    u8[8]      Session ID

68       4    u32 BE     Key: 'stst' (0x73747374) - Start/Stop state
72       4    u32 BE     Size: 17
76       1    u8         Is playing: 0x00 or 0x01
77       8    i64 BE     Beats (micro-beats)
85       8    i64 BE     Timestamp (microseconds)
```

**Total: 93 bytes.** All three payload entries (Timeline, Session, StartStopState) are required.

#### Key details

- **Tempo** is stored as microseconds per beat: `60,000,000 / BPM`
- **Node ID** must be unique per peer instance (random 8 bytes)
- **Session ID** identifies which session to join (random 8 bytes)
- **TTL** of 60 means peers consider you dead after 60 seconds without a heartbeat
- When a peer receives ALIVE, it sends back a **RESPONSE** (msg_type=0x02, same format)

### Audio Transport

- Audio packets sent via **UDP unicast** — IPv6 link-local between devices, IPv4 loopback for same-device subscribers
- One socket per address family (fd 47 for IPv4 control, fd 49 for IPv6 audio+control)
- Destination: subscriber's address (e.g. `fe80::149a:5b7a:8b9c:8773` for Live, `127.0.0.1` for local subscriber)
- All audio sent from a single thread
- `sendto()` syscall — not `send()` on connected sockets
- **Move's listening port** is the source port from `getsockname()` on the sendto socket — NOT the destination port (that's the peer's port)

### Bandwidth

Per subscribed channel: 574 bytes x 353 packets/sec ~ **202 KB/sec** (1.6 Mbps)
All 5 channels: ~ **1.0 MB/sec** (8.1 Mbps)

## Observations

- Mono synths send L==R (identical left/right channels)
- Silent tracks still transmit all-zero audio when subscribed
- Zero sequence gaps observed — reliable delivery within LAN
- Audio continuity between consecutive packets is smooth (small inter-packet deltas)
- Each channel has an independent sequence counter

## Key Files

| File | Description |
|------|-------------|
| `src/host/link_subscriber.cpp` | Standalone subscriber (C++17, Ableton Link SDK) |
| `src/host/link_audio.h` | Ring buffer structures, protocol constants |
| `src/move_anything_shim.c` | sendto() hook, session parser, ring buffers, FX routing |
| `libs/link/` | Ableton Link SDK (git submodule, GPL v2+) |

## Implementation Status

### Working

- **Standalone subscriber** (`link-subscriber`): joins Link session via SDK, discovers channels, subscribes to all 5 using `LinkAudioSource`. Triggers Move to stream audio without needing Live connected.
- **sendto() hook**: intercepts `chnnlsv` packets (msg_type=1 session, msg_type=6 audio)
- **TLV session parser**: extracts channels from "auca" tag, captures Move's network info
- **Auto-discovery fallback**: learns channels from audio packets if session parsing misses them (IPv4 loopback case)
- **Per-channel SPSC ring buffers**: 512 frames, lock-free, with BE->LE byte swap
- **Move audio -> shadow FX routing**: reads per-track audio from ring buffers, injects into shadow chain slots via `chain_set_inject_audio()` before FX processing
- **Feature toggle**: `"link_audio_enabled": true` in `/data/UserData/move-anything/config/features.json`
- **Heartbeat diagnostics**: `la_pkts` and `la_ch` counters in shim heartbeat log

### Not Yet Working

- **Shadow audio publishing to Live**: the raw chnnlsv publisher lacks proper Link SDK peer discovery, so Live cannot see the "ME" peer. Needs SDK integration (like the subscriber) to appear as a discoverable peer. Currently disabled.
- **Shadow UI**: no UI for enabling/disabling per-slot Move audio FX routing
- **Per-slot toggle**: currently all active slots with matching Move channels get injection; should be configurable

## Lessons Learned

1. **Dummy Sink required**: Move's `mPeerSendHandlers` map is only populated from PeerAnnouncements that contain channels (Sinks). A subscriber without any Sinks is invisible to Move's audio routing.

2. **Source creation not safe in callbacks**: Creating `LinkAudioSource` inside `setChannelsChangedCallback` holds `mCallbackMutex`, causing deadlock. Must defer to main loop.

3. **Vector reallocation crashes**: `std::vector::emplace_back` can move existing `LinkAudioSource` objects (triggering destructor + move constructor), causing SIGSEGV. Fix: `reserve()` before emplacing.

4. **IPv4 loopback for same-device**: When subscriber runs on the same device as Move, audio flows over IPv4 127.0.0.1 (not IPv6 link-local). Session parsing must not require IPv6 `addr_captured`.

5. **Standalone process beats in-shim approach**: Running the subscriber as a separate binary (~190 lines of C++) avoids hook conflicts that caused SIGSEGV when trying to self-subscribe from inside the LD_PRELOAD shim (~1400 lines of buggy C).
