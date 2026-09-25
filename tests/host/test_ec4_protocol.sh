#!/usr/bin/env bash
# Pins src/shared/ec4_protocol.mjs against the two references it was
# reconstructed from. Every expected vector below is copied from one of them,
# not derived from our own builder:
#   FF  = Faderfox_Universal_2 (Faderfox's Ableton script): consts.py
#         HIDE_TOTAL_DISPLAY / CLEAR_MAIN_DISPLAY, faderfox_display_element.py
#         get_message_header, faderfox_parameter_display.py get_display_msg,
#         FaderfoxSurface.py SYSEX_REQUEST_SETUP_REQUEST and its reply matcher
#   DBM = DrivenByMoss EC4Display.java / EC4ControlSurface.java
# All of them have since been confirmed on hardware (firmware 2.00; see
# docs/EC4_SURFACE.md). A correction from a device capture should say so here.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import * as p from "./src/shared/ec4_protocol.mjs";

let fails = 0;
const eq = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { console.log("FAIL " + name + "\n  got  " + g + "\n  want " + w); fails++; }
  else console.log("ok   " + name);
};
const H = [0xF0, 0, 0, 0, 0x4E, 0x2C, 0x1B];
const rep = (n, triple) => Array.from({ length: n }, () => triple).flat();

// FF consts.HIDE_TOTAL_DISPLAY
eq("hide overlay (FF)", p.hideTotalMsg(), [...H, 0x4E, 0x22, 0x15, 0xF7]);
// DBM setTotalDisplayVisible(true)
eq("show overlay (DBM)", p.showTotalMsg(), [...H, 0x4E, 0x22, 0x14, 0xF7]);
// FF consts.CLEAR_MAIN_DISPLAY: 64 x "-" from offset 0 on page 0x10
eq("names cleared to ---- (FF)",
   p.textMsg(p.PAGE_NAMES, [{ offset: 0, text: "-".repeat(64) }]),
   [...H, 0x4E, 0x22, 0x10, 0x4A, 0x20, 0x10, ...rep(64, [0x4D, 0x22, 0x1D]), 0xF7]);
// FF get_message_header(offset): charoffset = offset * 4
const cell = (n, text) => p.textMsg(p.PAGE_NAMES, [{ offset: n * 4, text }]);
eq("cell 5 header (FF)", cell(5, "AB  ").slice(0, 13),
   [...H, 0x4E, 0x22, 0x10, 0x4A, 0x21, 0x14]);
eq("cell 15 header (FF)", cell(15, "").slice(10, 13), [0x4A, 0x23, 0x1C]);
eq("cell text", cell(0, "AB  ").slice(13, 25),
   [0x4D, 0x24, 0x11, 0x4D, 0x24, 0x12, 0x4D, 0x22, 0x10, 0x4D, 0x22, 0x10]);
// FF get_display_msg(text, 0): page 0x13, text, then 4E 22 14 in the same message
const overlay = (text, opts) => p.textMsg(p.PAGE_TOTAL, [{ offset: 0, text: text.padEnd(80) }], opts);
const tot = overlay("HI", { show: true });
eq("overlay prefix (FF)", tot.slice(0, 13), [...H, 0x4E, 0x22, 0x13, 0x4A, 0x20, 0x10]);
eq("overlay first chars", tot.slice(13, 19), [0x4D, 0x24, 0x18, 0x4D, 0x24, 0x19]);
eq("overlay 80 chars then show (FF)", tot.length, 7 + 3 + 3 + 80 * 3 + 3 + 1);
eq("overlay trailer (FF)", tot.slice(-4), [0x4E, 0x22, 0x14, 0xF7]);
eq("overlay without show", overlay("HI").slice(-4, -1), [0x4D, 0x22, 0x10]);
// DBM EC4Display.writeLine: several offset runs in one message
eq("two runs in one message (DBM)",
   p.textMsg(3, [{ offset: 21, text: "A" }, { offset: 79, text: "B" }]),
   [...H, 0x4E, 0x22, 0x13, 0x4A, 0x21, 0x15, 0x4D, 0x24, 0x11,
    0x4A, 0x24, 0x1F, 0x4D, 0x24, 0x12, 0xF7]);
// FF SYSEX_REQUEST_SETUP_REQUEST, DBM requestDeviceInfo -- identical, no device id
eq("setup/group request (FF, DBM)", p.queryMsg(),
   [0xF0, 0x00, 0x00, 0x00, 0x4E, 0x20, 0x10, 0xF7]);

// FF _is_ec4_sysex_setup_response: 14 bytes, [7:9]=4E 28, [10:12]=4E 24;
// setup = [9] & 0x0F, group = [12] & 0x0F. DBM: value - 0x10.
eq("setup/group reply",
   p.parse([...H, 0x4E, 0x28, 0x1C, 0x4E, 0x24, 0x12, 0xF7]),
   [{ type: "setup", setup: 12 }, { type: "group", group: 2 }]);
// FF EC4_SYSEX_BUTTON_IDENTIFIER + EC4_SYSEX_SHIFT_BUTTON, then the value;
// FaderfoxSysexButtonElement: pressed iff value == 0x11
eq("shift press (FF)", p.parse([...H, 0x4E, 0x26, 0x11, 0x4E, 0x2E, 0x11, 0xF7]),
   [{ type: "key", key: "shift", pressed: true }]);
