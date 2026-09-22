# E16 partial OLED updates (SCANLINE / RECTANGLE / CLEAR / ACK / NACK)

Status: **built against a draft spec, not yet verified on hardware.** OXI's
firmware does not carry these opcodes yet (confirmed with the user
2026-09-22). Nothing here changes behavior until real firmware answers;
until then the new send path only fires against a device that never NACKs
or ACKs, which the surface already tolerates (a send with no reply just sits
in `lastSentBuf` as "believed shown").

## Problem

The parked E16 garbling bug ([[e16_garble_is_move_note_interleave]]) is a
duration problem: our SysEx shares one USB-MIDI cable with Move's own note
data (forced single-jack — [[e16_usba_xmos_multijack]]), and a message is
corrupted if it's still on the wire when Move emits a note. A 394-packet
FRAMEBUFFER (383 ms) is exposed for ~130 SPI frames; a single-frame message
(~3 ms) measured 78/78 clean. Nothing on our side could shrink FRAMEBUFFER,
because the spec had no partial update.

OXI's draft spec (shared 2026-09-21, not yet shipped) adds `OLED SCANLINE`
(one row), `OLED RECTANGLE` (an arbitrary region), `OLED CLEAR`, and
`OLED UPDATE ACK`/`NACK` with error codes including CRC mismatch. These
don't eliminate the sharing, but they cut the exposed window from ~130
frames to typically 1-5, and give us delivery feedback we've never had.

## Spec ambiguity — resolved by assumption, not evidence

The sheet's ID column lists all opcodes as two bytes (`0x06 0xXX`), and the
existing 8 messages' example bytes match that (`... 02 01 06 55 F7`). The 5
new messages' example bytes **drop the `0x06`** (`... 02 01 05 [payload] F7`
for SCANLINE). A loose note at the top of the sheet — "0x06 is the category
message, no longer necessary once in REMOTE MODE" — could explain this, but
doesn't clearly scope to only the new messages.

**Decision: follow the literal example bytes** (single-byte id, no `0x06`,
for SCANLINE/RECTANGLE/CLEAR/ACK/NACK only). This is the most concrete
evidence available. It is gated behind one exported constant
(`OLED_SUBCOMMAND_HAS_CATEGORY_PREFIX = false`) in `e16_protocol.mjs` so
flipping it after the first hardware smoke test is a one-line change, not a
rewrite.

No CRC field is sent. It's marked optional in the spec and its algorithm is
undocumented; there's no way to compute it correctly without either a spec
addendum or a hardware rig to reverse-engineer against.

## Components

**`src/shared/e16_protocol.mjs`** (extended)
- `scanlineMsg(y, rowBits)` — `rowBits`: 16 bytes, left-to-right, MSB first.
- `rectangleMsg(x, y, w, h, bits)` — `bits`: `ceil(w/8) * h` bytes, row-major,
  MSB first, rows byte-aligned.
- `clearMsg()`
- `parseOledUpdateReply(asm)` — returns `{ ok: true|false, cmd, status,
  addr }` for an ACK or NACK body, or `null` if `asm` isn't one. `addr` is
  decoded per the three address shapes (scanline: `{y}`; rectangle:
  `{x,y,w,h}`; clear: `{}`), `0xFF` fields treated as absent.
- Each builder gets a host test pinning it against the sheet's own example
  row, matching the existing `ringMsg`/`labelsMsg` tests.

**`src/shared/e16_canvas.mjs`** (extended)
- `readPixel(buf, x, y)` — read-only mirror of the existing `setPixel` bit
  math (page/column: `byteIdx = (y>>3)*128+x`, `bit = y&7`).
- `packRowMajor(buf, x, y, w, h)` — walks the region via `readPixel`, emits
  `ceil(w/8)*h` bytes MSB-first, one row per `ceil(w/8)` bytes. This is the
  transpose from the framebuffer's native page/column layout to the
  RECTANGLE/SCANLINE wire layout; it's the only place that conversion
  happens.

