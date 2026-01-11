# Plan: Track Arpeggiator

**Status: IMPLEMENTED (DSP + JS UI complete)**

## Overview

Add arpeggiator functionality. When a step has multiple notes, the arp plays them in a pattern instead of as a chord.

- **Track-level default**: Each track has arp mode, speed, and octave settings
- **Per-step override**: Steps can override arp mode and speed (like ratchet)
- **Ratchet ignored**: When arp is active, ratchet setting is ignored
- **Note length**: Arp cycles through pattern across the full note length

---

## UI Interaction

### Entry (from Track View - Normal Mode)

1. Hold **Shift** → Step 11 UI lights up (White)
2. Press **Step 11** → Enter Arp Mode for current track
3. Step 11 UI shows **Cyan** when track has arp enabled (even when shift not held)

### Arp Mode Controls

| Control | Function |
|---------|----------|
| Knob 1 | Select arp mode (turn right from Off to cycle through modes) |
| Knob 2 | Arp speed/rate |
| Knob 3 | Octave range |
| Jog click | Exit arp mode |
| Back | Exit arp mode |

### Display

```
ARP MODE
Track X
Mode: Up | 1/4 | Oct: +1
Knobs to adjust
```

---

## Per-Step Arp Override

While holding a step in normal track mode, additional knobs control per-step arp:

| Knob | Function | Values |
|------|----------|--------|
| Knob 3 | Step arp mode | Track (default), Off, Up, Down... |
| Knob 4 | Step arp speed | Track (default), 1/1, 1/2... |

**Display when adjusting:**
```
Step X
Arp: Up (step)
Speed: 1/4 (track)
```

**LED indicators:**
- Knob 3/4 lit when step has arp override

---

## Arp Modes

| Index | Mode | Pattern (C-E-G-B) | Description |
|-------|------|-------------------|-------------|
| 0 | Off | C+E+G+B | Play as chord (no arp) |
| 1 | Up | C-E-G-B | Low to high |
| 2 | Down | B-G-E-C | High to low |
| 3 | Up-Down | C-E-G-B-G-E | Up then down, repeat endpoints |
| 4 | Down-Up | B-G-E-C-E-G | Down then up, repeat endpoints |
| 5 | Up & Down | C-E-G-B-B-G-E-C | Up then down, no repeat |
| 6 | Down & Up | B-G-E-C-C-E-G-B | Down then up, no repeat |
| 7 | Random | ? | Shuffle each cycle |
| 8 | Chord | C+E+G+B, C+E+G+B... | Repeated chord hits |
| 9 | Outside-In | B-C-G-E | High/low alternating inward |
| 10 | Inside-Out | E-G-C-B | Middle outward alternating |
| 11 | Converge | C-B-E-G | Low/high pairs moving in |
| 12 | Diverge | E-G-C-B | Middle expanding out |
| 13 | Thumb | C-C-E-C-G-C-B | Bass note pedal |
| 14 | Pinky | B-B-G-B-E-B-C | Top note pedal |

---

## Arp Speed Options

Speed is relative to step duration (default: 1/4):

| Index | Name | Division |
|-------|------|----------|
| 0 | 1/1 | One note per step |
| 1 | 1/2 | Two notes per step |
| 2 | 1/3 | Three notes per step |
| 3 | 1/4 | Four notes per step (default) |
| 4 | 1/6 | Six notes per step |
| 5 | 1/8 | Eight notes per step |

---

## Arp Octave Range

Extends the pattern across octaves:

| Index | Name | Description |
|-------|------|-------------|
| 0 | 0 | No octave extension |
| 1 | +1 | Add one octave up |
| 2 | +2 | Add two octaves up |
| 3 | -1 | Add one octave down |
| 4 | -2 | Add two octaves down |
| 5 | ±1 | Add one octave up and down |
| 6 | ±2 | Add two octaves up and down |

---

## Data Storage

### Track State (JS - `lib/data.js`)

