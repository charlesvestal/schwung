/*
 * Peak envelope for a WAV/AIFF file, computed a little at a time and cached.
 * Ported from schwung-movy src/model/wav-peaks.ts, with permission.
 *
 * Three constraints shape this, and they pull against each other:
 *
 *   ACCURACY — a peak envelope that samples a handful of frames per column
 *     misses transients, and a granular sample is mostly transients. So the
 *     data chunk is STREAMED and every frame in a block contributes its max.
 *
 *   MEMORY — the file is never held. Blocks are read into one reusable buffer
 *     and collapsed into the per-column running max immediately, so the cost is
 *     O(width) regardless of a sample being 2 seconds or 2 minutes.
 *
 *   TIME — the shadow UI's tick IS its MIDI sampling interval, so a multi-
 *     megabyte read inside one tick would be felt as input lag. The job is
 *     resumable: each tick does BLOCKS_PER_TICK blocks and returns.
 *
 * Huge files are bounded rather than allowed to run for hundreds of ticks: past
 * MAX_BLOCKS the reader strides over the data, which trades exactness for a
 * fixed ceiling on total work. Normal samples fall well inside the budget and
 * are read in full.
 *
 * THE I/O IS INJECTED, which is our departure from movy. `std` and `os` are
 * QuickJS MODULES, so importing them here statically would make this file — and
 * everything that pulls it in, which includes viz_draw.mjs and therefore most
 * of the renderer — unloadable under node, where the host tests run. The device
 * wires the real pair in wav_io_qjs.mjs; the tests wire a node-backed one.
 * Without an IO this reports an error rather than throwing, so a caller that
 * forgets is visible instead of silently drawing nothing.
 */

/** Peaks are always computed at this width and RESAMPLED down to whatever the
 *  graphic currently spans. Width is deliberately NOT part of the cache key:
 *  anything that resizes the graphic would otherwise throw the envelope away
 *  and re-read the whole file, a visible stall on a knob turn. */
export const PEAK_WIDTH = 128;

const BLOCK_BYTES = 32768;
export const BLOCKS_PER_TICK = 2;
export const MAX_BLOCKS = 64;          /* <= 2 MB read for any one file */

/* ------------------------------------------------------------------- io */

let IO = null;

/**
 * @param {object} io
 * @param {(path:string) => ({read(buf,pos,len):number, seek(off,whence):number,
 *                            close():void}|null)} io.open
 * @param {(path:string) => ({size:number, mtime:number}|null)} io.stat
 */
export function setWavPeaksIO(io) { IO = io || null; }

/* --------------------------------------------------------------- decode */

const u16 = (b, i) => b[i] | (b[i + 1] << 8);
const u32 = (b, i) => (b[i] | (b[i + 1] << 8) | (b[i + 2] << 16)) + b[i + 3] * 16777216;
const s16 = (b, i) => { const v = b[i] | (b[i + 1] << 8); return (v & 0x8000) ? v - 65536 : v; };
/* 24-bit PCM. Worth its own branch rather than being rejected as exotic:
 * sample libraries ship it as a matter of course, and a sampler that cannot
 * draw its own library is not much of a feature. */
const s24 = (b, i) => {
    const v = b[i] | (b[i + 1] << 8) | (b[i + 2] << 16);
    return (v & 0x800000) ? v - 0x1000000 : v;
};
const u16be = (b, i) => (b[i] << 8) | b[i + 1];
const u32be = (b, i) => ((b[i] << 16) | (b[i + 1] << 8) | b[i + 2]) * 256 + b[i + 3];
const s16be = (b, i) => { const v = (b[i] << 8) | b[i + 1]; return (v & 0x8000) ? v - 65536 : v; };
const s24be = (b, i) => {
    const v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
    return (v & 0x800000) ? v - 0x1000000 : v;
};
/* float32 without a DataView: QuickJS has typed arrays, and one shared scratch
 * pair avoids allocating per sample. */
const f32buf = new ArrayBuffer(4);
const f32u8 = new Uint8Array(f32buf);
const f32f = new Float32Array(f32buf);
const f32 = (b, i) => {
    f32u8[0] = b[i]; f32u8[1] = b[i + 1]; f32u8[2] = b[i + 2]; f32u8[3] = b[i + 3];
    return f32f[0];
};

/* RIFF/WAVE: little-endian chunks, `fmt ` describes the codec, `data` holds it. */
function parseRiff(b, tag) {
    let cur = 12, fmtAt = -1, dataAt = -1, dataSize = 0;
    while (cur + 8 <= b.length) {
        const id = tag(cur);
        const sz = u32(b, cur + 4);
        if (id === "fmt ") fmtAt = cur + 8;
        else if (id === "data") { dataAt = cur + 8; dataSize = sz; break; }
        cur = cur + 8 + sz + (sz % 2);
    }
    if (fmtAt < 0 || dataAt < 0) return null;
    const fmt = u16(b, fmtAt);
    const bits = u16(b, fmtAt + 14);
    const blockAlign = Math.max(1, u16(b, fmtAt + 12));
    const codec =
        fmt === 1 && bits === 8 ? "pcm8"
        : fmt === 1 && bits === 16 ? "pcm16le"
        : fmt === 1 && bits === 24 ? "pcm24le"
        : fmt === 3 && bits === 32 ? "f32le"
        : null;
    return codec ? { dataOffset: dataAt, dataSize, blockAlign, codec } : null;
}

