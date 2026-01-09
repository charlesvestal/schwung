# Track View Refactoring Plan

## Current State
- `track.js` is 1071 lines with 5 modes mixed together
- Modes: normal, loop, spark, swing (bpm is handled via jog wheel in normal/swing)
- Each mode has its own input handling, LED updates, and display logic scattered throughout

## Goal
Break out each mode into its own file under `views/track/` so each mode is self-contained and easier to maintain.

## Proposed Structure

```
views/
  track.js          # Slim router/coordinator (keeps exports, delegates to modes)
  track/
    normal.js       # Default track editing mode
    loop.js         # Loop point editing mode
    spark.js        # Spark/condition editing mode
    swing.js        # Swing editing mode
    shared.js       # Shared utilities (updatePlayhead, track button handling, etc.)
```

## Mode Responsibilities

### normal.js (~400 lines)
- Step button press/release (note entry, step selection, length adjustment)
- Pad handling (note on/off for selected step)
- Knob handling (velocity, gate, ratchet, length, probability, condition)
- Knob touch tap-to-clear
- Step LEDs showing pattern content
- Pad LEDs showing selected step's notes
- Knob LEDs showing available controls

### loop.js (~80 lines)
- Step button handling for loop start/end selection
- Two-press workflow (first sets start, second sets end)
- Step LEDs showing loop range
- Dedicated display content

### spark.js (~150 lines)
- Step button handling for multi-select
- Knob handling for param spark, comp spark, jump
- Step LEDs showing spark assignments
- Dedicated display content
- Capture button to enter/exit

### swing.js (~60 lines)
- Jog wheel handling for swing adjustment
- Exit via jog click or back button
- Step LEDs (only step 7 lit)
- Dedicated display content

### shared.js (~100 lines)
- `updatePlayhead()` - playhead LED updates during playback
- `handleTrackButton()` - track selection (used by all modes)
- `updateTrackButtonLEDs()` - track button colors
- `updateTransportLEDs()` - play/rec/loop button colors
- `updateBackLED()`, `updateCaptureLED()`
- Common imports re-exported

## track.js Coordinator (~150 lines)
Responsibilities:
- Export `onEnter()`, `onExit()`, `onInput()`, `updateLEDs()`, `updateDisplayContent()`
- Route input to current mode's handler
- Route LED updates to current mode + shared
- Handle mode transitions (enter/exit loop, spark, swing)
- Handle shift+step7 (enter swing), capture (enter/exit spark), loop button

## Implementation Steps

### Phase 1: Create shared.js
1. Extract shared utilities that all modes use
2. Move track button handling, transport LEDs, playhead
3. Test that track.js still works with imports from shared.js

### Phase 2: Extract swing.js (simplest mode)
1. Create swing.js with onInput, updateStepLEDs, updateKnobLEDs, updateDisplay
2. Update track.js to delegate to swing.js when trackMode === 'swing'
3. Test swing mode works

### Phase 3: Extract loop.js
1. Create loop.js with loop edit logic
2. Update track.js to delegate
3. Test loop editing works

### Phase 4: Extract spark.js
1. Create spark.js with spark logic
2. Update track.js to delegate
3. Test spark mode works

### Phase 5: Extract normal.js
1. Move remaining logic to normal.js
2. track.js becomes pure coordinator
3. Final testing

## Interface Pattern

Each mode file exports:
```javascript
export function onInput(data) { ... }      // Returns true if handled
export function updateStepLEDs() { ... }
export function updatePadLEDs() { ... }
export function updateKnobLEDs() { ... }
export function updateDisplayContent() { ... }
export function onEnter() { ... }          // Optional, mode-specific init
export function onExit() { ... }           // Optional, mode-specific cleanup
```

track.js coordinator:
```javascript
import * as normal from './track/normal.js';
import * as loop from './track/loop.js';
import * as spark from './track/spark.js';
import * as swing from './track/swing.js';
import * as shared from './track/shared.js';

const modes = { normal, loop, spark, swing };

export function onInput(data) {
    // Handle mode transitions first (shift+step7, capture, loop button)
    // Then delegate to current mode
    return modes[state.trackMode].onInput(data);
}

export function updateLEDs() {
    modes[state.trackMode].updateStepLEDs();
    modes[state.trackMode].updatePadLEDs();
    modes[state.trackMode].updateKnobLEDs();
    shared.updateTrackButtonLEDs();
    shared.updateTransportLEDs();
    // etc.
}
```

## Benefits
- Each mode is self-contained and testable
- Easier to add new modes
- Clear separation of concerns
- Smaller files (~60-400 lines each vs 1071)
- Mode-specific logic isn't buried in switch statements
