# Move Sequencer - Full Architecture Plan

## Overview

An OP-Z inspired 8-track + master step sequencer for Ableton Move, acting as MIDI master for external devices (Ableton Live, VCV Rack, hardware synths).

## Track Structure

```
Tracks 1-4:  Drum/Percussion (default MIDI ch 1-4)
Tracks 5-8:  Melodic - Bass, Lead, Arp, Chord (default MIDI ch 5-8)
Track 9:     Master - transpose, chord progressions, global control
```

## Hardware Mapping

### Track Selection
```
Track buttons (notes 40-43):
  Press alone:       Select Track 1/2/3/4
  Shift + Press:     Select Track 5/6/7/8
  Shift + Loop:      Select Master track
```

### Step Editing (Hold step button at bottom)
```
Pads:               Select note (chromatic, 32 notes)
Knob 1 (CC 71):     Note length (1 step to 16 bars)
Knob 2 (CC 72):     Velocity (1-127)
Knob 3 (CC 73):     Probability (0-100%)
Knob 4 (CC 74):     Ratchet/repeats (1, 2, 3, 4, 6, 8)
Knob 5 (CC 75):     Condition (always, 1:2, 1:3, 1:4, 2:4, etc.)
Knob 6 (CC 76):     Swing offset (-50% to +50%)
Knob 7 (CC 77):     [Future: param lock target]
Knob 8 (CC 78):     [Future: param lock value]
```

### Track View (when track selected, not holding step)
```
Step buttons 1-16:  Show/edit pattern (lit = has note)
Pads Row 1:         Mute tracks 1-8
Pads Row 2:         Solo tracks 1-8
Pads Row 3:         [Future: pattern select 1-8]
Pads Row 4:         [Future: pattern select 9-16]

Knob 1:             Track length (1-64 steps)
Knob 2:             Track speed (1/4x, 1/2x, 1x, 2x, 4x)
Knob 3:             Track swing (0-75%)
Knob 4:             MIDI channel (1-16)
Knob 5-8:           [Future: track-level params]
```

### Master Track View
```
Pads Row 1:         Follow chord toggle (tracks 1-8)
                    Lit = track follows master transpose

Pads Row 2:         Track type selector (tracks 1-8)
                    Blue = Note, Green = Arp, Red = Drum, Yellow = Chord

Pads Row 3:         Root note (C, C#, D, D#, E, F, F#, G)
Pads Row 4:         Scale/Chord type (Maj, Min, Pent, Blues, etc.)

Step buttons:       Master transpose per step (+/- semitones)

Knob 1:             Global BPM (40-300)
Knob 2:             Global swing (0-75%)
Knob 3:             [Future: master filter]
Knob 4:             [Future: master drive]
```

### Transport & Global
```
Play (CC 85):       Start/Stop (sends MIDI Start/Stop)
Loop (CC 87):       Toggle clock output ON/OFF
Shift + Play:       [Future: pattern chain mode]
Shift + Capture:    Save pattern to slot
Shift + Jog:        Pattern select (rotate through slots)
```

## Data Structures

### Step
```c
typedef struct {
    uint8_t note;           // 0 = off, 1-127 = MIDI note
    uint8_t velocity;       // 1-127
    uint16_t length;        // in steps (1-256, where 16 = 1 bar)
    uint8_t probability;    // 0-100
    uint8_t ratchet;        // 1, 2, 3, 4, 6, 8
    uint8_t condition;      // enum: ALWAYS, EVERY_2, EVERY_3, EVERY_4, etc.
    int8_t swing_offset;    // -50 to +50 (percentage of step)
    // Future: param locks
} step_t;
```

### Track
```c
typedef struct {
    uint8_t type;           // TRACK_DRUM, TRACK_NOTE, TRACK_ARP, TRACK_CHORD
    uint8_t midi_channel;   // 0-15
    uint8_t length;         // 1-64 steps
    uint8_t speed;          // enum: SPEED_1_4, SPEED_1_2, SPEED_1, SPEED_2, SPEED_4
    uint8_t swing;          // 0-75
    uint8_t follow_chord;   // 0 or 1
    uint8_t muted;          // 0 or 1
    uint8_t solo;           // 0 or 1
    step_t steps[64];       // max 64 steps per track
} track_t;
```

### Pattern
```c
typedef struct {
    char name[32];
    uint16_t bpm;           // 40-300
    uint8_t swing;          // global swing 0-75
    uint8_t root_note;      // 0-11 (C to B)
    uint8_t scale;          // enum: MAJOR, MINOR, PENT, BLUES, etc.
    track_t tracks[8];      // 8 audio tracks
    int8_t master_transpose[16]; // per-step transpose
} pattern_t;
```

### Pending Notes (for overlapping note lengths)
```c
typedef struct {
    uint8_t note;
    uint8_t channel;
    double off_phase;       // when to send note-off
    uint8_t active;
} pending_note_t;

#define MAX_PENDING_NOTES 64
```

## Timing System

### Phase Accumulators (drift-free)
```c
// Per track (allows different speeds)
double track_phase[8];      // 0.0 to 1.0 per step

// Global
double clock_phase;         // for MIDI clock output
double master_phase;        // for master track timing

// Phase increment per sample:
// step_inc = (bpm * 4 * speed_mult) / (sample_rate * 60)
// clock_inc = (bpm * 24) / (sample_rate * 60)
```

### Note Length Timing
```
1 step   = phase increment of 1.0
4 steps  = 4.0 phase units
16 steps = 16.0 (1 bar)
256 steps = 256.0 (16 bars max)

Note-off scheduled at: trigger_phase + (length * step_phase_duration)
```

