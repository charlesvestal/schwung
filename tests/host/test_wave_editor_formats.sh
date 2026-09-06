#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The fullscreen wave editor read RIFF/WAVE at 8 or 16 bits and nothing else.
#
# The sample CELL's peaks come from wav_peaks.mjs, which reads WAV 8/16/24,
# float32 and AIFF -- so a Core Library kit (largely 24-bit WAV and AIFF)
# drew its waveform in the cell and then answered "unsupported wav format"
# the moment you clicked into the editor. Reported from the device. This
# drives the editor's own parser, lifted out of shadow_ui.js by name, over
# one synthesised file per layout and asserts the peak it finds.

fail() { echo "FAIL: $*" >&2; exit 1; }
command -v node >/dev/null 2>&1 || fail "node is required"

node -e '
const fs = require("fs");
const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const a = src.indexOf("function wavContentToBytes");
const b = src.indexOf("function getWavPositionWaveformPreview");
if (a < 0 || b < 0) { console.log("FAIL: parser functions not found"); process.exit(1); }
const parse = new Function(src.slice(a, b) + "\nreturn parseWavPositionPeaks;")();

const u16le = (v) => [v & 255, (v >> 8) & 255];
const u32le = (v) => [v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >>> 24) & 255];
const u16be = (v) => [(v >> 8) & 255, v & 255];
const u32be = (v) => [(v >>> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255];
const str = (s) => [...s].map((c) => c.charCodeAt(0));

/* A ramp to +0.75 over 64 frames, one channel. */
const N = 64, PEAK = 0.75;
function samples(bits, be, signed8) {
  const out = [];
  for (let i = 0; i < N; i++) {
    const v = (i / (N - 1)) * PEAK;
    if (bits === 8) out.push(signed8 ? (Math.round(v * 127) & 255) : (128 + Math.round(v * 127)));
    else if (bits === 16) { const s = Math.round(v * 32767); out.push(...(be ? u16be(s) : u16le(s))); }
    else if (bits === 24) { const s = Math.round(v * 8388607); const le = [s & 255, (s >> 8) & 255, (s >> 16) & 255]; out.push(...(be ? le.reverse() : le)); }
    else if (bits === 32) { const f = new Float32Array([v]); out.push(...new Uint8Array(f.buffer)); }
  }
  return out;
}
function wav(bits, fmt, extensible) {
  const data = samples(bits, false);
  const fmtBody = [...u16le(extensible ? 0xfffe : fmt), ...u16le(1), ...u32le(44100), ...u32le(44100 * bits / 8), ...u16le(bits / 8), ...u16le(bits)];
  const ext = extensible ? [...u16le(22), ...u16le(bits), ...u32le(4), ...u16le(fmt), ...new Array(14).fill(0)] : [];
  const fmtChunk = [...str("fmt "), ...u32le(fmtBody.length + ext.length), ...fmtBody, ...ext];
  const junk = [...str("smpl"), ...u32le(4), 0, 0, 0, 0];
  const dataChunk = [...str("data"), ...u32le(data.length), ...data];
  const body = [...str("WAVE"), ...fmtChunk, ...junk, ...dataChunk];
  return new Uint8Array([...str("RIFF"), ...u32le(body.length), ...body]);
}
function aiff(bits, form, comp) {
  const data = samples(bits, comp !== "sowt", true);
  const commBody = [...u16be(1), ...u32be(N), ...u16be(bits), 0x40, 0x0e, 0xac, 0x44, 0, 0, 0, 0, 0, 0];
  if (form === "AIFC") commBody.push(...str(comp), 4, ...str("none"), 0);
  const comm = [...str("COMM"), ...u32be(commBody.length), ...commBody];
  const ssndBody = [...u32be(0), ...u32be(0), ...data];
  const ssnd = [...str("SSND"), ...u32be(ssndBody.length), ...ssndBody];
  const body = [...str(form), ...comm, ...ssnd];
  return new Uint8Array([...str("FORM"), ...u32be(body.length), ...body]);
}
const cases = {
  "wav pcm8": wav(8, 1), "wav pcm16": wav(16, 1), "wav pcm24": wav(24, 1),
  "wav pcm24 extensible": wav(24, 1, true), "wav float32": wav(32, 3),
  "aiff 16": aiff(16, "AIFF", "NONE"), "aiff 24": aiff(24, "AIFF", "NONE"), "aiff 8": aiff(8, "AIFF", "NONE"),
  "aifc NONE 16": aiff(16, "AIFC", "NONE"), "aifc sowt 16": aiff(16, "AIFC", "sowt"),
};
let bad = 0;
for (const [name, bytes] of Object.entries(cases)) {
  const r = parse(bytes, 8);
  const last = r.points.length ? r.points[r.points.length - 1] : -1;
  if (r.error) { console.log(`FAIL: ${name}: ${r.error}`); bad++; continue; }
  if (Math.abs(last - PEAK) > 0.03) { console.log(`FAIL: ${name}: peak ${last.toFixed(3)}, want ${PEAK}`); bad++; continue; }
  if (r.points[0] > 0.15) { console.log(`FAIL: ${name}: first bin ${r.points[0].toFixed(3)} should be near silent -- byte order or sign is wrong`); bad++; continue; }
}
const bogus = parse(new Uint8Array(64).fill(65), 8);
if (!bogus.error) { console.log("FAIL: 64 bytes of A parsed as audio"); bad++; }
if (bad) process.exit(1);
console.log("  ok  the wave editor reads WAV 8/16/24/float and AIFF 8/16/24, big- and little-endian");
' || fail "parser"
echo "PASS: test_wave_editor_formats"
