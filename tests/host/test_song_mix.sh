#!/usr/bin/env bash
# Slot mute/solo follows Move's tracks at set load: src/shared/song_mix.mjs.
# Fixtures are shaped from real Song.abl files (firmware 2.1.x).
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { songMixState, songMixParamValue } from "./src/shared/song_mix.mjs";
let fails = 0;
const eq = (got, want, what) => {
    if (JSON.stringify(got) !== JSON.stringify(want)) {
        console.log("FAIL:", what, JSON.stringify(got), "want", JSON.stringify(want));
        fails++;
    }
};
const mixer = (speakerOn, solo) => ({ pan: 0, "solo-cue": solo, speakerOn, volume: 0, sends: [] });
const song = (...m) => ({ tracks: m.map(x => ({ name: "", mixer: x, devices: [] })) });

eq(songMixState(song(mixer(false, false), mixer(true, false), mixer(true, false), mixer(true, false))),
   { muted: [1,0,0,0], soloed: [0,0,0,0] }, "track 1 muted (Set 5)");
eq(songMixState(song(mixer(true, true), mixer(true, false), mixer(true, false), mixer(true, false))),
   { muted: [0,0,0,0], soloed: [1,0,0,0] }, "track 1 soloed (Set 3)");
eq(songMixState(song(mixer(true, false), mixer(true, true), mixer(true, true), mixer(true, false))),
   { muted: [0,0,0,0], soloed: [0,1,1,0] }, "two solos kept, not made exclusive");

/* the object form a truthiness test reads as unmuted */
eq(songMixState(song(mixer(true, false), mixer(true, false),
                     mixer({ value: false, presetValue: true }, false), mixer(true, false))),
   { muted: [0,0,1,0], soloed: [0,0,0,0] }, "speakerOn object form is muted");

/* a drum cell muted deeper in track 0 is not the track */
const s = song(mixer(true, false), mixer(true, false), mixer(true, false), mixer(true, false));
s.tracks[0].devices = [{ chains: [{ devices: [{ chains: [{ mixer: mixer({ value: false, presetValue: true }, false) }] }] }] }];
eq(songMixState(s), { muted: [0,0,0,0], soloed: [0,0,0,0] }, "drum-cell mute is not a track mute");

/* no whole answer -> no answer */
eq(songMixState(null), null, "null");
eq(songMixState({}), null, "no tracks");
eq(songMixState(song(mixer(true, false), mixer(true, false), mixer(true, false))), null, "three tracks");
eq(songMixState(song(mixer(true, false), mixer(true, false), mixer(undefined, false), mixer(true, false))),
   null, "a track without speakerOn");

eq(songMixParamValue({ muted: [1,0,0,1], soloed: [0,1,0,0] }), "1 0 0 1 0 1 0 0", "wire form");

if (fails) { console.log(fails + " failure(s)"); process.exit(1); }
console.log("test_song_mix: all passed");
'
