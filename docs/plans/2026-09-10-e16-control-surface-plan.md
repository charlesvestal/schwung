# E16 Control Surface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drive an OXI E16 as a Schwung control surface — 16 encoders over the whole chain, navigable from the device itself, with follow-focus as a mode.

**Architecture:** A surface beside the shadow UI, not a chain module. `src/shared/e16_*.mjs` owns focus, view model and input, consuming the existing page planner so no per-module knowledge is needed. C changes only at the forwarding gate in `schwung_shim.c`. Display is an offscreen 1-bit framebuffer rendered through the same `ctx` abstraction `src/shared/param_pages/` already uses.

**Tech Stack:** QuickJS `.mjs` modules, `tests/host/*.sh` running under node, C in `src/schwung_shim.c`, OXI REMOTE SysEx over USB-A cable 2.

**User decisions (already made):**
- "A" — navigate on the E16 itself, self-contained; do not start with assign-from-Move.
- "A" — Shift is the map: hold Shift for a navigator, release for parameters.
- "A" — matrix map: slots on the top row, the selected slot's chain on the other 12.
- "C" — compact occupied components, recomputing only when the chain shape changes.
- "A" — two authored grid pages at once (top 2×4 = page N, bottom 2×4 = page N+1), with a self-drawn screen rather than the 4-char LABELS message.
- "go ahead" — JS for the view model, C only at the forwarding gate.

---

## Prerequisite and sequencing

**Tasks 1–5 and 11 need no hardware and no firmware change.** They are pure modules with host tests.

**Tasks 6–10 need the E16 to enumerate as a single USB-MIDI port**, which today requires patched firmware (`~/Downloads/e16-firmware/`, and see the design doc). The shipping version needs OXI to make the port count configurable. Build and verify everything else first; the surface is inert but harmless with no device attached.

Design: `docs/plans/2026-09-10-e16-control-surface-design.md`.

---

### Task 1: Transcribe the OXI REMOTE protocol into the repo

**Goal:** The protocol lives in the repo rather than in a Google Sheet that reads as private.

**Files:**
- Create: `docs/E16_REMOTE.md`
- Modify: `CLAUDE.md` (Documentation Index — one line)

**Acceptance Criteria:**
- [ ] Every message id from the spec is documented with its payload layout
- [ ] The 8-to-7 packing rule is stated with a worked example
- [ ] The fixed remote-mode input mapping (CC 1–16 ch 1, notes 0–15, Shift note 16) is documented
- [ ] `CLAUDE.md`'s Documentation Index gains one line pointing at it

**Verify:** `grep -c "06 0[1234]" docs/E16_REMOTE.md` → at least 4

**Steps:**

- [ ] **Step 1: Write `docs/E16_REMOTE.md`**

Header `f0 00 21 5b 02 01`, then a two-byte id:

| id | message | payload |
|---|---|---|
| `06 55` | ENTER REMOTE | none |
| `06 53` | ACK (device → host) | none |
| `06 00` | EXIT REMOTE | none |
| `06 01` | LED | 5-byte chunks: encoder 0-15, led 0-15, R, G, B (0-127) |
| `06 04` | LED RING | 7-byte chunks: encoder, R, G, B, amount MSB, amount LSB, bipolar |
| `06 02` | OLED FRAMEBUFFER | 1024 raw bytes, SSD1306 page/column, 128×64 |
| `06 03` | OLED LABELS | 80 raw bytes: 16-char title + 16 × 4-char labels |

Packing: one MSB byte per group of ≤7, holding bit 7 of each following byte in
order, then those bytes with bit 7 cleared. Worked example:

```
raw     FF 00 80 7F 01 FE 55   AA
packed  25 7F 00 00 7F 01 7E 55   01 2A
        ^^ bits 0,2,5 set        ^^ bit 0 set
```

Input in remote mode is fixed regardless of scene: turns are CC 1–16 on channel
1, relative two's complement with acceleration (`0x01..0x08` CW, `0x7F..0x78`
CCW); encoder buttons are notes 0–15; Shift is note 16.

Record the source URL and that the sheet exports as CSV despite the preview.

- [ ] **Step 2: Add one line to `CLAUDE.md`'s Documentation Index**

```markdown
- `docs/E16_REMOTE.md` — OXI E16 remote-mode SysEx, and the fixed input mapping it forces.
```

- [ ] **Step 3: Commit**

```bash
git add docs/E16_REMOTE.md CLAUDE.md
git commit -m "docs: OXI E16 remote-mode protocol"
```

---

### Task 2: `e16_protocol.mjs` — packing and message builders

**Goal:** Every byte we will ever send the E16, built and unit-tested against captures taken from real hardware.

**Files:**
- Create: `src/shared/e16_protocol.mjs`
- Create: `tests/host/test_e16_protocol.sh`

**Acceptance Criteria:**
- [ ] `pack7` matches the worked example, including bytes with bit 7 set
- [ ] `enterMsg()` equals the exact bytes hardware acked: `F0 00 21 5B 02 01 06 55 F7`
- [ ] `ringMsg` emits one 7-byte chunk per encoder and accepts a 14-bit amount
- [ ] `framebufferMsg` accepts 1024 bytes and produces 1171 packed payload bytes
- [ ] Every builder returns flat USB-MIDI packets ready for `move_midi_external_send` (CIN 0x04/0x05/0x06/0x07, cable nibble 0)

**Verify:** `bash tests/host/test_e16_protocol.sh` → all assertions pass

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_e16_protocol.sh`:

```bash
#!/usr/bin/env bash
# Pins the E16 wire format against bytes captured from hardware on 2026-09-09.
# The ENTER vector is the exact message the device acked; if it changes, the
# device stops responding and nothing else in this feature can work.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { pack7, enterMsg, exitMsg, ringMsg, framebufferMsg, packetize }
  from "./src/shared/e16_protocol.mjs";

let fails = 0;
const eq = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { console.log("FAIL " + name + "\n  got  " + g + "\n  want " + w); fails++; }
  else console.log("ok   " + name);
};

