# Feature Plan: Track Transpose + Scale Detection in Master Mode

## Overview
Add transpose control in master mode with:
1. Piano-style pad display for selecting transpose values
2. Automatic scale detection highlighting in-scale notes
3. Global step-based transpose sequencing with variable durations

## Key Decisions
- **Live transpose**: Applied by scheduler at playback, stored notes unchanged
- **Octave shift**: MoveUp (CC 55) / MoveDown (CC 54) to shift displayed octave
- **chordFollow per track**: Tracks with chordFollow=true get transpose applied
- **Global sequence**: One transpose sequence shared across all chordFollow tracks
- **Sequential display**: Step LEDs show steps 1-16 in order
- **Fixed step colors**: Steps use a fixed color (not note-based)
- **Max 16 steps**: Hard limit on transpose sequence length
- **Empty = no transpose**: If no steps defined, plays with transpose 0

## Master View Layout
```
Row 1 (top, indices 24-31):     Chord follow toggle per track
Row 2 (indices 16-23):          Reserved / Black (future use)
Row 3 (indices 8-15):           Piano black keys
Row 4 (bottom, indices 0-7):    Piano white keys
---- step buttons: Global transpose sequence (max 16 steps) ----
```

## Chord Follow
- **chordFollow = true**: Track gets transpose applied (like NOTE tracks)
- **chordFollow = false**: Track plays notes as-is (like DRUM tracks)
- Toggle per track on row 1 pads in master view
- Scale detection only analyzes tracks with chordFollow=true

## Sequencer Type
- Global `sequencerType` setting (one type for now, extensible later)
- Stored per set for future expansion

## Piano Layout

```
Row 3 (indices 8-15):   [  ][C#][D#][  ][F#][G#][A#][  ]
Row 4 (indices 0-7):    [C ][D ][E ][F ][G ][A ][B ][C ]
```

### Octave Shifting
- **MoveUp (CC 55)**: Shift piano display up one octave
- **MoveDown (CC 54)**: Shift piano display down one octave
- Range: -24 to +24 (2 octaves each direction)

### Pad-to-semitone mapping
| Pad Index | Note | Base Semitone |
|-----------|------|---------------|
| 0 | C | 0 |
| 1 | D | 2 |
| 2 | E | 4 |
| 3 | F | 5 |
| 4 | G | 7 |
| 5 | A | 9 |
| 6 | B | 11 |
| 7 | C+1 | 12 |
| 8 | (gap) | - |
| 9 | C# | 1 |
| 10 | D# | 3 |
| 11 | (gap) | - |
| 12 | F# | 6 |
| 13 | G# | 8 |
| 14 | A# | 10 |
| 15 | (gap) | - |

Actual semitone = baseSemitone + (octaveOffset * 12)

## Global Transpose Sequence

One sequence affecting all tracks with chordFollow=true. Maximum 16 steps.

### Data Structure
```javascript
// Global transpose sequence (max 16 steps)
transposeSequence: [
  { transpose: 0, duration: 4 },   // Step 1: C for 1 bar
  { transpose: 5, duration: 8 },   // Step 2: F for 2 bars
  { transpose: 7, duration: 4 },   // Step 3: G for 1 bar
]
```

- `transpose`: -24 to +24 semitones
- `duration`: 1 to 64 beats (1 beat to 16 bars, default 4 = 1 bar)

### Step LED Display
```
LED 1  = Step 1 (if exists)
LED 2  = Step 2 (if exists)
...
LED 16 = Step 16 (if exists)
```

- Lit = step exists (fixed color, e.g., Cyan or White)
- Unlit/dim = no step at this position
- Current playing step = brighter or pulsing

### Interaction

| Action | Result |
|--------|--------|
| **Hold step + tap piano** | Set transpose value for that step (creates if new) |
| **Hold step + knob 1** | Adjust duration (1 beat to 16 bars) |
| **Hold step** | Piano highlights that step's current transpose value |
| **MoveDelete + step** | Remove step, shift subsequent steps back |
| **MoveUp** | Shift piano display up one octave |
| **MoveDown** | Shift piano display down one octave |

### Playback Behavior
- If sequence is empty: play with transpose = 0 (no effect)
- If steps defined: loop through them based on total duration
- **Resets to step 1 when play is pressed**
- Scheduler tracks beat position, calculates current transpose
- Only tracks with chordFollow=true receive transpose offset

### Example
```
Step 1: transpose=0,  duration=4  → C for 1 bar   (beats 0-3)
Step 2: transpose=5,  duration=8  → F for 2 bars  (beats 4-11)
Step 3: transpose=7,  duration=4  → G for 1 bar   (beats 12-15)
[loops after 16 beats total]
```