Add to `createEmptyTrack()`:
```javascript
{
    // ... existing fields ...
    arpMode: 0,       // 0=Off, 1=Up, 2=Down, etc.
    arpSpeed: 3,      // Index into speed options (default 1/4)
    arpOctave: 0      // Index into octave options
}
```

### Step State (JS - `lib/data.js`)

Add to `createEmptyStep()`:
```javascript
{
    // ... existing fields ...
    arpMode: -1,      // -1=use track, 0+=override
    arpSpeed: -1      // -1=use track, 0+=override
}
```

### Track State (DSP - `seq_plugin.c`)

Add to `track_t`:
```c
uint8_t arp_mode;    /* 0=Off, 1=Up, 2=Down, etc. */
uint8_t arp_speed;   /* 0=1/1, 1=1/2, 2=1/3, etc. (default 3=1/4) */
uint8_t arp_octave;  /* 0=none, 1=+1, 2=+2, 3=-1, 4=-2, 5=±1, 6=±2 */
```

### Step State (DSP - `seq_plugin.c`)

Add to `step_t`:
```c
int8_t arp_mode;     /* -1=use track, 0+=override */
int8_t arp_speed;    /* -1=use track, 0+=override */
```

---

## DSP Implementation

### Arp Pattern Generation

```c
/* Arp mode constants */
#define ARP_OFF           0
#define ARP_UP            1
#define ARP_DOWN          2
#define ARP_UP_DOWN       3   /* Includes endpoints twice */
#define ARP_DOWN_UP       4   /* Includes endpoints twice */
#define ARP_UP_AND_DOWN   5   /* Excludes repeated endpoints */
#define ARP_DOWN_AND_UP   6   /* Excludes repeated endpoints */
#define ARP_RANDOM        7
#define ARP_CHORD         8   /* Repeated chord hits */
#define ARP_OUTSIDE_IN    9   /* High/low alternating inward */
#define ARP_INSIDE_OUT    10  /* Middle outward alternating */
#define ARP_CONVERGE      11  /* Low/high pairs moving in */
#define ARP_DIVERGE       12  /* Middle expanding out */
#define ARP_THUMB         13  /* Bass note pedal */
#define ARP_PINKY         14  /* Top note pedal */
#define NUM_ARP_MODES     15

/* Arp speed divisions (notes per step) */
static const int ARP_DIVISIONS[] = {1, 2, 3, 4, 6, 8};
#define NUM_ARP_SPEEDS    6
#define DEFAULT_ARP_SPEED 3  /* 1/4 = 4 notes per step */

/* Arp octave options */
#define ARP_OCT_NONE      0
#define ARP_OCT_UP1       1
#define ARP_OCT_UP2       2
#define ARP_OCT_DOWN1     3
#define ARP_OCT_DOWN2     4
#define ARP_OCT_BOTH1     5
#define ARP_OCT_BOTH2     6
#define NUM_ARP_OCTAVES   7

/* Generate arp pattern for given notes
 * Sorts notes by pitch first, then returns indices in arp order
 * Applies octave extension if arp_octave > 0
 * Returns pattern length
 */
static int generate_arp_pattern(uint8_t *notes, int num_notes, int arp_mode,
                                 int arp_octave, uint8_t *out_pattern, int max_len);
```

### Modified Note Scheduling

**Key point**: All arp logic happens in DSP's `schedule_step_notes()`. Arp notes go through the same `schedule_note()` function as regular notes, so they receive:
- Transpose from chord follow / transpose sequence
- Swing timing
- Proper note-on/note-off scheduling

In `schedule_step_notes()`:

```c
static void schedule_step_notes(track_t *track, int track_idx, step_t *step, double base_phase) {
    int num_notes = step->num_notes;
    int note_length = step->length > 0 ? step->length : 1;

    /* Get transpose for this track (from chord follow / transpose sequence) */
    uint32_t global_step = (uint32_t)g_global_phase;
    int transpose = g_chord_follow[track_idx] ? get_transpose_at_step(global_step) : 0;

    /* Resolve arp settings (step override or track default) */
    int arp_mode = step->arp_mode >= 0 ? step->arp_mode : track->arp_mode;
    int arp_speed = step->arp_speed >= 0 ? step->arp_speed : track->arp_speed;
    int arp_octave = track->arp_octave;  /* Octave is track-only */

    /* If arp is off or only 1 note, use normal scheduling (with ratchet) */
    if (arp_mode == ARP_OFF || num_notes <= 1) {
        /* Existing chord/ratchet scheduling code */
        return;
    }

    /* Arp is active - ignore ratchet */

    /* Generate arp pattern (with octave extension) */
    uint8_t arp_pattern[64];  /* Larger for octave extensions */
    int pattern_len = generate_arp_pattern(step->notes, num_notes,
                                            arp_mode, arp_octave, arp_pattern, 64);

    /* Calculate timing across full note length */
    int notes_per_step = ARP_DIVISIONS[arp_speed];
    int total_arp_notes = notes_per_step * note_length;
    double note_duration = (double)note_length / total_arp_notes;

    /* Schedule arp notes, cycling through pattern */
    for (int i = 0; i < total_arp_notes; i++) {
        double note_phase = base_phase + (i * note_duration);
        int pattern_idx = i % pattern_len;
        int note_value = arp_pattern[pattern_idx];  /* Base note with octave offset */

        /* Apply transpose (from chord follow / transpose sequence) */
        int transposed_note = note_value + transpose;
        if (transposed_note < 0) transposed_note = 0;
        if (transposed_note > 127) transposed_note = 127;

        schedule_note(
            transposed_note,
            step->velocity,
            track->midi_channel,
            track->swing,
            note_phase,
            note_duration,
            step->gate
        );
    }
}
```

### Parameter Handling

**Track-level params** (`track_X_arp_mode`, `track_X_arp_speed`, `track_X_arp_octave`):

```c
else if (strcmp(param, "arp_mode") == 0) {
    int mode = atoi(val);
    if (mode >= 0 && mode < NUM_ARP_MODES) {
        g_tracks[track].arp_mode = mode;
    }
}
else if (strcmp(param, "arp_speed") == 0) {
    int speed = atoi(val);
    if (speed >= 0 && speed < NUM_ARP_SPEEDS) {
        g_tracks[track].arp_speed = speed;
    }
}
else if (strcmp(param, "arp_octave") == 0) {
    int oct = atoi(val);
    if (oct >= 0 && oct < NUM_ARP_OCTAVES) {
        g_tracks[track].arp_octave = oct;
    }
}
```

**Step-level params** (`track_X_step_Y_arp_mode`, `track_X_step_Y_arp_speed`):

```c
else if (strcmp(param, "arp_mode") == 0) {
    int mode = atoi(val);
    if (mode >= -1 && mode < NUM_ARP_MODES) {
        pattern->steps[step].arp_mode = mode;  /* -1 = use track */
    }
}
else if (strcmp(param, "arp_speed") == 0) {
    int speed = atoi(val);
    if (speed >= -1 && speed < NUM_ARP_SPEEDS) {
        pattern->steps[step].arp_speed = speed;  /* -1 = use track */
    }
}
```

---

## JS Implementation

### Files to Modify

| File | Changes |
|------|---------|
| `lib/constants.js` | Add `ARP_MODES`, `ARP_SPEEDS`, `ARP_OCTAVES` arrays |
| `lib/data.js` | Add `arpMode`, `arpSpeed`, `arpOctave` to track; `arpMode`, `arpSpeed` to step |
| `lib/state.js` | Add `enterArpMode()`, `exitArpMode()` |
| `views/track.js` | Add Shift+Step11 handler, route to arp mode |
| `views/track/arp.js` | **NEW** - Arp mode UI (like speed.js) |
| `views/track/normal.js` | Add Step11 UI LED handling, per-step arp knobs (3/4) |

