# Generate Pattern Mode - Implementation Plan

## Overview

A pattern generator that creates musically interesting patterns using algorithmic approaches. Accessed via **Shift + Step 14** in track view.

## User Flow

1. **Enter Mode**: Hold Shift, press Step 14 (which lights up while Shift held)
2. **Select Style**: Turn jog wheel to cycle through generation styles
3. **Adjust Parameters**: Use Knobs 1-8 to set generation parameters
4. **Generate**: Press jog wheel to create pattern
5. **Exit**: Press Back to exit without generating

### Pattern Placement Logic

- If current pattern is **empty** → generate on current pattern
- If current pattern has **content** → generate on next pattern and switch to it

---

## Display Layout

4 lines, 2 parameters each:

```
Len:16  Dns:50%
Voc:3   Scl:Min
Oct:3   Rng:2
Roo:C   Var:45
```

When turning jog wheel, briefly flash style name:

```
>>> EUCLIDEAN <<<
```

---

## Parameters (Knobs 1-8)

| Knob | Short | Parameter | Range | Default |
|------|-------|-----------|-------|---------|
| 1 | Len | Length | 1-64 steps | 16 |
| 2 | Dns | Density | 5-100% | 50% |
| 3 | Voc | Voices | 1-7 notes per step | 1 |
| 4 | Scl | Scale | 17 options | Detected |
| 5 | Roo | Root | Auto + C-B (13 options) | Auto |
| 6 | Oct | Octave | 1-6 (base octave) | 3 |
| 7 | Rng | Range | 1-4 octaves | 2 |
| 8 | Var | Variation | 0-127 (style-specific) | 64 |

### Scale Options (Knob 4)

| Index | Name | Notes |
|-------|------|-------|
| 0 | Detected | Use scale from chordFollow tracks |
| 1 | Chromatic | All 12 notes |
| 2 | Minor Penta | [0, 3, 5, 7, 10] |
| 3 | Major Penta | [0, 2, 4, 7, 9] |
| 4 | Blues | [0, 3, 5, 6, 7, 10] |
| 5 | Major | [0, 2, 4, 5, 7, 9, 11] |
| 6 | Natural Minor | [0, 2, 3, 5, 7, 8, 10] |
| 7 | Dorian | [0, 2, 3, 5, 7, 9, 10] |
| 8 | Mixolydian | [0, 2, 4, 5, 7, 9, 10] |
| 9 | Phrygian | [0, 1, 3, 5, 7, 8, 10] |
| 10 | Lydian | [0, 2, 4, 6, 7, 9, 11] |
| 11 | Locrian | [0, 1, 3, 5, 6, 8, 10] |
| 12 | Harmonic Minor | [0, 2, 3, 5, 7, 8, 11] |
| 13 | Melodic Minor | [0, 2, 3, 5, 7, 9, 11] |
| 14 | Whole Tone | [0, 2, 4, 6, 8, 10] |
| 15 | Dim HW | [0, 1, 3, 4, 6, 7, 9, 10] |
| 16 | Dim WH | [0, 2, 3, 5, 6, 8, 9, 11] |

### Root Options (Knob 5)

| Index | Value |
|-------|-------|
| 0 | Auto (use detected root) |
| 1-12 | C, C#, D, D#, E, F, F#, G, G#, A, A#, B |

---

## Generation Styles (Jog Wheel)

| Index | Style | Short | Description |
|-------|-------|-------|-------------|
| 0 | Random | RND | Pure random within constraints |
| 1 | Euclidean | EUC | Mathematically even note distribution |
| 2 | Rising | RIS | Notes trend from low to high |
| 3 | Falling | FAL | Notes trend from high to low |
| 4 | Arc | ARC | Low → high → low melodic contour |
| 5 | Pulse | PLS | Emphasis on downbeats (1, 5, 9, 13) |
| 6 | Offbeat | OFF | Syncopated, avoids downbeats |
| 7 | Clustered | CLU | Groups of notes with gaps between |

### How Variation (Knob 8) Affects Each Style

| Style | Variation Controls |
|-------|-------------------|
| Random | Random seed - turn to regenerate with new random values |
| Euclidean | Rotation - shifts pattern start position (0-15 offset) |
| Rising | Steepness - 0=gradual linear, 127=exponential jump |
| Falling | Steepness - same as rising |
| Arc | Peak position - 0=early peak, 64=middle, 127=late peak |
| Pulse | Accent strength - higher = more velocity on downbeats |
| Offbeat | Swing amount - 0=straight, 127=heavy swing |
| Clustered | Cluster size - 0=small bursts (2-3), 127=long runs (6-8) |

---

## Algorithm Details

### Common Logic (All Styles)