## Scale Detection Algorithm

### Step 1: Collect Notes
Gather all unique pitch classes (note mod 12) from **ALL patterns** of all tracks with chordFollow=true. This gives a complete picture of the musical content across the entire set.

### Step 2: Scale Templates (ordered by preference)
Simpler scales listed first - algorithm prefers matches earlier in the list when scores are equal.

```javascript
const SCALES = [
  // Pentatonic (5 notes) - most preferred
  { name: 'Minor Penta',     notes: [0, 3, 5, 7, 10] },
  { name: 'Major Penta',     notes: [0, 2, 4, 7, 9] },

  // Blues (6 notes)
  { name: 'Blues',           notes: [0, 3, 5, 6, 7, 10] },
  { name: 'Whole Tone',      notes: [0, 2, 4, 6, 8, 10] },

  // Common 7-note scales
  { name: 'Major',           notes: [0, 2, 4, 5, 7, 9, 11] },
  { name: 'Natural Minor',   notes: [0, 2, 3, 5, 7, 8, 10] },

  // Modes (7 notes)
  { name: 'Dorian',          notes: [0, 2, 3, 5, 7, 9, 10] },
  { name: 'Mixolydian',      notes: [0, 2, 4, 5, 7, 9, 10] },
  { name: 'Phrygian',        notes: [0, 1, 3, 5, 7, 8, 10] },
  { name: 'Lydian',          notes: [0, 2, 4, 6, 7, 9, 11] },
  { name: 'Locrian',         notes: [0, 1, 3, 5, 6, 8, 10] },

  // Other 7-note
  { name: 'Harmonic Minor',  notes: [0, 2, 3, 5, 7, 8, 11] },
  { name: 'Melodic Minor',   notes: [0, 2, 3, 5, 7, 9, 11] },

  // 8-note scales - least preferred
  { name: 'Diminished HW',   notes: [0, 1, 3, 4, 6, 7, 9, 10] },
  { name: 'Diminished WH',   notes: [0, 2, 3, 5, 6, 8, 9, 11] },
];
```

### Step 3: Score & Select
For each root (0-11) × each scale:
```
fitScore = (notes in scale) / (total unique notes)
sizeBonus = 1 / scale.notes.length  // prefer smaller scales
score = fitScore + (sizeBonus * 0.1)  // small bonus for simpler scales
```

Selection priority:
1. Highest fitScore (all notes must fit)
2. If tied: prefer smaller scale (fewer notes)
3. If still tied: prefer earlier in list (pentatonic > modes)

## Track View Pad Display (chordFollow tracks)

When on a track with chordFollow=true and NOT holding a step, pads show the detected scale.

### Pad Layout
Pads 0-31 = MIDI notes 36-67 (C2 to G4, ~2.5 chromatic octaves)

### Colors (when not holding step)
| State | Color |
|-------|-------|
| Root note | Track color (bright) |
| In-scale note | White |
| Out-of-scale note | Dim grey |
| Currently playing | Brightest (flash/pulse) |
| No scale detected yet | All dim grey |

