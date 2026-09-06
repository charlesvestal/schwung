/*
 * Where the audio lives in a WAV or AIFF file, and how to read one sample of
 * it. The CONTAINER half of both waveform readers, in one place.
 *
 * There were two copies. The sample CELL streams a file a block at a time
 * (wav_peaks.mjs, bounded per tick); the fullscreen EDITOR has the whole file
 * in memory and sweeps it once (shadow_ui.js). Those two jobs really are
 * different and stay apart -- but both first have to answer the same question,
 * "which bytes are the samples and how are they encoded", and answering it
 * twice is what let the two drift apart in three ways at once:
 *
 *   - WAVE_FORMAT_EXTENSIBLE (0xFFFE) was understood by the editor and not by
 *     the cell -- and EVERY 24-bit WAV that ffmpeg or sox writes is extensible,
 *     so the cell could not draw the most ordinary 24-bit file there is.
 *   - AIFF 8-bit is SIGNED. The cell read it as unsigned, so a quiet 8-bit
 *     AIFF drew as a full-scale block.
 *   - AIFC 'twos' -- byte-identical to NONE, and what macOS's own afconvert
 *     writes -- was rejected by both.
 *
 * A caller passes as many bytes as it happens to have: the cell hands over a
 * 4 KB header read, the editor the whole file. So `dataSize` is reported as the
 * file DECLARES it and is never clamped to the buffer -- a streaming caller
 * needs the real length, and a caller holding the whole file can clamp for
 * itself in one line.
 */

const byteAt = (b, i) => (!b || i < 0 || i >= b.length) ? 0 : (b[i] & 0xff);
const tag = (b, i) => String.fromCharCode(byteAt(b, i), byteAt(b, i + 1), byteAt(b, i + 2), byteAt(b, i + 3));
const u16le = (b, i) => byteAt(b, i) | (byteAt(b, i + 1) << 8);
const u32le = (b, i) => (byteAt(b, i) | (byteAt(b, i + 1) << 8) | (byteAt(b, i + 2) << 16)) + byteAt(b, i + 3) * 16777216;
const u16be = (b, i) => (byteAt(b, i) << 8) | byteAt(b, i + 1);
const u32be = (b, i) => ((byteAt(b, i) << 16) | (byteAt(b, i + 1) << 8) | byteAt(b, i + 2)) * 256 + byteAt(b, i + 3);

/* float32 without a DataView: QuickJS has typed arrays, and one shared scratch
 * pair avoids allocating per sample. */
const f32buf = new ArrayBuffer(4);
const f32u8 = new Uint8Array(f32buf);
const f32f = new Float32Array(f32buf);

/**
 * One sample as a float in -1..1, for every layout locateAudioData reports.
 * A table of small readers rather than a bit-twiddling generic, so each
 * format's sign and byte-order convention is stated exactly once.
 *
 * Hoist the reader OUT of a per-sample loop. Both callers do: a closure call
 * per sample beats the chain of string comparisons that picking the format
 * inside the loop costs, which is what the cell used to do.
 *
 * @param {string} kind
 * @returns {(bytes: Uint8Array, idx: number) => number | null}
 */
