# Module Development Guide

Modules are self-contained packages that extend Move Anything with new functionality.

## Module Structure

```
src/modules/your-module/
  module.json       # Required: module metadata
  ui.js             # Optional: JavaScript UI
  ui_chain.js       # Optional: Signal Chain UI shim
  dsp.so            # Optional: native DSP plugin
  dsp/              # Optional: DSP source code
    your_plugin.c
```

## module.json

```json
{
    "id": "your-module",
    "name": "Your Module Name",
    "version": "1.0.0",
    "description": "What your module does",
    "author": "Your Name",
    "ui": "ui.js",
    "ui_chain": "ui_chain.js",
    "dsp": "dsp.so",
    "api_version": 1
}
```

Required fields: `id`, `name`, `version`, `api_version`
Optional fields: `description`, `author`, `ui`, `ui_chain`, `dsp`, `defaults`, `capabilities`

Notes:
- `module.json` is parsed by a minimal JSON reader. Use double quotes for keys, lowercase `true`/`false`, and avoid comments.
- Keep `module.json` reasonably small (the loader caps it at 8KB).

### Capabilities

Add capability flags to enable special module behaviors. You can group them under
`capabilities` (recommended) or place them at the top level (the host searches
for keys anywhere in `module.json`).

```json
{
    "id": "your-module",
    "name": "Your Module",
    "version": "1.0.0",
    "api_version": 1,
    "capabilities": {
        "audio_out": true,
        "midi_in": true,
        "claims_master_knob": true
    }
}
```

| Capability | Description |
|------------|-------------|
| `audio_out` | Module produces audio |
| `audio_in` | Module uses audio input |
| `midi_in` | Module processes MIDI input |
| `midi_out` | Module sends MIDI output |
| `aftertouch` | Module uses aftertouch |
| `claims_master_knob` | Module handles volume knob (CC 79) instead of host |
| `raw_midi` | Skip host MIDI transforms (velocity curve, aftertouch filter); module may also bypass internal MIDI filters when set |
| `raw_ui` | Module owns UI input handling; host won't intercept Back to return to menu (use `host_return_to_menu()` to exit) |
| `chainable` | Marks a module as usable inside Signal Chain patches (metadata) |
| `component_type` | `sound_generator` for chainable synths (other values reserved for future use) |

### Defaults

Use `defaults` to pass initial parameters to DSP plugins at load time:

```json
{
    "defaults": {
        "preset": 0,
        "output_level": 50
    }
}
```

## Drop-In Modules

Modules are discovered at runtime from `/data/UserData/move-anything/modules`.
To add a new module, copy a folder with `module.json` (plus `ui.js` and `dsp.so`
if needed) and either restart Move Anything or call `host_rescan_modules()` in
your UI. No host recompile is required for new modules or UI updates.

## JavaScript UI (ui.js)

Module UIs are loaded as ES modules, so you can import shared utilities:

```javascript
import {
    MoveMainKnob, MoveShift, MoveMenu,
    MovePad1, MovePad32,
    MidiNoteOn, MidiCC
} from '../../shared/constants.mjs';

/* Module state */
let counter = 0;

/* Called once when module loads */
globalThis.init = function() {
    console.log("Module starting...");
    clear_screen();
    print(2, 2, "Hello Move!", 2);
}

/* Called every frame (~60fps) */
globalThis.tick = function() {
    // Update display here
}

/* Handle MIDI from external USB devices */
globalThis.onMidiMessageExternal = function(data) {
    // data = [status, data1, data2]
}

/* Handle MIDI from Move hardware */
globalThis.onMidiMessageInternal = function(data) {
    const isNoteOn = data[0] === 0x90;
    const note = data[1];
    const velocity = data[2];

    // Ignore capacitive touch from knobs
    if (note < 10) return;

    // Handle pad press
    if (isNoteOn && note >= 68 && note <= 99) {
        console.log("Pad pressed: " + note);
    }
}
```

### Signal Chain UI Shims

Modules can expose a full-screen UI when used as a Signal Chain MIDI source by
adding `ui_chain.js` (or setting `"ui_chain"` in `module.json` to a different
filename). The file should set `globalThis.chain_ui`:

```javascript
globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal,
    onMidiMessageExternal
};
```

Do not override `globalThis.init` or `globalThis.tick` in `ui_chain.js`.
Make sure to ship `ui_chain.js` in your build/install step if you use it.
The host itself ignores `ui_chain`; it is consumed by the Signal Chain UI when
loading a MIDI source module.

