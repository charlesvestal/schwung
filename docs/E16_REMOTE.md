# OXI E16 remote mode

What the E16 accepts when a host takes it over, and the fixed input map that
comes with it. Everything here is confirmed on hardware (2026-09-09) unless
marked otherwise.

## Getting the spec

OXI publishes it as a Google Sheet. The forum preview renders it as *"This
Sheet is private"* — that is the unauthenticated preview, not the sheet. It
exports:

```bash
curl -sL "https://docs.google.com/spreadsheets/d/1Yccnrluv10QL_PauMmtCt64EtYjfSeZEXKrlrw8P24w/export?format=csv&gid=1057524829"
```

Linked from [the lines thread on remote-controlling the E16](https://llllllll.co/t/remote-control-of-oxi-e16-with-maxmsp/74110),
which is also where the packing rule below was worked out.

## Messages

Header `f0 00 21 5b 02 01`, then a two-byte message id, then a packed payload,
then `f7`.

| id | message | payload |
|---|---|---|
| `06 55` | ENTER REMOTE MODE | none |
| `06 53` | REMOTE MODE ENTERED **ACK** (device to host) | none |
| `06 00` | EXIT REMOTE MODE | none |
| `06 01` | LED | 5-byte chunks: encoder 0-15, led 0-15, R, G, B (each 0-127) |
| `06 04` | LED RING | 7-byte chunks: encoder, R, G, B, amount MSB, amount LSB, bipolar |
| `06 02` | OLED FRAMEBUFFER | 1024 raw bytes, SSD1306 page/column, 128x64 |
| `06 03` | OLED LABELS | 80 raw bytes: 16-char title + 16 x 4-char labels |

LED and LED RING are **variable length** — repeat the chunk per encoder — which
is why a single changed value costs one chunk rather than a repaint.

The ring amount is 14 bits and maps to 0-100% of the ring; `bipolar` renders it
centred, extending left or right, instead of as an arc from zero.

FRAMEBUFFER replaces the whole screen; there is no partial update, and sending
one overrides LABELS and vice versa. Note the geometry: **128x64 mono is Move's
own display**, so mirroring a Schwung page onto an E16 is a packer rather than a
renderer.

## 8-to-7 packing

Payloads are 7-bit packed: for each group of up to 7 raw bytes, emit one byte
holding bit 7 of each following byte (bit *k* corresponds to byte *k*), then
those bytes with bit 7 cleared.

```
raw     FF 00 80 7F 01 FE 55   AA
packed  25 7F 00 00 7F 01 7E 55   01 2A
        ^^                        ^^
        bits 0,2,5 set            bit 0 set
```

**This is easy to get wrong in a way that tests do not catch.** For LABELS and
LED RING every payload byte is already below `0x80`, so the MSB byte is always
zero and the packing looks like padding — the lines thread describes it as *"leds
msg need an additional leading 0"*. FRAMEBUFFER carries real pixel bytes with
bit 7 set, so a packer that emits a constant zero works on everything except the
one message that matters.

Sizes: LABELS is 80 raw to 92 packed; FRAMEBUFFER is 1024 raw to 1171 packed.

## Input is fixed in remote mode

Whatever scene the device is on, once in remote mode:

| control | message |
|---|---|
| encoder turn | CC **1-16** on **channel 1**, relative with acceleration: `0x01..0x08` clockwise, `0x7F..0x78` counter-clockwise |
| encoder button | note **0-15**, channel 1 |
| Shift | note **16**, channel 1 |

That relative encoding is two's complement 7-bit — the same reading
`src/modules/chain/dsp/relative_cc.h` documents, and that header names the E16
in its own comment. Keep the two in agreement.

Because the map is fixed, the surface needs no per-device configuration and a
user's own scene cannot break it.

## Entering and leaving

There is no way to ask whether an E16 is attached: devices on Move's USB-A never
enumerate in Linux (see `docs/SYSEX.md` and issue #358). So a host seeks by
sending ENTER until an ACK arrives. Nine bytes is cheap enough to repeat.

Entering is visible — the device blanks its screen and rings, because the host
now owns them. The E16 has no battery, so unplugging it power-cycles it out of
remote mode; a periodic probe restores it with no user action.

Send EXIT when giving the device back, or it stays blank.

## The USB-A limitation

**Move's XMOS USB-host cannot exchange SysEx with a multi-jack USB-MIDI device.**
Measured 2026-09-09: CC and Program Change reach an E16 on USB-A and work, SysEx
never takes effect, and nothing the device sends ever arrives. Every device that
works on that port — an Arturia MiniLab, a WIDI BLE dongle, the DIN adapter in
issue #358 — presents a single jack. The E16 presents three (`Port 1/2/3`).

Patching the E16's firmware to enumerate one port makes remote mode work over
plain USB-A in **both** directions: ENTER acks, the 101-byte labels message
renders, and the encoders send. Two bytes, `bNumEmbMIDIJack` 3 to 1 in both
CS_ENDPOINT descriptors, no code touched.

So remote mode over USB-A needs OXI to make the port count configurable. This is
not Move-specific — any host with a limited USB-MIDI stack will hit it.

## Which mode to use, and what each costs

Measured at the carry's pacing (3 packets per SPI frame, 2.90 ms a frame):

| message | bytes | packets | time |
|---|---|---|---|
| `06 02` FRAMEBUFFER | 1576 | 394 | **383 ms** |
| `06 03` LABELS | 136 | 34 | **35 ms** |
| `06 04` RING (all 16) | 184 | 46 | **46 ms** |

There is no partial framebuffer, so a value that moved on a detent cannot be
worth a repaint — 383 ms is the whole cost of one, every time. The protocol's
own answer is the other mode, and Schwung uses both:

- **the LABEL carries the NAME** — 16 × 4 characters, redrawn when the page
  changes, which is rare
- **the RING carries the VALUE** — one chunk per detent, which is what a hand on
  a knob actually generates
- **the TITLE carries the READING** — 16 characters naming what is being turned
  and what it now says, since four characters cannot hold both

So the parameter view is LABELS and the Shift map is a FRAMEBUFFER, because a
map is a picture and a parameter page is sixteen names.

**The two modes OVERRIDE each other — they are not layers.** That makes
"nothing changed" different from "nothing to send": dismissing the map leaves
the map's picture on the panel while the surface believes the parameter view is
up, and every later value change goes out as a ring with no name beside it.
`createDisplay` therefore tracks what the DEVICE was last told (`shownKind`),
not what the surface last decided, and resends on a difference. It is the same
shape as the presence edge one layer in, and for the same reason: the device
forgets and never says so.

Four characters is the entire budget for a name, so the abbreviation drops
separators and takes the first four (`Osc Level` → `OSCL`, `Cutoff` → `CUTO`).
Initials were tried first and spend the budget badly — `Osc Level` → `OL` uses
two of four columns, and neither rule avoids collisions, so the simpler one
wins and the title disambiguates whatever is under the hand.

## Measuring it: pace and the refresh meter

Two switches, both file-armed, both off by default.

**Pace** — `/data/UserData/schwung/e16_pace`, outbound packets per SPI frame.
Read by shadow_ui once a second and published on the control block; the drain
consults it on the callback. A 394-packet framebuffer takes `ceil(394 / pace)`
frames at 2.90 ms each:

| pace | full repaint |
|---|---|
| 3 | 383 ms |
| 6 | 192 ms |
| 8 | 144 ms |
| 12 | 96 ms |
| 16 | 72 ms |

3 was never measured — it was the first value that stopped the garbling after
sending a framebuffer all at once failed. Walk up until the screen tears, then
back off one.

**Refresh meter** — `echo 6 > /data/UserData/schwung/e16_testpattern`. It
repaints continuously and draws a stepping column, a per-paint flicker block,
and `PAINTS` / `FPS` / `MS`.

**It exists because a slow SCREEN and slow VALUES are different subsystems and
look identical from outside.** A repaint is 394 paced packets. A value is an
IPC read at ~2.8 ms, served on the controller's rotation of roughly one key per
tick, so a full pass over sixteen cells takes far longer than a repaint — and
on top of that the settle waits for the hand to stop. Watching parameter
numbers move measures the sum of all three. Changing SLOTS is a repaint with no
value rotation in front of it, which is why that already felt quick while the
numbers felt slow.

Nothing on the meter comes from a parameter, so what it reports is the repaint
rate alone. It counts COMPLETED sends, never intents: a refused send is not a
paint, and a meter that climbed while the wire refused would be worse than no
meter.

## Why the framebuffer never became reliable

Measured across a full day on hardware, 2026-09-11. Recorded because the
conclusion is the opposite of where the evidence seemed to point at every
individual step.

**Four buffers sit between a drawn frame and the device, and every one of them
dropped packets INDIVIDUALLY when full.** A 1171-byte framebuffer is a RUN of
394 USB-MIDI packets that the receiver assembles into one message, so losing
any packet in the middle is not a late frame — it is a corrupt one, rendered as
a garbled screen. Three separate truncations were found and fixed, each hidden
behind the one above it:

1. `js_shadow_midi_send` wrote packets one at a time and dropped individually
   once the SHM buffer filled. The same function already refused an oversize
   message with the words *"refusing rather than truncating"* — that guard
   covered only a message larger than the WHOLE buffer, never one larger than
   the remaining room.
2. `ui_midi_carry_push` does the same at the carry, and its own comment says so
   (*"Refusing the newest packet truncates one message"*). The existing
   `wants_more` backpressure asks whether the carry is below half, while a
   snapshot can be the full buffer — so half-full plus a full snapshot
   overruns, mid-message. Snapshots are taken whole or deferred whole now.
3. A third source remains. After both fixes the screen went from constantly
   garbled to occasionally garbled and no further.

**And the pacing intuition was backwards the entire time.** 3 packets/frame was
never measured — it was the first value that stopped the original garbling,
which was really defect 1. Every later experiment contradicted the rate theory:

- pace 1 (1.14 s per frame) garbles BADLY — a slower drain leaves less room, so
  more refusals land mid-message rather than at a boundary
- pace 12 is worse than pace 8 — `MIDI_OUT` is a SHARED 20-slot region and the
  loss scales with how much of it we take
- pace 20 wedged the device outright and needed a replug; the cap is 12 now,
  with 8 slots reserved, and even 12 is too high in practice

There is no pace that is reliably clean. **The framebuffer approach is fighting
the transport's design**: a 394-packet message must survive four buffers intact
every single time, and a short parameter message has to survive none of them —
a 3-byte CC is one atomic packet that nothing on this path can split.

That is the case for driving the device with SHORT messages and letting it draw
its own UI, which is what OXI's own Lua scripting API exists for. LABELS (34
packets) is the same idea within remote mode, and was rejected only because
four characters cannot hold a parameter name.

## Decoding the firmware image (and the byte that ruins it)

The `.syx` is an unencrypted STM32 image, which is how the Lua API surface was
recovered when OXI published no reference for it. 163 messages, each
`F0 | 00 21 5B 02 01 | 00 7E | 4098 nibbles | F7`, high nibble first.

**Each block decodes to 2049 bytes and only 2048 of them are image: the last is
a checksum.** Keeping it inserts a stray byte every 2048, and the failure is
maddeningly partial — `strings` still works, because a one-byte shift leaves
most strings intact, so the image looks fine and every conclusion drawn from it
is worthless. Code does not survive it: the vector table is right (it is in the
first block) while everything after decodes as noise, which reads as "this
firmware must be compressed" rather than as a decoder bug.

Sanity check before trusting any analysis: the image must be exactly
163 x 2048 = 333,824 bytes, and `0x08030000 + 0x8a0` must disassemble as
coherent Thumb-2 (`--triple=thumbv7em-none-eabi`, base `0x08030000`, from the
reset vector `0x080308A1`).

Capstone reads it; Xcode's `llvm-objdump` will not take a raw binary at all.