export function sampleReader(kind) {
    switch (kind) {
    case "pcm8u":   return (b, i) => (byteAt(b, i) - 128) / 128;
    case "pcm8s":   return (b, i) => { const v = byteAt(b, i); return (v > 0x7f ? v - 0x100 : v) / 128; };
    case "pcm16le": return (b, i) => { const v = u16le(b, i); return (v > 0x7fff ? v - 0x10000 : v) / 32768; };
    case "pcm16be": return (b, i) => { const v = u16be(b, i); return (v > 0x7fff ? v - 0x10000 : v) / 32768; };
    case "pcm24le": return (b, i) => {
        const v = byteAt(b, i) | (byteAt(b, i + 1) << 8) | (byteAt(b, i + 2) << 16);
        return (v > 0x7fffff ? v - 0x1000000 : v) / 8388608;
    };
    case "pcm24be": return (b, i) => {
        const v = (byteAt(b, i) << 16) | (byteAt(b, i + 1) << 8) | byteAt(b, i + 2);
        return (v > 0x7fffff ? v - 0x1000000 : v) / 8388608;
    };
    case "f32le":   return (b, i) => {
        f32u8[0] = byteAt(b, i); f32u8[1] = byteAt(b, i + 1);
        f32u8[2] = byteAt(b, i + 2); f32u8[3] = byteAt(b, i + 3);
        return f32f[0];
    };
    case "f32be":   return (b, i) => {
        f32u8[3] = byteAt(b, i); f32u8[2] = byteAt(b, i + 1);
        f32u8[1] = byteAt(b, i + 2); f32u8[0] = byteAt(b, i + 3);
        return f32f[0];
    };
    default:        return null;
    }
}

/** Bytes one sample of `kind` occupies. */
export function sampleBytesFor(kind) {
    switch (kind) {
    case "pcm8u": case "pcm8s": return 1;
    case "pcm24le": case "pcm24be": return 3;
    case "f32le": case "f32be": return 4;
    default: return 2;
    }
}

/* RIFF/WAVE, and any leading junk before it. Some exporters put a wrapper
 * ahead of the header, so the tag is SCANNED for rather than required at 0 --
 * but both "RIFF" and "WAVE" must match, which is what keeps an AIFF that
 * merely contains the letters from being read as a WAV. */
function findRiffOffset(bytes) {
    if (!bytes || bytes.length < 12) return -1;
    if (tag(bytes, 0) === "RIFF" && tag(bytes, 8) === "WAVE") return 0;
    const limit = Math.min(bytes.length - 12, 4096);
    for (let i = 0; i <= limit; i++) {
        if (tag(bytes, i) === "RIFF" && tag(bytes, i + 8) === "WAVE") return i;
    }
    return -1;
}

function locateRiff(bytes, riffOffset) {
    let fmtOffset = -1, dataOffset = -1, dataSize = 0;
    let cursor = riffOffset + 12;
    while (cursor + 8 <= bytes.length) {
        const chunkId = tag(bytes, cursor);
        const chunkSize = u32le(bytes, cursor + 4);
        const chunkData = cursor + 8;
        if (chunkId === "fmt " && bytes.length - chunkData >= 16) {
            fmtOffset = chunkData;
        } else if (chunkId === "data") {
            dataOffset = chunkData;
            dataSize = chunkSize;
            break;
        }
        const next = chunkData + chunkSize + (chunkSize % 2);
        if (next <= cursor) break;
        cursor = next;
    }
    if (fmtOffset < 0 || dataOffset < 0 || dataSize <= 0) return { error: "missing wav chunks" };

    const audioFmt = u16le(bytes, fmtOffset);
    const channels = Math.max(1, u16le(bytes, fmtOffset + 2));
    const blockAlign = Math.max(1, u16le(bytes, fmtOffset + 12));
    const bits = u16le(bytes, fmtOffset + 14);
    /* 0xFFFE is WAVE_FORMAT_EXTENSIBLE; its sub-format GUID's first two bytes
     * name the real codec, and 24-bit files are usually written that way --
     * ffmpeg and sox both do, unconditionally. */
    let fmt = audioFmt;
    if (audioFmt === 0xfffe && u16le(bytes, fmtOffset + 16) >= 22) fmt = u16le(bytes, fmtOffset + 24);
    if (fmt !== 1 && fmt !== 3) return { error: "unsupported wav codec" };
    const kind = fmt === 1 && bits === 8 ? "pcm8u"
        : fmt === 1 && bits === 16 ? "pcm16le"
        : fmt === 1 && bits === 24 ? "pcm24le"
        : fmt === 3 && bits === 32 ? "f32le" : null;
    if (!kind) return { error: "unsupported wav format" };
    return { kind, channels, bits, blockAlign, dataOffset, dataSize };
}

