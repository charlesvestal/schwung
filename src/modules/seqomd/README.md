# SEQOMD - 16-Track MIDI Step Sequencer

SEQOMD turns your Ableton Move into a MIDI step sequencer. You get 16 tracks, each with 16 steps and 16 patterns. Steps can hold up to 7 notes with individual velocities, two CC values, probability, loop-based conditions, ratchets, and micro-timing offsets. There's a built-in arpeggiator with 15 modes that can be set per-track or overridden per-step. Tracks run at independent speeds and swing amounts. A transpose sequence lets you automate key changes across the pattern. Everything saves to 32 set slots on disk. It outputs MIDI over USB to control synths, drum machines, or your DAW.

## Core Specs

| Feature | Capacity |
|---------|----------|
| Tracks | 16 (independent MIDI channels, speeds, swing) |
| Steps per Pattern | 16 (adjustable loop points) |
| Patterns per Track | 16 (with snapshot recall) |
| Sets | 32 (complete states saved to disk) |
| Notes per Step | 7 (polyphonic with individual velocities) |
| Scheduled Notes | 512 simultaneous |

## Step Parameters

Each step supports:
- **Notes & Velocities**: Up to 7 polyphonic notes with per-note velocity
- **Note Length**: 1-16 steps
- **CC1 & CC2**: Two CC values per step with track defaults
- **Micro-timing**: ±24 ticks offset (half-step resolution)
- **Probability**: 5-100% trigger chance
- **Conditions**: Loop-based triggers (1:2, 2:3, 1:4, etc. - 72 total)
- **Ratchets**: 1x-8x with velocity ramp up/down modes (22 options)
- **Jump**: Non-linear sequencing to any step

## Arpeggiator

Full per-track arpeggiator with per-step overrides:
- **15 Modes**: Up, Down, Up-Down, Random, Chord, Converge, Diverge, Thumb, Pinky, Outside-In, Inside-Out, etc.
- **10 Speeds**: 1/32 to whole notes
- **Octave Extension**: None, ±1, ±2 octaves
- **Play Steps**: Binary skip patterns for complex rhythms
- **Continuous Mode**: Arp continues across triggers
- **Layer Modes**: Layer (overlap), Cut (monophonic), Legato

## Spark System (Conditional Triggering)

Three independent condition layers per step:
1. **Trigger Spark**: When does the step play?
2. **Parameter Spark**: When do CC locks apply?
3. **Component Spark**: When do ratchets/jumps apply?

Create evolving patterns like "play every loop, but filter sweep only every 4th."

## Transpose & Scale

- **16-step transpose sequence** with duration and conditions per step
- **Live transpose**: Piano pads for instant ±24 semitone shifts
- **Chord follow**: Per-track toggle for transpose response
- **Scale detection**: Auto-detects scale from played notes

## Track Features

- **Speed**: 1/4x to 4x multipliers (polyrhythms)
- **Swing**: 0-100% per track
- **MIDI Channel**: 1-16 per track
- **Mute**: Per-track control

## Navigation

| Action | Result |
|--------|--------|
| **Pad tap** (set view) | Load set, enter track view |
| **Menu** | Toggle pattern view |
| **Shift + Menu** | Toggle master view (transpose) |
| **Loop** (hold) | Edit loop points |
| **Capture + Step** | Edit spark conditions |
| **Shift + Step 1** | Return to set view |
| **Shift + Step 2** | Channel mode |
| **Shift + Step 5** | Speed mode |
| **Shift + Step 7** | Swing mode |
| **Shift + Step 11** | Arp mode |
| **Track buttons** | Select tracks 1-4 |
| **Shift + Track** | Select tracks 5-8 |
| **Jog wheel** | Scroll tracks (in 4-track groups) |

## Step Editing (Hold Step)

| Control | Function |
|---------|----------|
| **Pads** | Toggle notes (up to 7) |
| **Knob 1** | CC1 value (tap to clear) |
| **Knob 2** | CC2 value (tap to clear) |
| **Knob 3** | Arp override (tap to cycle modes) |
| **Knob 6** | Ratchet 1x-8x (tap to reset) |
| **Knob 7** | Probability/Condition (tap to clear) |
| **Knob 8** | Velocity (tap to reset to 100) |
| **Jog wheel** | Micro-timing offset |
| **Tap another step** | Set note length |

## Architecture

```
seqomd/
  module.json          # Module metadata
  ui.js                # Main UI entry point
  lib/
    constants.js       # Sequencer constants (speeds, conditions, etc.)
    state.js           # Mutable state + view transitions
    helpers.js         # Utility functions
    data.js            # Track/pattern data structures
    persistence.js     # Save/load sets to disk
    shared-constants.js # Colors, MIDI constants, button mappings
    shared-input.js    # LED control functions
  views/
    set.js             # Set selection (32 sets on pads)
    track.js           # Track view coordinator
    pattern.js         # Pattern selection grid
    master.js          # Master settings (transpose, sync)
    track/
      normal.js        # Main step editing mode
      loop.js          # Loop start/end editing
      spark.js         # Spark conditions
      arp.js           # Arpeggiator settings
      channel.js       # MIDI channel adjustment
      speed.js         # Track speed multiplier
      swing.js         # Track swing amount
  dsp/
    seq_plugin.c       # Main DSP entry point
    track.c            # Track playback and scheduling
    scheduler.c        # Note scheduling
    arpeggiator.c      # Arpeggiator implementation
    transpose.c        # Transpose sequence
    scale.c            # Scale detection
    params.c           # Parameter handling
    midi.c             # MIDI output
```

## Data Storage

Sets are saved to `/data/UserData/move-anything-data/seqomd/sets.json`

Each set contains 16 tracks × 16 patterns × 16 steps with full parameter data.
