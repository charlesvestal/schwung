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

import { locateAudioData, sampleReader, sampleBytesFor } from './wav_format.mjs';

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

/* The containers and the sample formats are read by wav_format.mjs, which the
 * fullscreen wave editor uses too. They were parsed separately once, and the
 * two copies drifted: this one could not read the WAVE_FORMAT_EXTENSIBLE that
 * every 24-bit WAV from ffmpeg or sox is written as, and it read AIFF 8-bit as
 * unsigned, which drew a quiet sample full-scale. */

/* ------------------------------------------------------------------ job */

/*
 * A page can hold MORE THAN ONE waveform, so this is a small set, not a slot.
 *
 * It was a single `cache` and one entry was all any page needed — until
 * detectSample started returning a graphic per file rather than one per page
 * ("EVERY file, not the first: breakbeat loads two samples side by side").
 * With one slot, drawing A and B alternately made each call evict the other's
 * finished envelope and restart its job, so neither ever completed and both
 * cells drew as an empty bracketed rectangle. Reported as breakbeat's B SMP
 * "drawing blank on the grid".
 *
 * `job` stays SINGULAR on purpose. The cache is what may hold several files;
 * the WORK is still one bounded batch per tick, which is the property that
 * keeps this off the frame budget. The caller advances one incomplete graphic
 * per tick and lets the rest wait their turn.
 *
 * Four entries covers the widest real page (breakbeat's two) with room, and
 * bounds the memory at four PEAK_WIDTH arrays of small numbers.
 */
const CACHE_MAX = 4;
let caches = [];        /* most-recently-touched first */
let job = null;

function findCache(key) {
    for (const c of caches) if (c.key === key) return c;
    return null;
}

/* Insert or replace, newest first, evicting the least recently touched. */
function putCache(entry) {
    const rest = caches.filter((c) => c.key !== entry.key);
    caches = [entry, ...rest].slice(0, CACHE_MAX);
    return entry;
}

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

        const parsed = locateAudioData(b);
        if (parsed.error) return null;

        const { dataOffset, dataSize, blockAlign, kind } = parsed;
        if (dataSize <= 0 || blockAlign <= 0) return null;
        /* Hoisted once per FILE. Picking the format inside the sample loop cost
         * a chain of string comparisons per sample, which is the one place in
         * this file where that is worth avoiding. */
        const read = sampleReader(kind);
        if (!read) return null;

        const frameCount = Math.max(1, Math.floor(dataSize / blockAlign));
        /* Block size must be a whole number of FRAMES. 32768 is not a multiple
         * of a 3-byte 24-bit frame, so an unaligned block would start
         * mid-sample and every value after the first block would decode as
         * noise — which still looks like a waveform. */
        const blockBytes = Math.max(blockAlign, Math.floor(BLOCK_BYTES / blockAlign) * blockAlign);
        const totalBlocks = Math.max(1, Math.ceil(dataSize / blockBytes));
        const buf = new ArrayBuffer(BLOCK_BYTES);
        return {
            key, path, width, kind, read, sampleBytes: sampleBytesFor(kind),
            points: new Array(width).fill(0),
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
        const read = j.read;
        const sampleBytes = j.sampleBytes;
        const firstFrame = Math.floor(byteStart / step);
        for (let off = 0; off + sampleBytes <= got; off += step) {
            let v = read(b, off);
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
        if (!findCache(`missing:${path}`)) {
            putCache({ key: `missing:${path}`, width, points: [], peak: 0,
                       done: true, error: "file not found" });
            return true;
        }
        return false;
    }
    const settled = findCache(key);
    if (settled && settled.done) return false;

    if (!job || job.key !== key) {
        job = startJob(path, width, key);
        if (!job) {
            putCache({ key, width, points: [], peak: 0, done: true, error: "unreadable wav" });
            return true;
        }
        putCache({ key, width, points: job.points, peak: 0, done: false, error: "" });
    }

    /* The entry SHARES the job's points array, so a partial envelope draws as
     * it fills. Re-found rather than carried from above: the putCache branch
     * may not have run this call. */
    const entry = findCache(key);
    let worked = false;
    for (let i = 0; i < BLOCKS_PER_TICK && job.block < job.totalBlocks; i++) {
        runBlock(job);
        job.block += job.blockStride;
        worked = true;
    }
    if (worked && entry) entry.peak = job.peak;  /* the scale tracks the data as it fills in */
    if (job.block >= job.totalBlocks) {
        putCache({ key, width, points: job.points, peak: job.peak, done: true, error: "" });
        job = null;
    }
    return worked;
}

/**
 * Has this path's envelope finished? Distinct from wavPeaks(path) being
 * non-null, which is also true of a job still in progress.
 *
 * This is what lets a caller with SEVERAL waveforms on one page advance them
 * one at a time without starving any: skip the settled ones, spend the tick on
 * the first that is not. Returns false for a path never seen, which is the
 * answer that makes such a caller pick it up.
 */
export function wavPeaksDone(path) {
    const c = wavPeaks(path);
    return !!(c && c.done);
}

/**
 * The current envelope — possibly partial while a job is running, null when
 * this path is not the one cached. NEVER does I/O, so it is safe to call from
 * a draw path on every frame; a read there would cost more than the whole page
 * render (~2.8 ms against 1.68 ms).
 */
export function wavPeaks(path) {
    if (!path) return null;
    for (const c of caches) {
        if (c.key.startsWith(`${path}:`) || c.key === `missing:${path}`) return c;
    }
    return null;
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
    caches = [];
    job = null;
}
