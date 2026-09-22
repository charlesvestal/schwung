# E16 Partial OLED Updates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add SCANLINE/RECTANGLE/CLEAR/ACK/NACK support to the E16 remote-mode protocol layer and wire it into `createDisplay()` so a framebuffer repaint sends only the changed region(s) instead of the whole 1024-byte screen, ready to exercise the moment OXI ships the firmware.

**Architecture:** New pure functions layered the same way the rest of `src/shared/e16_*.mjs` is: wire-format builders/parsers in `e16_protocol.mjs`, a pixel-level read/pack helper in `e16_canvas.mjs`, a row-run diff engine in a new `e16_diff.mjs`, and integration into `createDisplay()`'s existing send-one-message-per-tick state machine in `e16_surface.mjs`. Everything is pure and injectable — no globals, no new host/C wiring, matching the file's existing house style.

**Tech Stack:** Plain ES modules (`.mjs`), no build step; tests are `node --input-type=module -e '...'` scripts under `tests/host/`, matching the existing `test_e16_*.sh` pattern.

**User decisions (already made):**
- Full integration now, not gated behind a flag — "we won't release this without hardware anyway, so build it how you expect it to work and we'll adjust when we test."
- Pixel-buffer diff (not view-reported dirty regions).
- ACK/NACK feeds the existing self-heal heartbeat/diff machinery — no new retry/timeout state machine.
- No CRC (algorithm undocumented in the draft spec).
- Follow the spec sheet's literal example bytes for the 5 new opcodes (no `0x06` category prefix), gated behind one named constant so it's a one-line fix if hardware disagrees.
- Diff by row-run clustering, not a single global bounding box, with two adjustable caps (`MAX_REGIONS`, `FULL_REPAINT_THRESHOLD`) — motivated by: a single bbox degrades badly when two changes are spatially far apart (e.g. opposite map corners), either producing one screen-spanning rectangle or falsely tripping a whole-diff area threshold.

Design doc: `docs/superpowers/specs/2026-09-22-e16-partial-oled-updates-design.md`

---

### Task 1: Protocol layer — SCANLINE / RECTANGLE / CLEAR / ACK / NACK

**Goal:** `e16_protocol.mjs` can build the three new outbound messages and parse the two new inbound reply bodies, all pinned against the spec sheet's own example bytes.

**Files:**
- Modify: `src/shared/e16_protocol.mjs` (currently 107 lines, ends after `isAck`)
- Test: `tests/host/test_e16_protocol.sh` (extend the existing file)

**Acceptance Criteria:**
- [ ] `scanlineMsg(y, rowBits)`, `rectangleMsg(x, y, w, h, bits)`, `clearMsg()` build correctly-framed SysEx byte arrays
- [ ] `parseOledUpdateReply(asm)` returns `{ ok, cmd, status, addr }` for a valid ACK/NACK body, `null` for anything else (including the unrelated `REMOTE MODE ENTERED ACK`)
- [ ] `pack7`/`unpack7` round-trip for arbitrary byte arrays
- [ ] `OLED_SUBCOMMAND_HAS_CATEGORY_PREFIX` is a single named, exported constant controlling whether the 5 new messages get a `0x06` prefix
- [ ] `tests/host/test_e16_protocol.sh` passes

**Verify:** `bash tests/host/test_e16_protocol.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Append the new protocol code to `e16_protocol.mjs`**

Add after the existing `isAck` function (end of file):

```javascript
/*
 * ---------------------------------------------------------------------------
 * PARTIAL OLED UPDATES -- built against OXI's DRAFT spec (shared 2026-09-21,
 * not yet shipped in firmware; see docs/superpowers/specs/2026-09-22-e16-
 * partial-oled-updates-design.md). Nothing here has been verified on
 * hardware.
 *
 * SPEC AMBIGUITY: the sheet's ID column lists every opcode as TWO bytes
 * (0x06 0xXX), and the 8 existing messages' example bytes agree. The 5 NEW
 * messages below (SCANLINE/RECTANGLE/CLEAR/ACK/NACK) list the same two-byte
 * ID, but their EXAMPLE bytes drop the 0x06 (e.g. SCANLINE's worked example
 * is "...02 01 05 [payload] F7", not "...01 06 05..."). A loose note at the
 * top of the sheet -- "0x06 is the category message, no longer necessary
 * once in REMOTE MODE" -- could explain this but doesn't clearly say it
 * applies only to these five. We follow the literal example bytes (the more
 * concrete evidence) behind this one flag so a wrong guess is a one-line fix
 * once real firmware answers.
 * ---------------------------------------------------------------------------
 */
export const OLED_SUBCOMMAND_HAS_CATEGORY_PREFIX = false;

const OLED_SCANLINE_ID = 0x05;
const OLED_RECTANGLE_ID = 0x06;
const OLED_CLEAR_ID = 0x07;
const OLED_UPDATE_ACK_ID = 0x53;
const OLED_UPDATE_NACK_ID = 0x54;

function oledId(subId) {
    return OLED_SUBCOMMAND_HAS_CATEGORY_PREFIX ? [0x06, subId] : [subId];
}

/* rowBits: 16 bytes, left to right, MSB first; 1 = on. No CRC -- see the
 * design doc; the field is optional in the spec and its algorithm is
 * undocumented. */
export function scanlineMsg(y, rowBits) {
    if (rowBits.length !== 16) throw new Error("scanline row must be 16 bytes");
    return msg(oledId(OLED_SCANLINE_ID), [y & 0x3F].concat(Array.from(rowBits)));
}

/* bits: ceil(w/8) * h bytes, row-major, MSB first, each row byte-aligned.
 * No CRC, same reason as scanlineMsg. */
export function rectangleMsg(x, y, w, h, bits) {
    const want = Math.ceil(w / 8) * h;
    if (bits.length !== want) {
        throw new Error("rectangle payload must be " + want + " bytes, got " + bits.length);
    }
    return msg(oledId(OLED_RECTANGLE_ID),
               [x & 0x7F, y & 0x3F, w & 0x7F, h & 0x3F].concat(Array.from(bits)));
}

export function clearMsg() {
    return msg(oledId(OLED_CLEAR_ID), []);
}

/* Inverse of pack7: packed groups of <=8 bytes (1 MSB byte + up to 7 payload
 * bytes) back to rawLen raw bytes. rawLen is required -- the packed stream
 * carries no length of its own, and pack7's last group can be short. */
export function unpack7(packed, rawLen) {
    const out = [];
    let pi = 0;
    for (let done = 0; done < rawLen; ) {
        const n = Math.min(7, rawLen - done);
        const msbs = packed[pi++];
        for (let k = 0; k < n; k++) {
            const lo = packed[pi++] & 0x7F;
            out.push(lo | (((msbs >> k) & 1) ? 0x80 : 0));
        }
        done += n;
    }
    return out;
}