/*
 * FORM/AIFF(-C): big-endian chunks, and what much of Move's own Core Library
 * ships as. COMM carries the channel count and sample size; SSND holds the
 * audio after an 8-byte offset/blockSize preamble that is NOT part of it.
 *
 * AIFF-C adds a compression tag, and three of them are just a byte order:
 * 'NONE' and 'twos' are big-endian signed PCM, 'sowt' is the same samples
 * stored little-endian, 'raw ' is unsigned 8-bit. Anything else is genuinely
 * compressed and we say so.
 */
function locateAiff(bytes) {
    const form = tag(bytes, 8);
    if (form !== "AIFF" && form !== "AIFC") return { error: "not a wav file" };
    let channels = 0, bits = 0, compression = "NONE", dataOffset = -1, dataSize = 0;
    let cursor = 12;
    while (cursor + 8 <= bytes.length) {
        const chunkId = tag(bytes, cursor);
        const chunkSize = u32be(bytes, cursor + 4);
        const chunkData = cursor + 8;
        const available = Math.max(0, bytes.length - chunkData);
        if (chunkId === "COMM" && available >= 18) {
            channels = Math.max(1, u16be(bytes, chunkData));
            bits = u16be(bytes, chunkData + 6);
            /* AIFF-C: 4-char compression tag after the 10-byte sample rate. */
            if (form === "AIFC" && available >= 22) compression = tag(bytes, chunkData + 18);
        } else if (chunkId === "SSND" && available >= 8) {
            const skip = u32be(bytes, chunkData);
            dataOffset = chunkData + 8 + skip;
            dataSize = Math.max(0, chunkSize - 8 - skip);
            break;
        }
        const next = chunkData + chunkSize + (chunkSize % 2);
        if (next <= cursor) break;
        cursor = next;
    }
    if (!channels || !bits || dataOffset < 0 || dataSize <= 0) return { error: "missing aiff chunks" };

    const comp = compression.toUpperCase();
    /* 'twos' is big-endian signed PCM -- the same bytes as NONE, and what
     * macOS's own afconvert writes, so rejecting it turned any AIFC exported
     * from a Mac into "unsupported". */
    const bigEndian = comp === "NONE" || comp === "TWOS";
    const swapped = comp === "SOWT";
    const unsigned8 = comp === "RAW ";
    const float = comp === "FL32";
    if (!bigEndian && !swapped && !unsigned8 && !float) return { error: "unsupported aiff codec" };
    const kind = float ? (bits === 32 ? "f32be" : null)
        : bits === 8 ? (unsigned8 ? "pcm8u" : "pcm8s")
        : bits === 16 ? (swapped ? "pcm16le" : "pcm16be")
        : bits === 24 ? (swapped ? "pcm24le" : "pcm24be") : null;
    if (!kind) return { error: "unsupported aiff format" };
    return { kind, channels, bits, blockAlign: channels * sampleBytesFor(kind), dataOffset, dataSize };
}

/**
 * Locate the samples in a RIFF/WAVE or FORM/AIFF(-C) file.
 *
 * @param {Uint8Array} bytes  The whole file, or as much of its head as the
 *   caller has read -- 4 KB covers any real header plus the metadata chunks
 *   that sit before the audio.
 * @returns {{kind:string, channels:number, bits:number, blockAlign:number,
 *            dataOffset:number, dataSize:number} | {error:string}}
 *   `dataSize` is as DECLARED by the file and may exceed `bytes`; clamp it
 *   yourself if you are holding the whole thing.
 */
export function locateAudioData(bytes) {
    if (!bytes || bytes.length < 44) return { error: "file too small" };
    const riffOffset = findRiffOffset(bytes);
    if (riffOffset >= 0) return locateRiff(bytes, riffOffset);
    if (tag(bytes, 0) === "FORM") return locateAiff(bytes);
    return { error: "not a wav file" };
}