Example `ui_chain.js` wrapper:

```javascript
import {
    init,
    tick,
    onMidiMessageInternal,
    onMidiMessageExternal
} from './ui_core.mjs';

globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal,
    onMidiMessageExternal
};
```

### Menu Layout Helpers

For list-based screens (title/list/footer), use the shared menu layout helpers:

```javascript
import {
    drawMenuHeader,
    drawMenuList,
    drawMenuFooter,
    menuLayoutDefaults
} from '../../shared/menu_layout.mjs';

const items = [
    { label: "Velocity", value: "Hard" },
    { label: "Aftertouch", value: "On" }
];

drawMenuHeader("Settings");
drawMenuList({
    items,
    selectedIndex,
    listArea: {
        topY: menuLayoutDefaults.listTopY,
        bottomY: menuLayoutDefaults.listBottomWithFooter
    },
    valueAlignRight: true,
    getLabel: (item) => `${item.label}:`,
    getValue: (item) => item.value
});
drawMenuFooter("Back:back  </>:change");
```

`drawMenuList` will derive row count from the list area and scroll automatically. When `valueAlignRight` is enabled, labels are truncated with `...` if they would overlap the value.

## Menu System

For modules that need hierarchical settings menus, the shared menu system provides a complete solution for navigation, input handling, and rendering.

### Menu Item Types

Import factory functions from `menu_items.mjs`:

```javascript
import {
    MenuItemType,
    createSubmenu,
    createValue,
    createEnum,
    createToggle,
    createAction,
    createBack,
    formatItemValue,
    isEditable
} from '../../shared/menu_items.mjs';
```

| Type | Factory | Description |
|------|---------|-------------|
| `SUBMENU` | `createSubmenu(label, getMenu)` | Navigate to child menu |
| `VALUE` | `createValue(label, {get, set, min, max, step, fineStep, format})` | Numeric value with range |
| `ENUM` | `createEnum(label, {get, set, options, format})` | Cycle through string options |
| `TOGGLE` | `createToggle(label, {get, set, onLabel, offLabel})` | Boolean on/off |
| `ACTION` | `createAction(label, onAction)` | Execute callback on click |
| `BACK` | `createBack(label)` | Return to parent menu |

Example menu definition:

```javascript
function getSettingsMenu() {
    return [
        createEnum('Velocity', {
            get: () => host_get_setting('velocity_curve'),
            set: (v) => { host_set_setting('velocity_curve', v); host_save_settings(); },
            options: ['linear', 'soft', 'hard', 'full']
        }),
        createValue('AT Deadzone', {
            get: () => host_get_setting('aftertouch_deadzone'),
            set: (v) => { host_set_setting('aftertouch_deadzone', v); host_save_settings(); },
            min: 0, max: 50, step: 5, fineStep: 1
        }),
        createToggle('Aftertouch', {
            get: () => host_get_setting('aftertouch_enabled') === 1,
            set: (v) => { host_set_setting('aftertouch_enabled', v ? 1 : 0); host_save_settings(); }
        }),
        createSubmenu('Advanced', () => getAdvancedMenu()),
        createBack()
    ];
}
```

### Menu Navigation

The `menu_nav.mjs` module handles all input for menu navigation:

```javascript
import { createMenuState, handleMenuInput } from '../../shared/menu_nav.mjs';
import { createMenuStack } from '../../shared/menu_stack.mjs';

const menuState = createMenuState();
const menuStack = createMenuStack();

// Initialize with root menu
menuStack.push({ title: 'Settings', items: getSettingsMenu() });

// In onMidiMessageInternal:
function onMidiMessageInternal(data) {
    if ((data[0] & 0xF0) === 0xB0) {  // CC message
        const cc = data[1];
        const value = data[2];
        const current = menuStack.current();

        const result = handleMenuInput({
            cc, value,
            items: current.items,
            state: menuState,
            stack: menuStack,
            shiftHeld: isShiftHeld,
            onBack: () => host_return_to_menu()
        });

        if (result.needsRedraw) {
            redraw();
        }
    }
}
```

**Navigation behavior:**
- **Jog wheel**: Scroll list (navigation) or adjust value (editing)
- **Jog click**: Enter submenu, start/confirm edit, execute action
- **Up/Down arrows**: Scroll list
- **Left/Right arrows**: Quick-adjust values without entering edit mode
- **Back button**: Cancel edit or go back in menu stack