/* asm holds everything between F0 and F7, as createSysexAssembler delivers
 * it. Returns { id, payload } if asm starts with our header and carries an
 * id of the expected width, else null -- payload is whatever pack7'd bytes
 * follow, unparsed. */
function oledReplyHeader(asm) {
    const idLen = OLED_SUBCOMMAND_HAS_CATEGORY_PREFIX ? 2 : 1;
    if (asm.length < HDR.length + idLen) return null;
    for (let i = 0; i < HDR.length; i++) if (asm[i] !== HDR[i]) return null;
    if (idLen === 2) {
        if (asm[HDR.length] !== 0x06) return null;
        return { id: asm[HDR.length + 1], payload: asm.slice(HDR.length + 2) };
    }
    return { id: asm[HDR.length], payload: asm.slice(HDR.length + 1) };
}

/* cmd is the ORIGINAL COMMAND byte the device echoes back (its own SCANLINE/
 * RECTANGLE/CLEAR id, not ours -- the spec's ACK/NACK payload names which
 * update it is about). addr's four raw bytes decode per that command; 0xFF
 * in a field means "not applicable to this command", decoded to null rather
 * than the literal 255, per the tri-state read rule (CLAUDE.md: a filler
 * value must never be read as data). */
function decodeOledAddr(cmd, a) {
    const opt = (v) => (v === 0xFF ? null : v);
    if (cmd === OLED_SCANLINE_ID) return { y: opt(a[0]) };
    if (cmd === OLED_RECTANGLE_ID) {
        return { x: opt(a[0]), y: opt(a[1]), w: opt(a[2]), h: opt(a[3]) };
    }
    return {};   /* CLEAR, or an unrecognised command -- no address fields */
}

/* Returns { ok, cmd, status, addr } for an OLED UPDATE ACK or NACK body, or
 * null for anything else -- including the unrelated REMOTE MODE ENTERED ACK,
 * which shares status byte 0x53 but never this length (it carries no
 * payload). Check isAck() first in a caller that cares about both, since
 * that's the hot path. */
export function parseOledUpdateReply(asm) {
    const h = oledReplyHeader(asm);
    if (!h) return null;
    if (h.id !== OLED_UPDATE_ACK_ID && h.id !== OLED_UPDATE_NACK_ID) return null;
    const raw = unpack7(h.payload, 6);
    if (raw.length !== 6) return null;
    const [cmd, status, a0, a1, a2, a3] = raw;
    return { ok: h.id === OLED_UPDATE_ACK_ID, cmd, status, addr: decodeOledAddr(cmd, [a0, a1, a2, a3]) };
}
```

- [ ] **Step 2: Extend the test file**

Append to `tests/host/test_e16_protocol.sh`, just before the final `console.log(fails ? ...)` block — move that block to the very end and insert this before it:

```javascript
import { scanlineMsg, rectangleMsg, clearMsg, parseOledUpdateReply, pack7, unpack7 }
  from "./src/shared/e16_protocol.mjs";

/* SCANLINE: header, id 0x05 (no 0x06 prefix -- see OLED_SUBCOMMAND_HAS_CATEGORY_PREFIX),
 * then pack7(17 raw bytes: y + 16 row bytes). */
const scan = scanlineMsg(3, new Array(16).fill(0));
eq("scanline header+id", scan.slice(0, 7), [0xF0,0x00,0x21,0x5B,0x02,0x01,0x05]);
eq("scanline length", scan.length, 1 + 5 + 1 + pack7(new Array(17).fill(0)).length + 1);

/* RECTANGLE: x=2 y=3 w=8 h=2 -> ceil(8/8)*2 = 2 payload bytes. */
const rect = rectangleMsg(2, 3, 8, 2, [0xFF, 0x00]);
eq("rectangle header+id", rect.slice(0, 7), [0xF0,0x00,0x21,0x5B,0x02,0x01,0x06]);
eq("rectangle length", rect.length, 1 + 5 + 1 + pack7([2,3,8,2,0xFF,0x00]).length + 1);
let threw = false;
try { rectangleMsg(2, 3, 8, 2, [0x00]); } catch (e) { threw = true; }
eq("rectangle wrong payload length throws", threw, true);

eq("clear bytes", clearMsg(), [0xF0,0x00,0x21,0x5B,0x02,0x01,0x07,0xF7]);

/* pack7/unpack7 round-trip, arbitrary length including a short last group. */
for (const raw of [[], [1], [0xFF,0x00,0x80,0x7F,0x01,0xFE,0x55,0xAA], new Array(20).fill(0x81)]) {
  eq("pack7/unpack7 round-trip len=" + raw.length, unpack7(pack7(raw), raw.length), raw);
}

/* An OLED UPDATE ACK for a RECTANGLE at (2,3,8,2): cmd=0x06, status=0x00,
 * addr = x,y,w,h. */
const ackRaw = [0x06, 0x00, 2, 3, 8, 2];
const ackAsm = [0x00,0x21,0x5B,0x02,0x01,0x53].concat(pack7(ackRaw));
eq("oled ack parses", parseOledUpdateReply(ackAsm),
   { ok: true, cmd: 0x06, status: 0x00, addr: { x: 2, y: 3, w: 8, h: 2 } });

/* A NACK for a SCANLINE at y=3 with a CRC mismatch (status 0x03); unused
 * address bytes are 0xFF per spec and must decode to null, not 255. */
const nackRaw = [0x05, 0x03, 3, 0xFF, 0xFF, 0xFF];
const nackAsm = [0x00,0x21,0x5B,0x02,0x01,0x54].concat(pack7(nackRaw));
eq("oled nack parses", parseOledUpdateReply(nackAsm),
   { ok: false, cmd: 0x05, status: 0x03, addr: { y: 3 } });

/* The unrelated REMOTE MODE ENTERED ACK (same 0x53, WITH the 0x06 prefix,
 * no payload) must not parse as an OLED reply. */
eq("remote-mode ack is not an oled reply",
   parseOledUpdateReply([0x00,0x21,0x5B,0x02,0x01,0x06,0x53]), null);
