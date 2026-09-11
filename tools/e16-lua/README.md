# Driving the E16 from Schwung with its Lua API

`schwung.lua` runs on the device and draws the UI; Schwung sends only what
changed. Load it with the OXI App (Script editor → paste → send). Firmware
1.1.0 or later.

Written against the API reference in the E16 manual, chapter 6 — everything is
namespaced (`page.*`, `controller.*`, `midi.*`, `slots.*`, `leds.*`) with the
documented signatures.

## Why, and what it costs

Remote mode draws by shipping a 1024-byte framebuffer: a **394-packet SysEx**
that must cross four buffers intact, every one of which drops packets
individually when full. Three truncations were found and two fixed, and it
still garbles; no pacing helps, and the pacing intuition was backwards
throughout (`docs/E16_REMOTE.md`).

| | remote mode | Lua |
|---|---|---|
| page change | 394 packets | ~60 packets |
| value change | 394 packets | 5 bytes |
| text per encoder | arbitrary | **4 characters** |
| page title | arbitrary | **15 characters** |
| drawn by | us | the device |
| reliable | no | expected |

**The four-character ceiling is the device's, not the transport's.** The manual
is explicit: `n` writes to `control.abbr`, max 4 chars, and `slots.update()`
truncates identically. So Lua does *not* lift the limit that made remote mode's
LABELS message unacceptable — it is the same budget, delivered reliably. The
15-character title is the only place a full parameter name fits, which is why
the host sends a TITLE message for whatever encoder is under the hand.

## Wire format

`F0 00 21 5B 02 01 <id> … F7` — the same manufacturer header remote mode uses,
so Schwung's messages are distinguishable from other gear on the port.

| id | message | payload |
|---|---|---|
| `0x10` | PAGE | title `\0` label1 `\0` … label16 `\0`, 7-bit ASCII |
| `0x11` | VALUE | index (0–15), hi, lo — 14-bit, matching `enc.value` |
| `0x12` | TITLE | the focused parameter's name, in full |

Encoders keep sending **relative CC 1–16 on channel 1**, byte-identical to
remote mode, so `src/shared/e16_input.mjs` needs no change when the drawing
moves onto the device.

## What this does NOT fix

`controller.onSysex` is the only inbound host→script channel — there is no
`onMidi` or `onCC` binding. Move's XMOS still cannot send SysEx to a
multi-jack USB device, so the **1-port firmware patch remains required**. Lua
removes the transport fragility; it does not remove the firmware dependency.

## Host side, still to do

Emit the three messages above instead of a framebuffer. `e16_map.mjs`, the
focus/follow logic in `e16_surface.mjs` and `e16_input.mjs` carry over
unchanged; `e16_canvas.mjs`, both renderers, `framebufferMsg`, the display
pacing machinery and the pace file all go.
