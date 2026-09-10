#!/usr/bin/env bash
# SSD1306 page/column order is the one thing here that cannot be eyeballed on
# a device: a transposed buffer still draws SOMETHING, so it reads as a
# rendering bug rather than a format bug. Pin the bit positions.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { createCanvas } from "./src/shared/e16_canvas.mjs";
let fails = 0;
const eq = (n, g, w) => { if (g !== w) { console.log("FAIL " + n + " got " + g + " want " + w); fails++; } else console.log("ok   " + n); };

const c = createCanvas();
eq("buffer size", c.toBuffer().length, 1024);

c.fillRect(0, 0, 1, 1, 1);
eq("(0,0) -> byte 0 bit 0", c.toBuffer()[0], 0x01);

c.clear();
c.fillRect(0, 7, 1, 1, 1);
eq("(0,7) -> byte 0 bit 7", c.toBuffer()[0], 0x80);

c.clear();
c.fillRect(0, 8, 1, 1, 1);
eq("(0,8) -> byte 128 bit 0", c.toBuffer()[128], 0x01);

c.clear();
c.fillRect(127, 63, 1, 1, 1);
eq("(127,63) -> last byte bit 7", c.toBuffer()[1023], 0x80);

c.clear();
c.fillRect(200, 200, 5, 5, 1);
eq("clipped, no throw", c.toBuffer().reduce((a, b) => a + b, 0), 0);

c.clear();
c.print(0, 0, "Hi", 1);
eq("print marks pixels", c.toBuffer().reduce((a, b) => a + b, 0) > 0, true);
eq("textWidth is positive", c.textWidth("Hi") > 0, true);

/* font4x5 has NO LOWERCASE -- its CHARS run is uppercase, digits and
 * punctuation. print("cutoff") drew literally nothing while fontWidth4x5 still
 * returned a width for it, so layouts reserved space for glyphs that never
 * appeared. On hardware the E16 showed its header bars and a grid of identical
 * value readouts with every parameter name missing, which reads as a corrupted
 * framebuffer rather than as absent text.
 *
 * "Hi" is why the original assertion passed: the capital H inks. A lowercase-
 * only string is the case that mattered. */
const inkOf = (s) => { const k = createCanvas(); k.clear(); k.print(0, 0, s, 1);
                       return k.toBuffer().reduce((a, b) => a + b, 0); };
eq("lowercase draws", inkOf("cutoff") > 0, true);
eq("lowercase matches uppercase", inkOf("cutoff"), inkOf("CUTOFF"));

/* The width lie is half the bug: a caller that upper-cases for one and not the
 * other reintroduces it. */
const c2 = createCanvas();
eq("textWidth agrees with print", c2.textWidth("cutoff"), c2.textWidth("CUTOFF"));
eq("textWidth grows with text", c.textWidth("HHHH") > c.textWidth("H"), true);

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