### Encoder Acceleration

When editing numeric values with the jog wheel, acceleration provides smooth control:

```javascript
import { decodeDelta, decodeAcceleratedDelta } from '../../shared/input_filter.mjs';

// Simple delta (±1) for navigation
const delta = decodeDelta(ccValue);

// Accelerated delta for value editing
// Slow turns = step 1, fast turns = step up to 10
const accelDelta = decodeAcceleratedDelta(ccValue, 'my_encoder');
```

- Slow turns (<150ms between events): step = 1 (fine control)
- Fast turns (<25ms between events): step = 10 (coarse control)
- In between: interpolated step size
- Hold **Shift** for fine control (always step 1)

### Text Scrolling

Long labels automatically scroll after a delay:

```javascript
import { createTextScroller, getMenuLabelScroller } from '../../shared/text_scroll.mjs';

// Use singleton for menu labels
const scroller = getMenuLabelScroller();

// In tick():
scroller.setSelected(selectedIndex);  // Reset scroll on selection change
if (scroller.tick()) {
    redraw();  // Scroll position changed
}

// When rendering:
const displayText = scroller.getScrolledText(fullLabel, maxChars);
```

**Scroll behavior:**
- 2 second delay before scrolling starts
- ~100ms between scroll steps
- 2 second pause at end, then reset

### Menu Stack

For hierarchical menus with back navigation:

```javascript
import { createMenuStack } from '../../shared/menu_stack.mjs';

const stack = createMenuStack();

// Push root menu
stack.push({ title: 'Main', items: mainMenuItems });

// Navigate to submenu
stack.push({ title: 'Settings', items: settingsItems, selectedIndex: 0 });

// Go back
stack.pop();

// Get current menu
const current = stack.current();  // { title, items, selectedIndex }

// Get breadcrumb path
const path = stack.getPath();  // ['Main', 'Settings']
```

## Native DSP Plugin

For audio synthesis/processing, create a native plugin implementing the C API.

### Plugin API (v1)

```c
#include "plugin_api_v1.h"

static int on_load(const char *module_dir, const char *json_defaults) {
    // Initialize audio engine
    return 0;  // 0 = success
}

static void on_unload(void) {
    // Cleanup
}

static void on_midi(const uint8_t *msg, int len, int source) {
    // source: 0 = internal (Move), 1 = external (USB)
    // Handle MIDI for sound generation
}

static void set_param(const char *key, const char *val) {
    // Handle parameter changes from JS
}

static int get_param(const char *key, char *buf, int buf_len) {
    // Return parameter value
    return 0;
}

static void render_block(int16_t *out_lr, int frames) {
    // Generate 'frames' stereo samples
    // Output format: [L0, R0, L1, R1, ...]
    // Sample range: -32768 to 32767
}

/* Export the plugin API */
static plugin_api_v1_t api = {
    .api_version = 1,
    .on_load = on_load,
    .on_unload = on_unload,
    .on_midi = on_midi,
    .set_param = set_param,
    .get_param = get_param,
    .render_block = render_block,
};

const plugin_api_v1_t* move_plugin_init_v1(const host_api_v1_t* host) {
    return &api;
}
```

### Building DSP Plugins

Add to `scripts/build.sh`:

```bash
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/your-module/dsp/your_plugin.c \
    -o build/modules/your-module/dsp.so \
    -Isrc -Isrc/modules/your-module/dsp \
    -lm
```

## JS ↔ DSP Communication

Use `host_module_set_param()` and `host_module_get_param()` in your UI:

```javascript
// In ui.js
host_module_set_param("preset", "5");
let current = host_module_get_param("preset");
```

The DSP plugin receives these in `set_param()` and `get_param()`.

## Shared Utilities

Import path from modules: `../../shared/<file>.mjs`

| File | Contents |
|------|----------|
| `constants.mjs` | Hardware constants (pads, buttons, knobs), MIDI message types, colors |
| `input_filter.mjs` | Capacitive touch filtering, LED control, encoder delta decoding with acceleration |
| `menu_items.mjs` | Menu item types and factory functions |
| `menu_nav.mjs` | Menu input handling (jog wheel, arrows, back button) |
| `menu_stack.mjs` | Hierarchical menu navigation stack |
| `menu_render.mjs` | Menu rendering with scroll support |
| `menu_layout.mjs` | Title/list/footer menu layout helpers |
| `text_scroll.mjs` | Marquee scrolling for long text |
| `move_display.mjs` | Display utilities |

