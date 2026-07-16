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
setting off destroys the RTP-MIDI server and Avahi publication, closes the
ipMIDI socket, and drops all AppleMIDI sessions. The optional C++ adapter stays
loaded so a later toggle can restart the service without loading code on a
realtime thread.

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
- Queue overflow drops the newest packet. It must never corrupt an existing
  packet or block the SPI callback.
- SysEx is reserved as one bounded queue batch; if the complete message will
  not fit, it is dropped rather than injecting a truncated prefix.
- Starting and stopping the service is reconciled by the shim worker, not by an
  audio or SPI callback.

## Implementation boundary

Schwung dynamically loads `libschwung-rtpmidi.so` only when the setting is
enabled. That small C-ABI adapter owns a pinned `librtpmidid` `rtpserver_t` and
`mdns_rtpmidi_t`. The library is responsible for AppleMIDI negotiation, peer
lifecycle, clock sync, RTP framing and parsing, SysEx reassembly, outbound
delivery, and Avahi publication. Schwung retains only its USB-MIDI conversion,
injection queue, realtime outbound queue, ipMIDI parser, settings, and UI glue.

The library poller runs on Schwung's existing non-realtime network thread. Its
epoll set also contains the ipMIDI socket and stop pipe; the outbound queue is
drained at least every 5 ms. If the adapter or one of its optional shared
libraries cannot load, the main shim continues and ipMIDI remains available.
Failure to connect to Avahi affects Bonjour discovery but not direct AppleMIDI
connections or ipMIDI.

The pinned source is `libs/rtpmidid` at commit
`7f552d2e171465782fa10e6ad35116ff40bc9f66`. The adapter, rtpmidid, Avahi client,
and fmt libraries are dynamically linked; their notices are included in the
package and documented in `THIRD_PARTY_LICENSES.md`.

## Acceptance criteria

1. A clean cross-build produces the optional adapter and bundles its pinned
   rtpmidid, Avahi client, and fmt shared-library dependencies.
2. Enabling and disabling the setting starts and stops the service without
   touching the realtime path or requiring a reboot.
3. Portable unit tests cover raw ipMIDI parsing, running status, real-time
   interleaving, SysEx injection atomicity, fake-adapter lifecycle, and
   lock-free queue overflow/order behavior. A Linux integration test covers the
   real adapter's two-port AppleMIDI handshake, inbound note and SysEx,
   outbound note, disconnect, and restart.
4. Existing host/shadow/build tests continue to pass, apart from independently
   documented baseline failures.
5. The code contains no socket calls, filesystem calls, allocation, blocking
   waits, or mutex acquisition in the SPI/audio publishing path.