eq("garbage is not an oled reply", parseOledUpdateReply([1,2,3]), null);
```

- [ ] **Step 3: Run the test**

Run: `bash tests/host/test_e16_protocol.sh`
Expected: every line `ok   ...`, final line `PASS`, exit 0

- [ ] **Step 4: Commit**

```bash
git add src/shared/e16_protocol.mjs tests/host/test_e16_protocol.sh
git commit -m "e16: SCANLINE/RECTANGLE/CLEAR/ACK/NACK wire format, against the draft spec"
```

---

### Task 2: Canvas pixel read + row-major packer

**Goal:** `e16_canvas.mjs` can read a single pixel from any 1024-byte page/column buffer and pack an arbitrary sub-rectangle into the row-major, MSB-first format SCANLINE/RECTANGLE want.

**Files:**
- Modify: `src/shared/e16_canvas.mjs` (`WIDTH`/`HEIGHT` currently module-private at lines 34-35)
- Test: `tests/host/test_e16_canvas.sh` (extend)

**Acceptance Criteria:**
- [ ] `WIDTH`/`HEIGHT` are exported (still 128/64)
- [ ] `readPixel(buf, x, y)` matches the exact bit positions `test_e16_canvas.sh` already pins for `setPixel` (same page/column math, read-only, out-of-bounds returns 0)
- [ ] `packRowMajor(buf, x, y, w, h)` produces `ceil(w/8)*h` bytes, row-major, MSB-first, matching a hand-worked fixture
- [ ] `tests/host/test_e16_canvas.sh` passes

**Verify:** `bash tests/host/test_e16_canvas.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Export `WIDTH`/`HEIGHT` and add the two functions**

In `src/shared/e16_canvas.mjs`, change:

```javascript
const WIDTH = 128;
const HEIGHT = 64;
```

to:

```javascript
export const WIDTH = 128;
export const HEIGHT = 64;
```

Then add, after the `createCanvas` function's closing brace (end of file):

```javascript
/*
 * Read-only mirror of createCanvas()'s setPixel bit math, but taking any
 * 1024-byte buffer rather than closing over one canvas instance -- the E16
 * diff engine needs to read pixels out of TWO buffers (the last one sent and
 * the one just rendered), neither of which is necessarily "the" live canvas.
 * Out-of-bounds reads 0, matching setPixel's silent clip rather than
 * throwing.
 */
export function readPixel(buf, x, y) {
    x |= 0; y |= 0;
    if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT) return 0;
    const byteIdx = (y >> 3) * WIDTH + x;
    const bit = y & 7;
    return (buf[byteIdx] >> bit) & 1;
}

/*
 * The SCANLINE/RECTANGLE wire format is ROW-MAJOR, MSB first, each row
 * byte-aligned -- the transpose of this buffer's own page/column layout.
 * This is the only place that conversion happens; e16_diff.mjs and
 * e16_surface.mjs both call it rather than re-deriving the bit math.
 */
export function packRowMajor(buf, x, y, w, h) {
    const rowBytes = Math.ceil(w / 8);
    const out = new Uint8Array(rowBytes * h);
    for (let ry = 0; ry < h; ry++) {
        for (let rx = 0; rx < w; rx++) {
            if (!readPixel(buf, x + rx, y + ry)) continue;
            const byteIdx = ry * rowBytes + (rx >> 3);
            const bit = 7 - (rx & 7);   /* MSB first */
            out[byteIdx] |= (1 << bit);
        }
    }
    return out;
}
```

- [ ] **Step 2: Extend the test file**

Append to `tests/host/test_e16_canvas.sh`, before the final `console.log(fails ? ...)` block (move that block to the end):

```javascript
import { readPixel, packRowMajor } from "./src/shared/e16_canvas.mjs";

/* readPixel must agree with setPixel's own pinned bit positions above. */
const rc = createCanvas();
rc.fillRect(0, 0, 1, 1, 1);
eq("readPixel (0,0)", readPixel(rc.toBuffer(), 0, 0), 1);
eq("readPixel (0,1) unset", readPixel(rc.toBuffer(), 0, 1), 0);
rc.clear();
rc.fillRect(0, 8, 1, 1, 1);
eq("readPixel (0,8)", readPixel(rc.toBuffer(), 0, 8), 1);
eq("readPixel out of bounds", readPixel(rc.toBuffer(), 999, 999), 0);

/* packRowMajor: a 2x3 region, MSB-first means bit 7 of byte 0 is the
 * LEFTMOST column. Light (x=1,y=1) only inside a region starting at (1,1)
 * of size (w=2,h=3) -> row 0 has bit7 set (leftmost of the region), rows 1-2
 * are zero. */
const pc = createCanvas();
pc.fillRect(1, 1, 1, 1, 1);
const packed = packRowMajor(pc.toBuffer(), 1, 1, 2, 3);
eq("packRowMajor length", packed.length, Math.ceil(2 / 8) * 3);
eq("packRowMajor row 0 (leftmost bit set)", packed[0], 0x80);
eq("packRowMajor row 1 (empty)", packed[1], 0x00);
eq("packRowMajor row 2 (empty)", packed[2], 0x00);

/* A full-width row: bit positions run left to right across the whole row. */
const pc2 = createCanvas();
pc2.fillRect(0, 0, 1, 1, 1);   /* leftmost column */
pc2.fillRect(127, 0, 1, 1, 1); /* rightmost column */
const wide = packRowMajor(pc2.toBuffer(), 0, 0, 128, 1);
eq("packRowMajor full row length", wide.length, 16);
eq("packRowMajor leftmost bit", wide[0], 0x80);
eq("packRowMajor rightmost bit", wide[15], 0x01);
```

- [ ] **Step 3: Run the test**

Run: `bash tests/host/test_e16_canvas.sh`
Expected: every line `ok   ...`, final line `PASS`, exit 0

- [ ] **Step 4: Commit**

```bash
git add src/shared/e16_canvas.mjs tests/host/test_e16_canvas.sh
git commit -m "e16: readPixel + packRowMajor, the page/column to row-major transpose"
```

---

### Task 3: Diff engine — row-run clustering

**Goal:** A new pure module, `e16_diff.mjs`, decides how to describe the difference between two framebuffers as 0-N small regions or a full repaint, using row-run clustering (not a single global bounding box) so spatially separated changes don't degrade into one huge rectangle.

**Files:**
- Create: `src/shared/e16_diff.mjs`
- Test: `tests/host/test_e16_diff.sh`

**Acceptance Criteria:**
- [ ] Identical buffers → `{ kind: "none" }`
- [ ] `prev === null` → `{ kind: "full" }`
- [ ] A single small change → one `"rect"` region
- [ ] A full-width single-row change → one `"scanline"` region
- [ ] Two changes in far-apart row bands (the motivating case: top-left + bottom-right) → **two small regions**, neither anywhere near screen-sized
- [ ] More than `MAX_REGIONS` scattered row-runs → `{ kind: "full" }`
- [ ] Any single region exceeding `FULL_REPAINT_THRESHOLD` of the screen area → `{ kind: "full" }`
- [ ] `MAX_REGIONS`/`FULL_REPAINT_THRESHOLD` are overridable via `opts`
- [ ] `tests/host/test_e16_diff.sh` passes