// Worked example from docs/E16_REMOTE.md -- the only case with bit 7 set,
// which is the one that matters for the framebuffer and the one a packer
// written against labels-only payloads gets wrong.
eq("pack7 high bits",
   pack7([0xFF,0x00,0x80,0x7F,0x01,0xFE,0x55,0xAA]),
   [0x25,0x7F,0x00,0x00,0x7F,0x01,0x7E,0x55,0x01,0x2A]);

eq("pack7 empty", pack7([]), []);

// Captured from the wire, 2026-09-09: this exact message returned an ACK.
eq("enter bytes", enterMsg(),
   [0xF0,0x00,0x21,0x5B,0x02,0x01,0x06,0x55,0xF7]);
eq("exit bytes", exitMsg(),
   [0xF0,0x00,0x21,0x5B,0x02,0x01,0x06,0x00,0xF7]);

// One ring: 7 raw bytes -> one group -> 8 packed.
const r = ringMsg([{ enc: 0, r: 0, g: 60, b: 0, amount: 16383, bipolar: 0 }]);
eq("ring length", r.length, 1 + 5 + 2 + 8 + 1);
// Payload starts at index 9: F0, five header bytes, two id bytes, then the
// pack7 group byte. Blue precedes the amount -- pinned against the Max patch
// chunk `0 3 0 12 0 $1 0 0` from the lines thread, the only public ground
// truth for the field order. An earlier draft of this test read the amount at
// 12/13, which is off by one and can only be satisfied by reordering the
// protocol -- a test driving the implementation away from the wire format.
eq("ring chunk order", r.slice(9, 16), [0, 0, 60, 0, 0x7F, 0x7F, 0]);
eq("ring amount split", [r[13], r[14]], [0x7F, 0x7F]);

const fb = framebufferMsg(new Uint8Array(1024));
eq("framebuffer packed length", fb.length, 1 + 5 + 2 + 1171 + 1);

// packetize: 9 bytes -> 3 packets, last is CIN 0x07 (ends with three bytes).
const p = packetize(enterMsg());
eq("packet count", p.length / 4, 3);
eq("first cin", p[0], 0x04);
eq("last cin", p[8], 0x07);

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
```

- [ ] **Step 2: Run it and watch it fail**

```bash
bash tests/host/test_e16_protocol.sh
```
Expected: `Cannot find module .../e16_protocol.mjs`

- [ ] **Step 3: Write `src/shared/e16_protocol.mjs`**

```javascript
/*
 * OXI E16 remote-mode wire format. See docs/E16_REMOTE.md.
 *
 * Every builder returns RAW MESSAGE BYTES including F0/F7; packetize() turns
 * those into the flat 4-byte USB-MIDI packets move_midi_external_send wants.
 * The two are separate because the packetizing is Schwung's transport concern
 * and the message layout is OXI's -- and because a test that pins bytes is
 * only readable if the bytes are the message.
 */

const HDR = [0x00, 0x21, 0x5B, 0x02, 0x01];

/* One MSB byte per group of <=7, then those bytes with bit 7 cleared.
 *
 * For LABELS and RING every payload byte is already < 0x80, so the MSB byte is
 * always zero and the packing looks like padding. It is not: FRAMEBUFFER
 * carries real pixel bytes with bit 7 set, and a packer written to emit a
 * constant zero passes every test but the one that matters. */
export function pack7(raw) {
    const out = [];
    for (let i = 0; i < raw.length; i += 7) {
        const n = Math.min(7, raw.length - i);
        let msbs = 0;
        for (let k = 0; k < n; k++) if (raw[i + k] & 0x80) msbs |= (1 << k);
        out.push(msbs);
        for (let k = 0; k < n; k++) out.push(raw[i + k] & 0x7F);
    }
    return out;
}

function msg(id, raw) {
    return [0xF0].concat(HDR, id, raw && raw.length ? pack7(raw) : [], [0xF7]);
}

export function enterMsg() { return msg([0x06, 0x55]); }
export function exitMsg()  { return msg([0x06, 0x00]); }

export const ACK_BODY = HDR.concat([0x06, 0x53]);

/* rings: [{enc, r, g, b, amount (0-16383), bipolar}] -- variable length, so a
 * single changed encoder costs one chunk rather than a whole-screen repaint.
 * That asymmetry is the feature's rate strategy, not an optimisation. */
export function ringMsg(rings) {
    const raw = [];
    for (const x of rings) {
        raw.push(x.enc & 0x0F, x.r & 0x7F, x.g & 0x7F, x.b & 0x7F,
                 (x.amount >> 7) & 0x7F, x.amount & 0x7F, x.bipolar ? 1 : 0);
    }
    return msg([0x06, 0x04], raw);
}

/* leds: [{enc, led, r, g, b}] */
export function ledMsg(leds) {
    const raw = [];
    for (const x of leds) raw.push(x.enc & 0x0F, x.led & 0x0F,
                                   x.r & 0x7F, x.g & 0x7F, x.b & 0x7F);
    return msg([0x06, 0x01], raw);
}

/* buf: 1024 bytes, SSD1306 page/column. Whole screen only -- the spec has no
 * partial update, which is why navigation is the expensive send. */
export function framebufferMsg(buf) {
    if (buf.length !== 1024) throw new Error("framebuffer must be 1024 bytes");
    return msg([0x06, 0x02], Array.from(buf));
}

/* title: 16 chars, labels: 16 strings of <=4. Kept for the fallback renderer;
 * the surface draws pixels instead. */
export function labelsMsg(title, labels) {
    const pad = (s, n) => (s + " ".repeat(n)).slice(0, n);
    const raw = [];
    for (const c of pad(title, 16)) raw.push(c.charCodeAt(0) & 0x7F);
    for (let i = 0; i < 16; i++)
        for (const c of pad(labels[i] || "", 4)) raw.push(c.charCodeAt(0) & 0x7F);
    return msg([0x06, 0x03], raw);
}

/* Flat 4-byte USB-MIDI packets. Cable nibble left at 0 -- js_shadow_midi_send
 * overwrites it with 2 for the external port. */
