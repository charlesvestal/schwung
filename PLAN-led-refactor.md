# LED Refactor Plan

## Goals
1. **Pad ownership at view level** - Pads managed by view coordinator, not sub-modes
2. **Eliminate flicker** - Remove all clearAllLEDs() calls
3. **Reduce update frequency** - Throttle LED updates with configurable divisor

---

## Part 1: Pad Ownership Refactor (Track View)

### Current Architecture
```
track.js (coordinator)
  ├── clearAllLEDs() on every mode change (9 places!)
  └── delegates ALL LEDs to sub-mode

Sub-modes own ALL LEDs:
  - normal.js  → pads, steps, knobs, track btns, transport, capture, back
  - loop.js    → pads, steps, knobs, track btns, transport, capture, back
  - etc.
```

### Target Architecture
```
track.js (coordinator)
  ├── OWNS pads (piano layout, playing notes, held step notes)
  ├── NO clearAllLEDs() anywhere
  └── delegates non-pad LEDs to sub-mode

Sub-modes own only:
  - steps, knobs, track btns, transport, capture, back
```

### Changes Required

#### 1. track.js - Add pad management
- Import `getPadBaseColor`, `MovePads`, `TRACK_COLORS`, `getCurrentPattern`
- Add `updatePadLEDs()` function that handles:
  - Piano layout (base colors via getPadBaseColor)
  - Held step note highlighting
  - Playing note highlighting (litPads)
- Update `updateLEDs()` to call `updatePadLEDs()` first

#### 2. track.js - Remove ALL clearAllLEDs() calls
- Lines 84, 93, 102, 111, 120, 131, 148, 159, 171
- Just call `updateLEDs()` instead - cache handles duplicates

#### 3. Sub-modes - Remove pad LED code
- **loop.js**: Remove pad loop, remove getPadBaseColor import
- **channel.js**: Remove pad loop, remove getPadBaseColor import
- **speed.js**: Remove pad loop, remove getPadBaseColor import
- **swing.js**: Remove pad loop, remove getPadBaseColor import
- **spark.js**: Remove pad loop, remove getPadBaseColor import
- **normal.js**: Remove `updatePadLEDs()` function entirely

#### 4. Move litPads handling from ui.js to track.js
- ui.js tick() currently manages litPads for playing notes
- Move that logic to track.js `updatePadLEDs()`
- ui.js just calls `trackView.updatePadLEDs()` when step changes

---

## Part 2: Remove clearAllLEDs() from View Transitions

### Current calls in ui.js
```javascript
// Line 72: entering track view via Menu
clearAllLEDs();

// Line 81: entering track view via Back
clearAllLEDs();
```

### Current calls in set.js
```javascript
// Line 73: after loading set, entering track view
clearAllLEDs();
```

### Solution
- Remove all `clearAllLEDs()` calls
- Trust LED caching to prevent duplicate sends
- Each view's `updateLEDs()` sets everything it needs

---

## Part 3: Pattern & Master Views

### Pattern View (pattern.js)
- No sub-modes, no refactoring needed
- Pads show pattern grid - view-specific, not piano layout
- Just remove any clearAllLEDs() when entering/exiting

### Master View (master.js)
- No sub-modes, no refactoring needed
- Pads show:
  - Row 1: Chord follow toggles
  - Row 2: Reserved (black)
  - Rows 3-4: Piano for transpose
- Already handles held step highlighting
- Just ensure no clearAllLEDs() when entering/exiting

---

## Part 4: LED Update Throttling

### Configuration
```javascript
// In ui.js or state.js
const LED_UPDATE_DIVISOR = 4;  // Update every 4th tick (~86 Hz)
```

### Implementation
```javascript
let ledTickCounter = 0;

globalThis.tick = function() {
    ledTickCounter++;

    // Always update display (cheap)
    drawUI();

    // Always poll DSP at full rate (timing critical)
    if (state.playing) {
        pollPlayhead();
    }

    // Throttle LED updates
    if (ledTickCounter % LED_UPDATE_DIVISOR === 0) {
        if (state.ledsDirty) {
            getCurrentView().updateLEDs();
            state.ledsDirty = false;
        }
    }
};
```

