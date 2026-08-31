# SysEx on Schwung

What the two directions actually do, measured on hardware rather than read out
of the source. Written after #355, #358 and #361, because three people had
three different theories and all of them were about the wrong direction.

**SysEx IN and SysEx OUT fail for unrelated reasons.** Half the confusion in
the issue threads comes from treating them as one feature. Decide which one you
have before reading further.

## The rig, so anyone can reproduce this

Mac ↔ Move over **USB-C**, the **"Ableton Move Standalone Port"**. No external
MIDI hardware needed.

Measured 2026-08-31 with the shim's own `INsys` tap: USB-C MIDI **does** reach
the SPI mailbox, it lands on **cable 2** — the same cable as USB-A external
MIDI — and only that one of Move's four USB-C ports carries it. The other three
(Live, User, External) produce nothing in the mailbox.

```bash
ssh ableton@move.local 'touch /data/UserData/schwung/log_xmos_sysex_on'
# ... send ...
ssh ableton@move.local 'rm -f /data/UserData/schwung/log_xmos_sysex_on'
scp ableton@move.local:/data/UserData/schwung/xmos_sysex.txt .
```

The tap prints `PRE` (what Schwung queued outbound), `POSThw` (what XMOS left
after the transfer) and `INsys` (inbound, at the correct 8-byte stride, with
cable numbers).

**Disarm it when you stop.** It `write()`s on the SPI callback, which is a
realtime violation it is only allowed because it is off by default and capped.
And **do not delete `xmos_sysex.txt` while the flag is still set** — the shim
closes the fd on a later poll, so an early delete sends the whole capture into a
deleted inode and reads back as "nothing arrived", which looks exactly like the
failure you are hunting. Disarm, wait ~2.5 s, then delete.

## IN: the ceiling is the sender's BURST RATE, not the message size

Inbound SysEx truncates, silently, with framing intact and no `F7`.

| payload | packets | result |
|---|---|---|
| 158 B | 54 | INTACT |
| 250 B | 85 | INTACT |
| 316 B | 107 | INTACT |
| 400 B | 128 | **truncated at 381 B** |
| 512 B | 128 | **truncated at 381 B** |
| 632 B | 128 | **truncated at 381 B** |

Identical cut at three different sizes looks like a hard 384-byte message
limit. It is not:

| condition | result |
|---|---|
| one 632 B message | truncated at 381 B |
| two 316 B, back to back | first INTACT, second truncated at 237 B |
| two 316 B, **100 ms apart** | **both INTACT** (214 packets) |

The same total payload arrives complete when it is paced. So the limit is how
much can be **in flight** before the SPI drain keeps up — a FIFO in the XMOS,
below anything Schwung controls. Nothing in the host can widen it, and no host
change will.

### A sustained run, which is the shape that actually matters

One message says nothing about a bulk dump. #358 is 405 messages of 158 bytes
back to back, so the test that counts is a long run at a realistic rate.
100 × 158 B, scored per message:

| condition | rate | intact | short | corrupt |
|---|---|---|---|---|
| DIN rate (50 ms gap) | 3019 B/s | **100 / 100** | 0 | 0 |
| 5× DIN (10 ms gap) | 11730 B/s | **100 / 100** | 0 | 0 |
| flat out (no gap) | 3.0 MB/s | 2 | 1 | 0 |

**5400 packets at DIN rate with zero loss**, and the same again at nearly four
times DIN. So on a build carrying #355 and #361 the inbound path is clean at
every rate a MIDI device can actually produce; only an instantaneous burst
reaches the FIFO ceiling, and nothing on a MIDI cable bursts like that.

