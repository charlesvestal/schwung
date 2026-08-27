/**
 * gif.mjs — a minimal animated-GIF encoder for 1-bit device frames.
 *
 * Written rather than depended on: this repo vendors nothing for tooling, and
 * the whole format for a 2-colour image is a header, a palette, and LZW.
 *
 * Node-only. Nothing here ships to the device.
 */

/* GIF's LZW, which is not quite anyone else's.
 *
 * Codes are packed LSB-FIRST and the width grows as the table fills, so the
 * decoder has to be told to reset: a CLEAR code is emitted at the start and
 * again whenever the table reaches 4095. The minimum code size is 2 even for a
 * two-colour image — a 1-bit minimum is legal to write and refused by most
 * decoders, which is the one place this is easy to get subtly wrong and see it
 * only as "the browser will not show it".
 */
function lzw(indices, minCodeSize) {
    const clear = 1 << minCodeSize;
    const end = clear + 1;
    let dict = new Map();
    let next = end + 1;
    let codeSize = minCodeSize + 1;

    const out = [];
    let cur = 0, curBits = 0;
    const emit = (code) => {
        cur |= code << curBits;
        curBits += codeSize;
        while (curBits >= 8) { out.push(cur & 0xff); cur >>= 8; curBits -= 8; }
    };
    const reset = () => {
        dict = new Map();
        next = end + 1;
        codeSize = minCodeSize + 1;
    };

    emit(clear);
    let prefix = indices[0];
    for (let i = 1; i < indices.length; i++) {
        const k = indices[i];
        const key = prefix * 4096 + k;
        if (dict.has(key)) { prefix = dict.get(key); continue; }
        emit(prefix);
        dict.set(key, next);
        if (next === (1 << codeSize) && codeSize < 12) codeSize++;
        next++;
        if (next >= 4095) { emit(clear); reset(); }
        prefix = k;
    }
    emit(prefix);
    emit(end);
    if (curBits > 0) out.push(cur & 0xff);
    return out;
}

function subBlocks(bytes) {
    const out = [];
    for (let i = 0; i < bytes.length; i += 255) {
        const chunk = bytes.slice(i, i + 255);
        out.push(chunk.length, ...chunk);
    }
    out.push(0);
    return out;
}

/**
 * @param {Array<{pixels: Uint8Array, width: number, height: number}>} frames
 *        1 = lit. All frames must be the same size.
 * @param {object} o
 * @param {number} o.delayMs   per-frame delay
 * @param {number} [o.scale]   integer pixel scale
 * @returns {Buffer}
 */
export function encodeGif(frames, { delayMs, scale = 1 }) {
    if (!frames.length) throw new Error("no frames");
    const w = frames[0].width * scale, h = frames[0].height * scale;
    const b = [];
    const u16 = (v) => b.push(v & 0xff, (v >> 8) & 0xff);

    b.push(...[0x47, 0x49, 0x46, 0x38, 0x39, 0x61]);            /* GIF89a */
    u16(w); u16(h);
    /* global colour table, 1-bit colour resolution, 2 entries */
    b.push(0x80 | (0 << 4) | 0, 0, 0);
    b.push(0, 0, 0, 0xff, 0xff, 0xff);                          /* black, white */

    /* NETSCAPE2.0 loop-forever. Without it a viewer plays the strip once and
     * leaves the last frame up, which for a 160ms animation is indistinguish-
     * able from a still. */
    b.push(0x21, 0xff, 0x0b);
    for (const ch of "NETSCAPE2.0") b.push(ch.charCodeAt(0));
    b.push(0x03, 0x01, 0x00, 0x00, 0x00);

    /* GIF delays are in HUNDREDTHS of a second, so anything under 10ms rounds
     * to zero and many viewers then substitute 100ms. Floored at 2. */
    const delay = Math.max(2, Math.round(delayMs / 10));

    for (const f of frames) {
        b.push(0x21, 0xf9, 0x04, 0x04);   /* disposal 1: leave the frame up */
        u16(delay);
        b.push(0x00, 0x00);

        b.push(0x2c); u16(0); u16(0); u16(w); u16(h); b.push(0x00);

        const idx = new Uint8Array(w * h);
        for (let y = 0; y < h; y++) {
            const sy = (y / scale) | 0;
            for (let x = 0; x < w; x++) {
                idx[y * w + x] = f.pixels[sy * f.width + ((x / scale) | 0)] ? 1 : 0;
            }
        }
        const minCodeSize = 2;
        b.push(minCodeSize, ...subBlocks(lzw(idx, minCodeSize)));
    }

    b.push(0x3b);
    return Buffer.from(b);
}