### Common Imports

```javascript
import {
    // Colors
    Black, White, LightGrey, BrightRed, BrightGreen,

    // MIDI message types
    MidiNoteOn, MidiNoteOff, MidiCC,

    // Hardware buttons (CC numbers)
    MoveShift, MoveMenu, MoveBack, MoveCapture,
    MoveUp, MoveDown, MoveLeft, MoveRight,
    MoveMainKnob, MoveMainButton,

    // Grouped arrays (preferred)
    MovePads,         // [68-99] all 32 pads
    MoveSteps,        // [16-31] all 16 step buttons
    MoveCCButtons,    // All CC button numbers
    MoveRGBLeds,      // All RGB LED addresses
    MoveWhiteLeds,    // All white LED addresses
} from '../../shared/constants.mjs';

// Usage:
if (MovePads.includes(note)) { /* handle pad */ }
const padIndex = note - MovePads[0];  // 0-31
```

## Example Modules

See these modules for reference:

- **chain**: Signal chain with synths, MIDI FX, and audio FX
- **dx7**: DX7 FM synthesizer with native DSP (loads .syx patches)
- **sf2**: SoundFont synthesizer with native DSP
- **m8**: MIDI translator (UI-only, no DSP)
- **controller**: MIDI controller with banks (UI-only)

## Signal Chain Module

The Signal Chain module allows combining MIDI sources, MIDI effects, sound generators, and audio effects into configurable patches.

### Chain Structure

```
[Input or MIDI Source] → [MIDI FX] → [Sound Generator] → [Audio FX] → [Output]
```

### Available Components

| Type | Components |
|------|------------|
| MIDI Sources | Sequencers or other modules referenced via `midi_source` |
| Sound Generators | Line In, SF2, DX7, CLAP, plus any module marked `"chainable": true` with `"component_type": "sound_generator"` (for example `obxd`, `jv880`) |
| MIDI Effects | Chord generator (major, minor, power, octave), Arpeggiator (up, down, updown, random) |
| Audio Effects | Freeverb (reverb), CLAP effects |

### CLAP Host Module

The CLAP module (separate repo: `move-anything-clap`) hosts arbitrary CLAP audio plugins:

- Place `.clap` plugin files in `/data/UserData/move-anything/modules/clap/plugins/`
- Plugins are discovered at load time
- Use jog wheel to browse plugins, encoders to control parameters
- CLAP synths work as sound generators in Signal Chain
- CLAP effects can be used in the audio FX slot

### Patch Files

Patches are stored in `modules/chain/patches/` as JSON:

```json
{
    "name": "Arp Piano Verb",
    "version": 1,
    "chain": {
        "input": "pads",
        "midi_fx": {
            "arp": {
                "mode": "up",
                "bpm": 120,
                "division": "8th"
            }
        },
        "synth": {
            "module": "sf2",
            "config": {
                "preset": 0
            }
        },
        "midi_source": {
            "module": "sequencer"
        },
        "audio_fx": [
            {
                "type": "freeverb",
                "params": {
                    "room_size": 0.8,
                    "wet": 0.3
                }
            }
        ]
    }
}
```

JavaScript MIDI FX can be added per patch:

```json
"midi_fx_js": ["octave_up", "fifths"]
```

### Line In Sound Generator

The Line In sound generator passes external audio through the chain for processing:

```json
{
    "name": "Line In + Reverb",
    "chain": {
        "synth": {
            "module": "linein",
            "config": {}
        },
        "audio_fx": [
            {"type": "freeverb", "params": {"wet": 0.4}}
        ]
    }
}
```

Note: Audio input routing depends on the last selected input in the stock Move interface.

## Audio FX Plugin API

Audio effects use a simpler in-place processing API:

```c
typedef struct audio_fx_api_v1 {
    uint32_t api_version;
    int (*on_load)(const char *module_dir, const char *config_json);
    void (*on_unload)(void);
    void (*process_block)(int16_t *audio_inout, int frames);  // In-place stereo
    void (*set_param)(const char *key, const char *val);
    int (*get_param)(const char *key, char *buf, int buf_len);
} audio_fx_api_v1_t;

// Entry point
audio_fx_api_v1_t* move_audio_fx_init_v1(const host_api_v1_t *host);
```

## Audio Specifications

- Sample rate: 44100 Hz
- Block size: 128 frames
- Latency: ~3ms
- Format: Stereo interleaved int16

## Publishing to Module Store

