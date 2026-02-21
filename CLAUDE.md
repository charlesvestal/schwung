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
- `utility` - Utility modules
- `overtake` - Overtake modules (full UI control in shadow mode)
- `system` - System modules (Module Store), shown last

The main menu automatically organizes modules by category, reading from each module's `component_type` field.

### Plugin API v2 (Recommended)

V2 supports multiple instances and is required for Signal Chain integration:

```c
typedef struct plugin_api_v2 {
    uint32_t api_version;              // Must be 2
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_lr, int frames);
} plugin_api_v2_t;

// Entry point
extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host);
```

### Plugin API v1 (Deprecated)

V1 is a singleton API - only one instance can exist. Do not use for new modules:

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

// Display control
host_flush_display()          // Force immediate display update
host_set_refresh_rate(hz)     // Set display refresh rate
host_get_refresh_rate()       // Get current refresh rate

// File system utilities (used by Module Store)
host_file_exists(path)        // -> bool
host_read_file(path)          // -> string or null
host_http_download(url, dest) // -> bool
host_extract_tar(tarball, dir) // -> bool
host_remove_dir(path)         // -> bool
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
- `store_utils.mjs` - Module Store catalog fetching and install/remove functions

## Move Hardware MIDI

Pads: Notes 68-99
Steps: Notes 16-31
Tracks: CCs 40-43 (reversed: CC43=Track1, CC40=Track4)

Key CCs: 3 (jog click), 14 (jog turn), 49 (shift), 50 (menu), 51 (back), 71-78 (knobs)

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
  modules/
    chain/, controller/, store/     # Built-in modules (root level)
    sound_generators/<id>/          # External sound generators
    audio_fx/<id>/                  # External audio effects
    midi_fx/<id>/                   # External MIDI effects