```javascript
// 1. Determine which steps get notes (rhythm)
const activeSteps = calculateActiveSteps(length, density, style, variation);

// 2. For each active step, generate notes
for (const stepIdx of activeSteps) {
    const notes = generateNotes(stepIdx, voices, scale, root, octave, range, style, variation);
    const velocities = generateVelocities(notes.length, stepIdx, style, variation);

    pattern.steps[stepIdx].notes = notes;
    pattern.steps[stepIdx].velocities = velocities;
}
```

### Euclidean Algorithm

The Euclidean algorithm distributes `k` hits over `n` steps as evenly as possible. Famous for producing musical rhythms found worldwide.

```javascript
function euclidean(hits, steps) {
    // Bjorklund's algorithm
    // E(3,8) = [1,0,0,1,0,0,1,0] - Cuban tresillo
    // E(5,8) = [1,0,1,1,0,1,1,0] - Cuban cinquillo
    // E(7,16) = [1,0,1,1,0,1,0,1,1,0,1,0,1,1,0,1] - Brazilian samba

    let pattern = [];
    let counts = [];
    let remainders = [];

    let divisor = steps - hits;
    remainders.push(hits);
    let level = 0;

    while (remainders[level] > 1) {
        counts.push(Math.floor(divisor / remainders[level]));
        remainders.push(divisor % remainders[level]);
        divisor = remainders[level];
        level++;
    }
    counts.push(divisor);

    function build(level) {
        if (level === -1) {
            pattern.push(0);
        } else if (level === -2) {
            pattern.push(1);
        } else {
            for (let i = 0; i < counts[level]; i++) {
                build(level - 1);
            }
            if (remainders[level] !== 0) {
                build(level - 2);
            }
        }
    }

    build(level);
    return pattern; // Array of 0s and 1s
}

// Apply rotation based on Variation knob
function rotatePattern(pattern, rotation) {
    const offset = Math.floor((rotation / 127) * pattern.length);
    return [...pattern.slice(offset), ...pattern.slice(0, offset)];
}
```

### Note Generation Within Scale

```javascript
function generateNotes(stepIdx, voices, scaleNotes, root, baseOctave, octaveRange, style, variation) {
    const notes = [];
    const lowMidi = (baseOctave + 1) * 12 + root;  // e.g., octave 3, root C = MIDI 48
    const highMidi = lowMidi + (octaveRange * 12) - 1;

    // Get all valid MIDI notes in range that are in scale
    const validNotes = [];
    for (let midi = lowMidi; midi <= highMidi; midi++) {
        const pitchClass = (midi - root + 120) % 12;  // Normalize to 0-11 relative to root
        if (scaleNotes.includes(pitchClass)) {
            validNotes.push(midi);
        }
    }

    // Select notes based on style
    switch (style) {
        case 'rising':
            // Bias toward lower notes early, higher notes late
            const risePosition = stepIdx / length;  // 0.0 to 1.0
            const riseCenter = Math.floor(risePosition * validNotes.length);
            return selectNotesNear(validNotes, riseCenter, voices);

        case 'falling':
            const fallPosition = 1 - (stepIdx / length);
            const fallCenter = Math.floor(fallPosition * validNotes.length);
            return selectNotesNear(validNotes, fallCenter, voices);

        case 'arc':
            // Peak position based on variation
            const peakStep = Math.floor((variation / 127) * length);
            const arcPosition = stepIdx <= peakStep
                ? stepIdx / peakStep
                : 1 - ((stepIdx - peakStep) / (length - peakStep));
            const arcCenter = Math.floor(arcPosition * validNotes.length);
            return selectNotesNear(validNotes, arcCenter, voices);

        default:
            // Random selection from valid notes
            return selectRandomNotes(validNotes, voices);
    }
}

function selectNotesNear(validNotes, centerIdx, count) {
    // Select notes clustered around a center point
    // Good for chord voicings and melodic movement
    const notes = [];
    const used = new Set();

    for (let i = 0; i < count && notes.length < validNotes.length; i++) {
        // Spread notes around center with some randomness
        let idx = centerIdx + Math.floor((Math.random() - 0.5) * 8);
        idx = Math.max(0, Math.min(validNotes.length - 1, idx));

        // Find nearest unused note
        for (let offset = 0; offset < validNotes.length; offset++) {
            const tryIdx = (idx + offset) % validNotes.length;
            if (!used.has(tryIdx)) {
                notes.push(validNotes[tryIdx]);
                used.add(tryIdx);
                break;
            }
        }
    }

    return notes.sort((a, b) => a - b);  // Return sorted low to high
}
```

### Velocity Generation

