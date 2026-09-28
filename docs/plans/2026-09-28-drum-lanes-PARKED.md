# Drum lanes — PARKED 2026-09-28

Status: **parked, not there yet.** It works end to end on hardware, but it is
not finished. Branch `feat/drum-lanes` (worktree `schwung-drumlanes`), local
only, never pushed. Its companion module is **Loop Lanes**
(`../schwung-looplanes`, local git repo, no remote).

## The idea

Move's drum rack is a 16-lane step sequencer already, and **16 Pitches**
(Shift+Step 8, drum tracks only) gives every note on a pad its own pitch. So
4 drum tracks × 16 pads = **64 monophonic, pitched lanes**, stored, edited and
recorded natively by Move, with no second sequencer. The missing part is that
Move never lets the pitch out:

- A 16 Pitches note stays the **pad's own note**. The pitch is a per-note
  expression, Live's MPE note PitchBend (`"automations": {"PitchBend": [...]}`
  per note in Song.abl).
- With the track's MIDI output on, a **plain hit** goes out as the pad's note.
  A **16 Pitches note goes out as nothing at all**.

Schwung reads the notes out of Move and supplies the missing ones.

## What is built (branch commits, oldest first)

| Commit | What |
|---|---|
| `6a7029c56` | Prototype: sequenced + live 16 Pitches notes read from Move, played to a slot |
| `7701cb42e` | Sequenced path runs `DL_SEQ_LAG_MS` 4.6 ms behind the clock (measured) |
| `1f6ac2dc5` | MPE form (one channel per pad) — **superseded** |
| `6ab12704a` | Route by source track — **superseded** |
| `8ab70608a` | **Current design**: add only the missing notes to the track's own output, plus the Drum MPE slot toggle |

### Reading the notes

- **Sequenced**: `move_model.c` `playing_clips_update` decodes every track's
  playing clip from Move's live flip document. It is hash-verified and
  double-buffered. Each note carries `has_pitch` and `pitch_offset`, taken
  from the note blob's pitch lane (type −2, value × 48/8191 semitones).
- **Live**: Move's engine
  `EventBuffer<EndpointedMidiMessage, Distance<FrameKind>>` is found by the RTTI
  name of its `shared_ptr_emplace`.
  - Header: cap +0x18, cnt +0x20, data +0x28.
  - Record (40 bytes): f64 frame, u32 endpoint (127 = the selected track's
    live input), i32 note (or −2 for per-note pitch), f32 value, i64 note id,
    u32 kind (0 On, 1 Off, 2 CC, 3 PerNoteCC).
  - A note's pitch is the PerNoteCC −2 record carrying its id.
  - Measured on firmware 2.1.0 only.
- **Track output channel**: `MidiTrackState.mMidiOutputEndpoint` (member
  offset measured 2056, resolved by name at boot). Channel = endpoint + 1;
  −1 = output off. It is read into `mm_track_t.midi_out_ep`. Verified in
  Set 5: track 1 = 0, track 2 = 1, tracks 3 and 4 = none, which matches
  Song.abl.

### What is sent (`src/host/drum_lanes.[ch]`, pure, `tests/host/test_drum_lanes.c`)

On the track's output channel, through `shadow_chain_dispatch_midi_to_slots`,
the same dispatch as Move's MIDI_OUT echo. So receive channel, forward,
transpose and MIDI FX all treat these notes as Move's:

```
16 Pitches note-on   CC 3 = 1, pitch bend (8192 + semis·8191/48), NoteOn(pad note), CC 3 = 0
16 Pitches note-off  NoteOff(pad note), pitch bend centre
plain hit            nothing — Move already sent it
```

- **Track output off**: every hit is ours, sent on channel track+1
  (`dl_out_for`).
- **Plain hit on a lane still sounding a pitched note**: only the bend is
  reset, with no NoteOff. Move's own NoteOn for that pad note is already on
  its way, and our off would kill it.
- **The note-off follows its note-on's channel**, even if the output channel
  changes in between.
- A lane is monophonic, as Move's pads are. Different pads sound together,
  each latched at its own NoteOn.

### Switching it on

**Slot Settings → Drum MPE**, `slot:drum_mpe`, on all three surfaces:
- the two settings lists;
- a one-knob **Drum Rack** grid page after Sends (the Main page is full at
  eight knobs).

It is saved per set in `shadow_chain_config.json` as `drum_mpe`; an absent
value means off. A track's lanes run when some slot with Drum MPE on hears
that track's channel, or listens to All (`dl_tracks_wanted`, computed each
block in `drum_lanes_render_tick`). `drum_lanes.conf` is gone.