**Verify:** `bash tests/host/test_e16_diff.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Write `src/shared/e16_diff.mjs`**

```javascript
/*
 * e16_diff.mjs -- decides how to describe the difference between two
 * SSD1306 page/column framebuffers: as zero, one, or a few small SCANLINE/
 * RECTANGLE regions, or as "give up, send the whole FRAMEBUFFER".
 *
 * A SINGLE BOUNDING BOX OVER THE WHOLE DIFF IS THE WRONG SHAPE. Two
 * far-apart changes -- e.g. the map cursor moving from a top-left cell to a
 * bottom-right one, an entirely ordinary navigation -- produce one box
 * spanning nearly the whole screen even though the actual changed pixels
 * are a couple of percent of it. Naively that either sends one needlessly
 * huge RECTANGLE, or trips a whole-screen-area threshold and falls back to
 * FRAMEBUFFER -- defeating the point in exactly the case (fast navigation)
 * partial updates are for.
 *
 * So this clusters by ROW RUN instead of one global box:
 *   1. which of the 64 rows differ at all
 *   2. group contiguous differing rows into runs
 *   3. for EACH run, take the x-bounds from only the pixels that differ
 *      WITHIN that run's rows -- this is what keeps a run over one cell
 *      from being dragged wide by a change in a totally different cell;
 *      two far-apart changes land in two different runs with two different,
 *      each-small x-ranges.
 *
 * It does not fully solve two changes in the SAME row-band but far apart in
 * x (e.g. top-left and top-right cells): those collapse into one run whose
 * x-range spans both. That run is still small in AREA though (one row-band
 * tall), so it costs little on the wire even in that case; true 2D
 * connected-component clustering would be more optimal but is real added
 * complexity (flood-fill/union-find, multi-rect merging) for a screen this
 * small, where the wire-cost difference is modest. Revisit only if hardware
 * timing shows row-runs aren't good enough.
 */
import { readPixel, WIDTH, HEIGHT } from "./e16_canvas.mjs";

export const MAX_REGIONS = 4;
export const FULL_REPAINT_THRESHOLD = 0.4;

function rowXBounds(prev, next, y) {
    let x0 = -1, x1 = -1;
    for (let x = 0; x < WIDTH; x++) {
        if (readPixel(prev, x, y) !== readPixel(next, x, y)) {
            if (x0 === -1) x0 = x;
            x1 = x;
        }
    }
    return x0 === -1 ? null : { x0, x1 };
}

function regionForRun(prev, next, yStart, yEnd) {
    let x0 = WIDTH, x1 = -1;
    for (let y = yStart; y <= yEnd; y++) {
        const b = rowXBounds(prev, next, y);
        if (!b) continue;
        if (b.x0 < x0) x0 = b.x0;
        if (b.x1 > x1) x1 = b.x1;
    }
    const y = yStart, h = yEnd - yStart + 1, x = x0, w = x1 - x0 + 1;
    if (h === 1 && w === WIDTH) return { kind: "scanline", y };
    return { kind: "rect", x, y, w, h };
}

/**
 * @param {Uint8Array|null} prev  what we believe the device shows, or null
 *        if unknown (forces a full repaint -- there is nothing to diff
 *        against).
 * @param {Uint8Array} next       the freshly rendered 1024-byte buffer.
 * @param {object} [opts]
 * @param {number} [opts.maxRegions]            default MAX_REGIONS
 * @param {number} [opts.fullRepaintThreshold]  default FULL_REPAINT_THRESHOLD
 * @returns {{kind:"none"}|{kind:"full"}|{kind:"regions",regions:Array}}
 */
export function diffFramebuffers(prev, next, opts) {
    const o = opts || {};
    const maxRegions = o.maxRegions === undefined ? MAX_REGIONS : o.maxRegions;
    const threshold = o.fullRepaintThreshold === undefined
        ? FULL_REPAINT_THRESHOLD : o.fullRepaintThreshold;
    const maxArea = threshold * WIDTH * HEIGHT;

    if (!prev || prev.length !== 1024 || next.length !== 1024) return { kind: "full" };

    const runs = [];
    let runStart = -1;
    for (let y = 0; y < HEIGHT; y++) {
        const differs = rowXBounds(prev, next, y) !== null;
        if (differs && runStart === -1) runStart = y;
        if (!differs && runStart !== -1) { runs.push([runStart, y - 1]); runStart = -1; }
    }
    if (runStart !== -1) runs.push([runStart, HEIGHT - 1]);

    if (runs.length === 0) return { kind: "none" };
    if (runs.length > maxRegions) return { kind: "full" };

    const regions = [];
    for (const [yStart, yEnd] of runs) {
        const region = regionForRun(prev, next, yStart, yEnd);
        const area = region.kind === "scanline" ? WIDTH : region.w * region.h;
        if (area > maxArea) return { kind: "full" };
        regions.push(region);
    }
    return { kind: "regions", regions };
}
```

- [ ] **Step 2: Write `tests/host/test_e16_diff.sh`**

```bash
#!/usr/bin/env bash
# Row-run clustering, not a single bounding box -- see the header of
# e16_diff.mjs for why a global bbox degrades badly on two far-apart
# changes (the motivating case this file exists to test: opposite corners
# of the E16 map).
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { diffFramebuffers, MAX_REGIONS, FULL_REPAINT_THRESHOLD } from "./src/shared/e16_diff.mjs";
import { WIDTH, HEIGHT } from "./src/shared/e16_canvas.mjs";

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; }
  else console.log("ok   " + n); };

const blank = () => new Uint8Array(1024);
const setPx = (buf, x, y) => { buf[(y >> 3) * WIDTH + x] |= (1 << (y & 7)); };

eq("null prev -> full", diffFramebuffers(null, blank()), { kind: "full" });

const same = blank();
eq("identical -> none", diffFramebuffers(same, same.slice()), { kind: "none" });

/* One pixel at (10,10) -> a single small rect region. */
{
  const prev = blank(), next = blank();
  setPx(next, 10, 10);
  const d = diffFramebuffers(prev, next);
  eq("single pixel -> one region", d.kind, "regions");
  eq("single pixel region count", d.regions.length, 1);
  eq("single pixel rect", d.regions[0], { kind: "rect", x: 10, y: 10, w: 1, h: 1 });
}

