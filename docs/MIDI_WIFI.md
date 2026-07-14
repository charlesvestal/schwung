# MIDI over Wi-Fi

## Purpose

Schwung can expose Ableton Move as a network MIDI endpoint without requiring a
USB MIDI interface. The feature is optional and disabled by default.

The implementation supports:

- ipMIDI input on multicast group `225.0.0.37`, UDP port `21928`.
- AppleMIDI/RTP-MIDI input and output on UDP ports `5004` (control) and `5005`
  (MIDI data).
- Bonjour/mDNS advertisement as `_apple-midi._udp.local` so compatible clients
  can discover the Move automatically.

## User behavior

`Global Settings > Services > MIDI / Wi-Fi` enables or disables the service.
The value is stored as `midi_net_enabled` in
`/data/UserData/schwung/config/features.json` and survives upgrades. Turning the
setting off closes the UDP sockets and drops all AppleMIDI sessions.

Inbound network MIDI is treated as external cable-2 MIDI. Channel voice,
system-common, real-time, and SysEx messages up to 192 bytes are converted to
USB-MIDI packets and use the existing MIDI injection path. As a result:

- Move routes channel MIDI to its native tracks in the same way as USB-A MIDI.
- Schwung chain slots receive it through the existing channel/MPE routing.
- A foreground overtake module receives it through
  `onMidiMessageExternal(data)`.

Outbound cable-2 packets produced by Schwung UI modules or overtake DSP modules
are copied to established AppleMIDI peers. They continue to reach the physical
USB MIDI output as well; enabling Wi-Fi does not silently change the meaning of
the USB port. Outbound SysEx is not included in the first implementation.

## Realtime and concurrency requirements

- Socket operations, service discovery, session negotiation, and lifecycle
  changes run on non-realtime threads pinned away from SPI core 3.
- The network receive thread publishes into the existing bounded MPSC MIDI
  injection queue. It never writes the SPI mailbox directly.
- Realtime MIDI output paths publish to a bounded lock-free queue. The network
  thread performs all `sendto()` calls; realtime paths never take a mutex or
  wait for the network.
- Queue overflow drops the newest packet and increments a diagnostic counter.
  It must never corrupt an existing packet or block the SPI callback.
- SysEx is reserved as one bounded queue batch; if the complete message will
  not fit, it is dropped rather than injecting a truncated prefix.
- Starting and stopping the service is reconciled by the shim worker, not by an
  audio or SPI callback.

## AppleMIDI session scope

The responder accepts up to four simultaneous peers, handles the two-port
invitation handshake, clock synchronization, receiver feedback (ignored
because recovery journals are not implemented), session goodbye, idle-session
expiry, and RTP-MIDI command sections with running status and delta times.
Recovery journals are ignored; UDP loss is not reconstructed.

RTP packets are accepted only from the event address negotiated for the
matching SSRC. Datagram and command-section bounds are validated before they
are parsed; malformed command fragments are dropped and counted.

## Acceptance criteria

1. A clean cross-build includes the network MIDI sources in the shim.
2. Enabling and disabling the setting starts and stops the service without
   touching the realtime path or requiring a reboot.
3. Unit tests cover raw MIDI parsing, running status, real-time interleaving,
   SysEx segmentation, RTP-MIDI parsing, malformed packets, and lock-free queue
   overflow/order behavior.
4. Existing host/shadow/build tests continue to pass, apart from independently
   documented baseline failures.
5. The code contains no socket calls, filesystem calls, allocation, blocking
   waits, or mutex acquisition in the SPI/audio publishing path.
