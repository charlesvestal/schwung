#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE TWO WAVEFORM READERS MUST AGREE ABOUT A FILE.
#
# The sample CELL streams a file a block at a time (wav_peaks.mjs, bounded per
# tick); the fullscreen EDITOR holds it all and sweeps it once (shadow_ui.js).
# Different jobs, same question first: which bytes are the samples, and how are
# they encoded. That question was answered twice, and the two answers drifted --
# WAVE_FORMAT_EXTENSIBLE in one and not the other (which is EVERY 24-bit WAV
# ffmpeg or sox writes), AIFF 8-bit read as unsigned in one, and AIFC 'twos'
# rejected by both though it is byte-identical to NONE and is what macOS
# afconvert writes.
#
# So this drives BOTH readers over one corpus, one file per layout, and asserts
# three things per file: the peak is the amplitude actually written, the first
# bin is near silent (a wrong byte order or sign shows up there before anywhere
# else), and the two readers report the SAME peak. The last one is the guard
# against the drift; the first two are what make it worth anything.
#
# NO APOSTROPHES inside the node script: it is a quoted heredoc, but the shell
# quoting rules around these tests have bitten before.

fail() { echo "FAIL: $*" >&2; exit 1; }
command -v node >/dev/null 2>&1 || fail "node is required"

REPO="$PWD"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/run.mjs" <<'NODE'
import fs from "fs";

const REPO = process.argv[2];
const TMP = process.argv[3];

const fmt = await import(REPO + "/src/shared/param_pages/wav_format.mjs");
const wp = await import(REPO + "/src/shared/param_pages/wav_peaks.mjs");

/* The editor parser is lifted out of shadow_ui.js by name -- node cannot
 * import that file (it names QuickJS modules), and the shared module it
 * depends on is handed in as an argument. */
const src = fs.readFileSync(REPO + "/src/shadow/shadow_ui.js", "utf8");
const a = src.indexOf("function wavContentToBytes");
const b = src.indexOf("function getWavPositionWaveformPreview");
if (a < 0 || b < 0) { console.log("FAIL: editor parser not found in shadow_ui.js"); process.exit(1); }
const parseEditor = new Function("locateAudioData", "sampleReader", "sampleBytesFor",
    src.slice(a, b) + "\nreturn parseWavPositionPeaks;")(
    fmt.locateAudioData, fmt.sampleReader, fmt.sampleBytesFor);

wp.setWavPeaksIO({
    open(p) {
        let fd;
        try { fd = fs.openSync(p, "r"); } catch (e) { return null; }
        let pos = 0;
        return {
            read(buf, off, len) { const n = fs.readSync(fd, new Uint8Array(buf), off, len, pos); pos += n; return n; },
            seek(o) { pos = o; return 0; },
            close() { fs.closeSync(fd); },
        };
    },
    stat(p) {
        try { const st = fs.statSync(p); return { size: st.size, mtime: Math.floor(st.mtimeMs) }; }
        catch (e) { return null; }
    },
});