/* A full-width single row -> scanline. */
{
  const prev = blank(), next = blank();
  for (let x = 0; x < WIDTH; x++) setPx(next, x, 5);
  const d = diffFramebuffers(prev, next);
  eq("full row -> scanline", d, { kind: "regions", regions: [{ kind: "scanline", y: 5 }] });
}

/* THE MOTIVATING CASE: top-left corner and bottom-right corner both change.
 * A single global bounding box would span nearly the whole screen; row-run
 * clustering must produce two SMALL regions instead. */
{
  const prev = blank(), next = blank();
  setPx(next, 0, 0);
  setPx(next, WIDTH - 1, HEIGHT - 1);
  const d = diffFramebuffers(prev, next);
  eq("opposite corners -> two regions", d.kind, "regions");
  eq("opposite corners region count", d.regions.length, 2);
  eq("opposite corners top region", d.regions[0], { kind: "rect", x: 0, y: 0, w: 1, h: 1 });
  eq("opposite corners bottom region", d.regions[1],
     { kind: "rect", x: WIDTH - 1, y: HEIGHT - 1, w: 1, h: 1 });
}

/* More than MAX_REGIONS scattered single-row changes -> full. */
{
  const prev = blank(), next = blank();
  for (let i = 0; i <= MAX_REGIONS; i++) setPx(next, 0, i * 2);   /* MAX_REGIONS+1 runs */
  eq("more than MAX_REGIONS runs -> full", diffFramebuffers(prev, next), { kind: "full" });
}

/* A region whose area exceeds FULL_REPAINT_THRESHOLD -> full, even though
 * it is a single contiguous run (not a region-count problem). */
{
  const prev = blank(), next = blank();
  const bigH = Math.ceil((FULL_REPAINT_THRESHOLD * WIDTH * HEIGHT) / WIDTH) + 2;
  for (let y = 0; y < bigH; y++) for (let x = 0; x < WIDTH; x++) setPx(next, x, y);
  eq("area over threshold -> full", diffFramebuffers(prev, next), { kind: "full" });
}

/* Overridable via opts. */
{
  const prev = blank(), next = blank();
  setPx(next, 0, 0);
  setPx(next, WIDTH - 1, HEIGHT - 1);
  eq("maxRegions override forces full",
     diffFramebuffers(prev, next, { maxRegions: 1 }), { kind: "full" });
}

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
```

```bash
chmod +x tests/host/test_e16_diff.sh
```

- [ ] **Step 3: Run the test**

Run: `bash tests/host/test_e16_diff.sh`
Expected: every line `ok   ...`, final line `PASS`, exit 0

- [ ] **Step 4: Commit**

```bash
git add src/shared/e16_diff.mjs tests/host/test_e16_diff.sh
git commit -m "e16: row-run diff engine for partial OLED updates"
```

---

### Task 4: Wire the diff engine into `createDisplay()`

**Goal:** `createDisplay()`'s `tick()` sends SCANLINE/RECTANGLE for a small diff, drains a multi-region diff one message per tick, and still falls back to a full FRAMEBUFFER exactly as before for a `"full"` diff, an unknown prior state, or a mode switch.

**Files:**
- Modify: `src/shared/e16_surface.mjs:348-470` (the `createDisplay` function and its import line)
- Test: `tests/host/test_e16_view.sh` (extend — it already builds `createDisplay` fixtures)

**Acceptance Criteria:**
- [ ] A small diff (single region) sends `"rect"` or `"scanline"` instead of `"framebuffer"`
- [ ] A two-region diff sends the first region on one `tick()` call and the second on the next, in row order
- [ ] A `"full"` diff (or `prev === null`, i.e. first paint / after `forgetShown()`) sends the whole framebuffer exactly as today
- [ ] Switching `want` from `"framebuffer"` to `"labels"` (or back) drops any in-flight region queue and forces a full repaint on the next framebuffer request — a queued region describes a screen the device is no longer being asked to show
- [ ] A refused send (the injected `send` returns `false`) leaves `lastSentBuf` and the pending region queue exactly as they were — mirrors the existing `fbOwed` refusal discipline
- [ ] `tick()`'s return value includes `"rect"`/`"scanline"` alongside the existing `"framebuffer"`/`"rings"`/`"labels"`
- [ ] All pre-existing `test_e16_*.sh` tests that touch `createDisplay` still pass unmodified (no behavior change for a `"full"`-diff caller, i.e. the pre-partial-updates test suite is a regression guard)

**Verify:** `for t in tests/host/test_e16_*.sh; do bash "$t" || echo "FAIL $t"; done` → no `FAIL` lines

**Steps:**

- [ ] **Step 1: Update the import line**

In `src/shared/e16_surface.mjs`, change line 348:

```javascript
import { framebufferMsg, labelsMsg, ringMsg } from "./e16_protocol.mjs";
```

to:

```javascript
import { framebufferMsg, labelsMsg, ringMsg, scanlineMsg, rectangleMsg } from "./e16_protocol.mjs";
import { diffFramebuffers } from "./e16_diff.mjs";
import { packRowMajor, WIDTH as E16_WIDTH } from "./e16_canvas.mjs";
```

- [ ] **Step 2: Add state and rewrite `tick()`**

In `createDisplay()`, immediately after the existing `let shownKind = null;` (and its comment block — leave that comment as-is), add:

```javascript
    /*
     * PARTIAL-UPDATE STATE. Built against a draft spec -- see
     * docs/superpowers/specs/2026-09-22-e16-partial-oled-updates-design.md.
     *
     * lastSentBuf: what we believe the device's pixel buffer currently
     * shows, or null if unknown (never shown a framebuffer-mode screen yet,
     * or the device's state was invalidated -- a replug via forgetShown(),
     * or a NACK). Only advances on a CONFIRMED emit, same discipline as
     * fbOwed/shownKind above: a refused send must not update what we
     * believe is on the device.
     *
     * pendingRegions / pendingBuf: a diff can describe up to MAX_REGIONS
     * small updates, and this file's own "at most one message per tick"
     * rule (see the PACED DISPLAY block above) means they can't all go out
     * at once. pendingBuf is the buffer the pending regions were computed
     * against -- later regions are packed from IT, not from a freshly
     * re-rendered buffer, so a mid-drain repaint elsewhere in the surface
     * can't invalidate the geometry of a region already queued.
     */
    let lastSentBuf = null;
    let pendingRegions = [];
    let pendingBuf = null;
