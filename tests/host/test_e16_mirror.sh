#!/usr/bin/env bash
# THE E16 WEB MIRROR: the surface publishes what the device is believed to
# show -- the screen bytes it was sent and the last ring per knob -- through
# io.mirror (host_e16_mirror -> /dev/shm/schwung-e16-live -> display_server
# /stream-e16). One single-quoted node script: no apostrophes in it.
set -euo pipefail
cd "$(dirname "$0")/../.."
node --input-type=module -e '
import { createSurface } from "./src/shared/e16_surface.mjs";
import { unpack7 } from "./src/shared/e16_protocol.mjs";
let fails = 0;
const ok = (c, n) => { if (!c) { console.log("FAIL " + n); fails++; } else console.log("ok   " + n); };
let t = 0; const calls = []; const log = [];
const unpack = (p) => { const out = []; for (let i = 0; i < p.length; i += 4) { const cin = p[i] & 15;
  const n = cin === 5 ? 1 : cin === 6 ? 2 : 3; for (let k = 0; k < n; k++) out.push(p[i + 1 + k]); } return out; };
const s = createSurface({ now: () => t, send: (p) => { log.push(p); return true; },
  chainOf: () => ({ slots: [{ synth: "A" }, {}, {}, {}] }),
  mirror: (frame, rings, active) => calls.push({ frame: frame ? Array.from(frame) : null, rings: rings.slice(), active }) });
s.setEnabled(true);
const tick = (n) => { for (let i = 0; i < n; i++) { t += 25; const b = log.length; s.tick();
  /* the device: ENTER acked, OLED updates acked */
  for (const p of log.slice(b)) { const u = unpack(p); if (u[6] === 6 && u[7] === 0x55) s.feedMidi([0xF0, 0, 0x21, 0x5B, 2, 1, 0x53, 0xF7]);
    if (u[6] === 8) { const a = unpack7(u.slice(7, u.length - 1), 4);
      const body = [8, 0].concat(a); const packed = []; let msb = 0; body.forEach((v, k) => { if (v & 0x80) msb |= 1 << k; });
      packed.push(msb); body.forEach((v) => packed.push(v & 0x7F));
      s.feedMidi([0xF0, 0, 0x21, 0x5B, 2, 1, 0x53].concat(packed, [0xF7])); }
    if (u[6] === 7) s.feedMidi([0xF0, 0, 0x21, 0x5B, 2, 1, 0x53, 0, 7, 0, 0x7F, 0x7F, 0x7F, 0x7F, 0xF7]); } } };
tick(80);
ok(calls.length > 0, "the surface publishes the mirror");
const last = calls[calls.length - 1];
ok(last.active === true, "...as active once the device answers");
ok(last.frame && last.frame.length === 1024, "...with the 1024-byte screen it sent");
ok(last.rings.length === 16 * 6, "...and sixteen rings of six bytes");
const n0 = calls.length;
tick(10);
ok(calls.length === n0, "an unchanged screen is not republished every tick");
tick(45);
ok(calls.length > n0, "...but it is restated about once a second");
console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