### Loop Lanes (`../schwung-looplanes`, commit `70b9497`)

A sliced loop per pad. See its `docs/DESIGN.md`.
- The lane is the **note** (pad k = note 36+k), on any channel.
- CC 3 and the bend are latched per channel at each NoteOn.
- A plain hit plays the whole loop, one slice per 16th, in time. Its loop
  chokes the other pads' loops.
- A 16 Pitches note plays slice `round(bend) + Slice Offset`.
- Per pad: file (.wav / .rx2 / .rex), slices (Auto / 8 / 16 / 32 / 64), gain,
  tune, fine, decay, reverse, mode.
- The loader thread is SCHED_OTHER, and the module pins itself with
  `RTLD_NODELETE` so the thread cannot outlive `dlclose`. That exact crash was
  reproduced first.
- Pads follow finger hits through the `child_press_param` vouch.
- `midi_log` is still in the module as a diagnostic (`GET_PARAM synth:midi_log`,
  the last 12 messages). Remove it before any release.

## Hardware state when parked

- **Device runs this branch's host build** (installed with `install.sh local`)
  and the Loop Lanes build from `70b9497`.
- Set 5: slot 1 holds Loop Lanes and receives channel 1 (track 1's output).
  Drum MPE was switched on over testd; the UI has not saved it, so it will not
  survive a set reload.
- Charles's verdict on the pass: **"this is good but not there yet"**. The
  specifics were not recorded; ask before resuming.
- To return the device to main: build main and run
  `./scripts/install.sh local --skip-modules --skip-confirmation`.

## Open / next

1. **Find out what "not there yet" is**, first, before changing anything.
2. **Rebase.** The base `63483b3bd` is the old lanes-v2 branch, not `main`.
   `main` has since merged #552 and #553 (the live model reader and lanes on
   it), and `move_model.c` will conflict. Take `main`'s reader and re-apply
   the additions from this branch:
   - `has_pitch`;
   - `playing_clips_update` / `move_model_playing_notes`;
   - the EventBuffer live reader;
   - `midi_out_ep`.
3. **Unverified on hardware** under the current design:
   - Move truly sends nothing for a 16 Pitches note when output is on. This
     was measured once, early. An earlier bug looked like Move's plain note
     reaching the slot, and turned out to be a plain hit on the same channel.
   - Timing of the added notes against Move's own plain hits on one channel.
     The 4.6 ms lag was measured against Move's audio, not its MIDI out.
   - The track output off → track+1 fallback.
   - A 16 Pitches note at +0 with no PerNoteCC record would read as plain and
     not be sent. It is unknown whether Move always writes the record.
4. **Per-note Pressure** (lane type −1 / `"Pressure"`) is not forwarded.
5. A bend on one channel is last-wins for a synth that does not latch at
   NoteOn. That is inherent to single-channel augmentation and accepted.
6. **Docs not written**:
   - `docs/API.md` / `docs/MODULES.md` (the CC 3 + bend contract, so other
     modules can use it);
   - the manual (the Drum MPE setting);
   - `help_content.json`.
7. **Loop Lanes**:
   - remove `midi_log`;
   - no GitHub repo yet (create it under **charlesvestal** only, never
     charlesvestal-nrf, and only when asked);
   - not in the catalog.
8. **Device cleanup**:
   - test sets (idx 7 empty/unsaved "Set 9", 8 "Lane MidiOut", 9 "Pitch Map",
     28–30);
   - `/data/UserData/moveB/`;
   - `/data/UserData/schwung/ll_test/`;
   - the probe scripts in `/data/UserData/schwung/*.py`.

## Rejected on the way (so they are not re-tried)

- **More than four Move tracks**: Move stores a 5-track Song.abl but will not
  play the extra tracks.
- **A second MoveOriginal behind a fake ablspi**: it boots, but it is
  untenable (dropouts, capability lies).
- **Shift+pad / clip tricks**: Shift+pad launches a clip immediately.
- **Reading raw pad presses and keeping a pad map**: too fragile. The engine's
  own note records are the source.
- **One MIDI channel per pad (MPE)**: this needed Receive All and a separate
  routing path, and it doubled up with Move's own output. It was replaced by
  augmenting the output.
- **Per-slice tune tables**: per-pad Tune/Fine plus p-locks and automation
  cover it.
- **`restart-move.sh`** launches Move without MoveLauncher. Reboot instead.
