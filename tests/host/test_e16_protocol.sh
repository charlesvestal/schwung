#!/usr/bin/env bash
# Pins the E16 wire format against bytes captured from hardware on 2026-09-09.
# The ENTER vector is the exact message the device acked; if it changes, the
# device stops responding and nothing else in this feature can work.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { pack7, enterMsg, exitMsg, ringMsg, framebufferMsg, packetize }
  from "./src/shared/e16_protocol.mjs";
import { scanlineMsg, rectangleMsg, clearMsg, parseOledUpdateReply, unpack7,
         isAck, ACK_BODY, ACK_BODY_NO_CATEGORY }
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
eq("rectangle header+id (0x08, measured -- not the sheet 0x06)", rect.slice(0, 7), [0xF0,0x00,0x21,0x5B,0x02,0x01,0x08]);
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
const ackRaw = [0x08, 0x00, 2, 3, 8, 2];
const ackAsm = [0x00,0x21,0x5B,0x02,0x01,0x53].concat(pack7(ackRaw));
eq("oled ack parses", parseOledUpdateReply(ackAsm),
   { ok: true, cmd: 0x08, status: 0x00, addr: { x: 2, y: 3, w: 8, h: 2 } });

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

/* BOTH ENTER-ACK FORMS. Captured off the wire 2026-09-24 with the XMOS SysEx
 * tap, against firmware that ships the partial-update opcodes: the reply to
 * our ENTER is F0 00 21 5B 02 01 53 F7 -- NO 0x06. The old form must keep
 * working for devices on older firmware. */
eq("old-firmware ack (06 53) is an ack", isAck(ACK_BODY), true);
eq("new-firmware ack (53, no category) is an ack",
   isAck([0x00,0x21,0x5B,0x02,0x01,0x53]), true);
eq("the captured bytes are exactly ACK_BODY_NO_CATEGORY",
   ACK_BODY_NO_CATEGORY, [0x00,0x21,0x5B,0x02,0x01,0x53]);
/* Same header, same 0x53 -- the payload length is the only thing separating
 * an ENTER ack from an OLED UPDATE ack, in both directions. */
eq("new-firmware ENTER ack is NOT an oled reply",
   parseOledUpdateReply(ACK_BODY_NO_CATEGORY), null);
eq("an OLED UPDATE ack is NOT an ENTER ack", isAck(ackAsm), false);

/* WIRE CAPTURES from the device, 2026-09-24 -- the real replies, verbatim
 * (asm = everything between F0 and F7). These are ground truth; the
 * fixtures above are constructed. */
const cap = (hex) => hex.split(" ").map((h) => parseInt(h, 16));
eq("captured CLEAR ack decodes",
   parseOledUpdateReply(cap("00 21 5b 02 01 53 3c 07 00 7f 7f 7f 7f")),
   { ok: true, cmd: 0x07, status: 0, addr: {} });
eq("captured SCANLINE ack decodes (y=58)",
   parseOledUpdateReply(cap("00 21 5b 02 01 53 38 05 00 3a 7f 7f 7f")),
   { ok: true, cmd: 0x05, status: 0, addr: { y: 58 } });
eq("captured SCANLINE CRC-mismatch NACK decodes",
   parseOledUpdateReply(cap("00 21 5b 02 01 54 38 05 03 14 7f 7f 7f")),
   { ok: false, cmd: 0x05, status: 3, addr: { y: 20 } });
eq("captured RECTANGLE ack decodes (x=8 y=30 24x24)",
   parseOledUpdateReply(cap("00 21 5b 02 01 53 00 08 00 08 1e 18 18")),
   { ok: true, cmd: 0x08, status: 0, addr: { x: 8, y: 30, w: 24, h: 24 } });
/* The exact bytes the device drew from, re-built by our encoder. */
eq("rectangleMsg reproduces the bytes the device ACKed",
   rectangleMsg(8, 30, 24, 24, new Array(72).fill(0xFF)).slice(0, 12),
   cap("f0 00 21 5b 02 01 08 70 08 1e 18 18"));

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