### Persistence

All arp changes must call `saveCurrentSetToDisk()`:

**Track-level (in arp.js):**
```javascript
/* On any knob change */
state.tracks[state.currentTrack].arpMode = newMode;
setParam(`track_${state.currentTrack}_arp_mode`, String(newMode));
saveCurrentSetToDisk();
```

**Step-level (in normal.js):**
```javascript
/* When adjusting step arp via Knob 3/4 while holding step */
const step = getCurrentPattern(state.currentTrack).steps[state.heldStep];
step.arpMode = newMode;
setParam(`track_${state.currentTrack}_step_${state.heldStep}_arp_mode`, String(newMode));
saveCurrentSetToDisk();
```

### New File: `views/track/arp.js`

```javascript
/*
 * Track View - Arp Mode
 * Adjust track arpeggiator settings via knobs
 */

import { MoveKnob1, MoveKnob2, MoveStep11UI, ... } from "...";
import { ARP_MODES, ARP_SPEEDS } from '../../lib/constants.js';

export function onInput(data) {
    /* Knob 1 - arp mode */
    if (isCC && note === MoveKnob1) {
        // Cycle through arp modes
    }

    /* Knob 2 - arp speed */
    if (isCC && note === MoveKnob2) {
        // Cycle through arp speeds
    }
}

export function updateLEDs() {
    /* Step 11 lit, Knobs 1-2 lit */
}

export function updateDisplayContent() {
    const modeName = ARP_MODES[state.tracks[state.currentTrack].arpMode].name;
    const speedName = ARP_SPEEDS[state.tracks[state.currentTrack].arpSpeed].name;
    displayMessage("ARP MODE", `Track ${state.currentTrack + 1}`,
                   `${modeName} | ${speedName}`, "Knobs 1-2 to adjust");
}
```

### Constants (`lib/constants.js`)

```javascript
export const ARP_MODES = [
    { name: 'Off' },
    { name: 'Up' },
    { name: 'Down' },
    { name: 'Up-Down' },
    { name: 'Down-Up' },
    { name: 'Up & Down' },
    { name: 'Down & Up' },
    { name: 'Random' },
    { name: 'Chord' },
    { name: 'Outside-In' },
    { name: 'Inside-Out' },
    { name: 'Converge' },
    { name: 'Diverge' },
    { name: 'Thumb' },
    { name: 'Pinky' }
];

export const ARP_SPEEDS = [
    { name: '1/1', div: 1 },
    { name: '1/2', div: 2 },
    { name: '1/3', div: 3 },
    { name: '1/4', div: 4 },
    { name: '1/6', div: 6 },
    { name: '1/8', div: 8 }
];

export const ARP_OCTAVES = [
    { name: '0' },
    { name: '+1' },
    { name: '+2' },
    { name: '-1' },
    { name: '-2' },
    { name: '±1' },
    { name: '±2' }
];

export const DEFAULT_ARP_SPEED = 3;  /* 1/4 */
```

---

## Implementation Order

1. **DSP Core**
   - Add `arp_mode`, `arp_speed` to `track_t`
   - Add `generate_arp_pattern()` function
   - Modify `schedule_step_notes()` for arp scheduling
   - Add param get/set handlers

2. **JS Data Layer**
   - Add constants for arp modes/speeds
   - Update `createEmptyTrack()` with arp fields
   - Update migration for existing sets

3. **JS UI Layer**
   - Add `enterArpMode()`, `exitArpMode()` to state.js
   - Create `views/track/arp.js`
   - Update `views/track.js` with Shift+Step11 handler
   - Update `views/track/normal.js` for Step11 UI LED

4. **Persistence**
   - Ensure arp settings save/load with track data

5. **Testing**
   - Test arp patterns with different note counts
   - Test speed divisions
   - Test interaction with ratchets
   - Test with transpose/chord follow

