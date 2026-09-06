/*
 * qr.mjs — a QR encoder small enough to live on the device, for exactly one job.
 *
 * The Connect page has to put "http://<ip>:7700" on a 128x64 1-bit screen in a
 * form a phone can read. Everything here is scoped to that string and nothing
 * wider:
 *
 *   - BYTE MODE ONLY. Alphanumeric mode would be smaller, but its charset is
 *     uppercase-and-nine-symbols and a URL has lowercase letters, a colon and
 *     two slashes in it. Numeric mode cannot hold a letter at all. There is no
 *     input this function will ever be handed that byte mode does not cover, so
 *     the other two modes are absent rather than unreachable.
 *   - ECC LEVEL L. The camera is 20cm from a lit screen with no glare and no
 *     print, which is the easiest read a QR ever gets; spending a third of the
 *     symbol on error correction would buy nothing and cost a version, and a
 *     version costs four modules of edge on a screen where every pixel is
 *     already spoken for.
 *   - VERSIONS 1-4, AND THAT IS A HARD STOP RATHER THAN A DEFAULT.
 *
 * The version ceiling is the one number here that is load-bearing, so it is
 * stated rather than left as "what the table happens to hold". From version 5
 * up, a symbol's codewords are split across SEVERAL error-correction blocks,
 * interleaved, and with two block groups of different sizes. That is a real
 * chunk of code — group sizes, per-block generators, an interleave — and every
 * line of it would be dead: version 4 at level L holds 78 bytes, and the
 * longest string this module can be handed is "http://255.255.255.255:65535",
 * which is 28. So the table stops at 4, each of those four versions has exactly
 * ONE block, and encode() THROWS rather than silently emitting a symbol built
 * on a single-block assumption that no longer holds. A wrong QR is worse than
 * no QR: it scans, and it goes somewhere else.
 *
 * PURE. No host globals, no drawing, no io — it returns a boolean grid and the
 * caller decides how big a module is. tests/host/test_qr_encode.sh runs it in a
 * bare node process and decodes the result back.
 */

/* Total codewords, EC codewords, and the resulting data capacity, at level L.
 * Indexed by version. Every entry is ONE error-correction block; see the header
 * for why the table stops where it does. */
const VERSIONS_L = {
    1: { size: 21, total: 26,  ec: 7,  align: [] },
    2: { size: 25, total: 44,  ec: 10, align: [6, 18] },
    3: { size: 29, total: 70,  ec: 15, align: [6, 22] },
    4: { size: 33, total: 100, ec: 20, align: [6, 26] },
};

/** Data codewords available at a version, level L. */
function dataCapacity(v) {
    const t = VERSIONS_L[v];
    return t.total - t.ec;
}

/*
 * Byte-mode payload for `len` bytes: a 4-bit mode indicator, an 8-bit character
 * count (8 bits is the count length for byte mode at versions 1-9 — it widens
 * to 16 at version 10, which this module never reaches) and the bytes.
 */
function bitsNeeded(len) { return 4 + 8 + 8 * len; }

/** The smallest version that holds `len` bytes, or 0 if none does. */
export function chooseVersion(len) {
    for (let v = 1; v <= 4; v++) {
        if (bitsNeeded(len) <= dataCapacity(v) * 8) return v;
    }
    return 0;
}

/* ------------------------------------------------------------------ GF(256)
 *
 * The field QR uses: 2^8 modulo the primitive polynomial x^8+x^4+x^3+x^2+1
 * (0x11D). Log/antilog tables so multiplication is an add.
 */
