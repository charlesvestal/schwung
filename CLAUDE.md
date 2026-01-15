# CLAUDE.md

Instructions for Claude Code when working with this repository.

## CRITICAL: Do NOT Build

**NEVER run build commands** (`./scripts/build-docker.sh`, `./scripts/build.sh`, `npm run build`, etc.) unless explicitly requested by the user. The user will build and test manually.

## Project Overview

Move Anything is a framework for custom JavaScript and native DSP modules on Ableton Move hardware. This repo focuses on **SEQOMD** - an OP-Z inspired 8-track MIDI step sequencer.

## SEQOMD Sequencer

### Architecture

```
src/modules/seqomd/
  ui.js                    # Main router - delegates to views
  lib/
    constants.js           # Colors, speeds, conditions, etc.
    state.js               # All mutable state + view transitions
    helpers.js             # Utility functions
    data.js                # Track/pattern data structures
    persistence.js         # Save/load sets to disk
  views/
    set.js                 # Set selection (32 sets on pads)
    track.js               # Track view coordinator
    pattern.js             # Pattern selection grid
    master.js              # Master settings (channels, sync)
    track/
      normal.js            # Main step editing mode
      loop.js              # Loop start/end editing
      spark.js             # Spark conditions (param/comp spark, jump)
      channel.js           # MIDI channel adjustment
      speed.js             # Track speed multiplier
      swing.js             # Track swing amount
      shared.js            # Common re-exports
```

### View Hierarchy

```
Set View (startup)
    ↓ tap pad to select set
Track View (main editing)
    ├── Menu → Pattern View (toggle)
    ├── Shift+Menu → Master View (toggle)
    ├── Loop button (hold) → Loop Mode
    ├── Capture + step → Spark Mode
    └── Shift + step icon → Channel/Speed/Swing modes
```

### Navigation

| Action | Result |
|--------|--------|
| **Pad tap** (set view) | Load set, enter track view |
| **Menu** | Toggle pattern view |
| **Shift + Menu** | Toggle master view |
| **Back** | Return to track view |
| **Shift + Step 1** | Go to set view (saves current set) |
| **Loop** (hold) | Enter loop edit mode |
| **Capture + Step** | Enter spark mode |
| **Capture** (in spark) | Exit spark mode |
| **Jog click / Back** (in mode) | Exit channel/speed/swing mode |

### Track Mode Icons (Shift held)

When shift is held in track view, step LEDs clear and show mode entry icons:

| Step | Color | Action |
|------|-------|--------|
| Step 1 | White | Go to Set view (save & select sets) |
| Step 2 | BrightGreen | Channel mode (jog: MIDI channel 1-16) |
| Step 5 | Cyan | Speed mode (jog: 1/4x - 4x) |
| Step 7 | VividYellow | Swing mode (jog: 0-100%) |

Press the lit step to enter that mode. For channel/speed/swing: use jog wheel to adjust, then jog click or Back to exit.

### Track Colors

| Track | Color | Default Name |
|-------|-------|--------------|
| 1 | BrightRed | Kick |
| 2 | OrangeRed | Snare |
| 3 | VividYellow | Perc |
| 4 | BrightGreen | Sample |
| 5 | Cyan | Bass |
| 6 | RoyalBlue | Lead |
| 7 | Purple | Arp |
| 8 | White | Chord |

Track buttons select tracks 1-4. Shift + track buttons select tracks 5-8.

### Step Parameters

When holding a step in normal track mode:

| Knob | Parameter | Range |
|------|-----------|-------|
| Knob 1 | CC1 value | 0-127 (tap to clear) |
| Knob 2 | CC2 value | 0-127 (tap to clear) |
| Knob 3 | Arp override | Mode/Speed/Octave/Layer (tap to cycle) |
| Knob 6 | Ratchet | 1x, 2x, 3x, 4x, 6x, 8x (tap to reset) |
| Knob 7 | Probability/Condition | 5-100% / 1:2, 1:3, etc. (tap to clear) |
| Knob 8 | Velocity | 1-127, adjusts all notes (tap to reset to 100) |
| Jog wheel | Micro-timing offset | -24 to +24 ticks |
| Tap another step | Note length | 1-16 steps |
| Pads | Toggle notes | Up to 7 per step, each with own velocity |

### LED Conventions

- **Bright color**: Selected/active
- **Dim color**: Has content but not selected
- **Black**: Empty/inactive
- **White**: Playhead position
- **Cyan**: Loop points, speed mode
- **Purple**: Spark mode selection

### Data Storage

```
/data/UserData/move-anything-data/seqomd/
  sets.json              # All 32 sets
```

Each set contains 8 tracks × 30 patterns × 16 steps with full parameter data.

## Build Commands

```bash
./scripts/build-docker.sh    # Build with Docker (recommended)
./scripts/build.sh           # Build (requires cross-compiler)
./scripts/package.sh         # Create move-anything.tar.gz
./scripts/clean.sh           # Remove build/ and dist/
./scripts/install.sh         # Deploy to Move
./scripts/uninstall.sh       # Restore stock Move
```

## Host + Module System

```
Host (move-anything):
  - Owns /dev/ablspi0.0 for hardware communication
  - Embeds QuickJS for JavaScript execution
  - Manages module discovery and lifecycle
  - Routes MIDI to JS UI and DSP plugin

Modules (src/modules/<id>/):
  - module.json: metadata
  - ui.js: JavaScript UI (init, tick, onMidiMessage*)
  - dsp.so: optional native DSP plugin
```

### Key Source Files

- **src/move_anything.c**: Main host runtime
- **src/move_anything_shim.c**: LD_PRELOAD shim
- **src/host/plugin_api_v1.h**: DSP plugin C API
- **src/host/module_manager.c**: Module loading
- **src/host/menu_ui.js**: Host menu for module selection

### JS Host Functions

```javascript
host_list_modules()           // -> [{id, name, version}, ...]
host_load_module(id_or_index)
host_unload_module()
host_module_set_param(key, val)
host_module_get_param(key)
host_is_module_loaded()
host_get_current_module()
host_rescan_modules()
```

### Shared JS Utilities

Located in `src/shared/`:
- `constants.mjs` - MIDI CC/note mappings, colors, button constants
- `input_filter.mjs` - Capacitive touch filtering, setLED, setButtonLED
- `midi_messages.mjs` - MIDI helpers
- `move_display.mjs` - Display utilities

## Move Hardware MIDI

Pads: Notes 68-99 (32 pads, 4 rows × 8 columns)
Steps: Notes 16-31 (16 step buttons)
Tracks: CCs 40-43 (4 track buttons)

Key CCs: 14 (jog), 49 (shift), 50 (menu), 51 (back), 60 (copy), 85 (play), 86 (rec), 87 (loop)

Knobs: CCs 71-78 (endless encoders)
Knob touch: Notes 0-7 (capacitive sensing)

## LED Control

```javascript
setLED(note, color)       // Note-based RGB LEDs (pads, steps)
setButtonLED(cc, color)   // CC-based button LEDs (transport, knobs, etc.)
```

## Audio Specs

- Sample rate: 44100 Hz
- Block size: 128 frames
- Format: Stereo interleaved int16

## Testing

### SEQOMD DSP Tests

To compile and run the DSP unit tests:

```bash
cd src/modules/seqomd/dsp
gcc -o test_seq_plugin test_seq_plugin.c -I. -I../../.. -lm
./test_seq_plugin
```

The test file includes all DSP source files via `#include` directives, so only compile the test file itself.
