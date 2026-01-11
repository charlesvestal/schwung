# SEQOMD Sequencer Documentation

A 16-track, 16-pattern MIDI step sequencer for Ableton Move.

---

## Overview

- **16 Tracks** - Each with independent MIDI channel, speed, swing
- **16 Patterns per Track** - Switchable patterns with loop points
- **16 Steps per Pattern** - Notes, CCs, probability, conditions, ratchets
- **32 Sets** - Save/load complete song states

---

## Navigation

```
Set View (startup)
    │
    ├── [Pad] ──────────────────────► Track View (main editing)
    │                                      │
    │   ┌──────────────────────────────────┼──────────────────────────────────┐
    │   │                                  │                                  │
    │   ▼                                  ▼                                  ▼
    │ [Menu] ◄─────────────────► Pattern View          [Shift+Menu] ◄──► Master View
    │                                                                         │
    │                                                                    [Shift+Step5]
    │                                                                         ▼
    │                                                                    BPM Mode
    │
    └─────────────────────────────── [Shift+Step1] ◄── Track View
```

| From | Action | To |
|------|--------|-----|
| Set | Tap pad | Track (loads set) |
| Track | Menu | Pattern |
| Track | Shift+Menu | Master |
| Track | Shift+Step1 | Set (saves set) |
| Pattern/Master | Back | Track |
| Master | Shift+Step5 | BPM Mode |
| BPM Mode | Back | Master |

---

## Set View

Select which set (song) to work on. 32 sets displayed on pads.

### Pads (32)
| Color | Meaning |
|-------|---------|
| Cyan | Currently loaded set |
| Track color | Set has content |
| Black | Empty set |

### Controls
| Control | Action |
|---------|--------|
| Pad tap | Load set, enter Track view |
| Play | Start/stop playback |
| Rec | Toggle recording |

### Inactive
Track buttons, Knobs, Jog wheel, Steps

---

## Track View - Normal Mode

Main editing mode. Program notes, adjust step parameters.

### Step Buttons (16)
| Action | Result |
|--------|--------|
| Quick tap | Toggle step (add last note / clear) |
| Hold | Edit step parameters |
| Hold + tap another step | Set note length |

| LED Color | Meaning |
|-----------|---------|
| Track color (bright) | Currently held step |
| Track color (dim) | Step has content (in loop) |
| Navy | Step has content (outside loop) |
| White | Playhead position |
| Cyan | Note length visualization |
| Black | Empty step |

### Pads (32)
Piano keyboard layout (notes 36-67).

| Action | Result |
|--------|--------|
| Tap (no step held) | Select note for quick entry, preview sound |
| Tap (step held) | Toggle note on step (up to 4 notes) |

| LED Color | Meaning |
|-----------|---------|
| Track color | Note in held step / currently playing |
| Yellow | C notes |
| White/Grey | In detected scale |
| Dark grey | Out of scale / black keys |

### Track Buttons (4)
Scroll navigation for 16 tracks.

| Button | Shows |
|--------|-------|
| Top (CC 43) | Selected track (always) |
| 2nd-4th | Scrollable tracks (excluding selected) |

| Action | Result |
|--------|--------|
| Press | Select that track |

| LED Color | Meaning |
|-----------|---------|
| Track color (bright) | Selected track |
| Track color (dim) | Track has content |
| Dark grey | Empty track |
| Red | Muted track |

### Knobs

**Normal (no step held):**
| Knob | Function |
|------|----------|
| 1-2 | Send CC to track's MIDI channel |

**Holding step:**
| Knob | Function | Tap to Clear |
|------|----------|--------------|
| 1 | CC1 value (0-127) | Yes |
| 2 | CC2 value (0-127) | Yes |
| 7 | Ratchet (1x-8x) | Yes |
| 8 | Probability (5-100%) / Condition | Yes |

**Shift held:**
| Knob | Function |
|------|----------|
| 7 | Track speed |
| 8 | Track MIDI channel |

### Jog Wheel
| Context | Action |
|---------|--------|
| Normal | Scroll through tracks |
| Shift held | Scroll pattern view offset |
| Step held | Micro-timing offset (-24 to +24 ticks) |

