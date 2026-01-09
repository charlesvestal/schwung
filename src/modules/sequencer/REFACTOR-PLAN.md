# Sequencer UI Refactoring Plan

## Goal

Break up the large `ui.js` into a clean view-based architecture where each view is self-contained and doesn't bleed into other views.

## Current State

- `ui.js` is large and tangled
- Views (set, track, pattern, master) are mixed together
- Track buttons, transport, and state are shared messily
- `lib/` modules exist: constants.js, state.js, data.js, persistence.js

## Target Architecture

```
src/modules/sequencer/
├── ui.js                 # Thin router/orchestrator
├── lib/
│   ├── constants.js      ✓ done
│   ├── state.js          ✓ done (needs view field)
│   ├── data.js           ✓ done
│   └── persistence.js    ✓ done
└── views/
    ├── set.js            # Set selection view
    ├── track.js          # Track editing view
    ├── pattern.js        # Pattern selection view
    └── master.js         # Master/CC output view
```

## Views

### Set View
- **Purpose**: Select which set (song) to work on
- **Steps**: Show sets 1-16 (with shift: 17-32)
- **Pads**: Show sets or additional selection
- **Display**: Set info, which have content
- **Entry**: From track view via menu or gesture
- **Exit**: Selecting a set loads it and goes to track view

### Track View
- **Purpose**: Edit a track's steps and notes
- **Steps**: Show the 16 steps of current pattern
- **Pads**: Select notes (when holding step), play notes
- **Display**: Step info, track info, note names
- **Sub-modes** (internal to track view):
  - Normal: Step editing
  - Loop: Set loop points
  - Spark: Condition editing
  - Swing: Per-track swing adjustment
  - BPM: Global tempo adjustment

### Pattern View
- **Purpose**: Select patterns for each track
- **Steps/Pads**: Show available patterns
- **Display**: Pattern grid, current selections
- **Entry**: From track view (pattern button)
- **Exit**: Back to track view

### Master View
- **Purpose**: CC output mode for controlling external gear
- **Behavior**: Completely different from other views
- **Entry**: Shift + Menu from track view
- **Exit**: Back to track view

## View Interface

Each view module exports:

```javascript
export function onEnter() {
    // Called when switching TO this view
    // Initialize view-specific state
}

export function onExit() {
    // Called when switching AWAY from this view
    // Cleanup if needed
}

export function onInput(data) {
    // Handle MIDI input for this view
    // data = [status, note/cc, value]
    // Return true if handled, false to let router handle
}

export function updateLEDs() {
    // Update all LEDs for this view
}

export function updateDisplay() {
    // Render display for this view
}
```

## Router (ui.js)

The main ui.js becomes a thin router:

```javascript
import * as setView from './views/set.js';
import * as trackView from './views/track.js';
import * as patternView from './views/pattern.js';
import * as masterView from './views/master.js';

const views = { set: setView, track: trackView, pattern: patternView, master: masterView };

function getCurrentView() {
    return views[state.view];
}

// Handle global input first (transport, view switching)
// Then delegate to current view
globalThis.onMidiMessageInternal = function(data) {
    // Global handlers (transport, etc.)
    if (handleGlobalInput(data)) return;

    // Delegate to current view
    getCurrentView().onInput(data);
};

globalThis.tick = function() {
    getCurrentView().updateLEDs();
    getCurrentView().updateDisplay();
};
```

## Global Handlers (in router)

These are handled by the router before delegating to views:

- **Transport**: Play, Stop, Record (CC 85, 117, 119)
- **View switching**: Menu button, Shift+Menu
- **Track buttons**: Maybe (or let track/pattern views handle)

## Implementation Steps

### Phase 1: Prepare
- [x] Update lib/state.js to track current view properly
- [x] Create views/ directory
- [x] Update build.sh to copy views/

### Phase 2: Extract Views (one at a time)
- [x] Extract master.js (simplest, most isolated)
- [x] Extract set.js
- [x] Extract pattern.js
- [x] Extract track.js (most complex, do last)

### Phase 3: Refactor Router
- [x] Slim down ui.js to router logic
- [x] Move global handlers to router
- [x] Test all view transitions

### Phase 4: Cleanup
- [x] Remove dead code from ui.js
- [ ] Verify all functionality works
- [ ] Test on device

## Notes

- Each view should be self-contained
- Views don't import each other
- State transitions go through lib/state.js
- Router handles view switching logic
- Track view has internal sub-modes (not separate views)
