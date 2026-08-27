#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE PEAK ENVELOPE IS STREAMED, RESUMABLE AND BOUNDED.
#
# Three constraints pull against each other and all three are load-bearing:
#
#   ACCURACY  a peak envelope that samples a handful of frames per column misses
#             transients, and a granular sample is mostly transients -- so every
#             frame in a block contributes its max.
#   MEMORY    the file is never held. Blocks are read into one reusable buffer
#             and folded into the per-column running max immediately, so the
#             cost is O(width) whether the sample is 2 seconds or 2 minutes.
#   TIME      the shadow UI tick IS its MIDI sampling interval, so a multi-
#             megabyte read inside one tick would be felt as input lag. The job
#             does BLOCKS_PER_TICK blocks and returns.
#
# Drop any one of them and you still get a plausible-looking waveform, which is
# why each is pinned separately.
#
# THE I/O IS INJECTED. std/os are QuickJS MODULES, so a static import of them
# would break every host test that transitively pulls in viz_draw. The device
# wires the real ones; this injects a node-backed pair.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the wav peaks tests" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# 1 s of 44.1k mono 16-bit: silent first half, loud sine second half.
python3 - "$TMP/tone.wav" <<'PY'
import struct, math, sys
rate, n = 44100, 44100
f = bytearray()
for i in range(n):
    v = 0 if i < n // 2 else int(30000 * math.sin(i * 0.05))
    f += struct.pack("<h", v)
hdr  = b"RIFF" + struct.pack("<I", 36 + len(f)) + b"WAVE"
hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
hdr += b"data" + struct.pack("<I", len(f))
open(sys.argv[1], "wb").write(hdr + bytes(f))
PY

# A 24-bit file, deliberately LARGER than one 32768-byte block: 32768 is not a
# multiple of a 3-byte frame, so an unaligned block starts mid-sample and every
# value after the FIRST block decodes as noise. A file that fits inside one
# block cannot show that bug -- and the first version of this test used one.
python3 - "$TMP/tone24.wav" <<'PY'
import struct, math, sys
rate, n = 44100, 40000
f = bytearray()
for i in range(n):
    v = int(4000000 * math.sin(i * 0.05))
    f += struct.pack("<i", v)[0:3]
hdr  = b"RIFF" + struct.pack("<I", 36 + len(f)) + b"WAVE"
hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 3, 3, 24)
hdr += b"data" + struct.pack("<I", len(f))
open(sys.argv[1], "wb").write(hdr + bytes(f))
PY

# A file past the MAX_BLOCKS ceiling: 64 blocks x 32768 = 2 MB, so 1.4M frames
# of 16-bit mono (2.8 MB) forces the reader to STRIDE.
python3 - "$TMP/big.wav" <<'BIGPY'
import struct, math, sys
rate, n = 44100, 1400000
f = bytearray()
for i in range(n):
    f += struct.pack("<h", int(20000 * math.sin(i * 0.01)))
hdr  = b"RIFF" + struct.pack("<I", 36 + len(f)) + b"WAVE"
hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
hdr += b"data" + struct.pack("<I", len(f))
open(sys.argv[1], "wb").write(hdr + bytes(f))
BIGPY

TONE="$TMP/tone.wav" TONE24="$TMP/tone24.wav" BIG="$TMP/big.wav" GARBAGE="$TMP/notaudio.bin" \
node --input-type=module -e '
import { openSync, readSync, closeSync, statSync, writeFileSync } from "node:fs";
import { wavPeaksTick, wavPeaks, resamplePeaks, resetWavPeaks,
         setWavPeaksIO, PEAK_WIDTH, MAX_BLOCKS, BLOCKS_PER_TICK }
  from "./src/shared/param_pages/wav_peaks.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* The node-backed IO, shaped exactly like the QuickJS std/os pair. `reads`
   counts round trips so the resumability claim is measured, not assumed. */
let reads = 0, maxBufBytes = 0;
setWavPeaksIO({
  open: (path) => {
    let fd;
    try { fd = openSync(path, "r"); } catch (e) { return null; }
    let cursor = 0;
    return {
      read: (buf, pos, len) => {
        reads++;
        if (buf.byteLength > maxBufBytes) maxBufBytes = buf.byteLength;
        const n = readSync(fd, new Uint8Array(buf, pos, len), 0, len, cursor);
        cursor += n; return n;
      },
      seek: (off, whence) => { if (whence === 0) cursor = off; return 0; },
      close: () => closeSync(fd),
    };
  },
  stat: (path) => {
    try { const s = statSync(path); return { size: s.size, mtime: Math.floor(s.mtimeMs) }; }
    catch (e) { return null; }
  },
});

const TONE = process.env.TONE, TONE24 = process.env.TONE24;
const BIG = process.env.BIG, GARBAGE = process.env.GARBAGE;
writeFileSync(GARBAGE, Buffer.alloc(4096, 7));

const runToDone = (path, limit = 2000) => {
  let ticks = 0;
  while (ticks < limit) {
    wavPeaksTick(path);
    const c = wavPeaks(path);
    ticks++;
    if (c && c.done) break;
  }
  return ticks;
};