---

## Testing

Add tests to `dsp/test_seq_plugin.c`:

### Arp Pattern Tests

```c
TEST(arp_pattern_up) {
    /* 4 notes: C-E-G-B should produce 0-1-2-3 */
    uint8_t notes[] = {60, 64, 67, 71};
    uint8_t pattern[16];
    int len = generate_arp_pattern(notes, 4, ARP_UP, ARP_OCT_NONE, pattern, 16);
    ASSERT_EQ(len, 4);
    ASSERT_EQ(pattern[0], 60);
    ASSERT_EQ(pattern[1], 64);
    ASSERT_EQ(pattern[2], 67);
    ASSERT_EQ(pattern[3], 71);
}

TEST(arp_pattern_down) {
    uint8_t notes[] = {60, 64, 67, 71};
    uint8_t pattern[16];
    int len = generate_arp_pattern(notes, 4, ARP_DOWN, ARP_OCT_NONE, pattern, 16);
    ASSERT_EQ(len, 4);
    ASSERT_EQ(pattern[0], 71);
    ASSERT_EQ(pattern[3], 60);
}

TEST(arp_pattern_up_down) {
    /* Up-Down includes endpoints: C-E-G-B-G-E */
    uint8_t notes[] = {60, 64, 67, 71};
    uint8_t pattern[16];
    int len = generate_arp_pattern(notes, 4, ARP_UP_DOWN, ARP_OCT_NONE, pattern, 16);
    ASSERT_EQ(len, 6);
    ASSERT_EQ(pattern[0], 60);  /* C */
    ASSERT_EQ(pattern[3], 71);  /* B */
    ASSERT_EQ(pattern[4], 67);  /* G */
    ASSERT_EQ(pattern[5], 64);  /* E */
}

TEST(arp_pattern_up_and_down) {
    /* Up & Down excludes repeated endpoints: C-E-G-B-B-G-E-C */
    uint8_t notes[] = {60, 64, 67, 71};
    uint8_t pattern[16];
    int len = generate_arp_pattern(notes, 4, ARP_UP_AND_DOWN, ARP_OCT_NONE, pattern, 16);
    ASSERT_EQ(len, 8);
}

TEST(arp_pattern_thumb) {
    /* Thumb: bass pedal - C-C-E-C-G-C-B */
    uint8_t notes[] = {60, 64, 67, 71};
    uint8_t pattern[16];
    int len = generate_arp_pattern(notes, 4, ARP_THUMB, ARP_OCT_NONE, pattern, 16);
    ASSERT_EQ(pattern[0], 60);  /* C */
    ASSERT_EQ(pattern[1], 60);  /* C */
    ASSERT_EQ(pattern[2], 64);  /* E */
    ASSERT_EQ(pattern[3], 60);  /* C */
}

TEST(arp_pattern_octave_up1) {
    /* With +1 octave, pattern doubles in length */
    uint8_t notes[] = {60, 64, 67};
    uint8_t pattern[32];
    int len = generate_arp_pattern(notes, 3, ARP_UP, ARP_OCT_UP1, pattern, 32);
    ASSERT_EQ(len, 6);
    ASSERT_EQ(pattern[0], 60);
    ASSERT_EQ(pattern[3], 72);  /* C+12 */
}

TEST(arp_pattern_octave_both1) {
    /* With ±1 octave, pattern triples */
    uint8_t notes[] = {60, 64, 67};
    uint8_t pattern[32];
    int len = generate_arp_pattern(notes, 3, ARP_UP, ARP_OCT_BOTH1, pattern, 32);
    ASSERT_EQ(len, 9);  /* 3 notes × 3 octaves */
}
```

### Arp Scheduling Tests