### Behavior
- Scale detection recalculates when a note is added to any step
- Same algorithm as master view (all patterns, prefer simple scales)
- When holding a step: revert to current behavior (show step's notes)
- Currently playing note shows the actual entered note (unfiltered), just brightest
- For chordFollow=false tracks: keep current behavior (pads black)

### Example
If detected scale is C Minor Pentatonic (C, Eb, F, G, Bb):
```
Pad layout (partial, notes 36-47 = C2 to B2):
C2  C#2  D2  Eb2  E2  F2  F#2  G2  G#2  A2  Bb2  B2
[R] [  ] [  ] [S] [  ] [S] [  ] [S] [  ] [  ] [S] [  ]

R = Root (track color)
S = In-scale (white)
[  ] = Out-of-scale (dim)
```

## LED Colors Summary

### Piano (Pads rows 3-4)
| State | Color |
|-------|-------|
| White key, in scale | White |
| White key, out of scale | Dark grey |
| Black key, in scale | Light grey |
| Black key, out of scale | Black |
| Held step's transpose | Bright highlight |
| Gap pads (8, 11, 15) | Black |

### Chord Follow (Pads row 1)
| State | Color |
|-------|-------|
| chordFollow = true | Track color (bright) |
| chordFollow = false | Grey/dim |

### Transpose Steps (Step buttons)
| State | Color |
|-------|-------|
| Step exists | Fixed color (Cyan/White) |
| No step | Black/dim |
| Currently playing | Brighter |

### Buttons
| Button | State |
|--------|-------|
| MoveUp | Lit when can go higher |
| MoveDown | Lit when can go lower |

## Display Content
```
Line 1: "Scale: C Major"
Line 2: "Transpose: +5 (F)"
Line 3: "Step 2/3  Dur: 2 bars"
Line 4: "Oct: 0"
```

## Set Data Structure

```javascript
{
  tracks: [...],                    // existing: 8 tracks
  bpm: 120,                         // existing
  transposeSequence: [              // NEW: max 16 steps
    { transpose: 0, duration: 4 },
    { transpose: 5, duration: 8 },
  ],
  chordFollow: [false, false, false, false, true, true, true, true],  // NEW: 8 booleans
  sequencerType: 0,                 // NEW: global type (0 for now, extensible)
}
```

Default: tracks 1-4 chordFollow=false (drums), tracks 5-8 chordFollow=true (melodic)

## Technical Changes

### New: `lib/scale_detection.js`
- `detectScale(tracks, chordFollow)` → `{ root, scaleName, scaleNotes }`
- Scale templates and scoring algorithm
- Only analyzes tracks with chordFollow=true

### New: `lib/transpose_sequence.js`
- Global transpose sequence array (max 16)
- `setStep(index, transpose, duration)` - create/update step
- `removeStep(index)` - remove and shift subsequent steps
- `getTransposeAtBeat(beat)` - for playback (returns 0 if empty)
- `getTotalDuration()` - for loop point

### Modified: `views/master.js`
- Import MoveUp, MoveDown, MoveDelete
- Remove trackType logic, use chordFollow from set data
- Piano display on pad rows 3-4 with scale highlighting
- Transpose sequence on step LEDs (sequential, fixed color)
- Handle MoveUp/MoveDown for octave shift
- Handle hold step + piano for transpose entry
- Handle hold step + knob 1 for duration
- Handle MoveDelete + step for removal
- When holding step: highlight its transpose on piano
- Track held step state
- Track octave offset state

### Modified: `lib/state.js`
- Add `transposeSequence` (global array, max 16)
- Add `chordFollow` (array of 8 booleans)
- Add `sequencerType` (number, 0 for now)
- Add `transposeOctaveOffset` for display
- Add `currentTransposeBeat` for playback position
- Add `detectedScale` cache ({ root, scaleName, scaleNotes } or null)

### Modified: `lib/data.js`
- Add `createEmptyTransposeStep()` → `{ transpose: 0, duration: 4 }`
- Add `cloneTransposeSequence(seq)` for deep cloning
- Add migration for sets without transposeSequence/chordFollow

### Modified: `lib/persistence.js`
- Update `saveCurrentSet()` to include transposeSequence, chordFollow, sequencerType
- Update `loadSetToTracks()` to load new fields with defaults:
  - transposeSequence: [] (empty)
  - chordFollow: [false,false,false,false,true,true,true,true]
  - sequencerType: 0
- Add migration for old sets

### Modified: `views/track/normal.js`
- Import scale detection
- Update `updatePadLEDs()`:
  - If chordFollow=false: keep current (black pads)
  - If chordFollow=true and not holding step:
    - Root note = track color
    - In-scale = white
    - Out-of-scale = dim grey
    - Playing notes = brightest
  - If holding step: show step's notes (current behavior)
- Use cached `detectedScale` from state

### Modified: Scheduler/playback
- Track beat position in transpose sequence
- `getTransposeAtBeat(beat)` returns current transpose (0 if empty)
- Apply transpose offset when sending MIDI for tracks with chordFollow=true
- Loop sequence based on total duration

## Decision Log
| Date | Decision | Rationale |
|------|----------|-----------|
| 2026-01-09 | Piano layout | Intuitive musical layout |
| 2026-01-09 | Auto scale detection | OP-Z inspired |
| 2026-01-09 | Live transpose | Non-destructive, sequenceable |
| 2026-01-09 | Octave via MoveUp/MoveDown | Dedicated buttons |
| 2026-01-09 | Global transpose sequence | Simpler than per-track |
| 2026-01-09 | Sequential step display | Direct mapping |
| 2026-01-09 | Fixed step LED color | Simple, consistent |
| 2026-01-09 | Max 16 steps | Hard limit, fits hardware |
| 2026-01-09 | Empty = no transpose | Clean default behavior |
| 2026-01-09 | Highlight held step transpose | Visual feedback on piano |
| 2026-01-09 | chordFollow instead of trackType | Simpler, more flexible |
| 2026-01-09 | sequencerType global setting | Extensible for future |
| 2026-01-09 | Scale-aware pad display in track view | Visual scale feedback while editing |