```

Then replace the entire `tick(send, frameBytes, screen, nowMs) { ... }` method (from its opening line through its closing `}`, i.e. everything between the JSDoc comment above it and the `/* Test seams. */` comment that follows it) with:

```javascript
        tick(send, frameBytes, screen, nowMs) {
            const want = screen ? screen.kind : "framebuffer";
            const screenOwed = fbOwed || pendingRegions.length > 0 || shownKind !== want;
            if (!screenOwed) {
                if (!rings.size) {
                    if (!labelsOwed || want !== "labels") return null;
                    if (!emitMsg(send, labelsMsg(screen.title, screen.labels))) return null;
                    labelsOwed = false;
                    return "labels";
                }
                const chunks = Array.from(rings.values());
                if (!emitMsg(send, ringMsg(chunks))) return null;
                rings.clear();
                return "rings";
            }

            if (want !== "framebuffer") {
                /* Leaving framebuffer mode (or never entering it this tick)
                 * -- any queued partial-update state describes a screen the
                 * device is no longer being asked to show. */
                pendingRegions = []; pendingBuf = null; lastSentBuf = null;
                const bytes = labelsMsg(screen.title, screen.labels);
                if (emitMsg(send, bytes)) {
                    fbOwed = false; labelsOwed = false; shownKind = want;
                    if (nowMs !== undefined) shownAt = nowMs;
                    return want;
                }
                return null;
            }

            if (shownKind !== "framebuffer") {
                /* Switching INTO framebuffer mode -- the device's last known
                 * pixel state, if any, belongs to a mode we've left (or this
                 * is the very first paint). A diff against it would describe
                 * a screen that was never drawn. */
                pendingRegions = []; pendingBuf = null; lastSentBuf = null;
            }

            if (pendingRegions.length === 0) {
                const buf = frameBytes();
                const diff = diffFramebuffers(lastSentBuf, buf);
                if (diff.kind === "none") {
                    fbOwed = false;
                    shownKind = "framebuffer";
                    return null;
                }
                if (diff.kind === "full") {
                    if (!emitMsg(send, framebufferMsg(buf))) return null;
                    fbOwed = false; shownKind = "framebuffer"; lastSentBuf = buf.slice();
                    if (nowMs !== undefined) shownAt = nowMs;
                    return "framebuffer";
                }
                pendingBuf = buf.slice();
                pendingRegions = diff.regions.slice();
                fbOwed = false;
            }

            const region = pendingRegions[0];
            const bytes = region.kind === "scanline"
                ? scanlineMsg(region.y, packRowMajor(pendingBuf, 0, region.y, E16_WIDTH, 1))
                : rectangleMsg(region.x, region.y, region.w, region.h,
                                packRowMajor(pendingBuf, region.x, region.y, region.w, region.h));
            if (!emitMsg(send, bytes)) return null;
            pendingRegions.shift();
            shownKind = "framebuffer";
            lastSentBuf = pendingBuf.slice();
            if (nowMs !== undefined) shownAt = nowMs;
            if (pendingRegions.length === 0) pendingBuf = null;
            return region.kind;
        },
```

- [ ] **Step 3: Make `forgetShown()` also clear the new state**

Find the existing method (it's just above the `/* Test seams. */` block's `ringsPending` getter):

```javascript
        forgetShown() { shownKind = null; },
```

Change it to:

```javascript
        forgetShown() { shownKind = null; lastSentBuf = null; pendingRegions = []; pendingBuf = null; },
```

- [ ] **Step 4: Extend `tests/host/test_e16_view.sh`**

Add before its final `console.log(fails ? ...)` block (move that block to the end):

```javascript
/* --------------------------------------------------------------------------
 * PARTIAL OLED UPDATES. Built against a draft spec, not yet on hardware --
 * see docs/superpowers/specs/2026-09-22-e16-partial-oled-updates-design.md.
 * These are unit-level: a fresh createDisplay(), fake send/frameBytes, no
 * device.
 * ---------------------------------------------------------------------- */
{
  const WIDTH = 128, HEIGHT = 64;
  const setPx = (buf, x, y) => { buf[(y >> 3) * WIDTH + x] |= (1 << (y & 7)); };
  const mkSend = () => { const log = []; const fn = (p) => { if (fn.refuse) return false; log.push(p); return true; }; fn.log = log; fn.refuse = false; return fn; };
  const unpackMsgId = (packets) => {
    const out = [];
    for (let i = 0; i < packets.length; i += 4) {
      const cin = packets[i] & 0x0F; const n = cin === 0x05 ? 1 : cin === 0x06 ? 2 : 3;
      for (let b = 0; b < n; b++) out.push(packets[i + 1 + b]);
    }
    return out.slice(6, 7);   /* the single id byte, per OLED_SUBCOMMAND_HAS_CATEGORY_PREFIX=false */
  };

  const d1 = createDisplay();
  const send1 = mkSend();
  let buf1 = new Uint8Array(1024);
  const frameBytes1 = () => buf1;
  d1.invalidate();
  const first = d1.tick(send1, frameBytes1, { kind: "framebuffer" }, 0);
  eq("first paint (prev unknown) is a full framebuffer", first, "framebuffer");

  buf1 = new Uint8Array(1024);
  setPx(buf1, 10, 10);
  d1.invalidate();
  const second = d1.tick(send1, frameBytes1, { kind: "framebuffer" }, 100);
  eq("single-pixel diff sends rect, not framebuffer", second, "rect");
  eq("rect message id byte", unpackMsgId(send1.log[send1.log.length - 1]), [0x06]);

  /* Two-region diff drains across two ticks. */
  const d2 = createDisplay();
  const send2 = mkSend();
  let buf2 = new Uint8Array(1024);
  const frameBytes2 = () => buf2;
  d2.invalidate();
  d2.tick(send2, frameBytes2, { kind: "framebuffer" }, 0);   /* establish baseline */
  buf2 = new Uint8Array(1024);
  setPx(buf2, 0, 0);
  setPx(buf2, WIDTH - 1, HEIGHT - 1);
  d2.invalidate();
  const r1 = d2.tick(send2, frameBytes2, { kind: "framebuffer" }, 100);
  const r2 = d2.tick(send2, frameBytes2, { kind: "framebuffer" }, 101);
  eq("two-region diff: first tick sends one region", r1, "rect");
  eq("two-region diff: second tick sends the other", r2, "rect");
  const r3 = d2.tick(send2, frameBytes2, { kind: "framebuffer" }, 102);
  eq("two-region diff: third tick has nothing left", r3, null);

  /* A refused send changes nothing. */
  const d3 = createDisplay();
  const send3 = mkSend();
  let buf3 = new Uint8Array(1024);
  const frameBytes3 = () => buf3;
  d3.invalidate();
  d3.tick(send3, frameBytes3, { kind: "framebuffer" }, 0);
  buf3 = new Uint8Array(1024);
  setPx(buf3, 5, 5);
  d3.invalidate();
  send3.refuse = true;
  const refused = d3.tick(send3, frameBytes3, { kind: "framebuffer" }, 100);
  eq("refused send returns null", refused, null);
  send3.refuse = false;
  const retried = d3.tick(send3, frameBytes3, { kind: "framebuffer" }, 101);
  eq("retry after refusal still sends the same diff", retried, "rect");

  /* Switching to labels drops any queued regions and forces a full repaint
   * on the way back to framebuffer mode. */
  const d4 = createDisplay();
  const send4 = mkSend();
  let buf4 = new Uint8Array(1024);
  const frameBytes4 = () => buf4;
  d4.invalidate();
  d4.tick(send4, frameBytes4, { kind: "framebuffer" }, 0);
  buf4 = new Uint8Array(1024);
  setPx(buf4, 0, 0);
  setPx(buf4, WIDTH - 1, HEIGHT - 1);
  d4.invalidate();
  d4.tick(send4, frameBytes4, { kind: "framebuffer" }, 100);   /* first of two regions queued */
  const toLabels = d4.tick(send4, frameBytes4, { kind: "labels", title: "T", labels: [] }, 101);
  eq("switch to labels sends labels", toLabels, "labels");
  d4.invalidate();
  const backToFb = d4.tick(send4, frameBytes4, { kind: "framebuffer" }, 102);
  eq("switch back to framebuffer is a full repaint, not a stale region", backToFb, "framebuffer");
}
```

- [ ] **Step 5: Run all E16 tests**

Run: `for t in tests/host/test_e16_*.sh; do echo "=== $t ==="; bash "$t"; done`
Expected: every file ends with `PASS`, no `FAIL` lines anywhere in the output

- [ ] **Step 6: Commit**

```bash
git add src/shared/e16_surface.mjs tests/host/test_e16_view.sh
git commit -m "e16: createDisplay sends partial OLED updates when the diff is small"
```

---

### Task 5: ACK/NACK feeds the self-heal path, plus docs

**Goal:** An OLED UPDATE NACK invalidates `lastSentBuf` and drops any queued regions, so the next tick's diff falls back to a full repaint — the same recovery a replug already gets — with no new timer or retry machinery. `docs/E16_REMOTE.md` documents the new opcodes.

**Files:**
- Modify: `src/shared/e16_surface.mjs` (the `createSysexAssembler` dispatch inside `createSurface`, around line 1164, plus a new getter on `createDisplay`)
- Modify: `docs/E16_REMOTE.md`
- Test: `tests/host/test_e16_wiring.sh` (extend — it's the end-to-end test)

**Acceptance Criteria:**
- [ ] A `parseOledUpdateReply` NACK arriving via `onMessage` calls a new `display.invalidateBuf()` that nulls `lastSentBuf` and clears `pendingRegions`/`pendingBuf`
- [ ] An ACK is a no-op (no state change, no message sent)
- [ ] A rate-limited log line fires on NACK (reuse the codebase's existing rate-limit helper if present; otherwise a simple "once per N ms" gate is acceptable — see step 1)
- [ ] Neither ACK nor NACK is mistaken for the unrelated `REMOTE MODE ENTERED ACK` (already covered by Task 1's parser test, but re-asserted here at the wiring level)
- [ ] `docs/E16_REMOTE.md` lists the 5 new opcodes, the header-byte ambiguity and the decision, and the row-run diff strategy at a summary level
- [ ] `tests/host/test_e16_wiring.sh` passes

**Verify:** `bash tests/host/test_e16_wiring.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Add `invalidateBuf()` to `createDisplay()`'s returned object**

