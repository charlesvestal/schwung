# Questions for OXI — E16 as a host-driven control surface

Context: Schwung (Ableton Move firmware extension) drives an E16 over the
Move's USB-A host port. Remote mode works; two limits and one unexplained
behaviour decide how good it can be. Everything below was measured on hardware
on 2026-09-09..11, with our own transport bugs found and fixed first.

## 1. Port count — the hard blocker (highest priority)

**Move's USB host cannot send SysEx to a device presenting multiple embedded
MIDI jacks.** The E16 presents three (`Port 1/2/3`). CC and notes flow in both
directions normally; SysEx to the device is silently dropped.

Proven by patching the firmware image: setting `bNumEmbMIDIJack` to 1 (two
bytes at offsets `0x49B15` / `0x49B25` in 1.1.0) makes SysEx work perfectly in
both directions, and remote mode works end to end.

**Ask:** a supported way to run the E16 as a single-jack device — a setting, a
scene option, or an official build. Without it, remote mode and Lua both
require a patched firmware, which we will not ship to users.

## 2. Partial OLED update — the highest-value feature request

`06 02` FRAMEBUFFER replaces all 1024 bytes, and there is no sub-region
command. That makes a one-parameter change cost a **394-packet SysEx**:

| | packets | wire time at our pacing |
|---|---|---|
| FRAMEBUFFER | 394 | ~144 ms |
| LABELS | 34 | ~35 ms |
| RING | 46 | ~46 ms |

At ~144 ms per repaint we cannot show moving values and keep the LED rings
smooth — the link cannot carry both, so every design trades one for the other.
We have shipped three different compromises and users noticed all three.

**Ask:** a partial framebuffer write — page/column range plus bytes, in the
spirit of SSD1306 addressing. Even a quarter-screen region would make the
difference between "shows values" and "shows values smoothly".

## 3. Text width

`n` / `slots.update()` are capped at **4 characters** (manual 6.6/6.9), and
`page.setTitle()` at 15. Four characters cannot hold a parameter name:
`Env Attack` and `Env Amount` are both `ENVA`.

Our UI shows **two pages of eight encoders at once, each with its own header**
— the framebuffer can draw that; LABELS and Lua cannot, because there is one
title for the whole screen.

**Ask:** (a) more characters per cell, or a second text row; (b) a second
header, or any way to label the top and bottom halves separately.

## 4. Long-SysEx reliability — a question, not yet a bug report

After fixing three truncation bugs of our own (all in our buffers, all
confirmed fixed), a full framebuffer still corrupts **occasionally**. The
pattern is not what we expected:

- slower pacing is **worse** — at 1 packet/SPI frame (~1.14 s per message) it
  corrupts badly
- ~8 packets/frame is the best we found; 12 is worse again
- sending 20 packets/frame (filling Move's 20-slot MIDI_OUT mailbox) **wedged
  the E16** and it needed a replug

**Questions:** does the E16 time out or discard a SysEx that stays open too
long? Is there a maximum sustained packet rate, or a receive-buffer limit, for
a single long SysEx? Is the wedge at high rate a known failure?

We are happy to supply captures.

## 5. Lua

- Can a script switch pages programmatically, or only observe
  `page.onPageChange`? (Our slot map would become a page.)
- Is the script upload protocol documented? If Schwung could install its own
  script on connect, the user would not need the OXI App at all — that is the
  difference between a feature and a manual setup step for us.

## What we are not asking for

Nothing about remote mode's input map, the packing rule, or the message set —
those are clear, work as documented, and the spec sheet was enough to implement
against.