const EXP = new Uint8Array(512);
const LOG = new Uint8Array(256);
{
    let x = 1;
    for (let i = 0; i < 255; i++) {
        EXP[i] = x;
        LOG[x] = i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11d;
    }
    for (let i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
}

function gfMul(a, b) {
    if (a === 0 || b === 0) return 0;
    return EXP[LOG[a] + LOG[b]];
}

/** The generator polynomial for `n` error-correction codewords. */
function generatorPoly(n) {
    let poly = [1];
    for (let i = 0; i < n; i++) {
        const next = new Array(poly.length + 1).fill(0);
        for (let j = 0; j < poly.length; j++) {
            /* Coefficients run highest degree first, so multiplying by
             * (x + a^i) raises poly[j] one degree into next[j] and scales it
             * into next[j+1]. Swapping those two is the whole multiplication
             * backwards and still yields a plausible-looking polynomial of the
             * right length -- the symbol then differs from a correct one only
             * in its error-correction codewords, which is to say only in the
             * part no eye can check. */
            next[j] ^= poly[j];
            next[j + 1] ^= gfMul(poly[j], EXP[i]);
        }
        poly = next;
    }
    return poly;
}

/** Reed-Solomon remainder: the `n` error-correction codewords for `data`. */
export function ecCodewords(data, n) {
    const gen = generatorPoly(n);
    const rem = new Array(n).fill(0);
    for (let i = 0; i < data.length; i++) {
        const factor = data[i] ^ rem[0];
        rem.shift();
        rem.push(0);
        if (factor !== 0) {
            for (let j = 0; j < n; j++) rem[j] ^= gfMul(gen[j + 1], factor);
        }
    }
    return rem;
}

/* --------------------------------------------------------------- codewords */

/*
 * Text -> the full codeword stream (data followed by error correction).
 *
 * Terminator, bit padding and pad bytes are all here rather than split across
 * the caller, because getting any one of the three wrong produces a symbol that
 * still LOOKS like a QR and simply does not scan.
 */
function buildCodewords(bytes, version) {
    const cap = dataCapacity(version);
    const bits = [];
    const push = (value, n) => {
        for (let i = n - 1; i >= 0; i--) bits.push((value >> i) & 1);
    };

    push(0b0100, 4);          /* byte mode */
    push(bytes.length, 8);    /* character count, 8 bits at versions 1-9 */
    for (const b of bytes) push(b, 8);

    /* Terminator: up to four zero bits, and fewer if the capacity is nearly
     * used — it is "as many as fit", not "four". */
    const capBits = cap * 8;
    for (let i = 0; i < 4 && bits.length < capBits; i++) bits.push(0);
    /* Pad to a whole codeword. */
    while (bits.length % 8 !== 0) bits.push(0);

    const data = [];
    for (let i = 0; i < bits.length; i += 8) {
        let b = 0;
        for (let j = 0; j < 8; j++) b = (b << 1) | bits[i + j];
        data.push(b);
    }
    /* Pad codewords, alternating, until the data capacity is full. These two
     * values are specified constants, not filler of our choosing. */
    const PAD = [0xec, 0x11];
    for (let i = 0; data.length < cap; i++) data.push(PAD[i % 2]);

    return data.concat(ecCodewords(data, VERSIONS_L[version].ec));
}

/* ------------------------------------------------------------- the matrix */

/*
 * `fixed` is the reserved-module mask and it is as important as `grid`.
 *
 * Every function pattern (finders, separators, timing, alignment, the dark
 * module, the format-information strips) must be skipped by the data walk AND
 * left alone by the mask. Tracking "is this module reserved" separately from
 * "is this module dark" is what keeps those two rules from disagreeing — the
 * classic failure being a mask applied over the timing pattern, which produces
 * a symbol no reader can even locate.
 */
function blankMatrix(size) {
    const grid = [];
    const fixed = [];
    for (let y = 0; y < size; y++) {
        grid.push(new Uint8Array(size));
        fixed.push(new Uint8Array(size));
    }
    return { grid, fixed };
}

function placeFinder(m, size, ox, oy) {
    for (let dy = -1; dy <= 7; dy++) {
        for (let dx = -1; dx <= 7; dx++) {
            const x = ox + dx, y = oy + dy;
            if (x < 0 || y < 0 || x >= size || y >= size) continue;
            const inRing = (dx >= 0 && dx <= 6 && (dy === 0 || dy === 6)) ||
                           (dy >= 0 && dy <= 6 && (dx === 0 || dx === 6));
            const inCore = dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4;
            m.grid[y][x] = (inRing || inCore) ? 1 : 0;
            m.fixed[y][x] = 1;
        }
    }
}

function placeAlignment(m, size, centers) {
    for (const cy of centers) {
        for (const cx of centers) {
            /* The three positions that would sit on a finder are skipped. */
            const onFinder = (cx <= 8 && cy <= 8) ||
                             (cx >= size - 9 && cy <= 8) ||
                             (cx <= 8 && cy >= size - 9);
            if (onFinder) continue;
            for (let dy = -2; dy <= 2; dy++) {
                for (let dx = -2; dx <= 2; dx++) {
                    const ring = Math.max(Math.abs(dx), Math.abs(dy));
                    m.grid[cy + dy][cx + dx] = (ring === 1) ? 0 : 1;
                    m.fixed[cy + dy][cx + dx] = 1;
                }
            }
        }
    }
}

function placeTimingAndReserved(m, size) {
    for (let i = 8; i < size - 8; i++) {
        const on = (i % 2 === 0) ? 1 : 0;
        m.grid[6][i] = on; m.fixed[6][i] = 1;
        m.grid[i][6] = on; m.fixed[i][6] = 1;
    }
    /* Format information strips, reserved now and written after masking. */
    for (let i = 0; i <= 8; i++) {
        if (!m.fixed[8][i]) { m.fixed[8][i] = 1; m.grid[8][i] = 0; }
        if (!m.fixed[i][8]) { m.fixed[i][8] = 1; m.grid[i][8] = 0; }
    }
    for (let i = 0; i < 8; i++) {
        m.fixed[8][size - 1 - i] = 1; m.grid[8][size - 1 - i] = 0;
        m.fixed[size - 1 - i][8] = 1; m.grid[size - 1 - i][8] = 0;
    }
    /*
     * THE DARK MODULE IS SET LAST, and the ordering is the whole point.
     *
     * It sits at (size-8, 8), which is one module INSIDE the strip the loop
     * above reserves down column 8 -- so setting it first and reserving after
     * writes it and then zeroes it again. The symbol that results is spec-wrong
     * in exactly one module out of six hundred, and the decoders that tolerate
     * that are the reason it is worth asserting rather than eyeballing:
     * OpenCV read the broken symbol back perfectly, and the structural check in
     * tests/host/test_qr_encode.sh is what caught it.
     */
    m.grid[size - 8][8] = 1;
    m.fixed[size - 8][8] = 1;
}

/*
 * The data walk: two-module-wide columns, right to left, alternating upward and
 * downward, skipping reserved modules — and skipping COLUMN 6 entirely, because
 * the vertical timing pattern lives there and the column pairing is defined on
 * the grid with it removed. Forgetting that shifts every module after it by one
 * and is invisible until a reader fails.
 */
function placeData(m, size, codewords) {
    let bit = 0;
    const total = codewords.length * 8;
    const nextBit = () => {
        if (bit >= total) return 0;
        const b = (codewords[bit >> 3] >> (7 - (bit & 7))) & 1;
        bit++;
        return b;
    };
    let upward = true;
    for (let right = size - 1; right >= 1; right -= 2) {
        if (right === 6) right = 5;
        for (let i = 0; i < size; i++) {
            const y = upward ? size - 1 - i : i;
            for (let c = 0; c < 2; c++) {
                const x = right - c;
                if (m.fixed[y][x]) continue;
                m.grid[y][x] = nextBit();
            }
        }
        upward = !upward;
    }
}

const MASKS = [
    (x, y) => (x + y) % 2 === 0,
    (x, y) => y % 2 === 0,
    (x, y) => x % 3 === 0,
    (x, y) => (x + y) % 3 === 0,
    (x, y) => (Math.floor(y / 2) + Math.floor(x / 3)) % 2 === 0,
    (x, y) => ((x * y) % 2) + ((x * y) % 3) === 0,
    (x, y) => (((x * y) % 2) + ((x * y) % 3)) % 2 === 0,
    (x, y) => (((x + y) % 2) + ((x * y) % 3)) % 2 === 0,
];

function applyMask(m, size, maskIndex) {
    const out = blankMatrix(size);
    for (let y = 0; y < size; y++) {
        for (let x = 0; x < size; x++) {
            out.fixed[y][x] = m.fixed[y][x];
            out.grid[y][x] = m.fixed[y][x]
                ? m.grid[y][x]
                : (m.grid[y][x] ^ (MASKS[maskIndex](x, y) ? 1 : 0));
        }
    }
    return out;
}

/* The four penalty rules, scored exactly as specified. The mask with the lowest
 * total wins; a reader does not care which was chosen (it is in the format
 * bits) but a badly masked symbol reads slowly or not at all. */
function penalty(grid, size) {
    let score = 0;

    /* Rule 1: runs of five or more of the same colour, per row and per column. */
    const runs = (get) => {
        for (let a = 0; a < size; a++) {
            let run = 1;
            for (let b = 1; b < size; b++) {
                if (get(a, b) === get(a, b - 1)) {
                    run++;
                    if (run === 5) score += 3;
                    else if (run > 5) score += 1;
                } else run = 1;
            }
        }
    };
    runs((a, b) => grid[a][b]);
    runs((a, b) => grid[b][a]);

    /* Rule 2: every 2x2 block of one colour. */
    for (let y = 0; y < size - 1; y++) {
        for (let x = 0; x < size - 1; x++) {
            const v = grid[y][x];
            if (grid[y][x + 1] === v && grid[y + 1][x] === v && grid[y + 1][x + 1] === v) score += 3;
        }
    }

    /* Rule 3: the finder-like 1:1:3:1:1 pattern with four light modules on
     * either side, in either orientation. */
    const PAT = [1, 0, 1, 1, 1, 0, 1];
    const matchesAt = (get, a, b) => {
        for (let i = 0; i < 7; i++) if (get(a, b + i) !== PAT[i]) return false;
        let gapBefore = true, gapAfter = true;
        for (let i = 1; i <= 4; i++) {
            const before = b - i, after = b + 6 + i;
            if (before >= 0 && get(a, before) !== 0) gapBefore = false;
            if (after < size && get(a, after) !== 0) gapAfter = false;
        }
        return gapBefore || gapAfter;
    };
    for (const get of [(a, b) => grid[a][b], (a, b) => grid[b][a]]) {
        for (let a = 0; a < size; a++) {
            for (let b = 0; b + 7 <= size; b++) if (matchesAt(get, a, b)) score += 40;
        }
    }

    /* Rule 4: deviation of the dark-module proportion from 50%. */
    let dark = 0;
    for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) dark += grid[y][x];
    const pct = (dark * 100) / (size * size);
    score += Math.floor(Math.abs(pct - 50) / 5) * 10;

    return score;
}

