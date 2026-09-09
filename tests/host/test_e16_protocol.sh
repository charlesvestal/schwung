#!/usr/bin/env bash
# Pins the E16 wire format against bytes captured from hardware on 2026-09-09.
# The ENTER vector is the exact message the device acked; if it changes, the
# device stops responding and nothing else in this feature can work.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { pack7, enterMsg, exitMsg, ringMsg, framebufferMsg, packetize }
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

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