### Conditions (Spark-style)
```c
typedef enum {
    COND_ALWAYS = 0,    // play every time
    COND_1_2,           // play 1st of every 2 loops
    COND_2_2,           // play 2nd of every 2 loops
    COND_1_3,           // play 1st of every 3 loops
    COND_1_4,           // play 1st of every 4 loops
    COND_2_4,           // play 2nd of every 4 loops
    COND_3_4,           // play 3rd of every 4 loops
    COND_4_4,           // play 4th of every 4 loops
    COND_RANDOM_50,     // 50% chance
    COND_RANDOM_25,     // 25% chance
    COND_RANDOM_75,     // 75% chance
} condition_t;

// Track loop count to evaluate conditions
uint32_t track_loop_count[8];
```

## File Storage

### Directory Structure
```
/data/UserData/move-anything/modules/sequencer/
  patterns/
    pattern_01.json
    pattern_02.json
    ...
    pattern_16.json
  autosave.json         # auto-saved on changes
  settings.json         # global settings (last pattern, etc.)
```

### Pattern JSON Format
```json
{
  "name": "My Pattern",
  "bpm": 120,
  "swing": 0,
  "root": "C",
  "scale": "major",
  "tracks": [
    {
      "type": "drum",
      "channel": 1,
      "length": 16,
      "speed": 1.0,
      "swing": 0,
      "followChord": false,
      "muted": false,
      "steps": [
        {"note": 36, "vel": 100, "len": 1, "prob": 100, "ratch": 1, "cond": "always"},
        {"note": 0},
        {"note": 38, "vel": 80, "len": 1, "prob": 100, "ratch": 1, "cond": "always"},
        ...
      ]
    },
    ...
  ],
  "masterTranspose": [0, 0, 0, 0, 0, 0, 0, 0, 5, 5, 5, 5, 3, 3, 3, 3]
}
```

## Implementation Phases

### Phase 1: Multi-track Foundation
- [ ] Expand DSP to handle 8 tracks
- [ ] Add track selection UI (track buttons + shift)
- [ ] Per-track step arrays
- [ ] Per-track phase accumulators (independent timing)
- [ ] Per-track MIDI channel output

### Phase 2: Step Parameters
- [ ] Note length (1 step to 16 bars)
- [ ] Pending note system for overlapping notes
- [ ] Velocity per step
- [ ] Knob editing while holding step

### Phase 3: Step Components
- [ ] Probability (0-100%)
- [ ] Ratchet/repeats
- [ ] Conditions (1:2, 1:4, etc.)
- [ ] Per-step swing offset

### Phase 4: Track Parameters
- [ ] Track length (1-64 steps)
- [ ] Track speed multiplier
- [ ] Track swing
- [ ] Mute/Solo

### Phase 5: Master Track
- [ ] Master track UI
- [ ] Follow chord toggle per track
- [ ] Root note / scale selection
- [ ] Master transpose per step
- [ ] Chord-aware transposition for following tracks

### Phase 6: Save/Load
- [ ] JSON serialization
- [ ] Pattern slots (16)
- [ ] Auto-save
- [ ] Save/Load UI

### Phase 7: Track Types
- [ ] Drum mode (no transpose)
- [ ] Note mode (standard melodic)
- [ ] Arp mode (auto-arpeggiate chord)
- [ ] Chord mode (play full chords)

### Phase 8: Polish
- [ ] LED feedback for all modes
- [ ] Display showing current state
- [ ] Pattern chaining
- [ ] Copy/paste steps and tracks

## UI State Machine

```
                    ┌─────────────────┐
                    │   TRACK VIEW    │
                    │  (default mode) │
                    └────────┬────────┘
                             │
         ┌───────────────────┼───────────────────┐
         │                   │                   │
         ▼                   ▼                   ▼
┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
│   STEP EDIT     │ │  MASTER VIEW    │ │  PATTERN VIEW   │
│ (hold step btn) │ │ (shift+loop)    │ │ (shift+capture) │
└─────────────────┘ └─────────────────┘ └─────────────────┘
```

## Display Layout

### Track View
```
┌────────────────────────┐
│ Track 1: Kick    ch:1  │
│ Len:16  Spd:1x  Swg:0% │
│ ████░░░░████░░░░       │  <- step visualization
│ BPM:120  [PLAYING]     │
└────────────────────────┘
```

### Step Edit View
```
┌────────────────────────┐
│ Step 5 - Track 1       │
│ Note: C3   Vel: 100    │
│ Len: 2 steps  Prob:80% │
│ Ratch: 4x  Cond: 1:2   │
└────────────────────────┘
```

### Master View
```
┌────────────────────────┐
│ MASTER  Root: C  Major │
│ Follow: 1 2 . 4 5 . 7 8│  <- dots = not following
│ Types:  D D D D N A N C│  <- D=drum, N=note, A=arp, C=chord
│ Trans:  0 0 0 0 5 5 3 3│  <- transpose per step (partial)
└────────────────────────┘
```

## Open Questions

1. **Arp mode implementation**: How to handle arp patterns? Speed? Direction?
2. **Chord mode**: How many notes? Voicing options?
3. **Pattern chaining**: How to program chains? Song mode?
4. **Parameter locks**: Which parameters? How to select?
5. **Copy/paste**: Step-level? Track-level? Pattern-level?
6. **Undo**: How much history? Memory constraints?

## References

- [OP-Z Step Components](https://teenage.engineering/guides/op-z/step-components)
- [OP-Z Tracks](https://teenage.engineering/guides/op-z/tracks)
- [OP-Z Master Track](https://teenage.engineering/guides/op-z/master)
- [Elektron Conditional Trigs](https://www.elektronauts.com/t/a-]guide-to-conditional-trigs/42760)
