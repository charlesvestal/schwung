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
  - ui_chain.js: optional Signal Chain UI shim
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
src/modules/<id>/
  module.json       # Required - metadata and capabilities
  ui.js             # JavaScript UI
  dsp.so            # Optional native DSP plugin
```

Built-in modules (in main repo):
- `chain` - Signal Chain for combining components
- `controller` - MIDI Controller with 16 banks
- `store` - Module Store for downloading external modules

### Module Categorization

Modules declare their category via `component_type` in module.json:

```json
{
    "id": "my-module",
    "name": "My Module",
    "component_type": "sound_generator"
}
```

Valid component types:
- `featured` - Featured modules (Signal Chain), shown first
- `sound_generator` - Synths and samplers
- `audio_fx` - Audio effects
- `midi_fx` - MIDI processors
- `utility` - Utility modules (MIDI Controller, M8 emulator)
- `system` - System modules (Module Store), shown last

The main menu automatically organizes modules by category, reading from each module's `component_type` field.

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
host_list_modules()           // -> [{id, name, version, component_type}, ...]
host_load_module(id_or_index)
host_load_ui_module(path)
host_unload_module()
host_return_to_menu()
host_module_set_param(key, val)
host_module_get_param(key)
host_module_send_midi(msg, source)
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
- `menu_layout.mjs` - Title/list/footer menu layout helpers

## Move Hardware MIDI

Pads: Notes 68-99
Steps: Notes 16-31
Tracks: Notes 40-43

Key CCs: 14 (jog), 49 (shift), 50 (menu), 71-78 (knob LEDs)

Notes 0-9: Capacitive touch from knobs (filter if not needed)

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
  modules/chain/, controller/, store/  # Built-in modules
```

External modules are downloaded via Module Store to the same modules/ directory.

Original Move preserved as `/opt/move/MoveOriginal`.

## Signal Chain Module

The `chain` module implements a modular signal chain for combining components:

```
[Input or MIDI Source] → [MIDI FX] → [Sound Generator] → [Audio FX] → [Output]
```

### Module Capabilities for Chaining

Modules declare chainability in module.json:
```json
{
    "capabilities": {
        "chainable": true,
        "component_type": "sound_generator"
    }
}
```

Component types: `sound_generator`, `audio_fx`, `midi_fx`

### Chain Architecture

- Chain host (`modules/chain/dsp/chain_host.c`) loads sub-plugins via dlopen
- Forwards MIDI to sound generator, routes audio through effects
- Patch files in `modules/chain/patches/*.json` define chain configurations
- MIDI FX: chord generator, arpeggiator (up, down, up_down, random)
- Audio FX: freeverb
- MIDI sources (optional): DSP modules that generate MIDI; can provide `ui_chain.js` for full-screen chain UI

### Recording

Signal Chain supports recording audio output to WAV files:

- **Record Button** (CC 118): Toggles recording on/off
- **LED States**: Off (no patch), White (patch loaded), Red (recording)
- **Output**: Recordings saved to `/data/UserData/move-anything/recordings/rec_YYYYMMDD_HHMMSS.wav`
- **Format**: 44.1kHz, 16-bit stereo WAV

Recording uses a background thread with a 2-second ring buffer to prevent audio dropouts during disk I/O. Recording requires a patch to be loaded.

### External Modules

External modules are maintained in separate repositories and available via Module Store:

**Sound Generators:**
- `sf2` - SoundFont synthesizer (TinySoundFont)
- `dx7` - Yamaha DX7 FM synthesizer (Dexed/MSFA)
- `jv880` - Roland JV-880 emulator
- `obxd` - Oberheim OB-X emulator
- `clap` - CLAP plugin host

**Audio FX:**
- `cloudseed` - Algorithmic reverb
- `psxverb` - PlayStation SPU reverb
- `tapescam` - Tape saturation
- `spacecho` - RE-201 style tape delay

**Utilities:**
- `m8` - Dirtywave M8 Launchpad Pro emulator

External modules install their own Signal Chain presets via their install scripts.

## Module Store

The Module Store (`store` module) downloads and installs external modules from GitHub releases. The catalog is fetched from:
`https://raw.githubusercontent.com/charlesvestal/move-anything/main/module-catalog.json`

## Dependencies

- QuickJS: libs/quickjs/
- stb_image.h: src/lib/
- curl: libs/curl/ (for Module Store downloads)