External modules can be distributed via the built-in Module Store. Users can browse, install, update, and remove modules directly from their Move device.

### Requirements

1. Module builds as a self-contained tarball: `<id>-module.tar.gz`
2. Tarball extracts to a folder matching the module ID (e.g., `jv880/`)
3. GitHub repository with releases enabled
4. GitHub Actions workflow for automated builds

### Tarball Structure

```
<id>-module.tar.gz
  └── <id>/
      ├── module.json       # Required
      ├── ui.js             # Optional: JavaScript UI
      ├── dsp.so            # Optional: Native DSP plugin
      └── ...               # Other module files
```

### Release Workflow

1. **Make changes and update version** in `src/module.json`:
   ```json
   {
     "version": "0.2.0"
   }
   ```

2. **Commit and tag the release**:
   ```bash
   git add .
   git commit -m "Release v0.2.0"
   git tag v0.2.0
   git push && git push --tags
   ```

3. **GitHub Actions automatically**:
   - Builds the module using Docker cross-compilation
   - Creates `<id>-module.tar.gz`
   - Attaches it to the GitHub release

4. **Update the catalog** in `move-anything/module-catalog.json`:
   ```json
   {
     "id": "your-module",
     "latest_version": "0.2.0",
     "download_url": "https://github.com/user/repo/releases/download/v0.2.0/your-module.tar.gz"
   }
   ```

5. **Commit catalog update**:
   ```bash
   cd move-anything
   git add module-catalog.json
   git commit -m "Update your-module to v0.2.0"
   git push
   ```

### GitHub Actions Workflow Template

Add `.github/workflows/release.yml` to your module repository:

```yaml
name: Release

on:
  push:
    tags:
      - 'v*'

jobs:
  build:
    runs-on: ubuntu-latest

    steps:
      - uses: actions/checkout@v4

      - name: Set up Docker
        uses: docker/setup-buildx-action@v3

      - name: Build module
        run: ./scripts/build.sh

      - name: Package module
        run: |
          cd dist
          tar -czvf ../${{ github.event.repository.name }}-module.tar.gz */

      - name: Create Release
        uses: softprops/action-gh-release@v1
        with:
          files: ${{ github.event.repository.name }}-module.tar.gz
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
```

### Catalog Entry Schema

Each module in `module-catalog.json`:

```json
{
  "id": "module-id",
  "name": "Display Name",
  "description": "Short description",
  "author": "Author Name",
  "component_type": "sound_generator|audio_fx|midi_fx|midi_source",
  "latest_version": "1.0.0",
  "min_host_version": "1.0.0",
  "download_url": "https://github.com/user/repo/releases/download/v1.0.0/module.tar.gz"
}
```

### Component Types

| Type | Description |
|------|-------------|
| `sound_generator` | Synthesizers and samplers that produce audio |
| `audio_fx` | Audio effects that process audio |
| `midi_fx` | MIDI effects that transform MIDI |
| `midi_source` | Sequencers and generators that produce MIDI |

## Host Updates

The Move Anything host can also be updated via the Module Store. When an update is available, "Update Host" appears at the top of the Module Store category list.

### Releasing a Host Update

1. **Bump the version** in `src/host/version.txt`:
   ```
   1.0.1
   ```

2. **Build and package**:
   ```bash
   ./scripts/build.sh
   ```

3. **Create a GitHub release** with the tarball:
   ```bash
   gh release create v1.0.1 move-anything.tar.gz --title "v1.0.1" --notes "Release notes here"
   ```

4. **Update the catalog** in `module-catalog.json`:
   ```json
   {
     "host": {
       "name": "Move Anything",
       "latest_version": "1.0.1",
       "min_host_version": "1.0.0",
       "download_url": "https://github.com/charlesvestal/move-anything/releases/download/v1.0.1/move-anything.tar.gz"
     }
   }
   ```

5. **Push the catalog update**:
   ```bash
   git add module-catalog.json
   git commit -m "Update host to v1.0.1"
   git push
   ```

### How Updates Work

1. Module Store fetches `module-catalog.json` from the main branch
2. Compares `host.latest_version` to installed version in `/data/UserData/move-anything/host/version.txt`
3. If different, shows "Update Host" option with version numbers
4. Update downloads the tarball and extracts over the existing installation
5. User must restart Move Anything for changes to take effect

### Catalog Location

The Module Store fetches the catalog from:
```
https://raw.githubusercontent.com/charlesvestal/move-anything/main/module-catalog.json
```
