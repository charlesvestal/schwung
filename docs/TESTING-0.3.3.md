# Testing Procedure for v0.3.3

## Prerequisites
1. Build and install: `./scripts/build.sh && ./scripts/install.sh local`
2. Reboot Move or restart move-anything service
3. Enter Shadow UI mode: Hold Shift, press Volume knob, then Knob 8

---

## 1. MIDI FX System (NEW)

### 1.1 Arpeggiator Module
**Setup:** Create a test patch file at `/data/UserData/move-anything/patches/arp_test.json`:
```json
{
    "name": "Arp Test",
    "synth": {"module": "sf2", "preset": 0},
    "midi_fx": [
        {"type": "arp", "mode": "up", "bpm": "120", "division": "1/16"}
    ]
}
```

**Test Steps:**
1. [ ] Load Signal Chain module
2. [ ] Navigate to patches and load "Arp Test"
3. [ ] Hold a pad - should hear arpeggiated notes going up
4. [ ] Hold multiple pads - arp should cycle through all held notes
5. [ ] Release all pads - arp should stop, last note should release

**Arp Modes to Test:**
- [ ] `"mode": "up"` - notes ascend
- [ ] `"mode": "down"` - notes descend
- [ ] `"mode": "up_down"` - notes bounce up then down
- [ ] `"mode": "random"` - random note order

### 1.2 Chord Module
**Setup:** Create test patch `/data/UserData/move-anything/patches/chord_test.json`:
```json
{
    "name": "Chord Test",
    "synth": {"module": "sf2", "preset": 0},
    "midi_fx": [
        {"type": "chord", "chord_type": "major"}
    ]
}
```

**Test Steps:**
1. [ ] Load "Chord Test" patch
2. [ ] Play single pad - should hear major chord (root + 3rd + 5th)
3. [ ] Verify note-off releases all chord notes

**Chord Types to Test:**
- [ ] `"chord_type": "major"` - root, +4, +7 semitones
- [ ] `"chord_type": "minor"` - root, +3, +7 semitones
- [ ] `"chord_type": "power"` - root, +7 semitones
- [ ] `"chord_type": "octave"` - root, +12 semitones
- [ ] `"chord_type": "none"` - pass-through (no chord)

### 1.3 MIDI FX Chain (Chord + Arp)
**Setup:** Create test patch combining both:
```json
{
    "name": "Chord Arp Test",
    "synth": {"module": "sf2", "preset": 0},
    "midi_fx": [
        {"type": "chord", "chord_type": "minor"},
        {"type": "arp", "mode": "up", "bpm": "100", "division": "1/8"}
    ]
}
```

**Test Steps:**
1. [ ] Load patch - chord should generate notes, arp should arpeggiate them
2. [ ] Hold one pad - should hear minor chord notes arpeggiated
3. [ ] Verify correct note count (3 notes for minor chord)

---

## 2. Master Preset CRUD

### 2.1 Save Master Preset
1. [ ] Go to Master FX slot (bottom of slots list)
2. [ ] Load some FX modules (e.g., freeverb, cloudseed)
3. [ ] Adjust parameters with knobs
4. [ ] Press Shift+Select to enter save mode
5. [ ] Enter name using text entry
6. [ ] Confirm save
7. [ ] Verify preset appears in preset list

### 2.2 Load Master Preset
1. [ ] Navigate to Master FX presets
2. [ ] Select a saved preset
3. [ ] Verify FX modules load correctly
4. [ ] Verify parameter values restored

### 2.3 Delete Master Preset
1. [ ] Navigate to a saved master preset
2. [ ] Use delete function (Shift+Back or menu option)
3. [ ] Confirm deletion
4. [ ] Verify preset removed from list

### 2.4 Default Master Presets
1. [ ] Check that default presets exist: "Subtle Verb", "Lo-Fi Master", "Big Room"
2. [ ] Load each and verify they work

---

## 3. Slot Preset CRUD

### 3.1 Save Slot Preset
1. [ ] Select a chain slot (not Master FX)
2. [ ] Configure synth and FX
3. [ ] Use Shift+Select to save
4. [ ] Enter preset name
5. [ ] Verify saved to slot presets

### 3.2 Load Slot Preset
1. [ ] Navigate to slot's preset browser
2. [ ] Select a preset
3. [ ] Verify synth, FX, and knob mappings load

### 3.3 New Slot Preset (Clear)
1. [ ] With modules loaded, select "New Preset"
2. [ ] Verify all modules cleared
3. [ ] Verify knob mappings cleared

