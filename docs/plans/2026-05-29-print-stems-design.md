# Print Stems — Design

**Status:** Design accepted, pending implementation plan.
**Goal:** Bounce every populated clip in the active Move set to its own WAV, lay the result out as a sibling audio-clip set, so users can take Schwung-driven arrangements into Move's audio-clip workflow (or Live, or anywhere else) without DAW-side glue.

## Why

Today, Schwung sets are "live performance objects" — MIDI clips drive Schwung slots (and Move-native tracks), and the sound only exists at playback time. There is no path from Schwung to stems short of recording the master output through the sampler or external capture, which gives a single stereo mix and loses per-track separation.

Move recently added support for audio-clip tracks (`kind: "audio"`), with a well-defined `Song.abl` schema (1.8.3) and a clear file convention (`ableton:/user-library/Recordings/<name>.wav` → `/data/UserData/UserLibrary/Recordings/<name>.wav`). That makes a "Schwung set → audio-clip set" bounce both well-targeted and useful: the printed set plays on Move standalone, opens in Live as audio, or feeds Schwung's own song-mode as audio source instead of MIDI.

## Scope

**In scope**
- A new `Print Stems` tool (`component_type: tool`) under the Tools menu.
- Per-clip output: each populated `(track, col)` pad in the active set becomes one stereo WAV. (Rendering is per-column / parallel; output layout is per-clip.)
- Live-triggered capture (Schwung tells Move to fire the clip, Schwung records).
- Loop-wrap printing for clean gapless loops.
- Output: a sibling set `<original-name> Stems` referencing the printed WAVs as audio clips.