const u16le = (v) => [v & 255, (v >> 8) & 255];
const u32le = (v) => [v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >>> 24) & 255];
const u16be = (v) => [(v >> 8) & 255, v & 255];
const u32be = (v) => [(v >>> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255];
const str = (s) => [...s].map((c) => c.charCodeAt(0));

/* A ramp to a magnitude of 0.75 over 4096 frames, ALTERNATING SIGN frame by
 * frame. The alternation is the point: a first draft ramped positive only, and
 * a reader that treats signed samples as unsigned then decodes it perfectly --
 * every negative value, which is where a sign bug actually shows, was missing
 * from the corpus. Long enough that the cell reads more than one block for the
 * wide formats, short enough to stay a unit test. */
const N = 4096, PEAK = 0.75;
function samples(bits, be, signed8, chans) {
    const out = [];
    for (let i = 0; i < N; i++) {
        const mag = (i / (N - 1)) * PEAK;
        const v = (i % 2 === 0) ? mag : -mag;
        for (let c = 0; c < chans; c++) {
            if (bits === 8) out.push(signed8 ? (Math.round(v * 127) & 255) : (128 + Math.round(v * 127)));
            else if (bits === 16) { const s = Math.round(v * 32767); out.push(...(be ? u16be(s) : u16le(s))); }
            else if (bits === 24) {
                const s = Math.round(v * 8388607);
                const le = [s & 255, (s >> 8) & 255, (s >> 16) & 255];
                out.push(...(be ? le.reverse() : le));
            } else if (bits === 32) {
                const u = new Uint8Array(new Float32Array([v]).buffer);
                out.push(...(be ? [...u].reverse() : [...u]));
            }
        }
    }
    return out;
}

function wav(bits, fmtTag, { extensible = false, chans = 1 } = {}) {
    const data = samples(bits, false, false, chans);
    const align = chans * (bits / 8);
    const fmtBody = [...u16le(extensible ? 0xfffe : fmtTag), ...u16le(chans), ...u32le(44100),
        ...u32le(44100 * align), ...u16le(align), ...u16le(bits)];
    const ext = extensible
        ? [...u16le(22), ...u16le(bits), ...u32le(4), ...u16le(fmtTag), ...new Array(14).fill(0)]
        : [];
    const fmtChunk = [...str("fmt "), ...u32le(fmtBody.length + ext.length), ...fmtBody, ...ext];
    /* An odd-sized metadata chunk before the audio: the walk has to skip it AND
     * honour the pad byte, or every offset after it is one out. */
    const junk = [...str("LIST"), ...u32le(5), 1, 2, 3, 4, 5, 0];
    const dataChunk = [...str("data"), ...u32le(data.length), ...data];
    const body = [...str("WAVE"), ...fmtChunk, ...junk, ...dataChunk];
    return new Uint8Array([...str("RIFF"), ...u32le(body.length), ...body]);
}

function aiff(bits, form, comp, { chans = 1, ssndSkip = 0 } = {}) {
    const le = comp === "sowt";
    const unsigned8 = comp === "raw ";
    const data = samples(bits, !le, !unsigned8, chans);
    const commBody = [...u16be(chans), ...u32be(N), ...u16be(bits), 0x40, 0x0e, 0xac, 0x44, 0, 0, 0, 0, 0, 0];
    if (form === "AIFC") commBody.push(...str(comp), 4, ...str("none"), 0);
    const comm = [...str("COMM"), ...u32be(commBody.length), ...commBody];
    /* SSND carries an offset BEFORE the first frame, and those bytes are not
     * samples. They are filled LOUD (0x7f) on purpose: zeros there would
     * decode as silence and a reader that ignored the offset would still look
     * right, which is exactly the mutation this case exists to fail on. */
    const ssndBody = [...u32be(ssndSkip), ...u32be(0), ...new Array(ssndSkip).fill(0x7f), ...data];
    const ssnd = [...str("SSND"), ...u32be(ssndBody.length), ...ssndBody];
    const body = [...str(form), ...comm, ...ssnd];
    return new Uint8Array([...str("FORM"), ...u32be(body.length), ...body]);
}

const cases = {
    "wav pcm8": wav(8, 1),
    "wav pcm16": wav(16, 1),
    "wav pcm24": wav(24, 1),
    "wav pcm24 extensible": wav(24, 1, { extensible: true }),
    "wav pcm24 stereo extensible": wav(24, 1, { extensible: true, chans: 2 }),
    "wav float32": wav(32, 3),
    "aiff 8": aiff(8, "AIFF", "NONE"),
    "aiff 16": aiff(16, "AIFF", "NONE"),
    "aiff 24": aiff(24, "AIFF", "NONE"),
    "aiff 16 stereo": aiff(16, "AIFF", "NONE", { chans: 2 }),
    "aifc NONE 16": aiff(16, "AIFC", "NONE"),
    "aifc twos 16": aiff(16, "AIFC", "twos"),
    "aifc sowt 16": aiff(16, "AIFC", "sowt"),
    "aifc sowt 24": aiff(24, "AIFC", "sowt"),
    "aifc raw 8": aiff(8, "AIFC", "raw "),
    "aifc fl32": aiff(32, "AIFC", "fl32"),
    "aiff 16 with an SSND offset": aiff(16, "AIFF", "NONE", { ssndSkip: 6 }),
};

let bad = 0;
let n = 0;
for (const [name, bytes] of Object.entries(cases)) {
    const path = `${TMP}/${name.replace(/[^a-z0-9]+/gi, "_")}.snd`;
    fs.writeFileSync(path, bytes);

    const ed = parseEditor(bytes, 8);
    if (ed.error) { console.log(`FAIL: ${name}: editor: ${ed.error}`); bad++; continue; }
    const edPeak = ed.points[ed.points.length - 1];
    if (Math.abs(edPeak - PEAK) > 0.03) { console.log(`FAIL: ${name}: editor peak ${edPeak.toFixed(3)}, want ${PEAK}`); bad++; continue; }
    if (ed.points[0] > 0.15) { console.log(`FAIL: ${name}: editor first bin ${ed.points[0].toFixed(3)} should be near silent -- byte order or sign is wrong`); bad++; continue; }

    wp.resetWavPeaks();
    let guard = 0;
    while (!wp.wavPeaksDone(path) && guard++ < 2000) wp.wavPeaksTick(path);
    const cell = wp.wavPeaks(path);
    if (!cell || cell.error) { console.log(`FAIL: ${name}: cell: ${cell ? cell.error : "no envelope"}`); bad++; continue; }
    if (Math.abs(cell.peak - PEAK) > 0.03) { console.log(`FAIL: ${name}: cell peak ${cell.peak.toFixed(3)}, want ${PEAK}`); bad++; continue; }
    if (cell.points[0] > 0.15) { console.log(`FAIL: ${name}: cell first bin ${cell.points[0].toFixed(3)} should be near silent -- byte order or sign is wrong`); bad++; continue; }
    if (Math.abs(cell.peak - edPeak) > 0.02) { console.log(`FAIL: ${name}: the two readers DISAGREE -- cell ${cell.peak.toFixed(3)}, editor ${edPeak.toFixed(3)}`); bad++; continue; }
    n++;
}

/* Neither reader may invent audio out of a file that has none. */
const bogus = new Uint8Array(4096).fill(65);
if (!parseEditor(bogus, 8).error) { console.log("FAIL: the editor parsed 4 KB of A as audio"); bad++; }
const bogusPath = `${TMP}/bogus.snd`;
fs.writeFileSync(bogusPath, bogus);
wp.resetWavPeaks();
let g = 0;
while (!wp.wavPeaksDone(bogusPath) && g++ < 100) wp.wavPeaksTick(bogusPath);
const bogusCell = wp.wavPeaks(bogusPath);
if (!bogusCell || !bogusCell.error) { console.log("FAIL: the cell parsed 4 KB of A as audio"); bad++; }

/* A compressed AIFC is genuinely unreadable and must SAY so rather than
 * decoding the compressed bytes as if they were samples. */
const ima4 = aiff(16, "AIFC", "ima4");
if (parseEditor(ima4, 8).error !== "unsupported aiff codec") {
    console.log("FAIL: a compressed AIFC should report unsupported, got " + JSON.stringify(parseEditor(ima4, 8).error));
    bad++;
}

if (bad) process.exit(1);
console.log(`  ok  ${n} layouts read identically by the cell and the fullscreen editor`);
NODE

node "$TMP/run.mjs" "$REPO" "$TMP" || fail "the two readers do not agree"
echo "PASS: test_wav_format_readers"