/* ===================================================================== 1 ==
 * IT DECODES, and the shape is the shape of the file.
 */
{
  resetWavPeaks(); reads = 0;
  const ticks = runToDone(TONE);
  const c = wavPeaks(TONE);
  ok(!!c && c.error === "", "a RIFF/WAVE file decodes (error: " + (c && c.error) + ")");
  ok(c && c.points.length === PEAK_WIDTH, "one point per column at PEAK_WIDTH");
  ok(c && c.peak > 0.8 && c.peak <= 1, "the running peak found the loud half, got " + (c && c.peak.toFixed(3)));

  const first = c.points.slice(0, 60).reduce((a, b) => a + b, 0);
  const last  = c.points.slice(68).reduce((a, b) => a + b, 0);
  ok(first < 0.5, "the silent first half reads near zero, got " + first.toFixed(3));
  ok(last > 20, "the loud second half reads high, got " + last.toFixed(1));

  /* ------------------------------------------------------------- TIME */
  ok(ticks > 1, "the job took more than one tick -- it is resumable, not one big read");
  ok(reads > 1, "and it really did several reads, got " + reads);
}

/* ===================================================================== 2 ==
 * MEMORY. The file is never held: the read buffer is a fixed block, not the
 * file size. A 1 s 16-bit mono file is ~88 KB.
 */
{
  ok(maxBufBytes <= 65536,
     "the largest read buffer is a fixed block, not the file (got " + maxBufBytes + " bytes)");
}

/* ===================================================================== 3 ==
 * THE CEILING. A huge file must not run for hundreds of ticks -- past
 * MAX_BLOCKS the reader strides, trading exactness for a fixed cap.
 */
{
  ok(MAX_BLOCKS > 0 && MAX_BLOCKS <= 256, "MAX_BLOCKS bounds total work, got " + MAX_BLOCKS);
  ok(BLOCKS_PER_TICK >= 1 && BLOCKS_PER_TICK <= 8,
     "BLOCKS_PER_TICK keeps one tick small, got " + BLOCKS_PER_TICK);
  resetWavPeaks(); reads = 0;
  runToDone(TONE);
  ok(reads <= MAX_BLOCKS + 2,
     "a small file costs at most MAX_BLOCKS reads plus the header, got " + reads);

  /* THE CEILING ITSELF. A 2.8 MB file is 86 blocks; unstrided that is 86 reads
     and 43 ticks. Fixtures that all fit inside a handful of blocks never set
     the stride above 1 and prove nothing about the ceiling. */
  resetWavPeaks(); reads = 0;
  const bigTicks = runToDone(BIG);
  const bc = wavPeaks(BIG);
  ok(!!bc && bc.error === "", "the big file decodes (error: " + (bc && bc.error) + ")");
  ok(reads <= MAX_BLOCKS + 2,
     "a 2.8 MB file is STRIDED to the same ceiling, got " + reads + " reads");
  ok(bigTicks <= MAX_BLOCKS, "and it finishes within MAX_BLOCKS ticks, got " + bigTicks);
}

/* ===================================================================== 4 ==
 * 24-BIT. Sample libraries ship it as a matter of course, and a sampler that
 * cannot draw its own library is not much of a feature.
 *
 * The trap is block alignment: 32768 is not a multiple of a 3-byte frame, so
 * an unaligned block starts mid-sample and every value after the first block
 * decodes as noise -- which still LOOKS like a waveform.
 */
{
  resetWavPeaks();
  runToDone(TONE24);
  const c = wavPeaks(TONE24);
  ok(!!c && c.error === "", "a 24-bit file decodes (error: " + (c && c.error) + ")");
  /* The sine was written at 4000000/8388608 = 0.4768 of full scale. A LOOSE
     range (0.3..1) passes just as happily on misdecoded noise, which is how the
     frame-alignment bug survived the first version of this test. */
  ok(c && Math.abs(c.peak - 0.4768) < 0.02,
     "its peak is the amplitude actually written (0.477), not decoded noise, got "
     + (c && c.peak.toFixed(4)));
  const lit = c.points.filter((p) => p > 0.05).length;
  ok(lit > PEAK_WIDTH * 0.8,
     "a continuous sine fills nearly every column, got " + lit + "/" + PEAK_WIDTH);
}

/* ===================================================================== 5 ==
 * FAILURE IS REPORTED, NEVER THROWN, and never leaves a half-built picture.
 */
{
  resetWavPeaks();
  runToDone("/nonexistent/nope.wav", 8);
  const c = wavPeaks("/nonexistent/nope.wav");
  ok(!!c && c.done && c.error !== "", "a missing file reports an error and stops");
  ok(c && c.points.length === 0, "and offers no points to draw");

  resetWavPeaks();
  runToDone(GARBAGE, 8);
  const g = wavPeaks(GARBAGE);
  ok(!!g && g.done && g.error !== "", "a non-audio file reports an error and stops");
}

/* ===================================================================== 6 ==
 * RESAMPLING KEEPS THE PEAK, not the average -- averaging would flatten
 * exactly the transients the picture exists to show. And width is NOT part of
 * the cache key: resizing the graphic must not re-read the file.
 */
{
  const src = new Array(128).fill(0);
  src[7] = 1.0;                       /* a single transient */
  const out = resamplePeaks(src, 32);
  ok(out.length === 32, "resample gives the requested width");
  ok(Math.max.apply(null, out) === 1.0,
     "a lone transient SURVIVES the downsample -- peak, not average");

  resetWavPeaks();
  runToDone(TONE);
  const before = reads;
  resamplePeaks(wavPeaks(TONE).points, 30);
  resamplePeaks(wavPeaks(TONE).points, 60);
  ok(reads === before, "resampling to any width performs NO further reads");
}

/* ===================================================================== 7 ==
 * THE DRAW PATH NEVER DOES I/O. wavPeaks is called from a renderer on every
 * frame; a read there would cost more than the whole page render.
 */
{
  resetWavPeaks();
  runToDone(TONE);
  const before = reads;
  for (let i = 0; i < 50; i++) wavPeaks(TONE);
  ok(reads === before, "50 wavPeaks() calls performed NO reads, got " + (reads - before));
}

process.exit(fail ? 1 : 0);
'