export function packetize(bytes) {
    const out = [];
    let i = 0;
    while (bytes.length - i > 3) {
        out.push(0x04, bytes[i], bytes[i + 1], bytes[i + 2]);
        i += 3;
    }
    const left = bytes.length - i;
    const cin = { 1: 0x05, 2: 0x06, 3: 0x07 }[left];
    out.push(cin, bytes[i] || 0, bytes[i + 1] || 0, bytes[i + 2] || 0);
    return out;
}

/* asm holds everything between F0 and F7. */
export function isAck(asm) {
    if (asm.length !== ACK_BODY.length) return false;
    for (let i = 0; i < ACK_BODY.length; i++) if (asm[i] !== ACK_BODY[i]) return false;
    return true;
}
```

- [ ] **Step 4: Run the test**

```bash
bash tests/host/test_e16_protocol.sh
```
Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add src/shared/e16_protocol.mjs tests/host/test_e16_protocol.sh
git commit -m "e16: wire format, pinned to hardware captures"
```

---

### Task 3: `e16_canvas.mjs` — an offscreen 1-bit ctx

**Goal:** Render into a 1024-byte SSD1306 buffer using the same `ctx` interface `src/shared/param_pages/` already draws through, so the E16 screen reuses the fleet's renderers instead of growing a second one.

**Files:**
- Create: `src/shared/e16_canvas.mjs`
- Create: `tests/host/test_e16_canvas.sh`

**Acceptance Criteria:**
- [ ] Implements `fillRect(x, y, w, h, color)`, `print(x, y, text, color)`, `textWidth(text)`, `drawLine`, `clear()`
- [ ] `toBuffer()` returns exactly 1024 bytes in SSD1306 page/column order
- [ ] A pixel at (0,0) sets bit 0 of byte 0; a pixel at (0,7) sets bit 7 of byte 0; a pixel at (0,8) sets bit 0 of byte 128
- [ ] Drawing outside 128×64 is clipped, not thrown

**Verify:** `bash tests/host/test_e16_canvas.sh` → all assertions pass

**Steps:**

- [ ] **Step 1: Write the failing test**

```bash
#!/usr/bin/env bash
# SSD1306 page/column order is the one thing here that cannot be eyeballed on
# a device: a transposed buffer still draws SOMETHING, so it reads as a
# rendering bug rather than a format bug. Pin the bit positions.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { createCanvas } from "./src/shared/e16_canvas.mjs";
let fails = 0;
const eq = (n, g, w) => { if (g !== w) { console.log("FAIL " + n + " got " + g + " want " + w); fails++; } else console.log("ok   " + n); };

const c = createCanvas();
eq("buffer size", c.toBuffer().length, 1024);

c.fillRect(0, 0, 1, 1, 1);
eq("(0,0) -> byte 0 bit 0", c.toBuffer()[0], 0x01);

c.clear();
c.fillRect(0, 7, 1, 1, 1);
eq("(0,7) -> byte 0 bit 7", c.toBuffer()[0], 0x80);

c.clear();
c.fillRect(0, 8, 1, 1, 1);
eq("(0,8) -> byte 128 bit 0", c.toBuffer()[128], 0x01);

c.clear();
c.fillRect(127, 63, 1, 1, 1);
eq("(127,63) -> last byte bit 7", c.toBuffer()[1023], 0x80);

c.clear();
c.fillRect(200, 200, 5, 5, 1);   // fully off-screen
eq("clipped, no throw", c.toBuffer().reduce((a, b) => a + b, 0), 0);

eq("textWidth is positive", c.textWidth("Hi") > 0, true);
console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
```

- [ ] **Step 2: Run it and watch it fail**

Expected: `Cannot find module .../e16_canvas.mjs`

- [ ] **Step 3: Write `src/shared/e16_canvas.mjs`**

Back the canvas with a `Uint8Array(1024)` and set bits directly — no
intermediate pixel array, since the packing is the storage:

```javascript
/*
 * An offscreen 1-bit canvas that IS an SSD1306 framebuffer.
 *
 * It implements the same small surface the param_pages renderers draw through
 * (fillRect / print / textWidth / drawLine), which is what lets the E16 screen
 * reuse the fleet's renderers rather than growing a second drawing stack.
 * tools/param-pages/harness.mjs implements the same interface for PNG output on
 * the host, so a page can be rendered to a picture and to the device from one
 * code path.
 *
 * Bits are set straight into the packed buffer: byte = (y >> 3) * 128 + x,
 * bit = y & 7. Keeping a separate pixel array and packing at the end would be
 * one more representation to get transposed.
 */
import { FONT } from "./param_pages/font5x3.mjs";   // confirm export name

const W = 128, H = 64;

export function createCanvas() {
    const buf = new Uint8Array(W * H / 8);

    function px(x, y, color) {
        x |= 0; y |= 0;
        if (x < 0 || x >= W || y < 0 || y >= H) return;   // clip, never throw
        const i = (y >> 3) * W + x, m = 1 << (y & 7);
        if (color) buf[i] |= m; else buf[i] &= ~m;
    }

    return {
        width: W, height: H,
        clear() { buf.fill(0); },
        fillRect(x, y, w, h, color) {
            for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) px(x + i, y + j, color);
        },
        drawLine(x0, y0, x1, y1, color) { /* Bresenham, calling px() */ },
        print(x, y, text, color) { /* glyph lookup from FONT, px() per set bit */ },
        textWidth(text) { /* sum of glyph widths + 1px spacing */ },
        toBuffer() { return buf; },
    };
}
```

Implement `drawLine`, `print` and `textWidth` against whichever font module the
grid uses for small text — check `src/shared/param_pages/font5x3.mjs` and
`font_tamzen6x12.mjs` for their actual exports before wiring the import, and
match the metrics the grid uses so text measures the same on both screens.

