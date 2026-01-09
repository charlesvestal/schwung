# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

Move Anything is a framework for custom JavaScript and native DSP modules on Ableton Move hardware. It provides access to pads, encoders, buttons, display (128x64 1-bit), audio I/O, and MIDI via USB-A.

## Build Commands

```bash
./scripts/build.sh           # Build with Docker (auto-detects, recommended)
./scripts/package.sh         # Create move-anything.tar.gz
./scripts/clean.sh           # Remove build/ and dist/
./scripts/install.sh         # Deploy from GitHub release
./scripts/install.sh local   # Deploy from local build
./scripts/uninstall.sh       # Restore stock Move
```

Cross-compilation uses `${CROSS_PREFIX}gcc` for the Move's ARM architecture.

## Architecture

### Host + Module System

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

### Module Structure

```
src/modules/sf2/
  module.json       # Required
  ui.js             # JavaScript UI
  dsp.so            # Native DSP (built from dsp/)
  dsp/
    sf2_plugin.c
    third_party/tsf.h
```

### Plugin API (v1)

```c
typedef struct plugin_api_v1 {
    uint32_t api_version;
    int (*on_load)(const char *module_dir, const char *json_defaults);
    void (*on_unload)(void);
    void (*on_midi)(const uint8_t *msg, int len, int source);
    void (*set_param)(const char *key, const char *val);
    int (*get_param)(const char *key, char *buf, int buf_len);
    void (*render_block)(int16_t *out_interleaved_lr, int frames);
} plugin_api_v1_t;
```

Audio: 44100 Hz, 128 frames/block, stereo interleaved int16.

### JS Host Functions

```javascript
// Module management
host_list_modules()           // -> [{id, name, version}, ...]
host_load_module(id_or_index)
host_unload_module()
host_module_set_param(key, val)
host_module_get_param(key)
host_is_module_loaded()
host_get_current_module()
host_rescan_modules()

// Host volume control
host_get_volume()             // -> int (0-100)
host_set_volume(vol)          // set host volume

// Host input settings
host_get_setting(key)         // -> value (velocity_curve, aftertouch_enabled, aftertouch_deadzone)
host_set_setting(key, val)    // set setting
host_save_settings()          // persist to disk
host_reload_settings()        // reload from disk
```

### Host Volume Control

The volume knob (CC 79) controls host-level output volume by default. Volume is applied after module DSP rendering but before audio output.

Modules can claim the volume knob for their own use by setting `"claims_master_knob": true` in their module.json capabilities section. When claimed, the host passes the CC through to the module instead of adjusting volume.

### Shared JS Utilities

Located in `src/shared/`:
- `constants.mjs` - MIDI CC/note mappings
- `input_filter.mjs` - Capacitive touch filtering
- `midi_messages.mjs` - MIDI helpers
- `move_display.mjs` - Display utilities

## Move Hardware MIDI

Pads: Notes 68-99 (32 pads for notes/sounds)
Steps/Trigs: Notes 16-31 (16 step buttons for sequencing)
Tracks: Notes 40-43

Key CCs: 14 (jog), 49 (shift), 50 (menu), 71-78 (knob LEDs)

Notes 0-9: Capacitive touch from knobs (filter if not needed)

## LED Control

Two functions for controlling LEDs (from `input_filter.mjs`):

```javascript
setLED(note, color)       // Note-based RGB LEDs (pads, steps)
setButtonLED(cc, color)   // CC-based button LEDs (transport, etc.)
```

**Button LED mapping (input CC → LED control):**
```
Play:   CC 85 (MovePlay) → setButtonLED(MovePlay, color)
Record: CC 86 (MoveRec)  → setButtonLED(MoveRec, color)
Loop:   CC 87 (MoveLoop) → setButtonLED(MoveLoop, color)
```

Note: `MoveRecord` (118) is a separate RGB LED constant, but the record button's LED is controlled via CC 86 (`MoveRec`), not note 118.

## Audio Mailbox Layout

```
AUDIO_OUT_OFFSET = 256
AUDIO_IN_OFFSET  = 2304
AUDIO_BYTES_PER_BLOCK = 512
FRAMES_PER_BLOCK = 128
SAMPLE_RATE = 44100
```

Frame layout: [L0, R0, L1, R1, ..., L127, R127] as int16 little-endian.

## Deployment

On-device layout:
```
/data/UserData/move-anything/
  move-anything               # Host binary
  move-anything-shim.so       # Shim (also at /usr/lib/)
  host/menu_ui.js
  shared/
  modules/sf2/, m8/, controller/
```

Original Move preserved as `/opt/move/MoveOriginal`.

## Signal Chain Module

The `chain` module implements a modular signal chain for combining components:

```
[Input] → [MIDI FX] → [Sound Generator] → [Audio FX] → [Output]
```

### Module Capabilities for Chaining

Modules declare chainability in module.json:
```json
"capabilities": {
    "chainable": true,
    "component_type": "sound_generator"
}
```

Component types: `sound_generator`, `audio_fx`, `midi_fx`

### Chain Architecture

- Chain host (`modules/chain/dsp/chain_host.c`) loads sub-plugins via dlopen
- Forwards MIDI to sound generator, routes audio through effects
- Patch files in `modules/chain/patches/*.json` define chain configurations
- MIDI FX: chord generator, arpeggiator (up, down, up_down, random)
- Audio FX: freeverb

### External Modules

External modules (JV-880, OB-Xd) install their own chain presets via their install scripts.

## Dependencies

- QuickJS: libs/quickjs/
- stb_image.h: src/lib/
- TinySoundFont: src/modules/sf2/dsp/third_party/tsf.h
