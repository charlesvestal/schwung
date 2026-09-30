# Step Menu: Chance (Elektron-style) + Move's own Length/Velocity

Status: design, 2026-09-30. Branch `feat/step-menu`.

## The gesture

Hold a step, press **Menu**. A step menu comes up over Move's screen.

| While the step is held | Effect |
|---|---|
| Menu (first press) | open the step menu, field = **Chance** |
| Menu (again) | next field: Chance → Length → Velocity → Chance |
| Jog | edit the focused field |
| Release the step | close the menu; the note is untouched |

Menu is swallowed on BOTH edges while a step is held (latched, like every
other claimed button) -- measured below, Move otherwise flips Note↔Session and
the pads start launching clips.

## Fields

- **Chance** is OURS. The jog is swallowed (Move would otherwise change the
  note length under it). One list, Elektron order:
  `100%`, `99 98 96 94 91 87 81 75 67 59 50 41 33 25 19 13 9 6 4 3 1 %`,
  then `1:2 2:2 1:3 2:3 3:3 1:4 … 4:4 … 1:8 … 8:8`.
  A:B = play on pass A of every B passes of the clip's loop. The pass is
  `floor((song_beats - start_beats - (loop_start - region_start)...) / loop_len)`
  from the live model -- derived, never counted, so it cannot drift and it
  restarts with the clip.
- **Length** and **Velocity** are MOVE'S. The jog is passed THROUGH to Move
  (Length = Move's own hold-step + jog; Velocity = hold-step + Volume knob,
  so a Velocity jog detent is re-emitted as a CC 79 detent). The value drawn
  is read back from the live model's decoded note (`mm_note_t.dur`, `.vel`),
  so the menu shows Move's truth and edits land in Move's note, Move's
  instrument, Move's Undo.

## Checking

The menu draws the displayed page's 16 steps as bars (chance per step; an A:B
step draws its ratio as a dotted bar; empty steps blank), the held step
inverted, plus the held note's Length and Velocity as numbers. One gesture
answers "what is on this page" without stepping through it.

## Where Chance acts

Only on Schwung slot synths: the shim drops the note-on and its paired
note-off before the slot's chain. Move's own instrument still plays and the
step LED still says 122 -- stated in the manual, not hidden.

A note is identified by **Move's note id** (flip id, survives Undo) with its
pitch and start cached from the edited clip's decoded notes; at playback a
note-on on a track's slot is matched by (clip id, pitch, clip position ≈ start).
Live pad presses are indistinguishable from sequenced notes, so a roll is only
applied while the transport runs AND the clip has a stored note at that
position and pitch. Stopped transport never rolls.

Drum tracks: a step's chance applies to the notes at that step whose pitch is
the selected voice; melodic: to every note starting in the step (a chord drops
as a unit -- one roll per step per pass, shared).

## Architecture

Three places, each doing the one thing only it can:

- **Shim (SPI callback)** owns the GESTURE. The held-step mask is already
  tracked from the hardware mailbox; Move keeps the step press and release
  (display_mode stays 0 -- raising the shadow UI would re-route the step
  release through the withhold path, which has no press on record and so
  would never hand Move its release). Menu with exactly one step held is
  swallowed on both edges (latched); the jog is swallowed on Chance and passed
  through on Length, and on Velocity re-emitted as a Volume detent with its
  touch note. State goes to `shadow_control_t` (open, field, held step).
- **Model thread** owns the NOTES. The edited clip's notes are only readable
  there, so it publishes a per-page summary for the displayed 16 steps (note
  ids, pitches, starts, the held step's length/velocity) through a seqlock.
- **Chain (per slot, beside the lanes)** owns CHANCE: the store, the roll in
  `v2_on_midi` ahead of MIDI FX and synth, the paired note-off drop,
  persistence in the slot state, Undo journaling. It already receives the
  clip's phase, loop window and identity each frame (the lanes seam), and
  Move's notes arrive stamped exactly on their start phase (measured
  2026-09-17), so a note-on is matched by pitch + phase to a stored entry.
  Edits arrive as a `chance:` param from the UI/shim.
- **shadow_ui.js** DRAWS: a rect-overlay card (`display_overlay` mode 1, the
  Shift+knob overlay's mechanism) over Move's own screen.

Selected drum voice: not in Move's document (no member names it -- checked
all 93 classes). Read Move's own answer instead: the Note-mode pad LED at 122
is the selected cell, and on a drum rack the left 4x4 is notes 36..51. If the
held step's notes include that pitch, Chance scopes to it; otherwise to every
note starting in the step.

## Persistence

Per set, `set_state/<uuid>/step_chance.json`, keyed by clip id + note id. A
note that disappears (deleted) drops its entry at the next save; a clip that
is orphaned keeps entries dormant, same rule as lanes.

## Measured on hardware (2.1.x, 2026-09-30, test set idx 26)

- Hold an EMPTY step ~430 ms: no note is added (so step+Menu on an empty step
  is harmless).
- Tap a step: note added (LED 122).
- Hold a step WITH a note + jog: `Note Length` screen, `1.0 → 1.1 → 1.2` per
  detent (tenths of a step); the tail lights the next step 75.
- + jog click: nothing visible.
- + Menu: toggles Session/Note (the `Note Mode` card) -- must be swallowed.
- Release after a hold: the note stays (no toggle).

## Open / later

- Move-native chance: the note record's `u8 flag` (29-byte head) may be Move's
  enable flag. Not relied on.
- Copy/paste of steps mirrored onto chance entries (lanes already follow
  Move's paste via the model; same hook).