- [ ] **Step 4: Run the test**

Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add src/shared/e16_canvas.mjs tests/host/test_e16_canvas.sh
git commit -m "e16: offscreen 1-bit canvas in SSD1306 page/column order"
```

---

### Task 4: `e16_input.mjs` — decode the fixed remote-mode input

**Goal:** Turn raw MIDI into surface events, with the acceleration semantics the grid already uses.

**Files:**
- Create: `src/shared/e16_input.mjs`
- Create: `tests/host/test_e16_input.sh`

**Acceptance Criteria:**
- [ ] CC 1–16 ch 1 → `{type:"turn", enc: 0-15, ticks}` with `0x01..0x08` positive and `0x7F..0x78` negative
- [ ] Note 0–15 on ch 1 → `{type:"push", enc}` on note-on, `{type:"release", enc}` on note-off
- [ ] Note 16 → `{type:"shift", down}` — and the surface's shift state survives a missed note-off
- [ ] Anything else returns `null` rather than a partially-filled event
- [ ] Decoding matches `relative_cc.h`'s two's-complement reading exactly

**Verify:** `bash tests/host/test_e16_input.sh` → all assertions pass

**Steps:**

- [ ] **Step 1: Write the failing test**

```bash
#!/usr/bin/env bash
# The E16's relative encoding is the same two's complement relative_cc.h reads,
# and that header exists because chain_midi.c decoded only +/-1 and lost every
# fast turn (#402). A JS copy that repeats that bug would be invisible: the
# knobs would simply feel slow.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { decode } from "./src/shared/e16_input.mjs";
let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + " got " + a + " want " + b); fails++; }
  else console.log("ok   " + n); };

eq("cw one",   decode([0xB0, 1, 0x01]), { type: "turn", enc: 0, ticks: 1 });
eq("cw eight", decode([0xB0, 1, 0x08]), { type: "turn", enc: 0, ticks: 8 });
eq("ccw one",  decode([0xB0, 1, 0x7F]), { type: "turn", enc: 0, ticks: -1 });
eq("ccw eight",decode([0xB0, 1, 0x78]), { type: "turn", enc: 0, ticks: -8 });
eq("enc 16",   decode([0xB0, 16, 0x01]),{ type: "turn", enc: 15, ticks: 1 });
eq("centre is no movement", decode([0xB0, 1, 0x40]), null);

eq("push",    decode([0x90, 0, 0x7F]), { type: "push", enc: 0 });
eq("release", decode([0x80, 0, 0x00]), { type: "release", enc: 0 });
eq("note-on velocity 0 is a release",
   decode([0x90, 3, 0x00]), { type: "release", enc: 3 });
eq("shift down", decode([0x90, 16, 0x7F]), { type: "shift", down: true });
eq("shift up",   decode([0x80, 16, 0x00]), { type: "shift", down: false });

eq("wrong channel ignored", decode([0xB1, 1, 0x01]), null);
eq("cc 0 ignored",          decode([0xB0, 0, 0x01]), null);
eq("cc 17 ignored",         decode([0xB0, 17, 0x01]), null);
eq("note 17 ignored",       decode([0x90, 17, 0x7F]), null);

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
```

- [ ] **Step 2: Run it and watch it fail**

- [ ] **Step 3: Write `src/shared/e16_input.mjs`**

```javascript
/*
 * Remote mode forces a FIXED input map, whatever scene the device is on:
 * turns are CC 1-16 on channel 1, buttons are notes 0-15, Shift is note 16.
 * That is why the surface needs no per-device configuration and why a user's
 * own scene cannot break it.
 *
 * The relative encoding is two's complement, 7-bit -- the same reading
 * relative_cc.h documents for the chain's CC path, and it names the E16 in its
 * own comment. Keep the two in agreement.
 */

export const SHIFT_NOTE = 16;

function ticksOf(value) {
    if (value <= 0 || value > 127) return 0;
    if (value < 64) return value;
    if (value === 64) return 0;         /* no encoder emits this */
    return value - 128;
}

export function decode(msg) {
    if (!msg || msg.length < 3) return null;
    const type = msg[0] & 0xF0, ch = msg[0] & 0x0F;
    if (ch !== 0) return null;          /* remote mode is channel 1 only */
    const d1 = msg[1], d2 = msg[2];

    if (type === 0xB0) {
        if (d1 < 1 || d1 > 16) return null;
        const ticks = ticksOf(d2);
        if (ticks === 0) return null;
        return { type: "turn", enc: d1 - 1, ticks };
    }
    if (type === 0x90 || type === 0x80) {
        const down = type === 0x90 && d2 > 0;
        if (d1 === SHIFT_NOTE) return { type: "shift", down };
        if (d1 > 15) return null;
        return down ? { type: "push", enc: d1 } : { type: "release", enc: d1 };
    }
    return null;
}
```

- [ ] **Step 4: Run the test** → `PASS`

- [ ] **Step 5: Commit**

```bash
git add src/shared/e16_input.mjs tests/host/test_e16_input.sh
git commit -m "e16: decode remote-mode input"
```

---

### Task 5: `e16_map.mjs` — the Shift map model

**Goal:** Turn the live chain shape into 16 cells: slots on the top row, the selected slot's occupied components below.

**Files:**
- Create: `src/shared/e16_map.mjs`
- Create: `tests/host/test_e16_map.sh`

**Acceptance Criteria:**
- [ ] Cells 0–3 are the four slots, with `current` marked
- [ ] Cells 4–15 are the selected slot's **occupied** components in chain order (MIDI FX, synth, audio FX)
- [ ] An empty position produces no cell — holes are not destinations
- [ ] More than 12 occupied components paginates, and `pageCount` reports it
- [ ] The layout is recomputed only when the shape signature changes; the same chain twice returns the identical object
- [ ] A bus cell swaps the lower 12 to that slot's buses

**Verify:** `bash tests/host/test_e16_map.sh` → all assertions pass

**Steps:**

- [ ] **Step 1: Write the failing test**

```bash
#!/usr/bin/env bash
# The caps are 8 MIDI FX + 1 synth + 8 audio FX = 17 per slot, against 12
# cells -- so overflow is reachable in a normal rig and must not silently drop
# the tail. And the stability rule is behavioural, not cosmetic: recomputing
# per frame would move buttons under the user's fingers while they navigate.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { buildMap, shapeSignature } from "./src/shared/e16_map.mjs";
let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + " got " + a + " want " + b); fails++; }
  else console.log("ok   " + n); };

const chain = {
  slots: [
    { midiFx: ["arp"], synth: "braids", fx: ["freeverb", null, "tapescam"] },
    { midiFx: [], synth: null, fx: [] },
    { midiFx: [], synth: "dx7", fx: [] },
    { midiFx: [], synth: null, fx: [] },
  ],
};

