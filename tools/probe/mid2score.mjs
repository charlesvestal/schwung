#!/usr/bin/env node
/**
 * mid2score.mjs -- Standard MIDI File -> the flat, sample-timed score the
 * probe replays.
 *
 *   node tools/probe/mid2score.mjs in.mid out.json --seconds 27 [--transpose N]
 *
 * Output: {"events": [[sample, status, d1, d2], ...], "frames": N}
 *
 * Deliberately minimal -- note on/off only, all tracks merged, tempo honoured.
 * The probe scans this fixed shape rather than parsing arbitrary JSON, so the
 * format is a contract between these two files and nothing else.
 *
 * TWO THINGS THAT LOOK LIKE DETAILS AND ARE NOT:
 *
 *   - `frames` is LONGER than `seconds`, because a reverb or a long release
 *     is still sounding when the last note ends. Cutting the file at the last
 *     note-off is what makes an effect preview sound like it does nothing.
 *   - Every sounding note gets a note-off inside the window. A held note with
 *     no off drones through the tail and turns the ending into mush.
 */

import fs from "node:fs";

function readVar(b, i) {
  let v = 0;
  for (;;) {
    const c = b[i++];
    v = (v << 7) | (c & 0x7f);
    if (!(c & 0x80)) return [v, i];
  }
}

export function parseMidi(bytes, seconds, { sampleRate = 44100, transpose = 0, tailSeconds = 3 } = {}) {
  if (bytes.slice(0, 4).toString("ascii") !== "MThd") throw new Error("not a MIDI file");
  const ntrk = bytes.readUInt16BE(10);
  const div = bytes.readUInt16BE(12);
  if (div & 0x8000) throw new Error("SMPTE timing is not supported");

  let i = 14;
  const tracks = [];
  for (let t = 0; t < ntrk; t++) {
    if (bytes.slice(i, i + 4).toString("ascii") !== "MTrk") throw new Error("bad track header");
    const len = bytes.readUInt32BE(i + 4);
    tracks.push(bytes.slice(i + 8, i + 8 + len));
    i += 8 + len;
  }

  const raw = [];
  const tempos = [[0, 500000]];               // tick -> usec per quarter note
  for (const t of tracks) {
    let p = 0, tick = 0, running = null;
    while (p < t.length) {
      let d; [d, p] = readVar(t, p); tick += d;
      let st = t[p];
      if (st & 0x80) { running = st; p++; } else st = running;
      if (st === 0xff) {
        const meta = t[p++];
        let len; [len, p] = readVar(t, p);
        if (meta === 0x51) tempos.push([tick, (t[p] << 16) | (t[p + 1] << 8) | t[p + 2]]);
        p += len;
      } else if (st === 0xf0 || st === 0xf7) {
        let len; [len, p] = readVar(t, p); p += len;
      } else {
        const hi = st & 0xf0;
        const nbytes = hi === 0xc0 || hi === 0xd0 ? 1 : 2;
        const d1 = t[p], d2 = nbytes === 2 ? t[p + 1] : 0;
        p += nbytes;
        if (hi === 0x80 || hi === 0x90) raw.push([tick, hi, d1, d2]);
      }
    }
  }
  tempos.sort((a, b) => a[0] - b[0]);
  raw.sort((a, b) => a[0] - b[0]);

  const tickToSeconds = (tk) => {
    let s = 0, prev = 0, upq = tempos[0][1];
    for (const [ttk, tup] of tempos.slice(1)) {
      if (ttk >= tk) break;
      s += ((ttk - prev) / div) * (upq / 1e6);
      prev = ttk; upq = tup;
    }
    return s + ((tk - prev) / div) * (upq / 1e6);
  };

  const events = [];
  for (const [tk, hi, d1, d2] of raw) {
    const s = tickToSeconds(tk);
    if (s > seconds) break;
    // A note-on with velocity 0 IS a note-off; some files use only that form.
    const status = hi === 0x90 && d2 > 0 ? 0x90 : 0x80;
    const note = Math.max(0, Math.min(127, d1 + transpose));
    events.push([Math.round(s * sampleRate), status, note, status === 0x90 ? d2 : 0]);
  }

  // Close anything still held, or it drones through the tail.
  const lastOn = new Map(), lastOff = new Map();
  for (const [smp, st, n] of events) (st === 0x90 ? lastOn : lastOff).set(n, smp);
  for (const [n, on] of lastOn) {
    if (!lastOff.has(n) || lastOff.get(n) < on) events.push([Math.round(seconds * sampleRate), 0x80, n, 0]);
  }
  events.sort((a, b) => a[0] - b[0] || a[1] - b[1]);

  return { events, frames: Math.round((seconds + tailSeconds) * sampleRate) };
}

const isMain = process.argv[1] && import.meta.url.endsWith(process.argv[1].split("/").pop());
if (isMain) {
  const [src, dst] = process.argv.slice(2);
  const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? Number(process.argv[i + 1]) : d; };
  if (!src || !dst) { console.error("usage: mid2score.mjs in.mid out.json --seconds 27 [--transpose N]"); process.exit(2); }
  const score = parseMidi(fs.readFileSync(src), arg("--seconds", 27), { transpose: arg("--transpose", 0) });
  fs.writeFileSync(dst, JSON.stringify(score));
  const on = score.events.filter((e) => e[1] === 0x90);
  console.error(`${score.events.length} events (${on.length} notes), ${(score.frames / 44100).toFixed(1)}s, ` +
                `pitch ${Math.min(...on.map((e) => e[2]))}-${Math.max(...on.map((e) => e[2]))}`);
}