**Out of scope**
- Stem recording in skipback / quantized sampler. Those stay single-stereo.
- Offline (faster-than-realtime) rendering. Move-native tracks (drift, samplers) can't be replayed without involving Move.
- Stems-with-MFX-printed or other dry/wet contracts. v1 is "slot FX in, MFX out," no toggle.
- Bypassed-slot re-enable. v1 honors bypass state (bypassed slot prints as silence or is skipped).
- Per-clip patch overrides during print (you print what's currently loaded).

## UX

### Entry point

Tools menu → **Print Stems**. Opens fullscreen.

### Preconditions checked on entry

Before showing the overview, validate:

1. **An active set exists** (`active_set.txt` present, `Song.abl` parseable). Otherwise show "No active set" and refuse to enter.
2. **The active set has at least one populated clip.** Otherwise show "Set is empty."
3. **There is a free song slot** for the output set — unless the sibling `<name> Stems` set already exists, in which case re-print overwrites and no new slot is needed. Otherwise, count entries in `/data/UserData/UserLibrary/Sets/` and compare against the Move-firmware cap. If full, show "No free song slot — delete a set and retry." and refuse to enter.
4. **No other module owns SPI MIDI_OUT injection** (song-mode in play state, etc.). If contested, show "Stop song-mode first."

Failing any precondition exits cleanly — we never start a print we can't finish.

### Screens

1. **Overview.** Shows current set name, clip count (populated pads / total), estimated print time, and a single primary action: **Print All**. Jog click triggers it; Back exits. Estimate formula: `sum over used columns of ((pre_roll + max_loop_length_in_col + min_tail) + expected_inter_pass_silence)`, where `expected_inter_pass_silence` is a coarse heuristic (e.g. 2 s default; user-visible as "approx" since adaptive). (v1 only supports print-all; per-clip selection is a future iteration.)

2. **Progress.** Per-pass status: `[Pass N/M] Column D — printing 3 stems` plus an overall progress bar (passes done / total) and a list of which tracks are recording in the current pass. Sub-phase indicator (`pre-roll` / `recording` / `tail` / `clearing`). Jog click during progress = **Cancel** (with confirmation). On cancel: partial WAVs already written stay in `Recordings/`; the new set's `Song.abl` is **only** written at end, so a cancelled run leaves no half-formed set behind.

3. **Complete.** "Printed M stems to `<setname> Stems`. Jog click to open, Back to exit." Opening the new set is optional polish; v1 can stop at "saved" and leave set selection to the user.

### Trigger gating

Print Stems reads per-track audio buses directly; it does not output live audio of its own (Move continues to feed its mailbox to the speakers normally during print). It is **not** subject to the speaker-feedback gate. It does, however, take over the SPI MIDI_OUT stream during the print pass (to fire pads via `move_midi_inject_to_move`), so it is exclusive — block entry if any other module currently owns pad injection (song-mode in play state, etc.), and during the print suppress UI gestures that fire pads.

## Capture architecture

### One pass per column (parallel per-track capture)

Each Move track has its own pre-MFX audio bus inside Schwung:
- **Schwung-slot tracks** → `shadow_slot_deferred[t]`. Verify during implementation that this buffer is post–slot-FX (where the chain runs FX before the MFX bus), so the "slot FX in, MFX out" contract holds. If not, use whichever per-slot buffer sits between slot-FX and MFX in the chain.
- **Move-native tracks** → per-track Link Audio channel `(t+1)-MIDI`, read from `/schwung-link-in` SHM (channels `1-MIDI`..`4-MIDI`; the 5th `Main` channel is post-mix and unused here), latency-compensated by the existing per-slot delay buffer (`shadow_latency_delay_apply`).

This means we can record **all 4 stems simultaneously** during a single playback pass. We print one column at a time across all tracks instead of one clip at a time. Worst case: 8 passes total (one per column), regardless of how many cells are populated. Best case: fewer if some columns are unused.

**Once, before the first pass:** snapshot prior state (track mute/solo, metronome, transport). Force unsolo, mute-off, metronome-off; stop Move transport. Fire all 4 tracks' silence pads to halt any in-progress playback. Then run the same tail-clear loop as step 5's tail wait, until every per-track bus RMS is below the silence threshold — guarantees pass 1's pre-roll starts from a known-silent baseline regardless of what was ringing when the user entered Print Stems. (MFX is **not** bypassed — captures read from pre-MFX per-track buses, so MFX state is orthogonal. Speakers play the live mix during the print, same as normal playback.)

For each column `c` in 0..7 (skip columns where no track has a clip):

1. **Per-track pad selection.** For each track `t`: if `clipGrid[t][c]` exists, queue pad `(t, c)`. Else queue track `t`'s empty/silence pad (mirrors song-mode's `silencePads[]`).
2. **Fire all queued pads at the next bar boundary.** Burst of up to 4 `NoteOn` injections via `move_midi_inject_to_move`.
3. **Determine pass length.** `pass_loop_length = max(loop_length(t, c) for t in tracks_with_clip_in_col_c)`. Shorter clips loop multiple times during the pass; we capture exactly their own loop length per the wrap algorithm and discard repeats.
4. **Pre-roll: one full `pass_loop_length`, no recording.** Slot FX state starts silent (cleared by the previous pass's tail-clear or the initial setup), so pre-roll lets the new clips' FX warm up (reverb tank fills, LFOs settle into looped position) before the recorded loop starts.
5. **Record + tail capture:** for each track `t` with a clip in column `c`, capture into a per-track buffer continuously, partitioned into:
   - **Stem region:** first `loop_length(t, c)` samples after pre-roll.
   - **Tail region:** following samples up to either (a) a minimum of 2 × `loop_length(t, c)`, or (b) RMS drops below threshold (-60 dBFS over 50 ms), or (c) the 15 s hard cap, whichever yields the *longest* tail. Step 2 of the next pass (or end-of-print) does not fire until **every** track meets its tail-end condition.
6. **Stop everything.** Fire each track's empty/silence pad to halt playback. (Most slot FX will already be near-silent from step 5's tail wait, but this halts any clip whose voice was still sustaining.)
7. **Wrap-mix tail into stem head** per track. Offline pass after capture, before WAV finalize: `stem[i] += tail[i] + tail[i + L]` for `i ∈ [0, L)` where `L = loop_length_samples`. Standard perfect-loop bounce: the reverb tail at the end of the recorded loop flows into the start of the loop, the way it would on the next iteration of continuous playback.
8. **Finalize WAVs** per captured clip. Filename + format per the Output section. WAV write happens off the print loop on the existing sampler background writer thread.