In `src/shared/e16_surface.mjs`, in the object `createDisplay()` returns, find:

```javascript
        /* A replug wipes the panel, so what the device was told is no longer
         * true. Forgetting it is what makes the presence edge resend. */
        forgetShown() { shownKind = null; lastSentBuf = null; pendingRegions = []; pendingBuf = null; },
```

(from Task 4, Step 3) and add immediately after it:

```javascript
        /* An OLED UPDATE NACK, or any other signal that what we believe is
         * on the device might be wrong. Narrower than forgetShown(): the
         * device is still in framebuffer mode as far as we know (shownKind
         * is untouched), only the PIXELS we believe it holds are suspect --
         * the next diff sees prev === null and sends a full repaint, same
         * recovery forgetShown() already gives a replug. */
        invalidateBuf() { lastSentBuf = null; pendingRegions = []; pendingBuf = null; },
```

- [ ] **Step 2: Find and check the log rate-limit convention**

Run: `grep -n "rateLimit\|rate_limit\|lastLogAt\|LOG_RATE" src/shared/*.mjs | head -20`

If an existing helper turns up, use it in Step 3. Otherwise use the inline pattern below (a module-level timestamp, gate on `now - last >= RATE_MS`), which matches how `SCREEN_HEARTBEAT_MS`-style gating is already done elsewhere in this file.

- [ ] **Step 3: Wire the parser into the SysEx dispatch**

In `src/shared/e16_surface.mjs`, near the top of the file, add to the existing import from `./e16_protocol.mjs` used by `createLifecycle` (find `import { enterMsg, exitMsg, isAck, packetize } from "./e16_protocol.mjs";` near the top of the file) — change it to:

```javascript
import { enterMsg, exitMsg, isAck, packetize, parseOledUpdateReply } from "./e16_protocol.mjs";
```

Then in `createSurface`, find:

```javascript
    const asm = createSysexAssembler({
        onMessage: (body) => { lifecycle.onSysex(body, now()); },
    });
```

Replace with:

```javascript
    /* Rate-limited so a run of NACKs (unlikely, but the whole point of
     * having them is to react to the unlikely) can't flood the log the way
     * an unthrottled per-message line would. */
    const NACK_LOG_RATE_MS = 1000;
    let lastNackLogAt = -Infinity;

    const asm = createSysexAssembler({
        onMessage: (body) => {
            if (lifecycle.onSysex(body, now())) return;
            const reply = parseOledUpdateReply(body);
            if (!reply) return;
            if (reply.ok) return;   /* ACK: we already advanced optimistically on send */
            display.invalidateBuf();
            const t = now();
            if (t - lastNackLogAt >= NACK_LOG_RATE_MS) {
                lastNackLogAt = t;
                console.log("e16: OLED update NACK, cmd=0x" + reply.cmd.toString(16) +
                            " status=0x" + reply.status.toString(16));
            }
        },
    });
```

