/*
 * song_mix.mjs — each Move track's mute and solo, from a parsed Song.abl.
 *
 *   tracks[i].mixer.speakerOn   false = muted
 *   tracks[i].mixer.solo-cue    true  = soloed
 *
 * `speakerOn` CHANGES SHAPE when Move stores a preset value beside it — a bare
 * `false` becomes `{"value": false, "presetValue": true}` — so a truthiness
 * test reads the object form as UNMUTED. Both shapes go through `boolOf`.
 *
 * The C twin is src/host/song_abl_mix.h (boot); this one serves the set-change
 * path, where the UI has the file and JSON.parse to hand.
 *
 * Returns null unless all four tracks answered: a partial answer must not
 * become a partial overwrite of the slots' saved state.
 */

function boolOf(v) {
    if (typeof v === "boolean") return v;
    if (v && typeof v === "object" && typeof v.value === "boolean") return v.value;
    return null;
}

export function songMixState(song) {
    const tracks = song && Array.isArray(song.tracks) ? song.tracks : null;
    if (!tracks || tracks.length < 4) return null;
    const muted = [], soloed = [];
    for (let i = 0; i < 4; i++) {
        const mixer = tracks[i] && tracks[i].mixer;
        const on = boolOf(mixer && mixer.speakerOn);
        if (on === null) return null;
        muted.push(on ? 0 : 1);
        soloed.push(boolOf(mixer["solo-cue"]) ? 1 : 0);
    }
    return { muted, soloed };
}

/* The wire form of `slot:move_mix`: "m0 m1 m2 m3 s0 s1 s2 s3". */
export function songMixParamValue(state) {
    return state.muted.concat(state.soloed).join(" ");
}
