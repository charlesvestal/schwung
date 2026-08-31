#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Help body lines are drawn, never wrapped and never truncated.
#
# drawScrollableText (src/shared/scrollable_text.mjs) calls print(4, y, line),
# and print() (src/schwung_host.c) walks the string one glyph at a time.
# Nothing measures the string first and nothing clips it as a unit: pixels past
# x=127 are dropped by set_pixel with no error anywhere, so an over-long line
# loses its tail silently. That is why this is a test and not a style note.
#
# THE BUDGET IS PIXELS, NOT CHARACTERS. load_font auto-trims every glyph to its
# own inked extent (startX..endX), so the font is fixed-pitch in the atlas and
# PROPORTIONAL on screen: "." advances 3px and "W" advances 6px. A character
# count is therefore only a worst case -- measuring one against a flat limit of
# 20 reports lines broken that render perfectly, which is how this test was
# first written and why the note is here.
#
# Widths come from the FONT table in scripts/generate_font.py, which
# schwung_host.c names as the single source of truth for the atlas, so the test
# needs no build output.

node - <<'NODE'
const fs = require("fs");

const HELP = "src/shared/help_content.json";
const SCROLL = "src/shared/scrollable_text.mjs";
const HOST = "src/schwung_host.c";
const GEN = "scripts/generate_font.py";

const fails = [];
const check = (ok, msg) => { if (!ok) fails.push(msg); };

/* ---- 1. the drawing constants, read from the code that draws ---------- */

const scrollSrc = fs.readFileSync(SCROLL, "utf8");
const hostSrc = fs.readFileSync(HOST, "utf8");
const genSrc = fs.readFileSync(GEN, "utf8");

const screenM = scrollSrc.match(/const SCREEN_WIDTH = (\d+);/);
const printM = scrollSrc.match(/print\(\s*(\d+)\s*,\s*y\s*,\s*lines\[i\]/);
const spacingM = hostSrc.match(/load_font\("font\.png",\s*(\d+)\)/);
const cellM = genSrc.match(/CHAR_W,\s*CHAR_H\s*=\s*(\d+),\s*(\d+)/);

check(!!screenM, "could not read SCREEN_WIDTH from " + SCROLL);
check(!!printM, "could not find the body-line print() call in " + SCROLL);
check(!!spacingM, "could not read the font.png charSpacing from " + HOST);
check(!!cellM, "could not read CHAR_W from " + GEN);
if (fails.length) { fails.forEach(f => console.error("FAIL: " + f)); process.exit(1); }

const SCREEN_WIDTH = Number(screenM[1]);
const ORIGIN_X = Number(printM[1]);
const CHAR_SPACING = Number(spacingM[1]);
const CELL_W = Number(cellM[1]);
const CELL_H = Number(cellM[2]);

/* ---- 2. per-glyph advance, trimmed the way load_font trims ------------- */

/* Entries look like:   'x': [ '..#..', ... ],   with CHAR_H rows of CHAR_W. */
const widths = new Map();
const entryRe = /(?:'((?:\\.|[^'\\])*)'|"((?:\\.|[^"\\])*)")\s*:\s*\[([^\]]*)\]/g;
let m;
while ((m = entryRe.exec(genSrc)) !== null) {
    const rawKey = m[1] !== undefined ? m[1] : m[2];
    const key = rawKey.replace(/\\'/g, "'").replace(/\\"/g, '"').replace(/\\\\/g, "\\");
    if (key.length !== 1) continue;
    const rows = (m[3].match(/'([^']*)'/g) || []).map(s => s.slice(1, -1));
    if (rows.length !== CELL_H) continue;
    if (!rows.every(r => r.length === CELL_W)) continue;
    let first = -1, last = -1;
    for (let x = 0; x < CELL_W; x++) {
        for (let y = 0; y < CELL_H; y++) {
            if (rows[y][x] !== ".") {
                if (first === -1) first = x;
                last = x;
                break;
            }
        }
    }
    /* A blank cell keeps the full cell width -- load_font inserts a
     * space-width entry so the cursor still advances. */
    widths.set(key, first === -1 ? CELL_W : last - first + 1);
}

/* The key regex cannot see the backslash entry (its own key is an escape),
 * so measure that one from its rows directly. */
if (!widths.has("\\")) {
    const bs = genSrc.match(/'\\\\'\s*:\s*\[([^\]]*)\]/);
    if (bs) {
        const rows = (bs[1].match(/'([^']*)'/g) || []).map(s => s.slice(1, -1));
        if (rows.length === CELL_H && rows.every(r => r.length === CELL_W)) {
            let first = -1, last = -1;
            for (let x = 0; x < CELL_W; x++)
                for (let y = 0; y < CELL_H; y++)
                    if (rows[y][x] !== ".") { if (first === -1) first = x; last = x; break; }
            widths.set("\\", first === -1 ? CELL_W : last - first + 1);
        }
    }
}

check(widths.size > 90, "only parsed " + widths.size + " glyphs from " + GEN);
check(widths.get("W") === CELL_W, "expected W to be a full-cell glyph");
if (fails.length) { fails.forEach(f => console.error("FAIL: " + f)); process.exit(1); }

/* Rightmost inked pixel of a line, mirroring print()/glyph() exactly. */
function rightEdge(line) {
    let x = ORIGIN_X, last = ORIGIN_X - 1;
    for (const ch of line) {
        const w = widths.get(ch);
        if (w === undefined) { x += CHAR_SPACING; continue; }  /* glyph() miss */
        last = x + w - 1;
        x += w + CHAR_SPACING;
    }
    return last;
}

/* ---- 3. every help line must fit, and be drawable at all -------------- */

const help = JSON.parse(fs.readFileSync(HELP, "utf8"));
check(Array.isArray(help.sections) && help.sections.length > 0, HELP + " has no sections");

let lineCount = 0, widest = 0;
function walk(node, trail) {
    const here = trail + "/" + (node.title || "?");
    const lines = node.lines || [];
    for (const line of lines) {
        lineCount++;
        const edge = rightEdge(line);
        if (edge > widest) widest = edge;
        check(edge <= SCREEN_WIDTH - 1,
              here + ": line runs to x=" + edge + ", screen ends at " +
              (SCREEN_WIDTH - 1) + " -- " + JSON.stringify(line));
        for (const ch of line) {
            check(widths.has(ch),
                  here + ": " + JSON.stringify(ch) + " has no glyph in the font " +
                  "and draws as a " + CHAR_SPACING + "px gap -- " + JSON.stringify(line));
        }
    }
    check(lines.length > 0 || (node.children || []).length > 0,
          here + ": leaf has neither lines nor children");
    for (const kid of node.children || []) walk(kid, here);
}
for (const section of help.sections) walk(section, "");

check(lineCount > 100, "only " + lineCount + " help lines found -- did the file shrink?");

if (fails.length) { fails.forEach(f => console.error("FAIL: " + f)); process.exit(1); }
console.log("PASS: " + lineCount + " help lines, widest right edge x=" + widest +
            " of " + (SCREEN_WIDTH - 1) + " (origin x=" + ORIGIN_X +
            ", " + widths.size + " glyphs, spacing " + CHAR_SPACING + "px)");
NODE
