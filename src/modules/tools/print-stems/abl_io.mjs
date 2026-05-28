/*
 * abl_io.mjs — Song.abl parsing/serialization for Print Stems.
 *
 * Pure functions (no host I/O), mirroring the Python reference + tests in
 * tests/print_stems/abl_io.py so the two stay byte-compatible. ui.js does the
 * file I/O (active_set.txt, host_read_file) and calls these.
 *
 * Phase 3 ships parseClipGrid (clip-layout extraction). Phase 6 adds
 * buildStemsSongAbl + sampleUri to this same module.
 */

const NUM_TRACKS = 4;
const NUM_COLS = 8;

/* Extract a Move-shaped 4x8 clip grid from a parsed Song.abl object.
 *
 * Returns:
 *   {
 *     tempo: number,
 *     numTracks: number,                 // min(tracks.length, 4)
 *     timeSignature: {upper, lower},
 *     cells: 4 x 8 array of cell objects
 *   }
 *
 * Each cell:
 *   { exists, beats, loopEnabled, color, kind }
 *   - exists:      clip present in this (track, col)
 *   - beats:       loop length in beats (loop range if enabled, else region span)
 *   - loopEnabled: region.loop.isEnabled
 *   - color:       clip color (or null)
 *   - kind:        source track "kind" ("audio"/"midi"/"")
 *
 * Mirrors tests/print_stems/abl_io.py:parse_clip_grid.
 */
export function parseClipGrid(song) {
    const tracks = (song && song.tracks) || [];
    const numTracks = Math.min(tracks.length, NUM_TRACKS);

    const cells = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
        const track = t < tracks.length ? tracks[t] : null;
        const kind = track && typeof track.kind === "string" ? track.kind : "";
        const slots = (track && track.clipSlots) || [];
        const row = [];
        for (let c = 0; c < NUM_COLS; c++) {
            const slot = c < slots.length ? slots[c] : null;
            const clip = slot ? slot.clip : null;
            if (!clip) {
                row.push({ exists: false, beats: 0.0, loopEnabled: false, color: null, kind: kind });
                continue;
            }
            const region = clip.region || {};
            const loop = region.loop || {};
            const loopEnabled = !!loop.isEnabled;
            let beats;
            if (loopEnabled && (loop.end || 0) > (loop.start || 0)) {
                beats = loop.end - loop.start;
            } else {
                beats = (region.end || 0) - (region.start || 0);
            }
            row.push({
                exists: true,
                beats: beats,
                loopEnabled: loopEnabled,
                color: clip.color !== undefined ? clip.color : null,
                kind: kind,
            });
        }
        cells.push(row);
    }

    return {
        tempo: song ? song.tempo : undefined,
        numTracks: numTracks,
        timeSignature: (song && song.timeSignature) || { upper: 4, lower: 4 },
        cells: cells,
    };
}