const m = buildMap(chain, { slot: 0, page: 0 });
eq("four slot cells", m.cells.slice(0, 4).map(c => c.kind),
   ["slot","slot","slot","slot"]);
eq("current slot marked", m.cells[0].current, true);

// Holes are not destinations: fx[1] is null and must not produce a cell.
eq("occupied only", m.cells.slice(4).filter(Boolean).map(c => c.label),
   ["arp","braids","freeverb","tapescam"]);
eq("single page", m.pageCount, 1);

const full = { slots: [{ midiFx: Array(8).fill("mfx"), synth: "s",
                         fx: Array(8).fill("fx") }, {}, {}, {}] };
const f = buildMap(full, { slot: 0, page: 0 });
eq("17 components paginate", f.pageCount, 2);
eq("page 0 holds twelve", f.cells.slice(4).filter(Boolean).length, 12);
const f2 = buildMap(full, { slot: 0, page: 1 });
eq("page 1 holds the rest", f2.cells.slice(4).filter(Boolean).length, 5);

// Stability: same shape -> same signature -> caller may skip the rebuild.
eq("signature stable", shapeSignature(chain), shapeSignature(chain));
const changed = JSON.parse(JSON.stringify(chain));
changed.slots[0].fx[1] = "gate";
eq("signature moves on shape change",
   shapeSignature(changed) !== shapeSignature(chain), true);

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
```

- [ ] **Step 2: Run it and watch it fail**

- [ ] **Step 3: Write `src/shared/e16_map.mjs`**

Export `shapeSignature(chain)` (a string of slot/position/module ids) and
`buildMap(chain, {slot, page, showBuses})`. Cells are
`{kind, label, slot, component, current}` or `null` for a dark button. Compact
occupied components in chain order; page by twelve. `showBuses` replaces the
lower 12 with that slot's buses using the same rules.

The caller holds the last signature and rebuilds only when it changes — the
stability rule from the design lives at the call site, and this module stays
pure so the test can drive it.

- [ ] **Step 4: Run the test** → `PASS`

- [ ] **Step 5: Commit**

```bash
git add src/shared/e16_map.mjs tests/host/test_e16_map.sh
git commit -m "e16: map model -- occupied components, stable until shape changes"
```

---

### Task 6: Shim — forward cable-2 input to the shadow UI

**Goal:** The encoders and buttons reach JS outside overtake mode, gated so nothing changes for anyone not using the surface.

**Files:**
- Modify: `src/schwung_shim.c:9109` (the cable-2 note-on diversion) and the CC branch below it
- Modify: `src/host/shadow_constants.h` (one flag in `shadow_control_t`)
- Create: `tests/host/test_e16_forward_gate.sh`

**Acceptance Criteria:**
- [ ] A new `external_surface` flag in `shadow_control_t`, consuming reserved space, `sizeof` unchanged
- [ ] While set, cable-2 CC **and note** messages publish to the shadow UI outside overtake
- [ ] While set, cable-2 note-ons are **not** diverted into `shadow_queue_input_led` — that diversion currently swallows every encoder button
- [ ] While clear, behaviour is byte-identical to today
- [ ] The source pin fails if the note-on diversion loses its gate

**Verify:** `bash tests/host/test_e16_forward_gate.sh` → passes; and on device, `dd` the flag byte and confirm encoder turns reach the tool

**Steps:**

- [ ] **Step 1: Add the flag**

In `shadow_control_t`, take a byte from the reserved tail (see the SHM sizing
note in `CLAUDE.md` — the buffer has headroom and only shrinking fails the
build). Keep `sizeof` unchanged and leave `stay_in_shadow`'s raw offset alone;
`schwung-manager` reads it positionally.

- [ ] **Step 2: Gate the note-on diversion**

At `src/schwung_shim.c:9109`:

```c
/* Cable-2 note-ons are queued as LED commands for devices like the M8. An
 * external control surface's encoder BUTTONS are also cable-2 note-ons, and
 * the queue coalesces per note and never publishes them as input -- so with
 * the surface active this diversion silently eats every button press. */
if (cable == 0x02 && type == 0x90 &&
    !(shadow_control && shadow_control->external_surface)) {
    shadow_queue_input_led(src[j], status, d1, d2);
    continue;
}
```

- [ ] **Step 3: Publish cable-2 outside overtake**

In the non-overtake path, alongside the existing cable-0 CC forwarding, publish
cable-2 CC and note messages when `external_surface` is set. Follow the shape
#403 used when it added the cable-2 call into `shadow_master_fx_forward_midi` —
CC only there, because notes already arrive by the pad route; here both are
wanted, since the buttons are notes.

- [ ] **Step 4: Write the source pin**

```bash
#!/usr/bin/env bash
# The note-on diversion is a one-line `continue` that is correct for M8-style
# LED protocols and fatal for a control surface. If someone removes the gate
# while tidying, every encoder button dies and nothing logs it.
set -euo pipefail
cd "$(dirname "$0")/../.."
grep -q "external_surface" src/schwung_shim.c || { echo "FAIL: gate missing"; exit 1; }
awk '/cable == 0x02 && type == 0x90/{found=1} found && /external_surface/{ok=1} END{exit !ok}' \
  src/schwung_shim.c || { echo "FAIL: note-on diversion is ungated"; exit 1; }
