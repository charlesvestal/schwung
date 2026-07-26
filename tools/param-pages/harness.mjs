/**
 * harness.mjs — headless 128x64 1-bit display for the param-page renderer.
 *
 * Substitutes for a Move when there isn't one. Implements the same draw context
 * the device provides (`fillRect`, `print`, `textWidth`, `clearScreen`) and
 * renders text through the *actual device font atlas* (see font5x7.json /
 * gen_font_table.py), replicating js_display_load_font's auto-trim and
 * charSpacing. Previews are therefore pixel-identical to the OLED, not an
 * approximation — which is the whole point: layout decisions made here hold on
 * hardware.
 *
 * Outputs:
 *   toAscii()   one character per pixel — precise, 64 lines tall
 *   toBlocks()  half-block glyphs, two pixel rows per line — readable in a
 *               terminal at a glance, and what the snapshot tests store
 *   toPng(scale) a real PNG (zlib via node:zlib) for looking at later
 *
 * Node-only. Nothing here ships to the device.
 */

import fs from "node:fs";
import zlib from "node:zlib";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));

export const SCREEN_WIDTH = 128;
export const SCREEN_HEIGHT = 64;

let FONT = null;
function font() {
    if (!FONT) FONT = JSON.parse(fs.readFileSync(path.join(HERE, "font5x7.json"), "utf8"));
    return FONT;
}

/**
 * A 1-bit framebuffer plus the device's drawing verbs.
 * @param {number} w @param {number} h
 */
export function createFramebuffer(w = SCREEN_WIDTH, h = SCREEN_HEIGHT) {
    const px = new Uint8Array(w * h);
    const f = font();

    /* Pixels the renderer tried to draw outside the display. On the device
     * these are silently discarded, so counting them here is the only way to
     * catch a layout that overflows. */
    let clipped = 0;

    const setPixel = (x, y, color) => {
        x |= 0; y |= 0;
        if (x < 0 || y < 0 || x >= w || y >= h) { clipped++; return; }
        px[y * w + x] = color ? 1 : 0;
    };

    const fillRect = (x, y, rw, rh, color) => {
        x |= 0; y |= 0; rw |= 0; rh |= 0;
        for (let yy = y; yy < y + rh; yy++) {
            for (let xx = x; xx < x + rw; xx++) setPixel(xx, yy, color);
        }
    };

    /* Every character the device cannot draw. The atlas is ASCII-only, so a
     * stray "…" or "°" silently renders as nothing on hardware — the kind of
     * bug that is invisible in code review and obvious on the OLED. Tests
     * assert this stays empty across the fleet. */
    const missingGlyphs = new Set();

    const glyph = (ch, sx, sy, color) => {
        const g = f.glyphs[ch];
        if (!g) { missingGlyphs.add(ch); return sx + f.charSpacing; }
        for (let y = 0; y < g.rows.length; y++) {
            const bits = g.rows[y];
            if (!bits) continue;
            for (let x = 0; x < g.w; x++) {
                if (bits & (1 << x)) setPixel(sx + x, sy + y, color);
            }
        }
        return sx + g.w + f.charSpacing;
    };

    const print = (x, y, text, color) => {
        let cursor = x | 0;
        const s = String(text == null ? "" : text);
        for (const ch of s) cursor = glyph(ch, cursor, y | 0, color);
        return cursor;
    };

    const textWidth = (text) => {
        let width = 0;
        const s = String(text == null ? "" : text);
        for (const ch of s) {
            const g = f.glyphs[ch];
            if (!g) missingGlyphs.add(ch);
            width += g ? g.w + f.charSpacing : f.charSpacing;
        }
        return width;
    };

    const clearScreen = () => px.fill(0);

    /* ------------------------------------------------------------ output */

    const toAscii = (on = "#", off = ".") => {
        const lines = [];
        for (let y = 0; y < h; y++) {
            let line = "";
            for (let x = 0; x < w; x++) line += px[y * w + x] ? on : off;
            lines.push(line);
        }
        return lines.join("\n");
    };

    /* Two pixel rows per text row using half blocks: 128x64 becomes 32 lines,
     * which fits a terminal and a diff without losing a pixel. */
    const toBlocks = () => {
        const lines = [];
        for (let y = 0; y < h; y += 2) {
            let line = "";
            for (let x = 0; x < w; x++) {
                const top = px[y * w + x];
                const bot = (y + 1 < h) ? px[(y + 1) * w + x] : 0;
                line += top && bot ? "█" : top ? "▀" : bot ? "▄" : " ";
            }
            lines.push(line.replace(/\s+$/, ""));   /* trailing space is noise in a diff */
        }
        return lines.join("\n");
    };

    const toPng = (scale = 4, invert = false) => {
        const W = w * scale, H = h * scale;
        /* One byte per pixel, greyscale, with the PNG per-scanline filter byte. */
        const raw = Buffer.alloc((W + 1) * H);
        for (let y = 0; y < H; y++) {
            raw[y * (W + 1)] = 0;                       /* filter: none */
            const sy = (y / scale) | 0;
            for (let x = 0; x < W; x++) {
                const on = px[sy * w + ((x / scale) | 0)];
                const lit = invert ? !on : on;
                raw[y * (W + 1) + 1 + x] = lit ? 0xff : 0x00;
            }
        }
        const chunk = (type, data) => {
            const len = Buffer.alloc(4);
            len.writeUInt32BE(data.length);
            const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
            const crc = Buffer.alloc(4);
            crc.writeUInt32BE(crc32(body) >>> 0);
            return Buffer.concat([len, body, crc]);
        };
        const ihdr = Buffer.alloc(13);
        ihdr.writeUInt32BE(W, 0);
        ihdr.writeUInt32BE(H, 4);
        ihdr[8] = 8;    /* bit depth */
        ihdr[9] = 0;    /* colour type: greyscale */
        return Buffer.concat([
            Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
            chunk("IHDR", ihdr),
            chunk("IDAT", zlib.deflateSync(raw, { level: 9 })),
            chunk("IEND", Buffer.alloc(0)),
        ]);
    };

    return {
        width: w, height: h, pixels: px,
        /* the device draw-context surface */
        fillRect, print, textWidth, clearScreen, setPixel,
        /* inspection */
        toAscii, toBlocks, toPng, missingGlyphs,
        clipped: () => clipped,
        countLit: () => px.reduce((a, b) => a + b, 0),
    };
}

/** The object shape the renderer expects as its draw context. */
export function drawContext(fb) {
    return {
        fillRect: fb.fillRect,
        print: fb.print,
        textWidth: fb.textWidth,
    };
}

let CRC_TABLE = null;
function crc32(buf) {
    if (!CRC_TABLE) {
        CRC_TABLE = new Int32Array(256);
        for (let n = 0; n < 256; n++) {
            let c = n;
            for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
            CRC_TABLE[n] = c;
        }
    }
    let c = -1;
    for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
    return c ^ -1;
}