### Dirty Flag System
- Set `state.ledsDirty = true` when:
  - Playhead moves to new step
  - User presses button/pad
  - Mode changes
- Only update LEDs when dirty AND throttle allows

---

## Implementation Order

### Phase 1: Track View Pad Ownership
1. Add `updatePadLEDs()` to track.js
2. Update track.js `updateLEDs()` to call it
3. Remove pad code from loop.js
4. Remove pad code from channel.js
5. Remove pad code from speed.js
6. Remove pad code from swing.js
7. Remove pad code from spark.js
8. Remove `updatePadLEDs()` from normal.js
9. **Test**: Pads should look identical in all track modes

### Phase 2: Remove clearAllLEDs() from Track
1. Remove all 9 clearAllLEDs() calls from track.js
2. **Test**: Mode transitions should be smooth, no flash

### Phase 3: Remove clearAllLEDs() from View Transitions
1. Remove clearAllLEDs() from ui.js (lines 72, 81)
2. Remove clearAllLEDs() from set.js (line 73)
3. **Test**: View transitions smooth, no flash

### Phase 4: Move litPads Handling
1. Move playing note highlight logic from ui.js tick() to track.js
2. ui.js tick() just sets dirty flag when step changes
3. **Test**: Playing notes still highlight correctly

### Phase 5: Throttle LED Updates
1. Add LED_UPDATE_DIVISOR constant
2. Add ledTickCounter to tick()
3. Add ledsDirty flag to state
4. Set dirty flag on relevant events
5. Throttle LED updates in tick()
6. **Test**: UI responsive, playhead smooth at 6x speed

---

## Files to Modify

| File | Phase | Changes |
|------|-------|---------|
| track.js | 1, 2 | Add updatePadLEDs, remove clearAllLEDs |
| normal.js | 1 | Remove updatePadLEDs and pad code |
| loop.js | 1 | Remove pad LED code |
| channel.js | 1 | Remove pad LED code |
| speed.js | 1 | Remove pad LED code |
| swing.js | 1 | Remove pad LED code |
| spark.js | 1 | Remove pad LED code |
| ui.js | 3, 4, 5 | Remove clearAllLEDs, move litPads, add throttling |
| set.js | 3 | Remove clearAllLEDs |
| state.js | 5 | Add ledsDirty flag |

---

## Testing Checklist

### Pad Display
- [ ] Piano layout visible in normal mode
- [ ] Piano layout visible in loop mode
- [ ] Piano layout visible in channel mode
- [ ] Piano layout visible in speed mode
- [ ] Piano layout visible in swing mode
- [ ] Piano layout visible in spark mode
- [ ] Held step notes highlighted in track color
- [ ] Playing notes highlighted in track color
- [ ] C notes always VividYellow

### Transitions (No Flicker)
- [ ] Normal → Loop (hold Loop button)
- [ ] Loop → Normal (release Loop button)
- [ ] Normal → Channel (Shift+Step2)
- [ ] Channel → Normal (Back or jog click)
- [ ] Normal → Speed (Shift+Step5)
- [ ] Speed → Normal (Back or jog click)
- [ ] Normal → Swing (Shift+Step7)
- [ ] Swing → Normal (Back or jog click)
- [ ] Normal → Spark (Capture+Step)
- [ ] Spark → Normal (Capture)
- [ ] Track → Pattern (Menu)
- [ ] Pattern → Track (Back)
- [ ] Track → Master (Shift+Menu)
- [ ] Master → Track (Back)
- [ ] Track → Set (Shift+Step1)
- [ ] Set → Track (select pad)

### Performance
- [ ] Playhead smooth at 1x speed
- [ ] Playhead smooth at 4x speed
- [ ] Playhead smooth at 6x speed
- [ ] Button presses feel responsive
- [ ] No visible lag on mode changes