/* FORM/AIFF(-C): big-endian chunks. COMM carries the frame count and sample
 * size; SSND holds the audio after an 8-byte offset/blockSize preamble that is
 * NOT part of the samples. AIFF-C adds a compression tag — only the
 * uncompressed ones are readable, and 'sowt' means the samples are stored
 * little-endian despite the big-endian container. */
function parseAiff(b, tag) {
    const form = tag(8);
    if (form !== "AIFF" && form !== "AIFC") return null;
    let cur = 12, chans = 0, bits = 0, ssndAt = -1, ssndSize = 0;
    let compression = "NONE";
    while (cur + 8 <= b.length) {
        const id = tag(cur);
        const sz = u32be(b, cur + 4);
        if (id === "COMM") {
            chans = u16be(b, cur + 8);
            bits = u16be(b, cur + 14);
            /* AIFF-C: 4-char compression tag after the 10-byte sample rate. */
            if (form === "AIFC" && cur + 8 + 22 + 4 <= b.length) compression = tag(cur + 8 + 18);
        } else if (id === "SSND") {
            const off = u32be(b, cur + 8);
            ssndAt = cur + 16 + off;
            ssndSize = Math.max(0, sz - 8 - off);
            break;
        }
        cur = cur + 8 + sz + (sz % 2);
    }
    if (ssndAt < 0 || chans <= 0) return null;
    const swapped = compression === "sowt";
    if (compression !== "NONE" && compression !== "sowt") return null;   /* compressed */
    const codec =
        bits === 8 ? "pcm8"
        : bits === 16 ? (swapped ? "pcm16le" : "pcm16be")
        : bits === 24 ? (swapped ? "pcm24le" : "pcm24be")
        : null;
    const blockAlign = Math.max(1, chans * Math.floor(bits / 8));
    return codec ? { dataOffset: ssndAt, dataSize: ssndSize, blockAlign, codec } : null;
}

/* ------------------------------------------------------------------ job */

let cache = { key: "", width: 0, points: [], peak: 0, done: false, error: "" };
let job = null;

function fileSignature(path) {
    if (!IO || typeof IO.stat !== "function") return null;
    try {
        const st = IO.stat(path);
        if (!st) return null;
        return `${path}:${st.size || 0}:${st.mtime || 0}`;
    } catch (e) { return null; }
}

/* Read the header and locate the audio data. Only the first 4 KB is touched —
 * enough for the container header plus any metadata chunk sitting before the
 * audio. Understands RIFF/WAVE and FORM/AIFF(-C); both are chunk containers,
 * they just disagree on byte order and on what the fields are called. */
function startJob(path, width, key) {
    if (!IO || typeof IO.open !== "function") return null;
    let f = null;
    try {
        f = IO.open(path);
        if (!f) return null;
        const head = new ArrayBuffer(4096);
        const n = f.read(head, 0, 4096);
        const b = new Uint8Array(head, 0, Math.max(0, n));
        f.close();
        f = null;
        if (b.length < 44) return null;

        const tag = (i) => String.fromCharCode(b[i], b[i + 1], b[i + 2], b[i + 3]);
        const parsed = tag(0) === "RIFF" ? parseRiff(b, tag)
            : tag(0) === "FORM" ? parseAiff(b, tag)
            : null;
        if (!parsed) return null;

        const { dataOffset, dataSize, blockAlign, codec } = parsed;
        if (dataSize <= 0 || blockAlign <= 0) return null;

        const frameCount = Math.max(1, Math.floor(dataSize / blockAlign));
        /* Block size must be a whole number of FRAMES. 32768 is not a multiple
         * of a 3-byte 24-bit frame, so an unaligned block would start
         * mid-sample and every value after the first block would decode as
         * noise — which still looks like a waveform. */
        const blockBytes = Math.max(blockAlign, Math.floor(BLOCK_BYTES / blockAlign) * blockAlign);
        const totalBlocks = Math.max(1, Math.ceil(dataSize / blockBytes));
        const buf = new ArrayBuffer(BLOCK_BYTES);
        return {
            key, path, width, codec, points: new Array(width).fill(0),
            dataOffset, dataSize, blockAlign, frameCount,
            block: 0, totalBlocks, blockBytes,
            blockStride: Math.max(1, Math.ceil(totalBlocks / MAX_BLOCKS)),
            buf, view: new Uint8Array(buf), peak: 0,
        };
    } catch (e) {
        if (f) { try { f.close(); } catch (e2) { /* already gone */ } }
        return null;
    }
}