```c
TEST(arp_scheduling_basic) {
    /* Set up track with arp */
    set_param("track_0_arp_mode", "1");  /* Up */
    set_param("track_0_arp_speed", "3"); /* 1/4 */

    /* Add step with 3 notes */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_add_note", "64");
    set_param("track_0_step_0_add_note", "67");

    /* Process and verify 4 notes scheduled per step */
    start_playback();
    process_frames(128);
    /* Verify note-on messages */
}

TEST(arp_with_note_length) {
    /* Step with length=2 should produce 8 arp notes at 1/4 speed */
    set_param("track_0_arp_mode", "1");
    set_param("track_0_arp_speed", "3");
    set_param("track_0_step_0_length", "2");
    /* ... verify 8 notes scheduled across 2 steps */
}

TEST(arp_step_override) {
    /* Track has Up, step overrides to Down */
    set_param("track_0_arp_mode", "1");  /* Up */
    set_param("track_0_step_0_arp_mode", "2");  /* Down */
    /* ... verify step uses Down pattern */
}

TEST(arp_ignores_ratchet) {
    /* When arp active, ratchet should be ignored */
    set_param("track_0_arp_mode", "1");
    set_param("track_0_step_0_ratchet", "4");
    /* ... verify only arp notes, not ratcheted */
}
```

### Arp + Swing Tests

```c
TEST(arp_with_swing) {
    /* Swing should apply to arp notes based on global beat position */
    set_param("track_0_arp_mode", "1");
    set_param("track_0_arp_speed", "1");  /* 1/2 = 2 notes per step */
    set_param("track_0_swing", "67");     /* Triplet swing */

    /* Add 2-note chord */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_add_note", "64");

    start_playback();
    process_frames(128);

    /* First arp note should be on beat (no swing delay) */
    /* Second arp note should have swing delay applied */
    /* Verify timing of scheduled notes */
}

TEST(arp_swing_across_steps) {
    /* Swing pattern should continue correctly across arp notes */
    set_param("track_0_arp_mode", "1");
    set_param("track_0_arp_speed", "3");  /* 1/4 = 4 notes per step */
    set_param("track_0_swing", "60");

    /* Process multiple steps and verify swing pattern */
}

TEST(arp_swing_with_note_length) {
    /* Note length + arp + swing */
    set_param("track_0_arp_mode", "1");
    set_param("track_0_arp_speed", "2");  /* 1/3 */
    set_param("track_0_swing", "67");
    set_param("track_0_step_0_length", "2");

    /* Verify swing applies correctly across extended note */
}
```

### Arp + Transpose Tests

```c
TEST(arp_with_transpose) {
    /* Arp notes should be transposed when chord_follow enabled */
    set_param("track_0_chord_follow", "1");
    set_param("track_0_arp_mode", "1");
    set_param("transpose_step_0", "5");  /* +5 semitones */

    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_add_note", "64");

    start_playback();
    process_frames(128);

    /* Verify notes are 65, 69 (transposed +5) */
}

TEST(arp_transpose_with_octave) {
    /* Arp octave extension + transpose */
    set_param("track_0_chord_follow", "1");
    set_param("track_0_arp_mode", "1");
    set_param("track_0_arp_octave", "1");  /* +1 octave */
    set_param("transpose_step_0", "3");

    /* Verify all octave notes are transposed */
}
```

---

## Edge Cases

| Case | Handling |
|------|----------|
| Single note step | No arp (play single note normally) |
| Arp on + Ratchet | Arp takes priority, ratchet ignored |
| Pattern longer than step | Cycle through pattern |
| Random mode | Generate new shuffle each step trigger |
| Chord mode | All notes together, repeated at arp speed |
| Note length > 1 step | Arp cycles across full note length |
| 2 notes with Up-Down | Pattern: 0-1-0 (3 notes) |
| Step override = -1 | Use track default |
| Step override >= 0 | Use step value |
| Octave ±2 with high notes | Clamp to MIDI 0-127 |

---

## Future Enhancements

- **Arp Gate**: Separate gate control for arp notes
- **Arp Hold**: Latch arp even after step ends

