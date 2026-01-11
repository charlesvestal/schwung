# Plan: 16-Track UI Navigation

## Current State

- DSP now supports 16 tracks
- JS UI still configured for 8 tracks
- Hardware has 4 track buttons (+ shift = 8 combinations)

---

## Jog Wheel Navigation (Unified)

### Summary

| View | Jog | Shift + Jog |
|------|-----|-------------|
| **Track** | Scroll tracks horizontally (when not holding step) | Scroll patterns vertically |
| **Pattern** | Scroll tracks horizontally | Scroll patterns vertically |
| **Master** | Scroll tracks horizontally (when not holding transpose step) | Scroll patterns vertically |

**Note:** Selected track is excluded from scroll range - it's always pinned to top row.

### Current Behavior (to be changed)

| View | Jog | Shift + Jog |
|------|-----|-------------|
| Track | Micro-timing (holding step) | **BPM control** ← moving |
| Pattern | Vertical pattern scroll | Not implemented |
| Master | Duration adjust (holding step) | Not implemented |

---

## BPM Control (Moving to Master View)

### New Location: Master View with Shift + Step 5

**Behavior:**
1. In master view, hold Shift
2. Step 5 UI LED (MoveStep5UI) lights up
3. Press Step 5 to enter BPM mode
4. Screen shows BPM changer interface
5. Jog wheel adjusts BPM
6. Press Back to exit BPM mode

**Implementation:**
- Add `bpmMode` state flag
- Master view shift handling shows Step 5 UI lit
- Step 5 press enters BPM mode
- Back button or releasing shift exits

---

## Track View: Scrolling Track Display

### Core Idea

In track view, use the pads to show all 16 tracks with the selected track always at the top row. The jog wheel scrolls through remaining tracks.

### Visual Layout

```
Row 1 (top):    [Selected Track - always visible, bright]
Row 2:          [Track from scroll position]
Row 3:          [Track from scroll position + 1]
Row 4 (bottom): [Track from scroll position + 2]
```

### Behavior

1. **Selected track pinned to top row**
   - Current track always shows in row 1 (pads 24-31)
   - Bright color indicating selection

2. **Jog wheel scrolls other tracks (rows 2-4)**
   - Rotating jog scrolls 1 track at a time
   - Selected track is **skipped** in scroll range (shows 15 other tracks)
   - Wraps around

3. **Pad press selects track**
   - Press any pad in rows 2-4 to select that track
   - Selected track moves to top row
   - Previous selected track joins the scrollable pool

4. **Shift + Jog scrolls patterns vertically**
   - Changes which patterns are shown (for pattern selection)

5. **Track color indication**
   - Bright = selected track (top row)
   - Medium = has patterns/content
   - Dim = empty track

### Example

Current track: 5, Scroll position: 0 (showing tracks 1, 2, 3)

```
Row 1: [Track 5 - SELECTED - Cyan bright]
Row 2: [Track 1 - has content - Red medium]
Row 3: [Track 2 - has content - Orange medium]
Row 4: [Track 3 - empty - Yellow dim]
```

User scrolls jog +1 (now showing tracks 2, 3, 4):

```
Row 1: [Track 5 - SELECTED - Cyan bright]
Row 2: [Track 2 - has content - Orange medium]
Row 3: [Track 3 - empty - Yellow dim]
Row 4: [Track 4 - has content - Green medium]
```

User presses pad in row 3 (Track 3):

```
Row 1: [Track 3 - SELECTED - Yellow bright]
Row 2: [Track 1 - has content - Red medium]
Row 3: [Track 2 - has content - Orange medium]
Row 4: [Track 4 - has content - Green medium]
```

---

## Pattern View: Horizontal Track Scrolling

### Changes from Current

**Current:** 8 columns (tracks) × 4 rows (patterns), jog scrolls vertically

**New:** With 16 tracks, jog scrolls horizontally through tracks, shift+jog scrolls vertically through patterns

### Layout Options

