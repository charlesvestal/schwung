#!/usr/bin/env node
/**
 * demo_score.mjs -- the score every module is previewed with.
 *
 *   node tools/probe/scores/demo_score.mjs out.json [--transpose N] [--seconds 30]
 *
 * WHY THIS AND NOT A PIECE OF MUSIC. The first version used Bach's WTK I
 * Prelude I: public domain, arpeggiated, harmonically clean. It was the wrong
 * choice for one reason -- it never stops. A note every 0.28 s for 30 s gives
 * a long-release patch no room, and the fix attempted first (spreading the
 * phrase to suit each preset's measured release) made every preset play at a
 * DIFFERENT TEMPO, which destroys the one thing a shared score is for: two
 * modules being comparable.
 *
 * So the space is written IN, and the tempo is the same for everything:
 *
 *     0.0- 3.6s   ARP      eighth notes, one voice   -- attack, transient, timbre
 *     3.6- 4.4s   rest                               -- the arp's tail, alone
 *     4.4-12.8s   CHORDS   three, held, with gaps    -- voicing, width, and
 *                                                       where a knob is swept
 *    12.8-14.0s   rest
 *    14.0-21.0s   MELODY   mixed rhythm, one voice   -- how it plays a line
 *    21.0-23.0s   final chord, held
 *    23.0-30.0s   silence                            -- the tail, undisturbed
 *
 * The arp used to run to 7.2 s and the chords to 8.4 s, which is a long wait
 * for the part a listener is there for -- and, on an FX, a long wait before
 * anything moves.
 *
 * Every section ends in a rest, so a 3.4 s release is heard as a release
 * rather than smeared into the next phrase, and a 0.05 s one simply sounds
 * dry. No per-preset tempo, no stretching.
 *
 * A minor - F - G: no third in the bass, works under most patches, and says
 * nothing about genre.
 */

const BPM = 100;
const BEAT = 60 / BPM;                 // 0.6 s
const SR = 44100;

/* MIDI notes relative to A3 = 57. The phrase sits in the middle of the
 * keyboard; the bass rule transposes the whole thing down an octave. */
const Am = [57, 60, 64, 69];
const F  = [53, 57, 60, 65];
const G  = [55, 59, 62, 67];

function build() {
    const ev = [];                      // {t, dur, note, vel}
    const add = (t, dur, note, vel = 96) => ev.push({ t, dur, note, vel });

    // ---- ARP: up and down over the triad, eighth notes, one voice.
    const arp = [57, 60, 64, 69, 72, 69, 64, 60];
    for (let i = 0; i < 12; i++) {
        const bar = Math.floor(i / 4) % 3;
        const chord = [Am, F, G][bar];
        const step = arp[i % 8] - 57 + chord[0];
        add(i * BEAT * 0.5, BEAT * 0.45, step, 92 + (i % 8 === 0 ? 20 : 0));
    }

    // ---- CHORDS: held, with a real gap after each.
    let t = 4.4;
    for (const ch of [Am, F, G]) {
        for (const n of ch) add(t, BEAT * 3, n, 88);
        t += BEAT * 4.667;              // ~2.8 s: 1.8 s sounding, 1.0 s of air
    }

    // ---- MELODY: a line with breath in it.
    const mel = [[0, 2, 69], [2, 1, 72], [3, 1, 71], [4, 2, 69], [6, 2, 67],
                 [9, 2, 65], [11, 1, 64], [12, 3, 62], [16, 4, 60]];
    for (const [beat, len, note] of mel) add(14.0 + beat * BEAT * 0.5, BEAT * 0.5 * len * 0.9, note, 100);

    // ---- Final chord, then silence for the tail.
    for (const n of Am) add(21.0, BEAT * 3, n, 84);
    return ev;
}

export function demoScore({ transpose = 0, seconds = 30, sampleRate = SR } = {}) {
    const events = [];
    for (const e of build()) {
        const note = Math.max(0, Math.min(127, e.note + transpose));
        events.push([Math.round(e.t * sampleRate), 0x90, note, e.vel]);
        events.push([Math.round((e.t + e.dur) * sampleRate), 0x80, note, 0]);
    }
    events.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
    return { events, frames: Math.round(seconds * sampleRate) };
}

const isMain = process.argv[1] && process.argv[1].endsWith("demo_score.mjs");
if (isMain) {
    const fs = await import("node:fs");
    const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? Number(process.argv[i + 1]) : d; };
    const out = process.argv[2];
    if (!out) { console.error("usage: demo_score.mjs out.json [--transpose N] [--seconds 30]"); process.exit(2); }
    const s = demoScore({ transpose: arg("--transpose", 0), seconds: arg("--seconds", 30) });
    fs.writeFileSync(out, JSON.stringify(s));
    const on = s.events.filter((e) => e[1] === 0x90);
    console.error(`${s.events.length} events (${on.length} notes), ${(s.frames / SR).toFixed(1)}s, ` +
                  `pitch ${Math.min(...on.map((e) => e[2]))}-${Math.max(...on.map((e) => e[2]))}`);
}