**`src/shared/e16_diff.mjs`** (new, pure)
- `diffFramebuffers(prev, next)` — `prev` is `null` or a 1024-byte buffer,
  `next` is always a 1024-byte buffer. Returns:
  - `{ kind: "none" }` — byte-identical
  - `{ kind: "full" }` — `prev === null`, or the bounding box of differing
    pixels covers more than `FULL_REPAINT_THRESHOLD` (0.4) of the 128x64
    area — past that point RECTANGLE's header + packing overhead isn't
    worth it over just sending FRAMEBUFFER
  - `{ kind: "scanline", y }` — bounding box is exactly one row tall
  - `{ kind: "rect", x, y, w, h }` — otherwise, tightest bounding box
- No knowledge of MIDI, SysEx, or the display state machine — takes two
  buffers, returns a decision. Tested with fixture buffers: identical,
  one-pixel change, full-width one-row change, sparse-but-wide change (two
  corners lit → should still pick `full` once the bounding box swallows the
  whole screen), >40%-area change.

**`src/shared/e16_surface.mjs` (`createDisplay`)** (modified)
- New field `lastSentBuf` (`Uint8Array | null`), analogous to `shownKind`:
  only advances on a *confirmed* emit, exactly like `fbOwed`/`shownKind`
  today. `forgetShown()` (already called on replug) also nulls it — after a
  power cycle we don't know what's on the device, so the next repaint must
  be a full one.
- Where `tick()` currently does
  `fbOwed ? framebufferMsg(frameBytes()) : ...`, it now: renders once via
  `frameBytes()`, calls `diffFramebuffers(lastSentBuf, buf)`, and dispatches
  on `.kind` to `clearMsg`/`scanlineMsg`/`rectangleMsg`/`framebufferMsg`
  (an all-zero `next` is `kind: "full"` with an all-zero box today, which
  is fine — CLEAR is only reached if we add a special case for it later;
  not doing that now, YAGNI).
- On a confirmed emit, `lastSentBuf = buf.slice()` (a copy — `frameBytes()`
  may return a live, mutable canvas buffer per `e16_canvas.mjs`'s own
  contract: "Returns the LIVE buffer... callers that want a snapshot should
  slice it themselves").
- `tick()`'s return value gains two new kinds (`"scanline"`, `"rect"`)
  alongside the existing `"framebuffer"`/`"rings"`/`"labels"` for tests and
  logging.

**`createSysexAssembler` / lifecycle `onSysex`** (modified)
- New branch: try `parseOledUpdateReply(asm)` after the existing `isAck`
  check fails (it's a different body shape, so order doesn't matter for
  correctness, but ACK is checked first since it's the hot path).
- ACK: no-op. We already advanced `lastSentBuf` optimistically on send.
- NACK: null `lastSentBuf` entirely (not just the named region — NACKs
  should be rare, and partial invalidation buys little for the complexity).
  This makes the *next* tick's diff see `prev === null` → `kind: "full"`,
  same recovery the replug case already uses. A rate-limited log line
  records the NACK's status code, matching the house style for
  `param-slow`.
- No new timer, no retry state, no in-flight tracking — this is the same
  "restate on next tick" discipline the file already uses for the
  heartbeat, just event-triggered as well as time-triggered now.

## Out of scope

- CRC — no algorithm known.
- LABELS path — already cheap (34 packets), untouched.
- Actually validating any of this reduces garbling — impossible without
  hardware running the new firmware. This lands ready to flip on, not
  proven to help.

## Testing

All new code is pure and injectable, consistent with the rest of the E16
surface files. Host tests (`tests/host/test_e16_*.sh` pattern):
- protocol builders pinned against the sheet's example bytes
- `packRowMajor` against hand-worked fixtures (a few set pixels → known
  byte sequence)
- `diffFramebuffers` against fixture buffer pairs (see above)
- `createDisplay` extended: a small diff sends `rect`/`scanline` instead of
  `framebuffer`; a >40% diff still sends `framebuffer`; a NACK forces the
  next tick to `framebuffer` again; a refused send leaves `lastSentBuf`
  unchanged (mirrors the existing `fbOwed` refusal tests).