**Option A: 4 tracks visible at a time**
```
        Track N  Track N+1  Track N+2  Track N+3
Row 1:  [Pat+3]  [Pat+3]    [Pat+3]    [Pat+3]
Row 2:  [Pat+2]  [Pat+2]    [Pat+2]    [Pat+2]
Row 3:  [Pat+1]  [Pat+1]    [Pat+1]    [Pat+1]
Row 4:  [Pat+0]  [Pat+0]    [Pat+0]    [Pat+0]
```
- Each track gets 2 pad columns (8 pads)
- More detail per track

**Option B: 8 tracks visible at a time (current layout, scrollable)**
```
        T1  T2  T3  T4  T5  T6  T7  T8  (or T9-T16 when scrolled)
Row 1:  [ ] [ ] [ ] [ ] [ ] [ ] [ ] [ ]
Row 2:  [ ] [ ] [ ] [ ] [ ] [ ] [ ] [ ]
Row 3:  [ ] [ ] [ ] [ ] [ ] [ ] [ ] [ ]
Row 4:  [ ] [ ] [ ] [ ] [ ] [ ] [ ] [ ]
```
- Jog scrolls by 8 tracks (bank switching)
- Simpler, matches current mental model

---

## Master View: Horizontal Scrolling

Same horizontal track scrolling as pattern view for consistency:
- Jog scrolls tracks left/right
- Shift + Jog scrolls patterns vertically
- Chord follow row shows visible tracks

---

## Track Buttons (CC 40-43)

### Options

1. **Keep as quick-select** for tracks 1-4 / 5-8 (shift)
   - Familiar, but only reaches 8 of 16 tracks

2. **Bank switching**
   - Track buttons select within current visible bank
   - Jog wheel moves bank

3. **Relative selection**
   - Track buttons select from currently visible tracks in rows 2-4

**Recommendation:** Keep as quick-select for now, jog wheel handles full 16-track access.

---

## Colors for Tracks 9-16

Need 8 additional distinct colors. Options:

**Option A: Repeat with variation**
```
Track 9:  Pink (variation of Red)
Track 10: Peach (variation of Orange)
Track 11: Gold (variation of Yellow)
Track 12: Lime (variation of Green)
Track 13: Turquoise (variation of Cyan)
Track 14: Indigo (variation of Blue)
Track 15: Magenta (variation of Purple)
Track 16: Cream (variation of White)
```

**Option B: New palette**
Pick 8 entirely different colors from Move's palette.

---

## Implementation Notes

### JS Changes Needed

**constants.js:**
- `NUM_TRACKS = 16`
- `TRACK_COLORS[16]` - add 8 more colors
- `TRACK_COLORS_DIM[16]` - add 8 more
- `TRACK_NAMES[16]` - add 8 more names

**state.js:**
- `chordFollow[16]` - expand default array
- Add `trackScrollPosition` state variable
- Add `patternScrollPosition` (vertical) if not already

**track.js / normal.js:**
- Remove shift+jog BPM control
- Add jog → track scroll (when not holding step)
- Add shift+jog → pattern scroll
- Update pad LED logic for scrolling view
- Update pad press handling for track selection

**master.js:**
- Add shift + step 5 → BPM mode
- Add jog → track scroll (when not holding transpose step)
- Add shift+jog → pattern scroll

**pattern.js:**
- Change jog from vertical to horizontal scroll
- Add shift+jog for vertical pattern scroll

**data.js:**
- `createEmptyTracks()` already uses NUM_TRACKS loop ✓

**persistence.js:**
- Should handle variable track counts gracefully
- Migration for existing saves with 8 tracks?

---

## Pattern Count Reduction

Current: 30 patterns per track
New: 16 patterns per track

**Benefits:**
- Simpler navigation (4 rows × 4 scroll positions = 16 patterns)
- Memory savings: ~45% reduction per track
- 16 tracks × 16 patterns = 256 total patterns (still plenty)

**Changes:**
- DSP: `#define NUM_PATTERNS 16`
- JS: `NUM_PATTERNS = 16`

---

## Status

- [x] DSP updated to 16 tracks
- [x] DSP: Reduce NUM_PATTERNS from 30 to 16
- [ ] JS constants expanded (NUM_TRACKS, NUM_PATTERNS, colors, names)
- [ ] BPM control moved to master view
- [ ] Jog wheel navigation updated (all views)
- [ ] Track scrolling UI implemented
- [ ] Track colors decided
- [ ] Testing with hardware