echo PASS
```

- [ ] **Step 5: Build, deploy, verify on device**

```bash
SCHWUNG_BUILD_TEST_MODULES=1 ./scripts/build.sh
./scripts/install.sh local --skip-modules --skip-confirmation
```

With the flag set, open `tools/sysex-test` and confirm row 4 shows `cc 1=1 ch1`
on an encoder turn and a note on a button press.

- [ ] **Step 6: Commit**

```bash
git add src/schwung_shim.c src/host/shadow_constants.h tests/host/test_e16_forward_gate.sh
git commit -m "shim: gated cable-2 input forwarding for an external surface"
```

---

### Task 7: Lifecycle — the setting, the ENTER heartbeat, the ACK

**Goal:** The surface finds and holds the device with no way to ask whether one is attached.

**Files:**
- Create: `src/shared/e16_surface.mjs` (lifecycle only in this task)
- Modify: `src/shadow/shadow_ui.js` (Global Settings contract + tick hook + `onMidiMessageExternal`)
- Create: `tests/host/test_e16_lifecycle.sh`

**Acceptance Criteria:**
- [ ] Global Settings → System gains **External Surface**: `Off / E16`
- [ ] While enabled and unacked, ENTER is sent every ~2 s and no more often
- [ ] An ACK marks the device present and stops the heartbeat
- [ ] Losing the device (no ACK to a later probe) returns to seeking, so a power-cycle self-heals
- [ ] Disabling sends EXIT exactly once
- [ ] The Global Settings page list is unchanged in count — a new page would violate the one-section-one-page rule

**Verify:** `bash tests/host/test_e16_lifecycle.sh` → passes; on device, unplug and replug the E16 and confirm it returns to remote mode unaided

**Steps:**

- [ ] **Step 1: Write the failing test** — drive the state machine with an
injected clock and a fake sender, asserting: exactly one ENTER per 2 s while
seeking; zero sends after an ACK; one EXIT on disable; back to seeking after a
loss.

- [ ] **Step 2: Implement the state machine** in `e16_surface.mjs` as a pure
object taking `{now, send}` so the test needs no device:

```javascript
export function createLifecycle({ probeMs = 2000 } = {}) {
    let enabled = false, present = false, lastProbe = -Infinity;
    return {
        setEnabled(on, now, send) {
            if (on === enabled) return;
            enabled = on;
            if (!on) { present = false; send(exitMsg()); }
        },
        tick(now, send) {
            if (!enabled || present) return;
            if (now - lastProbe < probeMs) return;
            lastProbe = now;
            send(enterMsg());
        },
        onSysex(asm) { if (isAck(asm)) present = true; },
        get present() { return present; },
    };
}
```

- [ ] **Step 3: Wire the setting** into the synthesised Global Settings
contract on the **System** section. Use a plain enum row; do not add a `menu`,
which costs the section a second page.

- [ ] **Step 4: Reassemble inbound SysEx** in `shadow_ui.js`'s
`onMidiMessageExternal` — start on `F0`, complete on `F7`, skip `>= 0xF8`,
abort on any other status `>= 0x80`, cap the buffer. `docs/SYSEX.md` spells out
why the last two matter.

- [ ] **Step 5: Run the test** → `PASS`

- [ ] **Step 6: Commit**

```bash
git add src/shared/e16_surface.mjs src/shadow/shadow_ui.js tests/host/test_e16_lifecycle.sh
git commit -m "e16: setting, ENTER heartbeat and ACK detection"
```

---

### Task 8: The parameter view

**Goal:** Two authored grid pages on the 4×4, with rings showing values and turns writing parameters.

**Files:**
- Modify: `src/shared/e16_surface.mjs`
- Create: `src/shared/e16_view.mjs`
- Create: `tests/host/test_e16_view.sh`

**Acceptance Criteria:**
- [ ] Top 2×4 is page N and bottom 2×4 is page N+1 from `planPages()`, unmodified
- [ ] Encoder → cell mapping is stable and matches the drawn layout
- [ ] A turn goes through the grid's own knob-turn path, so enums quantize and read-only cells refuse
- [ ] A value change sends **one** ring chunk, not a framebuffer
- [ ] Navigation sends **one** framebuffer, and never more than one is in flight
- [ ] A component with fewer than 9 cells leaves the bottom half dark rather than borrowing from another level

**Verify:** `bash tests/host/test_e16_view.sh` → passes; on device, turn each encoder and confirm the matching parameter moves

**Steps:**

- [ ] **Step 1: Write the failing test** covering the encoder→cell mapping, the
"one ring chunk per value change" rule, and that a 5-cell page leaves 11
encoders unmapped.

- [ ] **Step 2: Build the view model** in `e16_view.mjs`: take the planner's
pages, pair N and N+1, and return `{cells[16], headers[2]}` where a cell is
`null` or `{key, label, value, min, max, bipolar, readOnly}`.

- [ ] **Step 3: Render** through `createCanvas()` — two half-width headers, cell
names beneath — and send with `framebufferMsg`.

- [ ] **Step 4: Wire input** — `decode()` → cell → the grid's turn path → param
write → ring update for that encoder only.

- [ ] **Step 5: Pace the sends.** One framebuffer in flight; coalesce ring
updates per encoder per tick. `docs/SYSEX.md`: 31 packets sent alone arrive
byte-perfect, 34 amid other traffic lost 8.

- [ ] **Step 6: Run the test** → `PASS`, then verify on device

- [ ] **Step 7: Commit**

```bash
git add src/shared/e16_view.mjs src/shared/e16_surface.mjs tests/host/test_e16_view.sh
git commit -m "e16: parameter view -- two authored pages, rings, paced sends"
```

---

### Task 9: The map view and navigation

**Goal:** Hold Shift to see and jump anywhere in the chain.

**Files:**
- Modify: `src/shared/e16_surface.mjs`, `src/shared/e16_view.mjs`
- Create: `tests/host/test_e16_nav.sh`

**Acceptance Criteria:**
- [ ] Holding Shift draws the map; releasing restores the parameter view
- [ ] Top-row push switches slot and redraws the lower 12
- [ ] Lower push jumps focus to that component and drops back to parameters
- [ ] Shift + turn pages within the component
- [ ] The bus cell swaps the lower 12 to that slot's buses
- [ ] A lost Shift note-off cannot strand the map — it clears on any parameter-view interaction

**Verify:** `bash tests/host/test_e16_nav.sh` → passes; on device, navigate to a component in another slot without touching Move

**Steps:**

- [ ] **Step 1: Write the failing test** driving a scripted event sequence
(shift down, push slot 2, push component 3, shift up) and asserting the
resulting focus and exactly two framebuffer sends.

- [ ] **Step 2: Add the map render** using `buildMap()` and `createCanvas()`.

- [ ] **Step 3: Add the shift state machine**, including the stranded-modifier
escape. `CLAUDE.md`'s pad-block lesson applies: a modifier state that can only
be cleared by an event you might not receive will eventually stick, and the fix
is an invariant restated each frame, not a longer exit list.

- [ ] **Step 4: Run the test** → `PASS`, then verify on device

- [ ] **Step 5: Commit**

```bash
git add src/shared/e16_surface.mjs src/shared/e16_view.mjs tests/host/test_e16_nav.sh
git commit -m "e16: Shift map and navigation"
```

---

### Task 10: Follow Focus

**Goal:** Optionally mirror the Move's screen instead of holding an independent focus.

**Files:**
- Modify: `src/shared/e16_surface.mjs`, `src/shadow/shadow_ui.js`
- Create: `tests/host/test_e16_follow.sh`

**Acceptance Criteria:**
- [ ] Global Settings → System gains **Follow Focus** (on/off), beside External Surface
- [ ] While on, focus tracks the shadow UI's current component and the map is disabled
- [ ] Follow is one-way — navigating on the E16 never moves Move's screen
- [ ] Turning it off restores the E16's own last focus rather than resetting

**Verify:** `bash tests/host/test_e16_follow.sh` → passes; on device, change component on Move and watch the E16 follow

**Steps:**

- [ ] **Step 1: Write the failing test** asserting that with follow on, a map
gesture is ignored, and that toggling off restores the prior independent focus.

- [ ] **Step 2: Implement** as a mode on the surface, not a second focus owner —
one focus variable, two sources, with follow winning while enabled.

- [ ] **Step 3: Run the test** → `PASS`, verify on device

- [ ] **Step 4: Commit**

```bash
git add src/shared/e16_surface.mjs src/shadow/shadow_ui.js tests/host/test_e16_follow.sh
git commit -m "e16: follow focus"
```

---

### Task 11: Ownership — the E16 claims CC 1–16 ch 1

**Goal:** The surface and #403's CC Map can never both act on one turn.

**Files:**
- Modify: `src/modules/chain/dsp/chain_midi.c` (the CC-map lookup)
- Create: `tests/host/test_e16_cc_claim.sh`

**Acceptance Criteria:**
- [ ] While `external_surface` is set, CC 1–16 on channel 1 never reaches the CC Map
- [ ] While clear, the CC Map's behaviour is unchanged
- [ ] The claimed range is defined once, in a header both sides read — not restated

**Verify:** `bash tests/host/test_e16_cc_claim.sh` → passes; on device, map CC 1 to a parameter, enable the surface, and confirm the parameter stops following the CC Map

**Steps:**

- [ ] **Step 1: Define the range once**

```c
/* src/host/e16_claim.h
 *
 * Remote mode emits CC 1-16 on channel 1, and a user may have mapped those
 * same numbers in the CC Map. Two owners acting on one turn is a parameter
 * that moves twice as far as it should -- with nothing logged, because both
 * are behaving correctly. The surface claims them while it is active.
 *
 * Defined here so the shim, the chain and tests read one fact. cc_reserved.h
 * exists for the same reason after the same class of bug. */