### Transport
| Button | Function |
|--------|----------|
| Play | Start/stop playback |
| Rec | Toggle recording mode |
| Loop | Shows custom loop (hold to edit) |
| Capture | Enter Spark mode (+ step) |

### Shift + Step Icons
When shift is held, step LEDs clear and show mode icons:

| Step | LED | Action |
|------|-----|--------|
| 1 | White | Go to Set view |
| 2 | White | Enter Channel mode |
| 5 | White | Enter Speed mode |
| 7 | White | Enter Swing mode |
| 8 | White | Toggle transpose for track |

Note: Step 8 LED shows Cyan when shift is not held if the current track has transpose enabled.

---

## Track View - Loop Mode

Edit loop start and end points. Enter by holding Loop button.

### Step Buttons
| Action | Result |
|--------|--------|
| First tap | Set loop start (or end) |
| Second tap | Set loop end (or start) |

| LED Color | Meaning |
|-----------|---------|
| Track color | Loop start/end points |
| Light grey | Steps within loop |
| Cyan | Currently selected first point |
| Black | Outside loop |

### Controls
| Control | Action |
|---------|--------|
| Loop release | Exit loop mode |

### Inactive
Knobs, Track buttons, Jog wheel, Pads (show piano)

---

## Track View - Spark Mode

Edit spark conditions (param spark, comp spark, jump) for multiple steps.
Enter with Capture + Step.

### Step Buttons
| Action | Result |
|--------|--------|
| Tap | Toggle step selection |

| LED Color | Meaning |
|-----------|---------|
| Purple | Selected step |
| Cyan | Has spark settings |
| Light grey | Has content |
| Black | Empty |

### Knobs (when steps selected)
| Knob | Function |
|------|----------|
| 1 | Jump target (-1 to 15) |
| 7 | Comp Spark condition |
| 8 | Param Spark condition |

### Jog Wheel
| Action | Result |
|--------|--------|
| Turn | Adjust micro-timing offset for all selected steps |

### Controls
| Control | Action |
|---------|--------|
| Capture | Exit spark mode |
| Back | Exit spark mode |

---

## Track View - Channel Mode

Adjust track MIDI channel. Enter with Shift + Step 2.

### Display
Shows current track and MIDI channel (1-16).

### Jog Wheel
| Action | Result |
|--------|--------|
| Turn | Change MIDI channel |

### Controls
| Control | Action |
|---------|--------|
| Jog click | Exit channel mode |
| Back | Exit channel mode |

### LED
Step 2 and Step 2 UI lit White.

---

## Track View - Speed Mode

Adjust track speed multiplier. Enter with Shift + Step 5.

### Display
Shows current track and speed (1/4x to 4x).

### Speed Options
1/4x, 1/3x, 1/2x, 2/3x, 1x, 3/2x, 2x, 3x, 4x

### Jog Wheel
| Action | Result |
|--------|--------|
| Turn | Change speed multiplier |

### Controls
| Control | Action |
|---------|--------|
| Jog click | Exit speed mode |
| Back | Exit speed mode |

### LED
Step 5 and Step 5 UI lit White.

---

## Track View - Swing Mode

Adjust track swing amount. Enter with Shift + Step 7.

### Display
Shows current track and swing (0-100%).

### Jog Wheel
| Action | Result |
|--------|--------|
| Turn | Change swing amount |

### Controls
| Control | Action |
|---------|--------|
| Jog click | Exit swing mode |
| Back | Exit swing mode |

### LED
Step 7 and Step 7 UI lit White.

---

## Pattern View

Select patterns for each track. 8 columns (tracks) x 4 rows (patterns).

### Pads (32)
Grid showing 8 tracks x 4 patterns.

| Action | Result |
|--------|--------|
| Tap | Select pattern for that track |

| LED Color | Meaning |
|-----------|---------|
| Track color (bright) | Current pattern for track |
| Track color (dim) | Pattern has content |
| Black | Empty pattern |

### Track Buttons (4)
Same scroll navigation as Track view.

| Button | Shows |
|--------|-------|
| Top | Selected track |
| 2nd-4th | Scrollable tracks |

### Knobs (1-8)
All lit Cyan. Send CC 1-8 on master channel (16).