/*
 * Format information: five bits (two for the EC level, three for the mask),
 * extended by a BCH(15,5) remainder and XORed with 0x5412 so an all-zero format
 * is not an all-light strip. Written into both copies — a reader uses whichever
 * it can see, so writing one and not the other yields a symbol that scans from
 * some angles and not others.
 */
function placeFormat(m, size, maskIndex) {
    const ECC_L = 0b01;
    let data = (ECC_L << 3) | maskIndex;
    let rem = data;
    for (let i = 0; i < 10; i++) rem = (rem << 1) ^ (((rem >> 9) & 1) * 0x537);
    const bitsVal = (((data << 10) | rem) ^ 0x5412) & 0x7fff;
    const bit = (i) => (bitsVal >> i) & 1;

    /*
     * ⚠ COLUMN 8 CARRIES THE LOW BITS OF COPY 1 AND ROW 8 THE HIGH ONES, AND
     * COPY 2 IS THE OTHER WAY ROUND. The two copies are not reflections of each
     * other, and writing them as if they were is a transposition that leaves
     * every function pattern in place, every data module correct, and a symbol
     * no reader will touch -- there is nothing to see in the picture, because
     * the fifteen modules that moved are the fifteen a human never reads.
     * `grid[row][col]`, so the row index comes first below; the specification
     * and most reference implementations write these as (x, y).
     */
    /* Copy 1: down column 8 to row 8, then left along row 8. */
    for (let i = 0; i <= 5; i++) m.grid[i][8] = bit(i);
    m.grid[7][8] = bit(6);
    m.grid[8][8] = bit(7);
    m.grid[8][7] = bit(8);
    for (let i = 9; i <= 14; i++) m.grid[8][14 - i] = bit(i);

    /* Copy 2: right along row 8 from the far edge, then up column 8 from the
     * bottom -- the low bits in the ROW here, where copy 1 put them in the
     * column. */
    for (let i = 0; i <= 7; i++) m.grid[8][size - 1 - i] = bit(i);
    for (let i = 8; i <= 14; i++) m.grid[size - 15 + i][8] = bit(i);
}