```

External modules are installed to category subdirectories based on their `component_type`.

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

### Shadow UI Parameter Hierarchy

Modules expose parameters to the Shadow UI via `ui_hierarchy` in their get_param response. The hierarchy defines menu structure, knob mappings, and navigation.

**Structure:**
```json
{
  "modes": null,
  "levels": {
    "root": {
      "label": "SF2",
      "list_param": "preset",
      "count_param": "preset_count",
      "name_param": "preset_name",
      "children": null,
      "knobs": ["octave_transpose", "gain"],
      "params": [
        {"key": "octave_transpose", "label": "Octave"},
        {"key": "gain", "label": "Gain"},
        {"level": "soundfont", "label": "Choose Soundfont"}
      ]
    },
    "soundfont": {
      "label": "Soundfont",
      "items_param": "soundfont_list",
      "select_param": "soundfont_index",
      "children": null,
      "knobs": [],
      "params": []
    }
  }
}
```

**Key fields:**

- `knobs`: Array of **strings** (parameter keys) mapped to physical knobs 1-8
- `params`: Array for menu items. Each entry is either:
  - **String**: Parameter key (e.g., `"gain"`)
  - **Editable param object**: `{"key": "gain", "label": "Gain"}`
  - **Navigation object**: `{"level": "soundfont", "label": "Choose Soundfont"}`
- `list_param`/`count_param`/`name_param`: For preset browser levels
- `items_param`/`select_param`: For dynamic item selection levels

**Important:** Use `key` (not `param`) for editable parameter objects. Metadata (type, min, max) comes from `chain_params`.

### Chain Parameters

Modules expose parameter metadata via `chain_params` in their get_param response. This is **required** for the Shadow UI to properly edit parameters (with correct step sizes, ranges, and enum options):

```c
if (strcmp(key, "chain_params") == 0) {
    const char *json = "["
        "{\"key\":\"cutoff\",\"name\":\"Cutoff\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"LP\",\"HP\",\"BP\"]}"
    "]";
    strcpy(buf, json);
    return strlen(json);
}
```

Parameter types: `float` (with `min`, `max`, `step`), `int` (with `min`, `max`), `enum` (with `options` array).
Optional fields: `default`, `unit`, `display_format`.

These provide metadata for the Shadow UI parameter editor alongside `ui_hierarchy` (which defines menu structure and knob mappings).

### Chain Architecture

- Chain host (`modules/chain/dsp/chain_host.c`) loads sub-plugins via dlopen
- Forwards MIDI to sound generator, routes audio through effects
- Patch files stored in `/data/UserData/move-anything/patches/*.json` on device define chain configurations
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
- `braids` - Mutable Instruments macro oscillator (47 algorithms)
- `rings` - Mutable Instruments resonator
- `sf2` - SoundFont synthesizer (TinySoundFont)
- `dexed` - 6-operator FM synthesizer (Dexed/MSFA)
- `minijv` - ROM-based PCM rompler emulator
- `obxd` - Oberheim OB-X emulator
- `clap` - CLAP plugin host

**Audio FX:**
- `cloudseed` - Algorithmic reverb
- `psxverb` - PlayStation SPU reverb
- `tapescam` - Tape saturation
- `tapedelay` - Tape delay with flutter and saturation

**Utilities/Overtake:**
- `m8` - Dirtywave M8 Launchpad Pro emulator
- `sidcontrol` - SID Control for SIDaster III
- `controller` - MIDI Controller with 16 banks (built-in)

External modules install their own Signal Chain presets via their install scripts.

## Shadow Mode

Shadow Mode runs custom signal chains alongside stock Move. The shim intercepts hardware I/O to mix shadow audio with Move's output.

### Shadow Mode Shortcuts

- **Shift+Vol+Track 1-4**: Open shadow mode / jump to slot settings (works from Move or Shadow UI)
- **Shift+Vol+Menu**: Jump directly to Master FX settings
- **Shift+Vol+Step2**: Open Global Settings
- **Shift+Vol+Jog Click**: Exit overtake module (when in overtake mode)

### Quantized Sampler

- Shift+Sample opens the sampler
- Choose source: resample (including Move Everything synths), or Move Input (whatever is set in the regular sample flow)
- Choose duration in bars (or until stopped). Uses MIDI clock to determine tempo, falling back to project tempo if not found.
- Starts on a note event or pressing play
- Recordings are saved to `Samples/Move Everything/`

Works for resampling your Move, including Move Everything synths, or a line-in source or microphone. You can use Move's built-in count-in for line-in recordings too.

### Skipback

Shift+Capture writes the last 30 seconds of audio to disk. Uses the same source as the quantized sampler (resample or Move Input). Saved to `Samples/Move Everything/Skipback/`.

### Shadow Architecture

```
src/move_anything_shim.c    # LD_PRELOAD shim - intercepts ioctl, mixes audio
src/shadow/shadow_ui.js     # Shadow UI - slot/patch management
src/host/shadow_constants.h # Shared memory structures
```

Key shared memory segments:
- `/move-shadow-audio` - Shadow's mixed audio output
- `/move-shadow-control` - Control flags and state (shadow_control_t)
- `/move-shadow-param` - Parameter get/set requests (shadow_param_t)
- `/move-shadow-ui` - UI state for slot info (shadow_ui_state_t)

### Shadow UI Flags

Communication between shim and Shadow UI uses flags in `shadow_control_t.ui_flags`:
- `SHADOW_UI_FLAG_JUMP_TO_SLOT (0x01)` - Jump to slot settings on open
- `SHADOW_UI_FLAG_JUMP_TO_MASTER_FX (0x02)` - Jump to Master FX on open
- `SHADOW_UI_FLAG_JUMP_TO_OVERTAKE (0x04)` - Jump to overtake module menu

### Shadow Slot Features

Each of the 4 shadow slots has:
- **Receive channel**: MIDI channel to listen on (default 1-4)
- **Forward channel**: Remap MIDI to specific channel for synths that require it (-1 = auto/passthrough)
- **Volume**: Per-slot volume control
- **State persistence**: Synth, audio FX, and MIDI FX states saved/restored automatically

### MIDI Cable Filtering

The shim filters MIDI by USB cable number in the hardware MIDI buffers:

**MIDI_IN buffer (offset 2048):**
- Cable 0: Internal Move hardware controls (pads, knobs, buttons)
- Cable 2: External USB MIDI input (devices connected to Move's USB-A port)

**MIDI_OUT buffer (offset 0):**
- Cable 0: Move's internal MIDI output
- Cable 2: External USB MIDI output

**Routing rules:**
- Normal shadow mode: Only cable 0 (internal controls) is processed
- Overtake mode: All cables are forwarded, including external USB MIDI (cable 2)
- External MIDI from cable 2 is routed to `onMidiMessageExternal` in overtake modules

**Important:** If Move tracks are configured to listen and output on the same MIDI channel, external MIDI may be echoed back. Configure Move tracks to use different channels than your external device to avoid interference.

### Master FX Chain

Shadow mode includes a 4-slot Master FX chain that processes mixed output from all shadow slots. Access via Shift+Vol+Menu.

### Overtake Modules

Overtake modules take complete control of Move's UI in shadow mode. They're accessed via the shadow UI's "Overtake Modules" menu.

**Module Requirements:**
- Set `"component_type": "overtake"` in module.json
- Use progressive LED initialization (buffer holds ~64 packets)
- Handle all MIDI input via `onMidiMessageInternal`/`onMidiMessageExternal`

**Lifecycle:**
1. Host clears all LEDs progressively (shows "Loading...")
2. ~500ms delay before calling module's `init()`
3. Module runs with full UI control
4. Shift+Vol+Jog Click triggers exit (host-level, always works)
5. Host clears LEDs progressively (shows "Exiting...")
6. Returns to Move

**LED Buffer Constraint:**
The MIDI output buffer holds ~64 USB-MIDI packets. Sending more than 60 LED commands per frame causes overflow. Use progressive LED initialization:

```javascript
const LEDS_PER_FRAME = 8;
let ledInitPending = true;
let ledInitIndex = 0;

globalThis.tick = function() {
    if (ledInitPending) {
        // Set 8 LEDs per frame
        setupLedBatch();
    }
    drawUI();
};
```

**Key Files:**
- `src/shadow/shadow_ui.js` - Overtake module loading and lifecycle
- `src/modules/controller/ui.js` - Example overtake module

**External Device Protocols:**
If your overtake module communicates with an external USB device that expects an initialization handshake (like the M8's Launchpad Pro protocol), be proactive—don't wait for the device to initiate. The device may have already sent its request before your module loaded due to the ~500ms init delay. Instead:
- Send your identification/init message in `init()` immediately
- Optionally retry periodically in `tick()` until connection is confirmed
- Detect connection from any valid response (not just the specific handshake message)

## Module Store

The Module Store (`store` module) downloads and installs external modules from GitHub releases. The catalog is fetched from:
`https://raw.githubusercontent.com/charlesvestal/move-anything/main/module-catalog.json`

### Catalog Format (v2)

```json
{
  "catalog_version": 2,
  "host": {
    "name": "Move Anything",
    "github_repo": "charlesvestal/move-anything",
    "asset_name": "move-anything.tar.gz",
    "latest_version": "0.3.11",
    "download_url": "https://github.com/.../move-anything.tar.gz",
    "min_host_version": "0.1.0"
  },
  "modules": [
    {
      "id": "mymodule",
      "name": "My Module",
      "description": "What it does",
      "author": "Your Name",
      "component_type": "sound_generator",
      "github_repo": "username/move-anything-mymodule",
      "asset_name": "mymodule-module.tar.gz",
      "min_host_version": "0.1.0"
    }
  ]
}
```

### How the Store Works

1. Fetches `module-catalog.json` and extracts `catalog.modules` array
2. For each module, queries GitHub API for latest release
3. Looks for asset matching `<module-id>-module.tar.gz`
4. Compares release version to installed version
5. Downloads and extracts tarball to category subdirectory (e.g., `modules/sound_generators/<id>/`)

### Adding a Module to the Catalog

Edit `module-catalog.json` and add an entry to the `modules` array (see format above).

## External Module Development

External modules live in separate repos (e.g., `move-anything-sf2`, `move-anything-obxd`).

### Module Repo Structure

```
move-anything-<id>/
  src/
    module.json       # Module metadata (id, name, version, capabilities)
    ui.js             # JavaScript UI
    dsp/              # Native DSP code (if applicable)
      plugin.c/cpp
  scripts/
    build.sh          # Build script (creates dist/ and tarball)
    install.sh        # Deploy to Move device
    Dockerfile        # Cross-compilation environment
  .github/
    workflows/
      release.yml     # Automated release on tag push
```

### Build Script Requirements

`scripts/build.sh` must:
1. Cross-compile DSP code for ARM64 (via Docker)
2. Package files to `dist/<module-id>/`
3. Create tarball at `dist/<module-id>-module.tar.gz`

Example tarball creation (at end of build.sh):
```bash
# Create tarball for release
cd dist
tar -czvf mymodule-module.tar.gz mymodule/
cd ..
```

### Release Workflow

`.github/workflows/release.yml` triggers on version tags:
```yaml
name: Release
on:
  push:
    tags: ['v*']

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: docker/setup-buildx-action@v3
      - run: |
          docker build -t module-builder -f scripts/Dockerfile .
          docker run --rm -v "$PWD:/build" -w /build module-builder ./scripts/build.sh
      - uses: softprops/action-gh-release@v1
        with:
          files: dist/<module-id>-module.tar.gz
```

### Releasing a New Version

1. Update version in `src/module.json`
2. Commit: `git commit -am "bump version to 0.2.0"`
3. Tag and push: `git tag v0.2.0 && git push --tags`
4. GitHub Actions builds and uploads tarball to release

The Module Store will see the new version within minutes.

See `BUILDING.md` for detailed documentation.

## Dependencies

- QuickJS: libs/quickjs/
- stb_image.h: src/lib/
- curl: libs/curl/ (for Module Store downloads)