### Jog Wheel
| Context | Action |
|---------|--------|
| Normal | Scroll tracks horizontally (1 at a time) |
| Shift held | Scroll patterns vertically |

### Display
Shows visible track range, pattern numbers, selected track, BPM.

### Controls
| Control | Action |
|---------|--------|
| Back | Return to Track view |

---

## Master View

Transpose control, scale detection, chord follow settings.

### Step Buttons (16)
Transpose sequence steps.

| Action | Result |
|--------|--------|
| Hold | Edit transpose step |
| Delete + tap | Remove transpose step |

| LED Color | Meaning |
|-----------|---------|
| Cyan | Has transpose value |
| White | Currently held |
| Green | Currently playing |
| Dark grey | Held but empty |
| Black | Empty |

### Pads

**Row 1 (top, 24-31): Chord Follow**
| Action | Result |
|--------|--------|
| Tap | Toggle chord follow for track |

| LED Color | Meaning |
|-----------|---------|
| Track color | Chord follow ON |
| Dark grey | Chord follow OFF |

**Row 2 (16-23): Reserved**
Black, unused.

**Rows 3-4 (0-15): Piano**
Set transpose value when holding a step.

| LED Color | Meaning |
|-----------|---------|
| Track color | Current transpose value |
| Yellow | Root note |
| White/Grey | In detected scale |
| Dark grey/Black | Out of scale |

### Knobs
| Knob | Function (when holding step) |
|------|------------------------------|
| 1 | Adjust duration (fine) |

### Jog Wheel
| Context | Action |
|---------|--------|
| Holding step | Adjust duration (coarse, 1 bar increments) |
| Normal | Scroll chord follow tracks (1 at a time) |

### Transport
| Button | Function |
|--------|----------|
| Loop | Toggle MIDI clock output |
| Up | Octave up (transpose display) |
| Down | Octave down (transpose display) |

### BPM Mode
Enter with Shift + Step 5.

| Control | Action |
|---------|--------|
| Jog wheel | Adjust BPM |
| Back | Exit BPM mode |

Step 5 UI lit Cyan when shift held or in BPM mode.

### Controls
| Control | Action |
|---------|--------|
| Back | Return to Track view (or exit BPM mode) |

### Track Buttons
Not used in Master view (all OFF).

---

## Track Colors

| Track | Color | Default Name |
|-------|-------|--------------|
| 1 | Red | Kick |
| 2 | Orange | Snare |
| 3 | Yellow | Perc |
| 4 | Green | Sample |
| 5 | Cyan | Bass |
| 6 | Blue | Lead |
| 7 | Purple | Arp |
| 8 | White | Chord |
| 9 | Pink | Kick2 |
| 10 | Burnt Orange | Snare2 |
| 11 | Lime | Perc2 |
| 12 | Teal | Sample2 |
| 13 | Azure | Bass2 |
| 14 | Violet | Lead2 |
| 15 | Magenta | Arp2 |
| 16 | Grey | Chord2 |

---

## Step Parameters

When holding a step in Track/Normal mode:

| Parameter | Control | Range |
|-----------|---------|-------|
| Notes | Pads | Up to 4 per step |
| Note length | Tap another step | 1-16 steps |
| CC1 | Knob 1 | 0-127 |
| CC2 | Knob 2 | 0-127 |
| Ratchet | Knob 7 | 1x, 2x, 3x, 4x, 6x, 8x |
| Probability | Knob 8 (CCW) | 5-100% |
| Condition | Knob 8 (CW) | 1:2, 1:3, 1:4... |
| Micro-timing | Jog wheel | -24 to +24 ticks |

---

## Conditions

Step trigger conditions (Knob 8 clockwise when holding step):

| Pattern | Meaning |
|---------|---------|
| --- | Always play (use probability) |
| 1:2 | Play on loop 1 of every 2 |
| 2:3 | Play on loop 2 of every 3 |
| !1:4 | Skip loop 1 of every 4 |
| etc. | Up to 8-loop cycles |

---

## Data Storage

```
/data/UserData/move-anything-data/sequencer/
  sets.json    # All 32 sets
```

Each set contains:
- 16 tracks x 16 patterns x 16 steps
- BPM, transpose sequence, chord follow settings
