#!/usr/bin/env bash
# Pins the E16 wire format against bytes captured from hardware on 2026-09-09.
# The ENTER vector is the exact message the device acked; if it changes, the
# device stops responding and nothing else in this feature can work.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { pack7, enterMsg, exitMsg, ringMsg, framebufferMsg, packetize }
  from "./src/shared/e16_protocol.mjs";
import { scanlineMsg, rectangleMsg, clearMsg, parseOledUpdateReply, unpack7 }
  from "./src/shared/e16_protocol.mjs";

let fails = 0;
const eq = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { console.log("FAIL " + name + "\n  got  " + g + "\n  want " + w); fails++; }
  else console.log("ok   " + name);
};

eq("pack7 high bits",
   pack7([0xFF,0x00,0x80,0x7F,0x01,0xFE,0x55,0xAA]),
   [0x25,0x7F,0x00,0x00,0x7F,0x01,0x7E,0x55,0x01,0x2A]);

eq("pack7 empty", pack7([]), []);

eq("enter bytes", enterMsg(),
   [0xF0,0x00,0x21,0x5B,0x02,0x01,0x06,0x55,0xF7]);
eq("exit bytes", exitMsg(),
   [0xF0,0x00,0x21,0x5B,0x02,0x01,0x06,0x00,0xF7]);

const r = ringMsg([{ enc: 0, r: 0, g: 60, b: 0, amount: 16383, bipolar: 0 }]);
eq("ring length", r.length, 1 + 5 + 2 + 8 + 1);
// Payload starts at index 9: F0, five header bytes, two id bytes, then the
// pack7 group byte. Blue precedes the amount -- pinned against the Max patch
// chunk `0 3 0 12 0 $1 0 0` from the lines thread, which is the only public
// ground truth for the field order.
eq("ring chunk order", r.slice(9, 16), [0, 0, 60, 0, 0x7F, 0x7F, 0]);
eq("ring amount split", [r[13], r[14]], [0x7F, 0x7F]);

const fb = framebufferMsg(new Uint8Array(1024));
eq("framebuffer packed length", fb.length, 1 + 5 + 2 + 1171 + 1);

const p = packetize(enterMsg());
eq("packet count", p.length / 4, 3);
eq("first cin", p[0], 0x04);
eq("last cin", p[8], 0x07);

/* SCANLINE: header, id 0x05 (no 0x06 prefix -- see OLED_SUBCOMMAND_HAS_CATEGORY_PREFIX),
 * then pack7(17 raw bytes: y + 16 row bytes). */
const scan = scanlineMsg(3, new Array(16).fill(0));
eq("scanline header+id", scan.slice(0, 7), [0xF0,0x00,0x21,0x5B,0x02,0x01,0x05]);
eq("scanline length", scan.length, 1 + 5 + 1 + pack7(new Array(17).fill(0)).length + 1);

/* RECTANGLE: x=2 y=3 w=8 h=2 -> ceil(8/8)*2 = 2 payload bytes. */
const rect = rectangleMsg(2, 3, 8, 2, [0xFF, 0x00]);
eq("rectangle header+id", rect.slice(0, 7), [0xF0,0x00,0x21,0x5B,0x02,0x01,0x06]);
eq("rectangle length", rect.length, 1 + 5 + 1 + pack7([2,3,8,2,0xFF,0x00]).length + 1);
let threw = false;
try { rectangleMsg(2, 3, 8, 2, [0x00]); } catch (e) { threw = true; }
eq("rectangle wrong payload length throws", threw, true);

/* FULL-EXTENT WIDTH AND HEIGHT ARE ORDINARY VALUES, NOT EDGE CASES: w=128 is
 * a full-width multi-row change (e.g. a status bar), h=64 is a full-height
 * single-column change (e.g. a cursor line). Both are one bit past what
 * 0x7F/0x3F can hold -- decode the wire bytes back out and confirm neither
 * silently becomes 0. */
const wBits = new Array(Math.ceil(128 / 8) * 1).fill(0);
const rectFullWidth = rectangleMsg(0, 0, 128, 1, wBits);
const rawFullWidth = unpack7(rectFullWidth.slice(7, rectFullWidth.length - 1), 4 + wBits.length);
eq("rectangle width 128 survives encoding, not truncated to 0", rawFullWidth[2], 128);

const hBits = new Array(Math.ceil(1 / 8) * 64).fill(0);
const rectFullHeight = rectangleMsg(0, 0, 1, 64, hBits);
const rawFullHeight = unpack7(rectFullHeight.slice(7, rectFullHeight.length - 1), 4 + hBits.length);
eq("rectangle height 64 survives encoding, not truncated to 0", rawFullHeight[3], 64);

eq("clear bytes", clearMsg(), [0xF0,0x00,0x21,0x5B,0x02,0x01,0x07,0xF7]);

/* pack7/unpack7 round-trip, arbitrary length including a short last group. */
for (const raw of [[], [1], [0xFF,0x00,0x80,0x7F,0x01,0xFE,0x55,0xAA], new Array(20).fill(0x81)]) {
  eq("pack7/unpack7 round-trip len=" + raw.length, unpack7(pack7(raw), raw.length), raw);
}

/* An OLED UPDATE ACK for a RECTANGLE at (2,3,8,2): cmd=0x06, status=0x00,
 * addr = x,y,w,h. */
const ackRaw = [0x06, 0x00, 2, 3, 8, 2];
const ackAsm = [0x00,0x21,0x5B,0x02,0x01,0x53].concat(pack7(ackRaw));
eq("oled ack parses", parseOledUpdateReply(ackAsm),
   { ok: true, cmd: 0x06, status: 0x00, addr: { x: 2, y: 3, w: 8, h: 2 } });

/* A NACK for a SCANLINE at y=3 with a CRC mismatch (status 0x03); unused
 * address bytes are 0xFF per spec and must decode to null, not 255. */
const nackRaw = [0x05, 0x03, 3, 0xFF, 0xFF, 0xFF];
const nackAsm = [0x00,0x21,0x5B,0x02,0x01,0x54].concat(pack7(nackRaw));
eq("oled nack parses", parseOledUpdateReply(nackAsm),
   { ok: false, cmd: 0x05, status: 0x03, addr: { y: 3 } });

/* The unrelated REMOTE MODE ENTERED ACK (same 0x53, WITH the 0x06 prefix,
 * no payload) must not parse as an OLED reply. */
eq("remote-mode ack is not an oled reply",
   parseOledUpdateReply([0x00,0x21,0x5B,0x02,0x01,0x06,0x53]), null);
eq("garbage is not an oled reply", parseOledUpdateReply([1,2,3]), null);

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
