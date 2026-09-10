# The E16 as a Schwung control surface

**Status:** design. Blocked on a firmware change from OXI (see Prerequisite).
**Date:** 2026-09-10

An OXI E16 — sixteen push/turn encoders in a 4×4 grid, an OLED, and RGB rings —
driven from Schwung as a hands-on surface over the whole chain, so parameters
are under your fingers while Move's own screen is doing something else.

## Prerequisite, and why this is blocked

**Move's XMOS USB-host cannot exchange SysEx with a multi-jack USB-MIDI
device.** Measured 2026-09-09 (see `docs/SYSEX.md` and issue #358): CC and
Program Change reach an E16 on USB-A and work, SysEx never takes effect, and
nothing the device sends ever arrives. Every device that works on that port —
an Arturia MiniLab, a WIDI BLE dongle, the DIN adapter in #358 — presents a
single jack. The E16 presents three.

This was proven rather than inferred, and the proof is the fix: patching the
E16's firmware so it enumerates **one** USB-MIDI port makes remote mode work
over plain USB-A **in both directions** — ENTER acks, the 101-byte labels
message renders, and the encoders send. Two bytes, `bNumEmbMIDIJack` 3 → 1 in
both CS_ENDPOINT descriptors; no code touched.

So the feature needs OXI to make the port count configurable, or to ship a
single-port mode. It is not Move-specific — any host with a limited USB-MIDI
stack hits it. Until then this design is buildable and testable but not
shippable.

**What works on stock firmware today:** CC in both directions. The input half of
this design can therefore be built and used against #403's CC Map before the
firmware question resolves.

## The protocol

OXI's "OXI REMOTE" spec, header `f0 00 21 5b 02 01` then a two-byte id. The
sheet reads as *"This Sheet is private"* in the forum preview because the
preview fetches it unauthenticated; it exports:

```
curl -sL "https://docs.google.com/spreadsheets/d/\
1Yccnrluv10QL_PauMmtCt64EtYjfSeZEXKrlrw8P24w/export?format=csv&gid=1057524829"
```

Implementation should transcribe it into `docs/` rather than depend on that URL.

| id | message |
|---|---|
| `06 55` / `06 53` / `06 00` | ENTER / ACK / EXIT remote mode |
| `06 01` | LED — 5-byte chunks: encoder, led, R, G, B (0-127) |
| `06 04` | LED RING — 7-byte chunks: encoder, R, G, B, amount MSB/LSB (14-bit), bipolar |
| `06 02` | OLED FRAMEBUFFER — 1024 raw bytes, SSD1306 page/column, **128×64** |
| `06 03` | OLED LABELS — 80 raw bytes: 16-char title + 16 × 4-char labels |

Payloads are **8-to-7 packed** (one MSB byte per group of ≤7). All of it is
confirmed on hardware.

**Input in remote mode is fixed**, whatever scene the device is on: turns are
**CC 1–16 on channel 1**, relative with acceleration (`0x01..0x08` /
`0x7F..0x78`); encoder buttons are **notes 0–15**; Shift is **note 16**. That
encoding is what `src/modules/chain/dsp/relative_cc.h` already decodes — it
names the E16 in its own comment.

## Architecture

A surface beside the shadow UI, not a module in a chain slot.

```
E16 ──USB-A cable 2──► shim ──/schwung-ui-midi──► shadow_ui.js
                                                    │
                                        src/shared/e16_surface.mjs
                                        focus state · view model · input
                                                    │
   ◄── move_midi_external_send ─── framebuffer / ring / ENTER
```

`e16_surface.mjs` owns focus `(slot, component, pageIndex)`, builds both views,
and turns encoder input into parameter writes. It consumes the existing planner
— `level_walk.mjs` → `planPages()` — so authored pages, ranges, enum options and
`visible_if` all work with **no per-module knowledge**, exactly as the knob grid
gets them today.

### Why JS, with C only at the gate

The expensive part already exists in JS: the planner, `chain_params` parsing,
`visible_if`, enum options, child-key resolution, and a renderer for this exact
128×64 geometry. Rebuilding that in C would duplicate the most intricate code in
the project, and this codebase's recurring failure is one fact with two
consumers drifting apart (`wav_format.mjs`, `SLOT_BUSES`). `shadow_ui` is also
SCHED_OTHER, so it may allocate and log; the shim is the SPI callback, where a
`get_param` costs 2.8 ms.

C does the one thing only C can: forwarding. **C forwards raw events, JS decides
what they mean.**

The counter-argument is real and worth recording: Move's own knobs have a C path
(`chain_midi.c:756` maps CC 71–78 with acceleration, which #403 extends). What
rules it out here is that an E16 encoder's meaning depends on the current page
plan, which only JS can compute — an all-C input path means shipping the plan to
C on every page change and keeping two copies of focus in sync.

Two things would reverse this: if the surface had to work with `shadow_ui` not
running (it runs continuously — display mode only decides who owns the screen),
or if encoder latency felt bad. The path is shim → SHM → 60 Hz loop →
`set_param`; Move's own knobs already go through the grid in JS.

### Required shim change

Outside overtake the shim does not forward cable-2 messages to the shadow UI
(`schwung_shim.c:9109`), and cable-2 **note-ons are diverted into the
LED-coalescing queue**, which would swallow the encoder buttons. Both need a
gated path, enabled only while the surface is active. #403 widened the same scan
for Master FX; follow that shape.

### Ownership: the E16 claims CC 1–16 ch 1

#403 lets a user assign arbitrary CCs to parameters, and remote mode emits CC
1–16 on channel 1 — numbers a user may already have mapped. **While the surface
is active it claims those, and they never reach the CC Map.** One owner per
message, decided by whether the surface is enabled, so a single turn cannot be
acted on twice.

## The two views

**Parameters (Shift up).** Top 2×4 is grid page *N*, bottom 2×4 is page *N+1* —
real authored pages, unmodified. Two pages at once rather than a re-planned
16-cell page, because `docs/PARAM_PAGES.md` is explicit that groupings are
authored and must keep their shape.

Drawn as a framebuffer we render ourselves, with a half-width header per page.
That is what escapes the 4-character label cap, and it is why the framebuffer is
preferred over LABELS despite costing more: the LABELS message carries **one**
16-char title for the whole screen, which two pages cannot share honestly.

LED rings carry values, using the 14-bit position and the bipolar flag from
`chain_params`.

**Map (Shift held).** Top row = the 4 slots, one lit. Lower 12 = the current
slot's **occupied** components in chain order. Press a top-row button to switch
slots, a lower button to jump. A dedicated cell swaps the lower 12 to that
slot's buses; the same gesture flips the top row between slots and Master /
Send A / Send B.

The map shows **what exists, not what could exist**. The caps are 8 MIDI FX + 1
synth + 8 audio FX = **17 per slot**, plus 8 buses × 8 inserts, plus 24
device-wide — around 348 destinations against 16 buttons. A real rig is a synth
and two or three FX. Occupied components compact into the 12 cells in chain
order, and positions recompute **only when the chain shape changes**, which is
rare, deliberate, and a moment when you are looking at Move anyway. Overflow
past 12 pages with Shift + turn.

Nothing is modal: the map exists only while the modifier is held, so there is no
state to be lost in.

## Input

| gesture | wire | action |
|---|---|---|
| turn | CC 1–16 ch 1, relative + accel | `relative_cc_ticks()` → the grid's knob-turn path |
| push | note 0–15 | the grid's click: dive, flip a two-option enum, fire a momentary |
| Shift | note 16 | map view while held |
| Shift + turn | | page the N/N+1 pair by two |
| Shift + push | | top row switches slot; lower 12 jumps to a component |

Reusing the grid's turn path is load-bearing: it is why enum quantization,
read-only refusal, momentary latching and `visible_if` gating all work without
the surface knowing anything about modules.

## Display rate

Deliberately asymmetric:

- **navigation** → framebuffer, 1024 raw / 1171 packed, only on focus or page change
- **value change** → LED RING, chunked per encoder: one knob is a single 7-byte
  chunk, ~15 bytes on the wire

So the common case is nearly free and the expensive send follows a discrete
human action. Measured 2026-09-09, outbound: 31 packets sent alone arrived
byte-perfect; 34 packets sent amid other traffic lost 8 whole packets. The
surface therefore **paces**: one framebuffer in flight at a time, a per-tick send
budget, ring updates coalesced per encoder.

**Measure the framebuffer rate early** — it is the one decision that could
reshape the display layer. The fallback is the 92-byte LABELS message with no
change to the view model, only the renderer.

## Follow focus

**Global Settings → External Surface**: `Off / E16`, plus **Follow Focus**.

Follow is **one-way** — Move's screen drives the E16, never the reverse — and
while it is on the E16's map is disabled, so the surface mirrors and nothing
else. That is what stops the two surfaces fighting over focus, and it makes the
setting mean one sentence: *is the E16 showing what Move shows, or its own
thing?*

## Lifecycle

There is no way to ask whether an E16 is attached — USB-A devices never
enumerate in Linux (#358). So while the setting is on, send ENTER every ~2 s
until an ACK arrives, then own the surface and draw. Nine bytes every two
seconds, and it makes the feature self-healing: the E16 has no battery, so
unplugging power-cycles it out of remote mode and the next ping restores it with
no user action. On disable or shutdown, send EXIT so the device becomes itself
again.

## Testing

`tests/host` covers the pure parts:

- relative-CC decode + acceleration → parameter value
- the map builder: chain shape → 12 cells, including >12 overflow and the
  recompute-on-shape-change rule
- the 8-to-7 packer, pinned against the byte-exact hardware captures from
  2026-09-09
- framebuffer size and packing

What tests cannot reach is feel: encoder latency, whether two pages at once
reads clearly, whether the map is legible at a glance. That is hardware, and it
is where the JS choice pays for itself — `shadow_ui.js` deploys as a file, while
a shim change is a cross-compile, a reinstall and a reboot.

## Deferred

- **LABELS renderer** as a framebuffer fallback — only if the rate demands it.
- **Per-set persistence of focus.** Session-only to start.
- **Mirroring Move's own OLED** to the E16. The framebuffer is the same 128×64
  geometry, so this is a packer rather than a renderer — tempting, and out of
  scope until the surface itself is proven.
- **The Lua path.** A script (`controller.onSysex`, `slots.update`, `leds.update`)
  could hold its own label table and let Schwung send compact updates instead of
  full repaints, and would leave the E16 usable as itself. It is rejected as the
  *first* implementation because it needs a script authored and uploaded per
  device from the OXI App on a Mac, capped at 8000 bytes — a setup dance for every
  user and a redeploy to every device on any protocol change. Revisit if repaint
  cost proves prohibitive.
- **On-device chain editing** (adding or swapping modules from the E16). The map
  navigates; it does not edit shape.

---

## Hardware findings, 2026-09-10

The bench pass found six defects that 325 host tests could not, and settled two
questions the design had guessed at. Recorded here because every one of them is
the kind of thing the next person re-derives.

**Six bugs, all invisible to host tests:**

1. **The claim excluded SysEx.** Narrowing the shim's cable-2 claim to CC and
   notes meant the E16's ACK was never published to JS, so `present` never
   flipped and every frame was withheld. The device sat in remote mode, blank.
2. **The ACK died at a CIN gate.** `cin < 0x08 || cin > 0x0E` drops SysEx before
   any cable test -- the gate `docs/SYSEX.md` names as why a chain slot is
   write-only for SysEx. Widening the CABLE condition was not enough. Both
   halves were individually correct; only the composition failed.
3. **`font4x5` has no lowercase.** `print("cutoff")` drew nothing while
   `fontWidth4x5` still returned a width, so layouts reserved space for glyphs
   that never appeared. `render_page_movy` has always called `caps()`; we did
   not. The original test passed because it used `"Hi"` -- the capital H inks.
4. **The outbound buffer was 1024 bytes against a 1576-byte frame.** The send
   wrote what fit, dropped the rest and returned false; the caller correctly
   re-owed the repaint and retried forever, six truncated bursts a second.
5. **A message too large to ever fit was retried, not refused.** "False means
   retry" is right for a full buffer because it drains; it is a livelock when
   the message can never fit.
6. **`short_name` was read from the wrong object.** It lives on
   `page.shortNames`, collected by `page_plan`; `getOrGuess` never carries it,
   so every cell fell through to the raw parameter id.

**The transport finding, which is the general one:** outbound SysEx loss on
USB-A is a function of RATE, not size. The carry filled every free MIDI_OUT
slot, sending ~6900 packets/s; a 394-packet framebuffer arrived with most of its
middle gone. Paced to a few packets per frame, a full-width bar drawn at the
bottom of the buffer landed exactly there. **This is #358's outbound twin and
the fix is transport-level** (`UI_MIDI_CARRY_PACKETS_PER_FRAME`), so every
module sending a bulk dump benefits.

**Two design guesses, now answered:**

- An E16 already in remote mode **does** re-ACK a repeated ENTER, so the
  keepalive design holds. Task 7 flagged this as unverified.
- The framebuffer **is** viable on this hardware, which the author who
  reverse-engineered the protocol never achieved. Lua was considered as an
  alternative and rejected on evidence: its entire display surface is a 15-char
  title, sixteen 4-char labels and the rings -- no pixel API at all -- so it is
  the same surface as the LABELS message plus a per-device App upload.

**Still open:** whether 1024 bytes spans the whole panel. Probe 5 (fill
everything) is armed and answers it by inspection; content was observed
surviving an off/on cycle, which a full frame of zeros should not allow.