This is worth stating plainly because it **does not reproduce #358**. That
report is ~0.8% loss over USB-A/DIN at ~3125 B/s — the rate that is perfect
here. Whatever is dropping those packets is on the USB-A side (the interface,
or the XMOS's USB-host path), not in the shared code both routes run through.

**This is why it has never been seen over DIN.** DIN MIDI is 3125 bytes/s, so a
158-byte message occupies ~50 ms and spreads over ~17 SPI frames — about three
packets per frame, never 53. A DIN device cannot burst hard enough to reach the
ceiling. The QY-70 loss in #358 is a different fault and this measurement does
not explain it. USB-C senders *can* burst that hard, which is the only reason
this is reachable at all.

Everything received below the ceiling was **byte-perfect** — first bad offset
`-1` in every intact row, including an aligned `00 00 00` run at body offset
60 that is exactly the packet #355 used to drop. That doubles as an on-hardware
confirmation of #355 independent of the JV-880.

## IN: and only an overtake tool ever sees it

Both cable-2 dispatchers gate on `cin < 0x08 || cin > 0x0E`
(`shadow_midi.c:832`, `:948`), which excludes the SysEx CINs `0x04`–`0x07`
*before* any channel filter. The one SysEx-carrying path to JS,
`shadow_ui_midi_publish`, sits inside `if (overtake_mode && shadow_ui_midi_shm)`
in `schwung_shim.c`.

**A chain slot is therefore write-only for SysEx.** If you are building an
editor as a slot module and waiting for the device to answer, that is the whole
bug — no amount of fixing your encoding will help. Build it as an overtake
tool, or expect send-only. `modules/midi_fx/sysex_probe` exists to keep that
honest: it emits an ECHO the moment an `F0` reaches `process_midi`, so the
claim is falsifiable rather than a code reading.

`onMidiMessageExternal([b0,b1,b2])` hands you three bytes with the **CIN already
stripped**, and nothing reassembles the run for you. There is no length field
to trust — an end-packet's trailing bytes are padding. Your assembler needs
four behaviours, and the last two are the ones people skip:

- start on `0xF0`, complete on `0xF7`
- **skip anything ≥ `0xF8`** — realtime legitimately interleaves mid-SysEx
- **abort on any other status byte ≥ `0x80`** — your message was interrupted,
  and splicing what follows onto it produces a plausible third message that is
  pure fiction
- cap the buffer, or one dropped `F7` leaks forever

## OUT: two doors, and they behave differently

| caller | path | on overflow |
|---|---|---|
| JS (`move_midi_external_send`) | shadow_ui SHM → `shadow_inject_ui_midi_out` | **held in the carry**, then dropped + counted |
| DSP (`host->midi_send_external`) | `ext_midi_ring` → mailbox | held in the ring, then drop-newest + counted |

Both take **flat 4-byte USB-MIDI packets**, not raw SysEx bytes — a CIN byte
followed by three data bytes, concatenated into one array. Hand either of them
`[0xF0, 0x00, 0x21, …]` and it goes out as garbage. `F0` and `F7` are ordinary
data to the packetizer, so include them in the message you packetize:

| CIN | meaning | data bytes used |
|---|---|---|
| `0x04` | SysEx continues | 3 |
| `0x05` | ends with one byte | 1 |
| `0x06` | ends with two bytes | 2 |
| `0x07` | ends with three bytes | 3 |

Leave the cable nibble at 0; the host routes to cable 2 itself. One aside for
anyone debugging with notes instead: at cable 0 a `0x90` or `0xB0` status is
intercepted as an LED write (`shadow_midi.c:513`) and never reaches the wire.

Until 2026-08-31 the JS door **destroyed** anything past the 20 packets that fit
in one frame: `shadow_inject_ui_midi_out` memset its source before placing
anything and walked off the end of the mailbox. See `ui_midi_out_carry.h` for
the full account. The ceiling was ~20 packets per 60 Hz tick = 3600 bytes/s
against DIN's 3125 — 15% of margin, no backpressure, silent loss past it. It is
now ~20 per **frame** (344 Hz), with the remainder carried in order.

**A `false` return means retry.** It has always meant that; it just could not
fire usefully before. Now it is the backpressure signal: above high water the
host stops draining the SHM buffer, the buffer fills, and your send returns
`false`. A module that paces on it is pacing on the actual mailbox.

## Testing your own module

Two probes ship in-tree, scoring against one shared generator so a result from
either is directly comparable:

- `src/modules/tools/sysex-test` — the JS/tool path. Pads pick a payload size
  (64 / 158 / 316 / 632 B); any other pad sends. The screen shows TX bytes and
  packets, whether the host refused, and a verdict on anything received.
- `src/modules/midi_fx/sysex_probe` — the slot path. A note-on emits a dump
  sized by velocity; an inbound `F0` emits an ECHO. It answers **on the wire**
  rather than in a counter, because every readout available on the SPI callback
  is either an RT violation or needs somebody watching the OLED.

Order to test in, cheapest first — three failures look identical from outside:

1. **Universal Identity Request**, `F0 7E 7F 06 01 F7`. Six bytes, two packets,
   no device-specific knowledge. If the device answers, your encoding and your
   cable are both fine.
2. **Then** the device-specific message. If identity works and yours does not,
   the problem is your message content, not Schwung.
3. **Then** the logs. `ui-midi:` is the inbound ring dropping; `ui-midi-out:` is
   the outbound carry dropping; `ext-midi:` is the DSP ring. Their *absence*
   during a dump is the pass condition.

## Known-unknown

Schwung addresses a **single port** on USB-A. A device that enumerates as a
composite with several MIDI ports and listens on the second one is, as far as
anyone has established, unreachable. Nobody has confirmed this against a
multi-port device.