/* One block: fold every frame in it into the column it belongs to. Channel 0
 * only — blockAlign steps over the interleaved channels. */
function runBlock(j) {
    let f = null;
    try {
        f = IO.open(j.path);
        if (!f) return false;
        const byteStart = j.block * j.blockBytes;
        const want = Math.min(j.blockBytes, j.dataSize - byteStart);
        if (want <= 0) { f.close(); return false; }
        f.seek(j.dataOffset + byteStart, 0);          /* 0 = SEEK_SET */
        const got = f.read(j.buf, 0, want);
        f.close();
        f = null;
        if (got <= 0) return false;

        const b = j.view;
        const step = j.blockAlign;
        const codec = j.codec;
        const sampleBytes = codec === "pcm8" ? 1
            : codec === "pcm24le" || codec === "pcm24be" ? 3
            : codec === "f32le" ? 4 : 2;
        const firstFrame = Math.floor(byteStart / step);
        for (let off = 0; off + sampleBytes <= got; off += step) {
            let v = 0;
            if (codec === "pcm16le") v = s16(b, off) / 32768;
            else if (codec === "pcm16be") v = s16be(b, off) / 32768;
            else if (codec === "pcm24le") v = s24(b, off) / 8388608;
            else if (codec === "pcm24be") v = s24be(b, off) / 8388608;
            else if (codec === "pcm8") v = (b[off] - 128) / 128;
            else v = f32(b, off);
            if (v < 0) v = -v;
            if (v > 1) v = 1;
            const frame = firstFrame + off / step;
            let col = Math.floor((frame * j.width) / j.frameCount);
            if (col < 0) col = 0; else if (col >= j.width) col = j.width - 1;
            if (v > j.points[col]) j.points[col] = v;
            /* Running peak, folded in here rather than rescanned per frame: the
             * renderer normalises against it so a quiet sample still uses the
             * full height. */
            if (v > j.peak) j.peak = v;
        }
        return true;
    } catch (e) {
        if (f) { try { f.close(); } catch (e2) { /* already gone */ } }
        return false;
    }
}

/* ----------------------------------------------------------------- public */

/**
 * Advance the job for `path`. Call once per TICK, never from a render path.
 * @returns {boolean} true when the picture changed, so the caller can mark the
 *   frame dirty without repainting on idle ticks.
 */
export function wavPeaksTick(path) {
    if (!path) return false;
    const width = PEAK_WIDTH;
    const key = fileSignature(path);
    if (!key) {
        if (cache.key !== `missing:${path}`) {
            cache = { key: `missing:${path}`, width, points: [], peak: 0,
                      done: true, error: "file not found" };
            return true;
        }
        return false;
    }
    if (cache.key === key && cache.done) return false;

    if (!job || job.key !== key) {
        job = startJob(path, width, key);
        if (!job) {
            cache = { key, width, points: [], peak: 0, done: true, error: "unreadable wav" };
            return true;
        }
        cache = { key, width, points: job.points, peak: 0, done: false, error: "" };
    }

    let worked = false;
    for (let i = 0; i < BLOCKS_PER_TICK && job.block < job.totalBlocks; i++) {
        runBlock(job);
        job.block += job.blockStride;
        worked = true;
    }
    if (worked) cache.peak = job.peak;    /* the scale tracks the data as it fills in */
    if (job.block >= job.totalBlocks) {
        cache = { key, width, points: job.points, peak: job.peak, done: true, error: "" };
        job = null;
    }
    return worked;
}

/**
 * The current envelope — possibly partial while a job is running, null when
 * this path is not the one cached. NEVER does I/O, so it is safe to call from
 * a draw path on every frame; a read there would cost more than the whole page
 * render (~2.8 ms against 1.68 ms).
 */
export function wavPeaks(path) {
    if (!path) return null;
    if (!cache.key.startsWith(`${path}:`) && cache.key !== `missing:${path}`) return null;
    return cache;
}

/**
 * Collapse the full-width envelope onto `width` columns, keeping the PEAK of
 * each source range — averaging would flatten exactly the transients the
 * picture exists to show.
 */
export function resamplePeaks(points, width) {
    if (width <= 0 || !points || points.length === 0) return [];
    if (points.length === width) return points;
    const out = new Array(width).fill(0);
    for (let i = 0; i < width; i++) {
        const a = Math.floor((i * points.length) / width);
        const b = Math.max(a + 1, Math.floor(((i + 1) * points.length) / width));
        let mx = 0;
        for (let j = a; j < b && j < points.length; j++) if (points[j] > mx) mx = points[j];
        out[i] = mx;
    }
    return out;
}

/** Test seam: the cache is module-level so a job survives across ticks. */
export function resetWavPeaks() {
    cache = { key: "", width: 0, points: [], peak: 0, done: false, error: "" };
    job = null;
}