/*
 * The unmasked symbol: every function pattern, the reservation mask, and the
 * data walked into place. Exported because it is the only seam a test can use
 * to tell the three independent ways this can be wrong apart -- a misplaced
 * function pattern, a wrong reservation, and a wrong walk all show up in the
 * finished symbol as "it does not scan", and the finished symbol is masked, so
 * nothing about it can be read by eye.
 */
export function buildBase(bytes, version) {
    const t = VERSIONS_L[version];
    const size = t.size;
    const m = blankMatrix(size);
    placeFinder(m, size, 0, 0);
    placeFinder(m, size, size - 7, 0);
    placeFinder(m, size, 0, size - 7);
    placeTimingAndReserved(m, size);
    placeAlignment(m, size, t.align);
    placeData(m, size, buildCodewords(bytes, version));
    return m;
}

/**
 * Encode `text` as a QR symbol.
 *
 * @param {string} text  ASCII. Anything outside 0x00-0xFF is rejected rather
 *   than silently truncated: this module has no UTF-8 path, and a mangled URL
 *   that still scans is the worst outcome available.
 * @returns {{size: number, modules: Uint8Array[]}} `modules[y][x]` is 1 for a
 *   dark module. No quiet zone — the caller owns the margin, because on a
 *   128x64 screen the four-module quiet zone is a real fraction of the width
 *   and where it comes from is a layout decision.
 * @throws if the text is empty, non-ASCII, or too long for version 4.
 */
export function encode(text) {
    const s = String(text == null ? "" : text);
    if (!s.length) throw new Error("qr: empty text");
    const bytes = [];
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        if (c > 0xff) throw new Error("qr: non-ASCII input at " + i);
        bytes.push(c);
    }
    const version = chooseVersion(bytes.length);
    if (!version) {
        throw new Error("qr: " + bytes.length + " bytes exceeds version 4 (78); " +
                        "versions 5+ need multi-block error correction, which is not implemented");
    }

    const t = VERSIONS_L[version];
    const size = t.size;
    const base = buildBase(bytes, version);

    let best = null, bestScore = Infinity, bestMask = 0;
    for (let mi = 0; mi < 8; mi++) {
        const cand = applyMask(base, size, mi);
        placeFormat(cand, size, mi);
        const sc = penalty(cand.grid, size);
        if (sc < bestScore) { bestScore = sc; best = cand; bestMask = mi; }
    }

    return { size, modules: best.grid, version, mask: bestMask };
}