eq("shift release (FF)", p.parse([...H, 0x4E, 0x26, 0x11, 0x4E, 0x2E, 0x10, 0xF7]),
   [{ type: "key", key: "shift", pressed: false }]);
eq("user key 3 (FF 26 14)", p.parse([...H, 0x4E, 0x26, 0x14, 0x4E, 0x2E, 0x11, 0xF7]),
   [{ type: "key", key: "user3", pressed: true }]);
// FF get_ec4_bp_sysex_button(i) = 2A 1i
eq("shift + push 5 (FF 2A 15)", p.parse([...H, 0x4E, 0x2A, 0x15, 0x4E, 0x2E, 0x11, 0xF7]),
   [{ type: "key", key: "push5", pressed: true }]);
eq("unknown command kept, not dropped",
   p.parse([...H, 0x4E, 0x2F, 0x13, 0xF7]),
   [{ type: "unknown", cmd: 0x4E, func: 0x2F, value: 0x13 }]);
eq("not an EC4 (the E16 ENTER ack)", p.parse([0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x53, 0xF7]), null);
eq("a truncated message is not parsed", p.parse([...H, 0x4E, 0x28]), null);

// consts.py CHARS
eq("ASCII letters/digits", ["A", "z", "0", "-", " "].map(p.charCode), [0x41, 0x7A, 0x30, 0x2D, 0x20]);
eq("underscore is not ASCII on the EC4", p.charCode("_"), 0xC4);
eq("umlaut", p.charCode("ä"), 0x7B);
eq("unknown -> 0x1F (FF)", p.charCode("$"), 0x1F);

// the frame-atomic budget: <= 12 packets is placed whole (ui_midi_out_carry.h)
eq("one cell fits one SPI frame (9 packets)", p.packetize(cell(3, "CUTO")).length / 4, 9);
eq("all 64 names in one message do not (69)", p.packetize(cell(0, " ".repeat(64))).length / 4, 69);
eq("packetize tail CIN", p.packetize([0xF0, 1, 2, 3, 0xF7]).slice(4), [0x06, 3, 0xF7, 0]);

// ---- the Schwung setup as one single-setup download (download type 2) ----
// Its bytes for setup 12 matched the EC4s own "Send current setup" capture
// exactly (firmware 2.00); here the structure is pinned so it cannot drift.
{
  const m = p.schwungSetupMsg(11, 0);
  eq("single setup: 14294 bytes, as the EC4 sends one", m.length, 14294);
  eq("single setup: header -- EC4, download type 2 (one setup), app 2.0",
     m.slice(0, 16), [0xF0, 0, 0, 0, 0x41, 0x20, 0x1B, 0x42, 0x20, 0x12, 0x43, 0x20, 0x12, 0x44, 0x20, 0x10]);
  eq("single setup: trailer -- download stop, EC4", m.slice(-4), [0x4F, 0x20, 0x1B, 0xF7]);
  // walk the pages: address, 64 data bytes, a CRC that sums them, 30 zero bytes
  const walk = (m) => { const pages = []; let i = 16, crcOk = true;
  while (m[i] === 0x49) {
    const addr = (((m[i + 1] & 15) << 4 | (m[i + 2] & 15)) << 8) | ((m[i + 4] & 15) << 4 | (m[i + 5] & 15));
    i += 6; const data = [];
    for (let k = 0; k < 64; k++) { data.push((m[i + 1] & 15) << 4 | (m[i + 2] & 15)); i += 3; }
    const crc = (((m[i + 1] & 15) << 4 | (m[i + 2] & 15)) << 8) | ((m[i + 4] & 15) << 4 | (m[i + 5] & 15));
    if (crc !== (data.reduce((a, b) => a + b, 0) & 0xFFFF)) crcOk = false;
    i += 6 + 30; pages.push({ addr, data });
  } return { pages, crcOk }; };
  const { pages, crcOk } = walk(m);
  eq("single setup: 61 pages", pages.length, 61);
  eq("single setup: every page CRC is the sum of its data", crcOk, true);
  const s = 11, a = pages.map((q) => q.addr);
  eq("single setup: push buttons 1, group names, encoders, push buttons 2 at the setups addresses",
     [a[0], a[4], a[5], a[52], a[53], a[60]],
     [0x0B00 + s * 256, 0x1C00 + s * 64, 0x2000 + s * 3072, 0x2000 + s * 3072 + 47 * 64, 0xE000 + s * 512, 0xE000 + s * 512 + 7 * 64]);
  const enc = pages[5].data;   // group 1: bytes 0..63 of 192
  eq("single setup: encoders are CCR1 on channel 1, CC 1-16",
     [enc.slice(0, 16), enc.slice(16, 32)], [Array(16).fill(0x00), Array.from({ length: 16 }, (_, k) => k + 1)]);
  eq("single setup: group names G01..", String.fromCharCode(...pages[4].data.slice(0, 8)), "G01 G02 ");
  eq("single setup: encoder names are ---- (host-writable)",
     String.fromCharCode(...pages[7].data.slice(0, 8)), "--------");
  const ch6 = walk(p.schwungSetupMsg(0, 5)).pages;   // group 1 is pages 5 (bytes 0-63) and 6 (64-127)
  eq("single setup: the channel is the low nibble of type and push type (channel 6)",
     [ch6[5].data[0], ch6[6].data[112 - 64]], [0x05, 0x15]);
}

if (fails) { console.log(fails + " failure(s)"); process.exit(1); }
'
echo "PASS: test_ec4_protocol"
