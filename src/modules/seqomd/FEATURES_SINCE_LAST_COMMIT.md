# SEQOMD Features Added Since Last Commit

## Overview
The sequencer was renamed from "Step Sequencer" to "SEQOMD" and received major feature additions inspired by OP-Z step components.

---

## 1. Shift Mode - Track Settings
**When:** Hold Shift while on a track

**Controls:**
- **Knob 7:** Track Speed - cycles through 9 speed options:
  - 1/4x, 1/3x, 1/2x, 2/3x, 1x (default), 3/2x, 2x, 3x, 4x
- **Knob 8:** MIDI Channel - changes the track's output channel (1-16)

**Display:** Shows small overlay with current channel and speed

---

## 2. CC Output from Knobs

### Pattern Mode (Menu button)
- All 8 knobs send **CC 1-8** on **channel 16** (master channel)
- Useful for controlling external effects/parameters

### Track Mode (normal view)
- **Knobs 1-2** send CCs on the track's MIDI channel
- CC numbers: `20 + (track * 2) + knobIndex`
  - Track 1: CC 20, 21
  - Track 2: CC 22, 23
  - ...
  - Track 8: CC 34, 35

---

## 3. Knob LED Indicators
Knob LEDs show different states based on mode:

- **Pattern Mode:** All 8 knobs lit in track color
- **Master Mode:** All 8 knobs lit in white (one per track)
- **Shift Mode:** Knobs 7-8 lit in track color (speed/channel)
- **Holding Step:**
  - Knobs 1-2: Lit if step has CC value
  - Knobs 7-8: Lit if step has ratchet/probability/condition
  - Others: Off

---

## 4. Per-Step CC Locks (Parameter Locks)

**How to use:**
1. Hold a step button
2. Turn knob 1 or 2 to set CC value for that step
3. When step triggers, CC is sent before the note

**Tap to clear:** Tap (touch without turning) knob 1 or 2 to clear CC from step

**Visual feedback:**
- Step LEDs show slightly different when step has CC locks
- Knob LEDs light up when holding a step with CC values

---

## 5. Probability (Knob 8 - Turn Down)

**When holding a step:**
- Turn knob 8 **counter-clockwise** to decrease probability
- Range: 100% down to 5% in **5% increments**
- Each trigger has X% chance to play

**Tap knob 8:** Clears the entire step (all notes, CCs, parameters)

---

## 6. Conditions (Knob 8 - Turn Up)

**When holding a step:**
- Turn knob 8 **clockwise** to cycle through conditions
- Conditions determine which loop iterations trigger the step

**Available conditions:**
```
--- (none/always)
1:2, 2:2           (every 2 loops)
1:3, 2:3, 3:3      (every 3 loops)
1:4, 2:4, 3:4, 4:4 (every 4 loops)
1:5, 2:5, 3:5, 4:5, 5:5 (every 5 loops)
1:6, 2:6, 3:6, 4:6, 5:6, 6:6 (every 6 loops)
1:8, 2:8, 3:8, 4:8, 5:8, 6:8, 7:8, 8:8 (every 8 loops)
```

**Example:** `2:4` means "play on the 2nd of every 4 loops"

---

## 7. Ratchet (Knob 7)

**When holding a step:**
- Turn knob 7 to set number of sub-triggers per step
- Options: **1x, 2x, 3x, 4x, 6x, 8x**

**Behavior:**
- Notes are triggered multiple times within the step
- Gate time is divided by ratchet count
- Creates rapid-fire/roll effects

**Tap knob 7:** Resets ratchet to 1x

---

## 8. Step Length (Note Length)

**How to set:**
1. Hold a step button
2. Press another step button **ahead** of it
3. Note length = distance between the two steps

**Example:** Hold step 1, press step 4 → Step 1 has length of 4 steps

**Toggle off:** Press the same length-end step again to reset to length 1

**Visual feedback:**
- Length "tails" shown in **Cyan** on step LEDs
- When holding a step, its length tail is highlighted

---

## 9. Module Renamed
- Changed from "Step Sequencer" to "SEQOMD"
- Updated in module.json, ui.js, and seq_plugin.c

---

## Technical Changes

### DSP (seq_plugin.c)
- Added fields to `step_t`: probability, condition_n, condition_m, condition_not, ratchet, length
- Added fields to `track_t`: loop_count, ratchet_count, ratchet_total, note_length_total, note_gate, note_length_phase
- Implemented probability check using xorshift32 PRNG
- Implemented condition check based on loop_count
- Implemented ratchet sub-triggers in render_block
- Implemented note length timing (notes sustain across multiple steps)
- Added parameter handlers for all new step parameters

### UI (ui.js)
- Added MoveTracks constant import
- Added SPEED_OPTIONS array (9 speed choices)
- Added RATCHET_VALUES array [1, 2, 3, 4, 6, 8]
- Added CONDITIONS array (33 conditions)
- Updated createEmptyStep() with new fields
- Added shift mode display and knob handling
- Added CC sending functions (sendCCExternal, updateAndSendCC)
- Added knob LED control (updateKnobLEDs)
- Added knob touch handling for tap-to-clear/reset
- Added step length visual display and interaction
- Fixed encoder delta calculation (128 - velocity for CCW)

### Shared (constants.mjs)
- Added MoveTracks export: [MoveRow4, MoveRow3, MoveRow2, MoveRow1]

---

## Removed Features
- PRE condition (first loop only) - removed
- FILL condition (last loop) - removed
- Condition NOT toggle (tap knob 8) - changed to clear step instead