### 3.4 Delete Slot Preset
1. [ ] Navigate to a saved slot preset
2. [ ] Delete it
3. [ ] Verify removed from list

---

## 4. Knob Functionality

### 4.1 Global Slot Knob Mappings
1. [ ] Load a patch with knob_mappings defined
2. [ ] Turn mapped knobs - parameters should change
3. [ ] Touch knob (don't turn) - overlay shows current value
4. [ ] Turn unmapped knob - overlay shows "not mapped"

### 4.2 Shift+Knob Parameter Peek
1. [ ] Hold Shift and turn a knob
2. [ ] Verify overlay shows parameter name and value
3. [ ] Release Shift - overlay should dismiss

### 4.3 Hierarchy Editor Knobs
1. [ ] Enter hierarchy editor for a component
2. [ ] Navigate to a parameter level
3. [ ] Turn knobs - mapped params should adjust
4. [ ] Verify overlay shows correct param names

---

## 5. Synth Activation

### 5.1 All Sound Generators
Test each available synth module loads and produces sound:
- [ ] sf2 (SoundFont) - load preset, play pads
- [ ] dexed (Dexed FM) - load preset, play pads
- [ ] minijv (Mini-JV rompler) - load preset, play pads
- [ ] obxd (OB-X) - load preset, play pads
- [ ] linein (Line In) - connect audio, verify passthrough
- [ ] clap (CLAP host) - if available, load plugin

### 5.2 Synth Switching
1. [ ] Load synth A, play notes
2. [ ] Switch to synth B without stopping
3. [ ] Verify clean transition (no stuck notes)
4. [ ] Play notes on synth B

---

## 6. Audio FX Chain

### 6.1 FX Loading
1. [ ] Add freeverb to FX1 slot
2. [ ] Play synth - verify reverb applied
3. [ ] Add another FX to FX2
4. [ ] Verify both FX process in series

### 6.2 FX Parameter Control
1. [ ] Adjust FX parameters via hierarchy editor
2. [ ] Verify audio changes match parameter changes
3. [ ] Map FX param to knob, adjust via knob

### 6.3 Master FX
1. [ ] Load FX in Master FX slots (fx1-fx4)
2. [ ] Verify Master FX applies to all slot outputs
3. [ ] Adjust Master FX params

---

## 7. Stability Tests

### 7.1 Audio Dropout Test
1. [ ] Load complex patch (synth + 2 audio FX + MIDI FX)
2. [ ] Play continuously for 2+ minutes
3. [ ] Navigate UI while playing
4. [ ] [ ] No audio dropouts or glitches

### 7.2 Rapid Pad Test
1. [ ] Rapidly press/release multiple pads
2. [ ] Continue for 30+ seconds
3. [ ] [ ] No crashes or freezes
4. [ ] [ ] All notes release properly

### 7.3 MIDI Routing Isolation
1. [ ] In Shadow UI mode, turn knobs
2. [ ] [ ] Knob touches (notes 0-9) should NOT trigger synth
3. [ ] [ ] Knob CCs (71-78) should NOT trigger sounds
4. [ ] Play pads - should trigger synth normally

### 7.4 Patch Switching Stress
1. [ ] Rapidly switch between patches (10+ times)
2. [ ] [ ] No stuck notes
3. [ ] [ ] No crashes
4. [ ] [ ] Audio resumes correctly after each switch

---

## 8. Navigation & UI

### 8.1 Back Navigation
1. [ ] Deep navigate: Slots → Slot → Chain Edit → Component → Hierarchy
2. [ ] Press Back repeatedly
3. [ ] Verify correct navigation back through each level

### 8.2 Slot Selection
1. [ ] Use Track buttons (CC 40-43) to select slots
2. [ ] Verify correct slot highlights
3. [ ] Verify knob context switches to selected slot

### 8.3 Jog Wheel
1. [ ] In list views, jog up/down - selection moves
2. [ ] In parameter edit, jog - value changes
3. [ ] Click jog - confirms selection

---

## Test Results Summary

| Category | Pass | Fail | Notes |
|----------|------|------|-------|
| MIDI FX - Arp | | | |
| MIDI FX - Chord | | | |
| MIDI FX - Chain | | | |
| Master Preset CRUD | | | |
| Slot Preset CRUD | | | |
| Knob Functionality | | | |
| Synth Activation | | | |
| Audio FX Chain | | | |
| Stability | | | |
| Navigation | | | |

**Tested By:** _______________
**Date:** _______________
**Build Version:** _______________
