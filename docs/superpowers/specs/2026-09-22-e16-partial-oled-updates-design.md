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

A single bounding box over the whole diff is the wrong shape: two
far-apart changes (e.g. the map cursor moving from a top-left cell to a
bottom-right one — a plausible, ordinary navigation) produce a box
spanning nearly the whole screen even though the actual changed pixels are
a couple of percent of it. Naively that either sends one needlessly huge
RECTANGLE or trips a whole-screen-area threshold and falls back to
FRAMEBUFFER — defeating the point in exactly the case (fast navigation)
partial updates are for.

So `diffFramebuffers` clusters by **row runs**, not one global box:

1. Compute a 64-entry boolean array — does this row differ at all.
2. Group contiguous `true` rows into runs.
3. For each run, take the tight *x*-bounds from only the pixels that
   differ within that run's rows (not the whole screen) — this is what
   keeps a run over the top-left cell from being dragged wide by a change
   in the bottom-right cell; the two land in different runs entirely.
4. Any run whose rect area exceeds `MAX_REGION_AREA` (see constants below)
   invalidates the *whole* diff to `"full"` — a single run that large means
   most of that row-band changed, and RECTANGLE's per-message overhead
   stops paying for itself.
5. More runs than `MAX_REGIONS` also invalidates to `"full"` — bounds the
   worst case on *message count*, independent of how small each region is
   (a screen with many scattered single-pixel changes is a redraw, not a
   diff).
6. A run that is exactly one row tall and spans (close to) the full width
   becomes a `"scanline"` region instead of `"rect"` — cheaper on the wire
   (18 raw bytes vs. RECTANGLE's coordinate + dimension header).

`diffFramebuffers(prev, next, opts)` — `prev` is `null` (unknown device
state → always `"full"`) or a 1024-byte buffer; `next` is always a
1024-byte buffer; `opts` optionally overrides the two constants below.
Returns:
- `{ kind: "none" }` — byte-identical
- `{ kind: "full" }` — `prev === null`, or region/area caps exceeded per
  above
- `{ kind: "regions", regions: [...] }` — 1 to `MAX_REGIONS` entries, each
  `{ kind: "scanline", y }` or `{ kind: "rect", x, y, w, h }`, in row order

Two adjustable constants, exported so a caller (or a test, or a future
tuning pass once real hardware timing exists) can override them:
- `MAX_REGIONS` (default 4) — caps message count per diff
- `FULL_REPAINT_THRESHOLD` (default 0.4, as a fraction of 128×64) — caps
  area per region; also used as the whole-diff fallback when `prev` is
  unusable

No knowledge of MIDI, SysEx, or the display state machine — takes two
buffers (+ options), returns a decision. Tested with fixture buffers:
identical; one-pixel change; full-width one-row change; two changes in
different row-bands (must produce 2 *small* regions, not 1 screen-sized
box — this is the case that motivated the row-run design, so it gets a
named test); a diff wide enough in one row-band to become a `"scanline"`;
more than `MAX_REGIONS` scattered changes (→ `"full"`); a single region
over `FULL_REPAINT_THRESHOLD` (→ `"full"`); `prev === null` (→ `"full"`).

**`src/shared/e16_surface.mjs` (`createDisplay`)** (modified)
- New field `lastSentBuf` (`Uint8Array | null`), analogous to `shownKind`:
  only advances on a *confirmed* emit, exactly like `fbOwed`/`shownKind`
  today. `forgetShown()` (already called on replug) also nulls it — after a
  power cycle we don't know what's on the device, so the next repaint must
  be a full one.
- New field `pendingRegions` (array, possibly empty) — a diff can produce
  up to `MAX_REGIONS` messages, and the file's existing "at most one
  message per tick" rule (rule 1 of the PACED DISPLAY block) means they
  can't all go out at once. Same shape as the existing `rings` coalescing:
  a queue drained one entry per tick, never grown past what one un-acted
  diff produced (a second diff before the queue empties *replaces* it
  rather than appending — the newer picture is the one worth sending, and
  an unbounded queue is how a busy screen turns into an ever-growing send
  backlog).
- Where `tick()` currently does
  `fbOwed ? framebufferMsg(frameBytes()) : ...`:
  - If `pendingRegions` is non-empty, send `scanlineMsg`/`rectangleMsg`
    for `pendingRegions.shift()`'s region and stop — this drains ahead of
    computing a fresh diff, so a multi-region diff finishes before
    anything newer preempts it.
  - Otherwise, when a repaint is owed: render once via `frameBytes()`,
    call `diffFramebuffers(lastSentBuf, buf)`. `kind: "none"` sends
    nothing. `kind: "full"` sends `framebufferMsg(buf)` as today.
    `kind: "regions"` sends the *first* region now and sets
    `pendingRegions` to the rest.
- On any confirmed emit that is part of a diff (full or regional),
  `lastSentBuf = buf.slice()` (a copy — `frameBytes()` may return a live,
  mutable canvas buffer per `e16_canvas.mjs`'s own contract: "Returns the
  LIVE buffer... callers that want a snapshot should slice it
  themselves"). Advancing on the *first* region of a multi-region diff
  (rather than waiting for the queue to drain) is deliberate and matches
  the existing "believed state" discipline: each region is independently a
  correct partial description of `buf`, so a NACK or a fresh diff request
  arriving mid-drain reasons about a partially-updated `lastSentBuf`
  exactly the way it already reasons about a partially-applied optimistic
  send elsewhere in this file.
- `tick()`'s return value gains two new kinds (`"scanline"`, `"rect"`)
  alongside the existing `"framebuffer"`/`"rings"`/`"labels"` for tests and
  logging.

**`createSysexAssembler` / lifecycle `onSysex`** (modified)
- New branch: try `parseOledUpdateReply(asm)` after the existing `isAck`
  check fails (it's a different body shape, so order doesn't matter for
  correctness, but ACK is checked first since it's the hot path).
- ACK: no-op. We already advanced `lastSentBuf` optimistically on send.
- NACK: null `lastSentBuf` **and** clear `pendingRegions` entirely (not
  just the named region — NACKs should be rare, and partial invalidation
  buys little for the complexity). This makes the *next* tick's diff see
  `prev === null` → `kind: "full"`, same recovery the replug case already
  uses. A rate-limited log line records the NACK's status code, matching
  the house style for `param-slow`.
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
- `diffFramebuffers` against fixture buffer pairs (see the named cases
  above, including the two-row-bands-far-apart case)
- `createDisplay` extended: a small diff sends `rect`/`scanline` instead of
  `framebuffer`; a two-region diff drains across two ticks, first region
  first; a >`MAX_REGIONS` or >`FULL_REPAINT_THRESHOLD` diff still sends
  `framebuffer`; a NACK forces the next tick to `framebuffer` again *and*
  drops any queued regions; a refused send leaves `lastSentBuf` and
  `pendingRegions` unchanged (mirrors the existing `fbOwed` refusal tests).