```javascript
function generateVelocities(noteCount, stepIdx, length, style, variation) {
    const velocities = [];
    const baseVelocity = 90;
    const variance = 25;

    for (let i = 0; i < noteCount; i++) {
        let vel = baseVelocity;

        switch (style) {
            case 'pulse':
                // Accent downbeats
                const isDownbeat = stepIdx % 4 === 0;
                const accentAmount = (variation / 127) * 30;
                vel = isDownbeat ? baseVelocity + accentAmount : baseVelocity - accentAmount/2;
                break;

            case 'rising':
                // Crescendo
                vel = 60 + (stepIdx / length) * 60;
                break;

            case 'falling':
                // Decrescendo
                vel = 120 - (stepIdx / length) * 60;
                break;

            default:
                // Random variation
                vel = baseVelocity + (Math.random() - 0.5) * variance;
        }

        // Add slight random humanization
        vel += (Math.random() - 0.5) * 10;

        // Clamp to valid MIDI range
        velocities.push(Math.max(1, Math.min(127, Math.round(vel))));
    }

    return velocities;
}
```

### Clustered Style Algorithm

```javascript
function generateClusteredSteps(length, density, variation) {
    const activeSteps = [];
    const totalNotes = Math.floor((density / 100) * length);

    // Cluster size based on variation (2-8 notes per cluster)
    const minCluster = 2;
    const maxCluster = 2 + Math.floor((variation / 127) * 6);

    let remaining = totalNotes;
    let position = 0;

    while (remaining > 0 && position < length) {
        // Random cluster size
        const clusterSize = Math.min(
            remaining,
            minCluster + Math.floor(Math.random() * (maxCluster - minCluster + 1))
        );

        // Add cluster
        for (let i = 0; i < clusterSize && position < length; i++) {
            activeSteps.push(position++);
            remaining--;
        }

        // Gap after cluster (2-6 steps)
        const gap = 2 + Math.floor(Math.random() * 5);
        position += gap;
    }

    return activeSteps;
}
```

---

## Implementation Structure

### New Files

```
src/modules/seqomd/
  lib/
    generator.js          # Pattern generation algorithms
  views/
    track/
      generate.js         # Generate mode UI view
```

### State Additions (state.js)

```javascript
// Generate mode state
generateMode: false,
generateStyle: 0,           // Index into STYLES array
generateParams: {
    length: 16,
    density: 50,
    voices: 1,
    scale: 0,               // 0 = Detected
    root: 0,                // 0 = Auto
    octave: 3,
    range: 2,
    variation: 64
}
```

### Constants Additions (constants.js)

```javascript
export const GENERATE_STYLES = [
    { name: 'Random', short: 'RND' },
    { name: 'Euclidean', short: 'EUC' },
    { name: 'Rising', short: 'RIS' },
    { name: 'Falling', short: 'FAL' },
    { name: 'Arc', short: 'ARC' },
    { name: 'Pulse', short: 'PLS' },
    { name: 'Offbeat', short: 'OFF' },
    { name: 'Clustered', short: 'CLU' }
];

export const GENERATOR_SCALES = [
    { name: 'Detected', short: 'Det', notes: null },
    { name: 'Chromatic', short: 'Chr', notes: [0,1,2,3,4,5,6,7,8,9,10,11] },
    { name: 'Minor Penta', short: 'mPn', notes: [0, 3, 5, 7, 10] },
    { name: 'Major Penta', short: 'MPn', notes: [0, 2, 4, 7, 9] },
    // ... etc
];

export const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
```

### View Integration

In `views/track/normal.js`, when Shift is held:
- Light up Step 14 with a distinct color (e.g., `HotMagenta`)
- On Step 14 press: enter generate mode

In generate mode view (`views/track/generate.js`):
- Handle jog wheel for style selection
- Handle knob changes for parameters
- Handle jog press for generation
- Handle Back for exit

---

## LED Feedback

### When Shift Held (in track view)
- Step 14 lights up `HotMagenta` to indicate generate mode available

### In Generate Mode
- All step LEDs off initially
- Knob LEDs show parameter colors:
  - Knob 1-2: Cyan (rhythm params)
  - Knob 3: Green (voices)
  - Knob 4-5: Purple (scale/root)
  - Knob 6-7: Yellow (range)
  - Knob 8: HotMagenta (variation)

### After Generation (Preview)
- Generated steps light up in track color
- Brief flash to confirm generation

---

## Edge Cases

1. **Density 100% + Length 64**: All 64 steps filled - OK
2. **Density 5% + Length 4**: Only ~0.2 steps, round up to 1 step minimum
3. **Voices 7 + Small range**: May not have 7 unique notes available, use what's available
4. **Detected scale with no notes**: Fall back to chromatic
5. **Current pattern at index 15 with content**: Wrap to pattern 0? Or stay at 15?

---

## Testing Ideas

1. Generate with Euclidean E(3,8) - should match tresillo rhythm
2. Generate Rising with low density - notes should trend upward
3. Generate with detected scale - should use correct notes from chordFollow
4. Generate when pattern has content - should go to next pattern
5. Variation knob on Euclidean - should rotate pattern

---

## Future Enhancements (Not in v1)

- Save favorite presets
- Generate to multiple patterns at once
- Velocity curves (not just random)
- Ratchet probability
- Generate CC automation
- Style-specific sub-modes (e.g., different Euclidean presets like "Tresillo", "Cinquillo")