### Cross-clip tail bleed

Slot FX state (reverb tank, delay buffers, modulation phase) persists across pad triggers. Without intervention, column A's tail rings during column B's capture, polluting column B's stem with column A's audio. Step 5's adaptive tail wait is the mitigation: we keep recording into the prior pass's tail buffer until every slot's per-track RMS drops below threshold before firing the next pass's pads. The wait is adaptive — near-zero for dry patches, longer for big reverbs. The same problem would exist in a serial per-clip print; parallel pass-per-column doesn't introduce it.

If a user wants to avoid the wait entirely, they can use shorter / drier patches, or (future iteration) toggle a "reset slot DSP between passes" mode that force-reloads each slot's patch between columns — heavier but deterministic.

### Why pass-per-column, not pass-per-clip

- ~4–8× faster overall: 32 single-clip passes (worst case) → 8 column passes (worst case).
- Each track stem still captures **only its own clip**, in isolation w.r.t. the others' audio — the per-track buses are pre-mix, so track 2's audio doesn't leak into track 1's stem even though both are playing.
- Pre-roll cost amortizes: one pre-roll per pass instead of one per clip.

### Capture bus details

Both per-track buses are pre-MFX, so the "MFX out" contract is honored by reading from them directly — no MFX state mutation required.

- **Schwung-slot tracks:** `shadow_slot_deferred[t]`. Verify during implementation that slot FX runs upstream of this buffer; if it runs downstream, the slot-FX-printed half of the contract would also need a different read point.
- **Move-native tracks:** the per-track Link Audio buffer that gets summed into the rebuilt mailbox under `rebuild_from_la` mode. Post-latency-comp, pre-MFX. Need to verify the read path is exposed from shim to the print thread; if not, expose it as part of the implementation (small SHM surface or direct read of the existing per-slot delay buffer's output).

Both buses are stereo int16 at 44.1 kHz / 128 frames per block — same format as the existing sampler captures, so the WAV writer is reusable verbatim.

### Loop length & tempo

- **Tempo:** `song.tempo` from `Song.abl`. Frozen at print start; do not respond to MIDI clock changes mid-print.
- **Loop length in beats:**
  - `region.loop.isEnabled` and `loop.end > loop.start`: `loop.end - loop.start`.
  - Otherwise: `region.end - region.start` (one-shot; see below).
- **Loop length in samples:** `beats × (60 / tempo) × sample_rate` (round to nearest sample).
- **Bars:** `beats / time_signature.upper` (assume 4/4 for v1; honor `timeSignature.lower` if it ever varies — currently constant).

### One-shot clips

Clips with `loop.isEnabled == false` or no loop range: record exactly `(region.end - region.start)` samples plus tail (2× region length), **no wrap-mix**. Written as a non-looping audio clip in the output set (`loop.isEnabled: false`).

### Bypass state

If the patch for track `t` has the slot bypassed, the stem will be silent. v1 still records it (so the layout mirrors 1:1) and writes the WAV. (Alternative: skip and leave the audio-clip slot empty. Decide during implementation; both are easy.)

## Output format

### File layout

```
/data/UserData/UserLibrary/
  Recordings/
    <setname> Stem T1 A.wav        # 44.1k/16-bit/stereo, mirrors source layout
    <setname> Stem T1 C.wav
    <setname> Stem T2 A.wav
    ...
  Sets/
    <new-uuid>/
      <setname> Stems/
        Song.abl                    # references the WAVs above
```

Flat WAV naming (no subdirs in `Recordings/`) until we verify whether `sampleUri` accepts subpaths. Naming pattern: `<setname> Stem T<track+1> <colLetter>.wav` (e.g. `Set 26 Stem T2 D.wav`). Track is 1-indexed to match the user-facing track numbering; column uses the A–H letter, matching the set-overview label. Re-printing wipes any prior matching WAVs (filename collisions are overwrite, not suffix-appended — otherwise the new `Song.abl` would reference the wrong file).

### `Song.abl` shape

Mirror Set 26's schema (`http://tech.ableton.com/schema/song/1.8.3/song.json`):

- Top-level: `$schema`, `tempo` (= source tempo), `timeSignature`, `stepEditorResolution`, `globalGrooveAmount: 0`, `rootNote`, `scale`, `melodicLayout`, `returnTracks: []`, `masterTrack: {…}`, `scenes`, `grooves: []`, `metadata`. Copy the unchanged structural pieces from the source `Song.abl` (rootNote, scale, melodicLayout, scenes, masterTrack) so the new set "feels" like the source.
- `tracks`: 4 entries, all `kind: "audio"`. For each:
  - `name: ""` (or copy source track name if present), `color` (copy from source), `isSelected: false`
  - `devices: []` (no chain — FX already printed)
  - `mixer: { pan: 0, volume: <0dB>, sends: [], "solo-cue": false, speakerOn: true }` (match Set 26's default)
  - `clipSlots`: 8 entries. For populated slots: `{ "hasStop": true, "clip": { ...audio clip... } }`. For empty: `{ "hasStop": false }` (or whatever Move uses for empty audio slots — verify against Set 26's empty slots).

- Audio clip body:
  ```json
  {
    "name": "",
    "color": <copied-from-source-clip>,
    "isEnabled": true,
    "timeSignature": { "upper": 4, "lower": 4 },
    "region": {
      "start": 0.0,
      "end": <beats>,
      "loop": { "start": 0.0, "end": <beats>, "isEnabled": <true|false> }
    },
    "stepEditorScrollPosition": 0.0,
    "sampleUri": "ableton:/user-library/Recordings/<urlencoded>.wav",
    "warping": { "markers": [], "tempoAfterLastMarker": <song-tempo> },
    "gain": 0.0,
    "transpose": 0,
    "detune": 0.0,
    "envelopes": []
  }
  ```

### Set registration

After writing `Song.abl`, the set needs to appear in Move's set browser. Determine during implementation whether this is automatic (filesystem watch) or requires a dbus notification / restart. Set 26 was created via Move's own UI; we replicate its directory + JSON and assume Move picks it up. If it doesn't, identify Move's actual refresh trigger (dbus signal, filesystem touch, etc.) and invoke it after write.

## State save & restore

Before print starts, snapshot:
- Track mute/solo states (forced to unmuted, unsoloed during print)
- Metronome on/off (forced off during print)
- Move transport state (stop if playing)

After print ends (or on cancel), restore the snapshot. Restore must run even on JS exception — wrap the print loop in try/finally. Slot bypass and MFX state are **not** mutated by Print Stems (captures bypass MFX by reading pre-MFX buses; bypassed slots are honored as-is, see Bypass state above), so they don't need snapshotting.

## Edge cases

| Case | Behavior |
|---|---|
| No populated clips | Show "Set is empty" on overview, no print action. |
| All slots in a track empty | Track still rendered as audio in output set, all `clipSlots` empty. (Or skip the track entirely — minor decision.) |
| Move not booted to a set | Show "No active set" and refuse to enter. (Mirror song-mode's `loadSetData` failure path.) |
| Song slots full | Refuse to enter. "No free song slot — delete a set and retry." Checked on entry, not at end of print, so we never finish recording and then fail to write the set. |
| External MIDI clock during print | Force internal clock for duration; restore after. Tempo is frozen at `song.tempo` regardless. |
| User triggers a pad mid-print | Suppress: the tool owns the SPI MIDI_OUT stream, route only its own injections. |
| Tail still ringing at hard cap (15s) | Stop the tail capture, log a warning into the progress UI ("T2 col E: tail did not decay, stem may be truncated"), proceed to next pass. |
| Filename collision in `Recordings/` | Overwrite. (Suffix-appending would break the `Song.abl` reference unless we also pre-scan and pick a non-colliding base; simpler to wipe-and-re-print, which is what re-running Print Stems on the same source set means anyway.) |
| Sibling set `<name> Stems` already exists | Delete the existing sibling set folder and re-create. Matches the wipe-and-re-print model. Confirm with the user via a pre-print prompt: "Re-print? This will overwrite the existing `<name> Stems` set." |
| Disk full | Detect on WAV writer error, abort with "Disk full" message, leave partial outputs. |
| New UUID collision | Generate via existing UUID utility; collision probability negligible. |
| Set name contains characters that need escaping in URI | URL-encode reserved chars per RFC 3986. Spaces → `%20`. |

## Open questions to resolve during implementation

1. **Empty audio clipSlot shape.** Set 26's `clipSlots` array has 8 entries even when most are empty. Need to confirm the JSON shape of an empty slot (`{ "hasStop": false }` or `{}` or missing key entirely) by inspecting an empty slot in Set 26.
2. **Subdirs in `sampleUri`.** Test whether `ableton:/user-library/Recordings/Subdir/foo.wav` resolves. If yes, prefer per-set subdirs for cleanliness.
3. **Default mixer values.** Pull exact `volume`, `pan` defaults from Set 26's tracks. Match them.
4. **Clip launch quantization.** Confirm whether `move_midi_inject_to_move(NoteOn, padNote)` fires at next-bar quantization (Move's default clip launch) or immediately. If immediate, we need our own bar-boundary wait.
5. **Set browser refresh.** Confirm Move picks up new sets dropped into `Sets/<uuid>/` without firmware restart.
6. **Per-track Link Audio read surface.** Verify the per-track pre-MFX buffer is reachable from a non-RT print thread, or add a thin SHM/snapshot surface for it. The latency-comp ring (`shadow_latency_delay_apply`) already produces the right buffer; we just need read access without breaking the SPI callback's RT guarantees.
7. **`shadow_slot_deferred[t]` position in the slot chain.** Confirm this buffer is post–slot-FX (otherwise the "slot FX in" half of the contract fails). If it's pre-FX, identify the correct downstream buffer.
8. **Move song-slot cap.** Determine the actual maximum number of sets Move's firmware accepts (32? 64? higher?) so the "no free song slot" precondition can use the correct constant. Until confirmed, conservatively use the lowest plausible value or count active subdirectories under `Sets/` and compare against a configurable cap (start at 32).

## Future work (not v1)

- Per-clip selection (print this clip / these clips, not the whole set).
- Stem recording in quantized sampler (one-shot "jam → stems").
- Dry/wet toggle (print MFX, print pre-slot-FX, etc.).
- Re-print single clips into an existing stems set (replace stem, preserve arrangement edits).
- "Reset slot DSP between passes" mode for deterministic, bleed-free prints regardless of FX tail length.
- Click-track stem.
- Print song-mode arrangements (each song-mode entry → a longer multi-bar audio clip).
- Subdir naming once `sampleUri` subdirs are verified.

## Acceptance criteria

- Printing Set 26 (or any Schwung-driven set) produces a sibling `<name> Stems` set that opens in Move.
- Each populated MIDI/Schwung clip has a corresponding audio clip at the same (track, col), playing back a recognizable rendering of its source.
- Loop boundaries are gapless when looped in the audio set (no click, no missing tail).
- Snapshotted state (track mute/solo, metronome, transport) is restored after print, whether print succeeded, failed, or was cancelled.
- Disk-full and "no active set" cases show a clear message and don't corrupt state.
- A fully populated set (32 clips, 8 columns used) prints in roughly `8 × ((pre_roll + record + min_tail) × max_loop_length_in_col + inter_pass_silence)` real-time seconds, ≈ `8 × (1 + 1 + 2) × max_loop_length + 8 × inter_pass_silence`. For a typical 4-bar / 120 BPM set with mostly-dry FX: 8 × 4 × 8 sec + 8 × ~2 sec ≈ ~4½ minutes. Wet-FX sets are bounded by the 15 s tail-clear cap per pass.