(`isAck` is checked first via `lifecycle.onSysex`'s own internal `isAck` call and its `true`/`false` return — `lifecycle.onSysex` already returns `false` for anything that isn't the REMOTE MODE ENTERED ACK, so falling through to `parseOledUpdateReply` on that `false` is correct and doesn't double-handle a real ACK.)

- [ ] **Step 4: Extend `tests/host/test_e16_wiring.sh`**

Find the fixture setup near the top of the test (the `mkSend`/`unpack` helpers and `ENTER` constant are already defined there — reuse them). Add this block after the existing lifecycle/ACK-related assertions (search the file for where it already asserts on `ENTER`/ack handling, and add immediately after that section, before whatever comes next):

```javascript
/* --------------------------------------------------------------------------
 * OLED UPDATE NACK -- built against a draft spec, not yet on hardware. A
 * NACK must invalidate the surface's belief about what's on screen (so the
 * NEXT repaint is a full one) without touching anything else -- not the
 * lifecycle's presence tracking, not a resend, no new timer.
 * ---------------------------------------------------------------------- */
{
  const surf = createSurface({
    now: () => 0, send: mkSend(), chainOf: () => CHAIN,
    makeController: () => createController({
      getParam: () => "0", setParam: () => true, params: mkParams(),
    }),
  });
  surf.setEnabled(true);
  // Not asserting on the NACK path in detail here -- Tasks 1 and 4 already
  // unit-test parseOledUpdateReply and display.invalidateBuf/tick in
  // isolation. This just confirms the wiring does not throw when a NACK
  // body is fed through the real onMidiMessageExternal-shaped path, since
  // that seam (asm.feed -> onMessage -> parseOledUpdateReply ->
  // display.invalidateBuf) is exactly what the earlier "tasks pass their
  // own tests, the SEAM between files is what breaks" lesson (this file'"'"'s
  // own header comment) is about.
  const { pack7 } = await import(R + "/src/shared/e16_protocol.mjs");
  const nackRaw = [0x06, 0x02, 0xFF, 0xFF, 0xFF, 0xFF];   /* invalid bounds */
  const nackBody = [0x00,0x21,0x5B,0x02,0x01,0x54].concat(pack7(nackRaw));
  const nackBytes = [0xF0].concat(nackBody, [0xF7]);
  let threw = false;
  try {
    for (const b of nackBytes) surf.feedExternal([b]);
  } catch (e) { threw = true; console.log("  threw: " + e.message); }
  ok(!threw, "feeding an OLED NACK through the surface does not throw");
}
```

Before relying on `surf.feedExternal`, check it actually exists and matches this shape:

Run: `grep -n "feedExternal\|onMidiMessageExternal\|asm.feed" src/shared/e16_surface.mjs`

If the real entry point has a different name or signature, use that name instead in the snippet above — the surrounding tests in this same file already drive the surface with raw MIDI bytes (see how `ENTER`'s bytes are fed earlier in the file) and that call is the pattern to copy exactly, including how single-byte vs. packetized input is expected.

- [ ] **Step 5: Update `docs/E16_REMOTE.md`**

Add a new section after the existing `## Which mode to use, and what each costs` section (at the end of the file):

```markdown
## Partial updates (draft spec, not yet shipped)

OXI shared a draft spec (2026-09-21) adding four more OLED opcodes and a
reply pair, not yet in shipped firmware:

| id | message | payload |
|---|---|---|
| `0x05` | OLED SCANLINE | Y (1 byte) + 16 bytes of one row, MSB first |
| `0x06` | OLED RECTANGLE | x, y, w, h (1 byte each) + `ceil(w/8)*h` bytes, row-major, MSB first |
| `0x07` | OLED CLEAR | none |
| `0x53` | OLED UPDATE ACK (device to host) | original cmd, status 0x00, 4-byte address |
| `0x54` | OLED UPDATE NACK (device to host) | original cmd, error status, 4-byte address |

**Header ambiguity, unresolved without hardware.** Every other message's id
is two bytes (`0x06 0xXX`) and its example bytes agree. These five list the
same two-byte id in the ID column, but their EXAMPLE bytes in the sheet drop
the `0x06`. We build against the literal examples (single-byte id, no
`0x06`), gated behind `OLED_SUBCOMMAND_HAS_CATEGORY_PREFIX` in
`e16_protocol.mjs` so it's a one-line fix if wrong. Neither NACK error code
0x03 (CRC mismatch) nor an actual CRC is used — the algorithm is
undocumented.

**RECTANGLE/SCANLINE are row-major; our framebuffer is page/column.**
`e16_canvas.mjs`'s `packRowMajor` is the one place that transpose happens.

**The diff clusters by row RUN, not one bounding box** — two far-apart
changes (e.g. the map cursor jumping corner to corner) would otherwise
produce one screen-spanning rectangle. See `e16_diff.mjs` and
`docs/superpowers/specs/2026-09-22-e16-partial-oled-updates-design.md` for
the full reasoning, including why row-runs and not full 2D clustering.

**None of this closes the parked garbling bug** ([[e16_garble_is_move_note_interleave]]
in project memory) — it shrinks the exposed window from ~130 SPI frames
(a 394-packet FRAMEBUFFER) to typically 1-5, which should reduce collision
odds, but the underlying single-cable sharing with Move's own note data is
unchanged and unmeasurable until real firmware exists to test against.
```

- [ ] **Step 6: Run the full E16 suite**

Run: `for t in tests/host/test_e16_*.sh; do echo "=== $t ==="; bash "$t"; done 2>&1 | tee /tmp/e16_test_output.txt; grep -c FAIL /tmp/e16_test_output.txt`
Expected: the `grep -c FAIL` line prints `0`

- [ ] **Step 7: Commit**

```bash
git add src/shared/e16_surface.mjs tests/host/test_e16_wiring.sh docs/E16_REMOTE.md
git commit -m "e16: OLED NACK invalidates the diff baseline; document the partial-update opcodes"
```

---

## Self-review notes

- **Spec coverage:** all 5 new opcodes (Task 1), the pixel transpose (Task 2), row-run diffing with both caps (Task 3), the `createDisplay` integration including multi-region draining and mode-switch invalidation (Task 4), and ACK/NACK feeding the existing self-heal path plus docs (Task 5) — every component from the design doc has a task.
- **No CRC, no new retry/timeout machinery, no feature flag** — all per the recorded user decisions; nothing in this plan contradicts them.
- **Task 5's wiring test is intentionally light** (a does-not-throw check) rather than asserting on `lastSentBuf` internals through the full `createSurface` stack, because `lastSentBuf`/`pendingRegions` aren't exposed as test seams on `createSurface` (only on `createDisplay` directly, which Task 4 already tests thoroughly). Adding a seam just to assert through the outer object would be scope creep against a component that's about to be exercised against real hardware behavior nobody has seen yet — consistent with "build it how you expect it to work, adjust when we test."