static inline int e16_claims_cc(int channel, int cc) {
    return channel == 0 && cc >= 1 && cc <= 16;
}
```

- [ ] **Step 2: Consult it in the CC-map path**, gated on the flag.

- [ ] **Step 3: Write the test** covering both flag states and the boundaries
(CC 0 and 17 are never claimed; channel 2 is never claimed).

- [ ] **Step 4: Run, verify on device, commit**

```bash
git add src/host/e16_claim.h src/modules/chain/dsp/chain_midi.c tests/host/test_e16_cc_claim.sh
git commit -m "e16: claim CC 1-16 ch1 so the surface and the CC Map cannot collide"
```

---

### Task 12: Hardware verification

**Goal:** Confirm on the device what host tests structurally cannot: feel, legibility and rate.

**USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

**Files:** none — this is measurement.

**Acceptance Criteria:**
- [ ] Every encoder moves its matching parameter, and the mapping matches the drawn layout — checked on a module with more than 8 parameters
- [ ] Navigating between two components in different slots takes under a second, measured with a stopwatch or a log timestamp
- [ ] A framebuffer redraw arrives intact: send 20 navigations in a row and confirm no corrupted screen
- [ ] Ring positions match parameter values across the full range, including a bipolar parameter
- [ ] Unplugging and replugging the E16 restores remote mode with no user action

**Verify:** Arm `touch /data/UserData/schwung/log_xmos_sysex_on`, run the sequence, and confirm the capture shows no truncated outbound message; disarm afterwards.

**Steps:**

- [ ] **Step 1: Deploy** `./scripts/install.sh local --skip-modules --skip-confirmation`

- [ ] **Step 2: Run each acceptance item and capture the result** — a photo of
the E16 screen for legibility, the tap capture for rate, a note of the measured
navigation latency.

- [ ] **Step 3: If the framebuffer rate fails**, switch the renderer to
`labelsMsg` — 92 bytes against 1171. The view model does not change; only the
renderer does. Record the measurement that forced it.

- [ ] **Step 4: Record findings** in the design doc's Deferred section and, if
the rate limit bit, on issue #358.

---

## Self-review

**Spec coverage:** protocol → Tasks 1–2; framebuffer → 3; input → 4; map → 5, 9;
shim gate → 6; lifecycle → 7; parameter view and rate → 8; follow focus → 10;
ownership vs #403 → 11; testing → each task plus 12. The Deferred section is
deliberately unimplemented.

**Placeholder scan:** Tasks 8–10 give acceptance criteria and step intent
without full code, because they compose modules defined in Tasks 2–5 whose
signatures are fixed there. Tasks 2–5, which introduce new interfaces, carry
complete code and complete tests.

**Type consistency:** `pack7`, `enterMsg`, `exitMsg`, `ringMsg`, `ledMsg`,
`framebufferMsg`, `labelsMsg`, `packetize`, `isAck` (Task 2); `createCanvas`
(Task 3); `decode`, `SHIFT_NOTE` (Task 4); `buildMap`, `shapeSignature`
(Task 5); `createLifecycle` (Task 7) — used under those names throughout.

---

## Known gap, found during implementation

**The chain DSP cannot read `shadow_control_t.external_surface`.** Task 11's
ownership check is in `chain_midi.c`, which is a dlopen'd plugin with no
link-time access to the shim's SHM layout, so it currently reads a file-scope
`g_e16_surface_active` that is always 0. The claim is therefore inert: the CC
Map's behaviour is provably unchanged, and the surface does not yet actually own
CC 1-16.

This was a hole in the plan, not in the implementation — Task 11's acceptance
criteria assumed a flag the DSP can see, and it cannot. Closing it needs one of:

- the shim writing the flag to the chain via the existing param channel
  (`chain:external_surface`), which is the smallest change and matches how other
  shim-to-chain state already travels; or
- the check moving up into the shim, before the message is handed to the chain
  at all — arguably the better home, since the shim is where the surface's
  ownership is decided and it already reads the flag.

Prefer the second on the next pass. Until then, treat Task 11 as "the predicate
and its test are correct and pinned; the wire is not connected."

## Two more findings from Task 8

**`pageSlotKeys` predates `as_page`, and a module-owned page would go dark.**
It tests `kind === PAGE_KNOBS` strictly. `CLAUDE.md` records that a module can
own a PAGE and that it is a PAGE_KNOBS page with a drawer, NOT a new kind --
which is why three controller gates were changed to ask `pageHasKnobs` ("does it
have keys") instead. `pageSlotKeys` is a fourth site that was not. The surface
works around it locally, but the underlying gate is wrong for the knob grid too:
a module-owned canvas page has keys and would answer no. Worth fixing upstream,
out of scope here.

**A bottom-half turn must move the controller's page first, and that is a
sharing hazard.** The controller has ONE current page, so applying a turn to the
lower 2x4 requires `goToPage(N+1, {remember:false})` before `onKnobTurn(slot)`,
or it writes page N's key at the same slot number -- two small in-range integers,
both valid, nothing logged. The consequence for wiring: **the surface must hold
its own controller instance**, not share Move's, or every bottom-half turn drags
Move's screen to the next page. The design already says the surface owns its own
focus; this is the mechanical reason it has to.

---

## Task 13: assemble the surface (DONE)

**Tasks 1-11 build every component and wire none of them together.** The
lifecycle runs; the shim gate is written and restated; the view, the map, the
navigation and follow focus are complete, pure and mutation-tested. Nothing
constructs them. `e16Nav` is `null` in `shadow_ui.js` and `createNav` /
`createDisplay` are never called, so with a device attached Schwung enters remote
mode and then draws nothing.

**This is a planning error, and it happened three times in one plan** — the same
shape each time:

| gap | who could see it |
|---|---|
| the CC claim reads a static that is always 0 | neither Task 6 nor Task 11 |
| the setting never reached `shadow_control->external_surface` | neither Task 6 nor Task 7 |
| nothing constructs the view or nav | none of Tasks 8, 9, 10 |

Every task was scoped to its own files and passed its own tests. The seams
between them belonged to no task, so nobody was responsible for them and nothing
failed when they were missing. **A plan that enumerates components must also name
the assembly, and give it acceptance criteria that only pass when the thing
actually runs.** Component tests cannot see this: each half is correct.

### What remains

- Construct `createDisplay()`, `createNav()` and the existing lifecycle together
  in `shadow_ui.js`, and hold the result where the tick can reach it.
- Give the nav its sources: the chain shape for `buildMap` (read the same way the
  chain editor reads it), a page plan for the focused component, and parameter
  reads for the view.
- **The surface needs its OWN controller instance**, not Move's — see the
  shared-controller hazard above. A bottom-half turn moves the controller's page
  before applying the turn, so a shared controller drags Move's screen along with
  every lower-row knob.
- Route `onMidiMessageExternal` through `decode()` into the nav, after the
  lifecycle's assembler has had the SysEx.
- Drain the paced sender through `packetize()` + `move_midi_external_send()`,
  honouring the `false`-means-retry contract.

### Acceptance criteria that would have caught all three gaps

- [x] With the setting on and a fake device acking, a scripted component change
      produces a framebuffer on the wire — asserted end to end, from the setting
      to the bytes, not from any one module's unit
- [x] With the setting off, nothing is sent at all
- [x] A turn arriving as raw MIDI bytes moves a parameter — the whole path,
      `onMidiMessageExternal` to `set_param`

### What was built, and where the assembly actually lives

`createSurface()` in `src/shared/e16_surface.mjs`, not in `shadow_ui.js`.

That is the whole answer to "why did eleven green tasks ship a feature that did
not run": **`shadow_ui.js` cannot be imported under node**, so anything living
there can only ever be GREPPED — and a grep is precisely what could not see this
gap. Composed in a `.mjs`, the entire path (a setting, a clock, raw MIDI bytes
in, USB-MIDI packets out) is driven by `tests/host/test_e16_wiring.sh` with no
device and no shim. What is left in `shadow_ui.js` is five injected seams and two
calls, and only those are source-pinned.

Three rules were found by writing the assembly and are enforced there:

- **The surface holds its OWN controller**, built from a `makeController`
  FACTORY the host supplies. A host that handed over the grid's existing
  controller would be indistinguishable from one that built a second, and the
  symptom — Move's screen dragged to page N+1 by every lower-row knob — is two
  small in-range integers disagreeing with nothing logged. The factory takes a
  LIVE view of the focus, not a slot number: one controller outlives many jumps.
- **At most one message per tick, across BOTH producers.** `createDisplay`
  enforces that budget inside itself but cannot see the lifecycle, which sends
  on the same port. A keepalive ENTER landing in the same tick as a 391-packet
  framebuffer is exactly the "amid other traffic" case that lost 8 whole
  packets. The two are joined in `tick()` because that is the only place that
  can see both.
- **No device, no frames and no reads.** A frame sent while seeking goes
  nowhere, and the device that acks a moment later comes up showing the last
  process's screen while the surface believes it has painted; `ctl.tick()` is a
  ~2.8 ms staggered IPC read whose only consumer is a screen nobody is looking
  at. Both are gated on `present`, and the repaint is re-owed on every
  false->true edge — which is what makes a replug (the E16 has no battery, so
  unplugging clears its screen) redraw itself with no user action.

**Task 12 (hardware verification) is still open**; nothing here has been on a
device.
