# Module Development Guide

Modules are self-contained packages that extend Schwung with new functionality.

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
    "abbrev": "MOD",
    "description": "What your module does",
    "author": "Your Name",
    "ui": "ui.js",
    "ui_chain": "ui_chain.js",
    "dsp": "dsp.so",
    "api_version": 2
}
```

Required fields: `id`, `name`, `version`, `api_version`
Optional fields: `description`, `author`, `ui`, `ui_chain`, `dsp`, `defaults`, `capabilities`, `abbrev`

**Notes:**
- `api_version`: Use `2` for new modules (supports multiple instances, required for Signal Chain)
- `abbrev`: Short display name (3-6 chars) for Shadow UI slot display (e.g., "SF2", "Dexed", "CLAP")
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
| `skip_led_clear` | Host skips clearing LEDs on module load/unload — preserves Move's native pad colors (useful for modules that overlay highlights on existing clip colors) |
| `component_type` | Module category: `sound_generator`, `audio_fx`, `midi_fx`, `utility`, `system`, `featured`, `overtake`, or `tool` |

### Tool Config

Tool modules (`"component_type": "tool"`) appear in the Tools menu and support additional options via `tool_config`:

```json
{
    "tool_config": {
        "interactive": true,
        "skip_file_browser": true
    }
}
```

| Field | Description |
|-------|-------------|
| `interactive` | Tool takes over the UI (like an overtake module) rather than running headlessly |
| `skip_file_browser` | Tool does not use the file browser on launch (goes straight to its own UI) |
| `input_extensions` | Array of file extensions the tool accepts (e.g., `[".wav"]`) |
| `allow_new_file` | Show a "+ New File" action in the file browser |
| `command` | Shell command to run for non-interactive tools |
| `overtake` | Set to `false` to prevent tool from using overtake display mode |

Interactive tools use `host_exit_module()` to return to the tools menu when the user presses Back.

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

Modules are discovered at runtime from `/data/UserData/schwung/modules`.
To add a new module, copy a folder with `module.json` (plus `ui.js` and `dsp.so`
if needed) and either restart Schwung or call `host_rescan_modules()` in
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

### Plugin API v2 (Recommended)

V2 supports multiple instances and is **required for Signal Chain integration**:

```c
#include "host/plugin_api_v1.h"  /* v2 API is defined in this file */

typedef struct my_instance {
    // Your synth state here
    float sample_rate;
    int preset;
} my_instance_t;

static void* create_instance(const char *module_dir, const char *json_defaults) {
    my_instance_t *inst = calloc(1, sizeof(my_instance_t));
    inst->sample_rate = 44100.0f;
    // Parse json_defaults if needed
    return inst;
}

static void destroy_instance(void *instance) {
    free(instance);
}

static void on_midi(void *instance, const uint8_t *msg, int len, int source) {
    my_instance_t *inst = (my_instance_t*)instance;
    // source: 0 = internal (Move), 1 = external (USB)
}

static void set_param(void *instance, const char *key, const char *val) {
    my_instance_t *inst = (my_instance_t*)instance;
    // Handle parameter changes
}

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    my_instance_t *inst = (my_instance_t*)instance;
    // Return parameter value, ui_hierarchy, chain_params, etc.
    return -1;
}

static void render_block(void *instance, int16_t *out_lr, int frames) {
    my_instance_t *inst = (my_instance_t*)instance;
    // Generate 'frames' stereo samples
    // Output format: [L0, R0, L1, R1, ...]
}

/* Export the plugin API */
static plugin_api_v2_t api = {
    .api_version = 2,
    .create_instance = create_instance,
    .destroy_instance = destroy_instance,
    .on_midi = on_midi,
    .set_param = set_param,
    .get_param = get_param,
    .render_block = render_block,
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t* host) {
    return &api;
}
```

### Runtime Modulation Callbacks (Chain Host)

When a plugin runs inside Signal Chain, `host_api_v1_t` may provide optional modulation callbacks:

```c
int (*mod_emit_value)(void *ctx,
                      const char *source_id,
                      const char *target,
                      const char *param,
                      float signal,
                      float depth,
                      float offset,
                      int bipolar,
                      int enabled);
void (*mod_clear_source)(void *ctx, const char *source_id);
void *mod_host_ctx;
```

Use these to publish temporary modulation overlays without overwriting a target parameter's saved base value.

Guidelines:
- `source_id`: stable ID for the modulation source instance/lane.
- `target`: `"synth"`, `"fx1"`, `"fx2"`, `"midi_fx1"`, or `"midi_fx2"`.
- `enabled=0` or `mod_clear_source(...)`: clears that source's contribution.
- Missing/stale targets should fail silently (do not crash or spam logs).
- Multiple sources can target the same parameter; the host sums contributions and clamps to target range.

### Plugin API v1 (Deprecated)

V1 is a singleton API - only one instance can exist. **Do not use for new modules:**

```c
#include "plugin_api_v1.h"

static int on_load(const char *module_dir, const char *json_defaults) {
    return 0;  // 0 = success
}

static void on_unload(void) { }

static void on_midi(const uint8_t *msg, int len, int source) { }

static void set_param(const char *key, const char *val) { }

static int get_param(const char *key, char *buf, int buf_len) {
    return -1;
}

static void render_block(int16_t *out_lr, int frames) { }

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

## Shadow UI Parameter Hierarchy

Modules expose a navigable parameter hierarchy to the Shadow UI via `ui_hierarchy` in module.json or `get_param("ui_hierarchy")`. The hierarchy uses a **levels dictionary** format with named levels:

```json
{
  "ui_hierarchy": {
    "levels": {
      "root": {
        "name": "My Synth",
        "params": [
          {"key": "cutoff", "name": "Cutoff", "type": "int", "min": 0, "max": 127},
          {"key": "mode", "name": "Mode", "type": "enum", "options": ["LP", "HP", "BP"]},
          {"level": "advanced", "label": "Advanced Settings"}
        ],
        "knobs": ["cutoff", "mode"]
      },
      "advanced": {
        "name": "Advanced",
        "params": [
          {"key": "drive", "name": "Drive", "type": "float", "min": 0, "max": 1}
        ],
        "knobs": ["drive"]
      }
    }
  }
}
```

### Level Fields

| Field | Description |
|-------|-------------|
| `name` / `label` | Display name for the level |
| `params` | Array of parameter items (see below) |
| `knobs` | Array of parameter keys mapped to physical knobs 1-8 |
| `list_param` / `count_param` / `name_param` | For preset browser levels |
| `items_param` / `select_param` | For dynamic item selection levels |
| `child_prefix` / `child_count` / `child_label` | For repeated elements (see below) |

### Parameter Item Types

Each entry in `params` is either:

- **Editable param**: `{"key": "cutoff", "name": "Cutoff", "type": "int", "min": 0, "max": 127}`
- **Navigation link**: `{"level": "advanced", "label": "Advanced Settings"}`

**Important:** Use `key` (not `param`) for editable parameter objects. Metadata (type, min, max) can come from either the hierarchy or `chain_params`.

### Parameter Types

| Type | Fields | Description |
|------|--------|-------------|
| `int` | `min`, `max` | Integer value with knob control |
| `float` | `min`, `max`, `step` | Float value (0.0-1.0 typical) |
| `enum` | `options` | List of string options |
| `mode` | `options` | Mode selector (like enum, triggers mode switch) |

### Child Selectors (for repeated elements)

For synths with multiple similar elements (tones, operators, parts), use child selectors:

```json
{
  "levels": {
    "tones": {
      "name": "Tones",
      "child_prefix": "nvram_tone_",
      "child_count": 4,
      "child_label": "Tone",
      "params": [
        {"key": "cutofffrequency", "name": "Cutoff"},
        {"key": "resonance", "name": "Resonance"},
        {"key": "level", "name": "Level"}
      ],
      "knobs": ["cutofffrequency", "resonance", "level"]
    }
  }
}
```

The Shadow UI will show a selector (Tone 1, Tone 2, etc.) and prefix parameter keys with `child_prefix` + index (e.g., `synth:nvram_tone_0_cutofffrequency`).

### Example: Chord MIDI FX Hierarchy

```json
{
  "levels": {
    "root": {
      "name": "Chord",
      "params": [
        {"key": "type", "name": "Type", "type": "enum",
         "options": ["none", "major", "minor", "dim", "aug", "sus2", "sus4", "power", "octave"]},
        {"key": "inversion", "name": "Inversion", "type": "enum",
         "options": ["root", "1st", "2nd", "3rd"]},
        {"key": "strum", "name": "Strum", "type": "int", "min": 0, "max": 100}
      ],
      "knobs": ["type", "inversion", "strum"]
    }
  }
}
```

## Chain Parameters (Knob Mappings)

Modules can expose quick-access knob mappings via `chain_params`. This can be defined statically in `module.json` or dynamically via `get_param("chain_params")`.

### Static Definition (module.json)

For modules with fixed parameters, define in capabilities:

```json
{
  "capabilities": {
    "chain_params": [
      {
        "key": "cutoff",
        "name": "Cutoff",
        "type": "int",
        "min": 0,
        "max": 127,
        "default": 64
      },
      {
        "key": "type",
        "name": "Filter Type",
        "type": "enum",
        "options": ["lowpass", "highpass", "bandpass"],
        "default": "lowpass"
      }
    ]
  }
}
```

### Dynamic Definition (get_param)

For modules with state-dependent parameters:

```c
int get_param(void *instance, const char *key, char *buf, int buf_len) {
    if (strcmp(key, "chain_params") == 0) {
        const char *json = "["
            "{\"key\": \"cutoff\", \"name\": \"Cutoff\", \"type\": \"int\", \"min\": 0, \"max\": 127, \"value\": 64},"
            "{\"key\": \"resonance\", \"name\": \"Resonance\", \"type\": \"int\", \"min\": 0, \"max\": 127, \"value\": 32}"
        "]";
        strncpy(buf, json, buf_len);
        return strlen(json);
    }
    return -1;
}
```

### Parameter Types

| Type | Fields | Description |
|------|--------|-------------|
| `int` | `min`, `max`, `default`/`value` | Integer value |
| `float` | `min`, `max`, `default`/`value` | Float value |
| `enum` | `options`, `default`/`value` | List of string options |
| `filepath` | `root`, `start_path`, `filter`, `default`/`value` | Opens Shadow UI file browser and stores selected path |
| `module_picker` | `allow_none`, `allow_self`, `allowed_targets`, `param_key` | Dynamic enum from loaded chain components |
| `parameter_picker` | `target_key`, `numeric_only`, `allow_none` | Dynamic enum from selected target's exposed params |

#### `filepath` in module.json

Use `type: "filepath"` in `capabilities.chain_params` to let Shadow UI open a reusable file browser.

```json
{
  "capabilities": {
    "chain_params": [
      {
        "key": "sample_file",
        "name": "Sample File",
        "type": "filepath",
        "root": "/data/UserData",
        "start_path": "/data/UserData/UserLibrary/Samples",
        "filter": ".wav",
        "default": ""
      }
    ]
  }
}
```

`filepath` fields:

- `key` (required): Parameter key passed to `set_param`.
- `name` (required): Label shown in Shadow UI.
- `type` (required): Must be `"filepath"`.
- `root` (optional, recommended): Absolute folder where browsing starts and is constrained.
- `start_path` (optional): Absolute folder or file path used as the initial location when current value is empty.
- `filter` (optional): File extension filter as a string or array, for example `".wav"` or `[".wav", ".aif"]`.
- `live_preview` (optional): When true, moving the file-browser cursor over files temporarily sets the parameter to that file until the user confirms or cancels.
- `browser_hooks` (optional): Event hooks to run additional parameter writes at browser lifecycle points. Supported keys: `on_open`, `on_preview`, `on_cancel`, `on_commit`.
- `default` or `value` (optional): Initial absolute path. If the path exists and is inside `root`, the browser opens to the parent folder and highlights the file.

Behavior notes:

- Selected files are stored as absolute paths.
- Initial browser location priority is: current/default value, then `start_path`, then `root`.
- If the chosen start location is missing, invalid, or outside `root`, the browser falls back to `root`.
- With `live_preview: true`, preview changes are temporary: Back cancels and restores the original value, Click commits the highlighted file.
- `browser_hooks` action format is `{ "key": "<param>", "value": "<string>", "restore": true|false }`. Non-prefixed keys are resolved against the active component prefix.
- `browser_hooks` supports value placeholders: `$path`/`$selected_path` and `$filename`/`$selected_filename`.
- For pad samplers, you can suspend auto-pad switching while browsing by adding `{"key":"ui_auto_select_pad","value":"off","restore":true}` to `browser_hooks.on_open`.
- Example user sample file path: `/data/UserData/UserLibrary/Samples/Drums/Kick01.wav`.

#### Dynamic Target Pickers

Use `module_picker` and `parameter_picker` for chain-aware target routing without custom UI code.

- `module_picker`: Renders as a normal enum editor whose options are refreshed from currently loaded chain components.
- `parameter_picker`: Renders as a normal enum editor whose options are refreshed from the component selected by `target_key`.
- `allow_none` (optional, default true): Includes an empty option for clearing assignment.
- `allow_self` (module_picker only, optional, default false): Allows selecting the hosting component itself.
- `allowed_targets` (module_picker only, optional): Comma-separated string or array of target IDs to whitelist.
- `param_key` (module_picker, optional): Companion parameter key to clear when target changes (for example `lfo1_target_param`).
- `target_key` (parameter_picker, recommended): Companion key holding selected module target.
- `numeric_only` (parameter_picker, optional, default true): Restricts options to float/int parameters.

These map to knobs 1-8 in the Shadow UI for quick access.

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
| `filepath_browser.mjs` | Reusable filesystem browser helpers for `chain_params` type `filepath` |
| `logger.mjs` | Unified logging utilities |
| `screen_reader.mjs` | Screen reader announce/announceMenuItem/announceView helpers |
| `sampler_overlay.mjs` | Quantized sampler UI overlay |
| `text_entry.mjs` | On-screen keyboard for text input |
| `store_utils.mjs` | Module Store catalog fetching and install/remove functions |
| `scrollable_text.mjs` | Scrollable text component |
| `sound_generator_ui.mjs` | Sound generator UI helpers |
| `chain_param_utils.mjs` | Chain parameter handling utilities |
| `chain_ui_views.mjs` | Shadow UI view components |
| `parse_move_manual.mjs` | Move manual content parsing |

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

### C Shared Utilities

For native code, shared headers are in `src/host/`:

| File | Contents |
|------|----------|
| `js_display.h/c` | Display primitives (set_pixel, draw_rect, print), font loading, QuickJS bindings |
| `shadow_constants.h` | Shadow mode shared memory names, buffer sizes, control structures |
| `plugin_api_v1.h` | DSP plugin interface |
| `audio_fx_api_v2.h` | Audio effects plugin interface |

## Help Content (help.json)

Modules can provide on-device help accessible from the Shadow UI's Help viewer (Shift+Vol+Menu → Help). Add a `help.json` file to your module's source directory.

### File Location

```
src/modules/your-module/
  module.json
  ui.js
  help.json          # Help content for the Help viewer
```

The host scans all installed module directories for `help.json` at runtime. Module help topics appear alphabetically in the "Modules" section of the Help viewer.

### Format

Help content is a tree of sections and leaf topics:

```json
{
  "title": "Your Module",
  "children": [
    {
      "title": "Overview",
      "lines": [
        "Brief description",
        "of your module.",
        "",
        "Second paragraph",
        "with more detail."
      ]
    },
    {
      "title": "Controls",
      "children": [
        {
          "title": "Knob Mapping",
          "lines": [
            "Knob 1: Cutoff",
            "Knob 2: Resonance",
            "Knob 3: Attack",
            "Knob 4: Release"
          ]
        },
        {
          "title": "Other Settings",
          "lines": [
            "Detail about other",
            "settings here."
          ]
        }
      ]
    },
    {
      "title": "MIDI",
      "lines": [
        "MIDI behavior",
        "description."
      ]
    }
  ]
}
```

### Node Types

| Type | Fields | Description |
|------|--------|-------------|
| Branch | `title`, `children` | Navigable folder (shows as a list) |
| Leaf | `title`, `lines` | Scrollable text content |

- **Branch nodes** have a `children` array of other branch or leaf nodes. Nesting depth is unlimited.
- **Leaf nodes** have a `lines` array of strings displayed as scrollable text.

### Text Formatting Rules

The display is 128x64 pixels with a ~20 character line width. Pre-wrap your text accordingly:

- Keep lines to **20 characters or fewer**
- Use empty strings (`""`) for blank lines between paragraphs
- Indent continuation lines with a leading space for readability:
  ```json
  "lines": [
    "Knob 3: Contour",
    " (filter env depth)"
  ]
  ```

### Packaging

Include `help.json` in your build script so it ends up in the distributed tarball:

```bash
# In scripts/build.sh, after copying other files to dist/<id>/
[ -f src/help.json ] && cp src/help.json dist/<id>/help.json
```

The host discovers `help.json` automatically — no changes to `module.json` are needed.

### Recommended Sections

A typical module help file includes:

| Section | Content |
|---------|---------|
| Overview | What the module does, key features |
| Controls / Knob Mapping | Which knobs control which parameters |
| MIDI | MIDI behavior, supported CCs, channel info |
| Presets | List of factory presets (if applicable) |

## Example Modules

See these modules for reference:

- **chain**: Signal chain with synths, MIDI FX, and audio FX
- **dexed**: Dexed FM synthesizer with native DSP (loads .syx patches)
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
| Sound Generators | Line In, SF2, Dexed, CLAP, plus any module marked `"chainable": true` with `"component_type": "sound_generator"` (for example `obxd`, `minijv`) |
| MIDI Effects | Chord (15 chord types with inversions, voicings, strum), Arpeggiator (off, up, down, up_down, random with BPM/division/sync), Velocity Scale (min/max velocity mapping), plus external MIDI FX via Module Store |
| Audio Effects | Freeverb (reverb), CLAP effects, plus external audio FX via Module Store (CloudSeed, PSXVerb, Tapescam, etc.) |

### CLAP Host Module

The CLAP module (separate repo: `move-anything-clap`) hosts arbitrary CLAP audio plugins:

- Place `.clap` plugin files in `/data/UserData/schwung/modules/clap/plugins/`
- Plugins are discovered at load time
- Use jog wheel to browse plugins, encoders to control parameters
- CLAP synths work as sound generators in Signal Chain
- CLAP effects can be used in the audio FX slot

### Patch Files

Patches are stored in `/data/UserData/schwung/patches/` on the device as JSON:

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

Audio effects use an in-place processing API. The v2 API supports multiple instances:

```c
typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void (*destroy_instance)(void *instance);
    void (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);  // Optional
} audio_fx_api_v2_t;

// Entry point
audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host);
```

The `on_midi` callback is optional (can be NULL). Implement it to receive MIDI from capture rules or other sources.

## MIDI FX Plugin API

MIDI effects transform or generate MIDI messages. They use a separate API defined in `src/host/midi_fx_api_v1.h`:

```c
typedef struct midi_fx_api_v1 {
    uint32_t api_version;  /* Must be 1 (MIDI_FX_API_VERSION) */

    void* (*create_instance)(const char *module_dir, const char *config_json);
    void (*destroy_instance)(void *instance);

    /* Transform incoming MIDI. Returns number of output messages (0 = swallow, >1 = expand).
     * out_msgs: array of 3-byte MIDI messages
     * max_out: maximum output messages (MIDI_FX_MAX_OUT_MSGS = 16) */
    int (*process_midi)(void *instance,
                        const uint8_t *in_msg, int in_len,
                        uint8_t out_msgs[][3], int out_lens[],
                        int max_out);

    /* Time-based generation (e.g., arpeggiator). Called each audio block.
     * Returns number of output messages to inject. */
    int (*tick)(void *instance,
                int frames, int sample_rate,
                uint8_t out_msgs[][3], int out_lens[],
                int max_out);

    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
} midi_fx_api_v1_t;

// Entry point
midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host);
```

**Key differences from sound generators and audio FX:**

- `process_midi()` transforms incoming MIDI — can output 0 (swallow), 1 (pass/modify), or multiple messages (e.g., chord generates multiple notes)
- `tick()` handles time-based generation (e.g., arpeggiator note sequencing) — called every audio block
- No `render_block()` — MIDI FX don't process audio
- Maximum 2 native MIDI FX per chain (`MAX_MIDI_FX`)
- Maximum 16 output messages per `process_midi()` call (`MIDI_FX_MAX_OUT_MSGS`)

### Building MIDI FX

MIDI FX are built identically to other native plugins:

```bash
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/midi_fx/your-fx/dsp/your_fx.c \
    -o build/modules/midi_fx/your-fx/dsp.so \
    -Isrc
```

### Built-in MIDI FX

| Module | ID | Description |
|--------|----|-------------|
| Chord | `chord` | Chord generator (15 types, inversions, voicings, strum) |
| Arpeggiator | `arp` | Arpeggiator (up, down, up_down, random with tempo sync) |
| Velocity Scale | `velocity_scale` | Velocity range mapping (min/max) |

### MIDI FX module.json Example

```json
{
    "id": "velocity_scale",
    "name": "Velocity Scale",
    "abbrev": "VS",
    "version": "0.1.0",
    "builtin": true,
    "capabilities": {
        "chainable": true,
        "component_type": "midi_fx",
        "ui_hierarchy": {
            "levels": {
                "root": {
                    "name": "Velocity Scale",
                    "params": [
                        {"key": "min", "name": "Min", "type": "int", "min": 1, "max": 127},
                        {"key": "max", "name": "Max", "type": "int", "min": 1, "max": 127}
                    ],
                    "knobs": ["min", "max"]
                }
            }
        }
    }
}
```

## Host API (Passed to Plugins)

All plugin init functions receive a `host_api_v1_t` struct providing access to host services:

```c
typedef struct host_api_v1 {
    uint32_t api_version;

    /* Audio constants */
    int sample_rate;         /* 44100 */
    int frames_per_block;    /* 128 */

    /* Direct mailbox access (use with care) */
    uint8_t *mapped_memory;
    int audio_out_offset;    /* Offset to audio output in mailbox */
    int audio_in_offset;     /* Offset to audio input in mailbox */

    /* Logging */
    void (*log)(const char *msg);

    /* MIDI send functions
     * msg: 4-byte USB-MIDI packet [cable|CIN, status, data1, data2]
     * Returns: bytes queued, or 0 on failure */
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);

    /* Clock status for sync-aware plugins */
    int (*get_clock_status)(void);
} host_api_v1_t;
```

## Audio Specifications

- Sample rate: 44100 Hz
- Block size: 128 frames
- Latency: ~3ms
- Format: Stereo interleaved int16

## Shadow Mode Integration

Shadow Mode runs custom signal chains alongside stock Ableton Move. Your modules are **automatically available** in shadow mode without any additional work - no recompilation required.

### How It Works

Shadow mode loads the chain module and patches. When you install a module via Module Store (or manually copy it to the modules directory), it becomes available in Shadow Mode.

Modules and patches are read from:
- Modules: `/data/UserData/schwung/modules/`
- Patches: `/data/UserData/schwung/patches/`

### Making Your Module Shadow-Compatible

If your module is chainable (sound generator or audio FX), it works in shadow mode automatically. Ensure your `module.json` has:

```json
{
    "capabilities": {
        "chainable": true,
        "component_type": "sound_generator"
    }
}
```

Valid `component_type` values for chainable modules:
- `sound_generator` - Synths and samplers
- `audio_fx` - Audio effects

### Creating Shadow Chain Patches

Patches are JSON files in `/data/UserData/schwung/patches/`. To create a patch using your module:

```json
{
    "name": "My Synth + Reverb",
    "version": 1,
    "chain": {
        "input": "pads",
        "synth": {
            "module": "your-module-id",
            "config": {
                "preset": 0
            }
        },
        "audio_fx": [
            {
                "type": "freeverb",
                "params": {
                    "room_size": 0.7,
                    "wet": 0.25
                }
            }
        ]
    }
}
```

The `module` field must match the `id` in your module's `module.json`.

### Shadow Mode MIDI Routing

Each shadow slot listens on a configurable MIDI channel (default 1-4):

| Shadow Slot | Default Channel |
|-------------|-----------------|
| Slot A | Ch 1 |
| Slot B | Ch 2 |
| Slot C | Ch 3 |
| Slot D | Ch 4 |

**Forward Channel:** Some synths need MIDI on a specific channel regardless of slot. Configure via the slot's "Forward Ch" setting:
- **Auto**: Pass MIDI through on the receive channel (default)
- **1-16**: Remap all MIDI to that specific channel before sending to the synth

Each shadow slot can load a different chain patch. Slot settings and synth states persist across restarts.

### Testing Shadow Mode

1. Build and install Schwung: `./scripts/build.sh && ./scripts/install.sh local`
2. Launch stock Move (or reboot device)
3. Toggle shadow mode: **Shift + touch Volume + touch Knob 1**
4. Use jog wheel to select a slot, click to browse patches
5. Load a patch that uses your module
6. Set a Move track to MIDI channel 5-8 and play pads

### Capture Rules

Chain patches can capture specific Move controls exclusively. When a slot with capture rules is focused in the shadow UI, captured controls are blocked from reaching Move and routed to the slot's DSP.

**Patch-level capture:**

```json
{
    "name": "Performance Effect",
    "synth": { "module": "sf2" },
    "audio_fx": [{ "type": "freeverb" }],
    "capture": {
        "groups": ["steps"]
    }
}
```

**Capture format:**

```json
{
    "capture": {
        "groups": ["steps", "pads"],
        "notes": [60, 61, 62],
        "note_ranges": [[68, 75]],
        "ccs": [118, 119],
        "cc_ranges": [[100, 110]]
    }
}
```

All fields are optional and combine as a union.

**Control group aliases:**

| Alias | Type | Values | Description |
|-------|------|--------|-------------|
| `pads` | notes | 68-99 | 32 performance pads |
| `steps` | notes | 16-31 | 16 step sequencer buttons |
| `tracks` | CCs | 40-43 | 4 track buttons |
| `knobs` | CCs | 71-78 | 8 encoders |
| `jog` | CC | 14 | Main encoder |

**Module-level capture (for Master FX):**

Audio FX modules can define capture rules in their `module.json`:

```json
{
    "id": "perfverb",
    "capabilities": {
        "component_type": "audio_fx",
        "capture": {
            "groups": ["steps"]
        }
    }
}
```

**Note:** Audio FX modules that want to receive captured MIDI must implement `on_midi` in their API. If `on_midi` is NULL, captured MIDI is blocked from Move but not routed to the FX.

### Shadow Mode Configuration

Shadow slot configuration is stored in `/data/UserData/schwung/shadow_chain_config.json`:

```json
{
    "patches": [
        { "name": "SF2 + Freeverb", "channel": 5 },
        { "name": "Dexed + Freeverb", "channel": 6 },
        { "name": "OB-Xd + Freeverb", "channel": 7 },
        { "name": "Mini-JV + Freeverb", "channel": 8 }
    ]
}
```

The shadow UI updates this file when you select patches for each slot.

## Overtake Modules

Overtake modules take complete control of Move's UI while running in shadow mode. Unlike regular shadow mode (which displays a custom UI alongside Move), overtake modules fully replace Move's display and control all LEDs.

### Configuration

Set `component_type: "overtake"` in module.json:

```json
{
    "id": "controller",
    "name": "MIDI Controller",
    "version": "1.0.0",
    "component_type": "overtake",
    "ui": "ui.js",
    "api_version": 2
}
```

Overtake modules appear in a dedicated section of the shadow UI menu.

### Lifecycle

When an overtake module is loaded:

1. **LED Clearing**: The host progressively clears all LEDs (pads, steps, buttons, knob indicators)
2. **Loading Screen**: "Loading..." is displayed during LED clearing
3. **Deferred Init**: After ~500ms delay, the module's `init()` is called
4. **Module Takes Over**: Module controls display and all LEDs

The progressive LED clearing prevents MIDI buffer overflow (the buffer holds ~64 packets).

### Host-Level Escape

The host provides a built-in escape mechanism that always works, regardless of module implementation:

**Shift + Volume Touch + Jog Click** exits any overtake module

The host tracks shift and volume touch state locally (not relying on the shim's tracking, which doesn't work in overtake mode) to ensure the escape always functions.

### Progressive LED Handling

The MIDI output buffer is limited (~64 packets). Sending all LED commands at once causes buffer overflow. Use progressive LED handling:

**In the host (LED clearing):**
```javascript
const LEDS_PER_BATCH = 20;
let ledClearIndex = 0;

function clearLedBatch() {
    // Clear 20 LEDs per frame until done
    // Covers: pads (68-99), steps (16-31), buttons, knob indicators
}
```

**In your module (initialization):**

Use the shared `setLED()` and `setButtonLED()` from `input_filter.mjs` — they provide caching (skip duplicate sends) and correct MIDI packet formatting:

```javascript
import {
    MoveBack, MoveCapture, MoveUndo, MoveLoop, MoveCopy, MoveMute, MoveDelete,
    MovePads, White, DarkGrey,
    WhiteLedDim, WhiteLedMedium, WhiteLedBright
} from '/data/UserData/schwung/shared/constants.mjs';

import { setLED, setButtonLED } from '/data/UserData/schwung/shared/input_filter.mjs';

let ledInitPending = false;
let ledInitIndex = 0;
const LEDS_PER_FRAME = 8;

globalThis.init = function() {
    ledInitPending = true;
    ledInitIndex = 0;
};

function setupLedBatch() {
    const leds = [];
    // Button LEDs (CC-based) — use setButtonLED
    leds.push({ type: 'cc', id: MoveBack, color: WhiteLedDim });
    leds.push({ type: 'cc', id: MoveCapture, color: WhiteLedDim });
    // Pad LEDs (note-based) — use setLED
    for (const pad of MovePads) {
        leds.push({ type: 'note', id: pad, color: DarkGrey });
    }

    const end = Math.min(ledInitIndex + LEDS_PER_FRAME, leds.length);
    for (let i = ledInitIndex; i < end; i++) {
        if (leds[i].type === 'cc') setButtonLED(leds[i].id, leds[i].color);
        else setLED(leds[i].id, leds[i].color);
    }
    ledInitIndex = end;
    if (ledInitIndex >= leds.length) ledInitPending = false;
}

globalThis.tick = function() {
    if (ledInitPending) {
        setupLedBatch();
    }
    drawUI();
};
```

**Important:** Always use the shared `setLED()` and `setButtonLED()` from `input_filter.mjs` rather than calling `move_midi_internal_send()` directly. The shared helpers handle LED caching and correct USB-MIDI cable byte formatting. Use absolute import paths (`/data/UserData/schwung/shared/...`) for module location independence.

### LED Addresses

When clearing or setting LEDs, address both note-based and CC-based LEDs:

| Type | Addressing | Values |
|------|-----------|--------|
| Pads | Notes | 68-99 |
| Steps | Notes | 16-31 |
| Knob touch | Notes | 0-7 |
| Step icons | CCs | 16-31 |
| Track buttons | CCs | 40-43 |
| Shift | CC | 49 |
| Menu/Back/Capture | CCs | 50-52 |
| Up/Down | CCs | 54-55 |
| Undo/Loop/Copy | CCs | 56, 58, 60 |
| Left/Right | CCs | 62-63 |
| Knob indicators | CCs | 71-78 |
| Play/Rec | CCs | 85-86 |
| Mute | CC | 88 |
| Record/Delete | CCs | 118-119 |

### MIDI Routing

In overtake mode:
- All internal MIDI is passed to the module's `onMidiMessageInternal`
- External MIDI is passed to `onMidiMessageExternal`
- The host intercepts Shift+Vol+Jog before the module sees it (for escape)
- Modules can send MIDI out via `move_midi_external_send` and `move_midi_internal_send`

### Example: MIDI Controller

The built-in MIDI Controller module (`src/modules/controller/`) demonstrates overtake patterns:

- 16 banks of pad/knob mappings
- Step buttons switch banks
- Jog wheel and Up/Down buttons for octave shift
- Progressive LED initialization
- Dynamic C note highlighting based on octave

## Publishing to Module Store

External modules can be distributed via the built-in Module Store. Users can browse, install, update, and remove modules directly from their Move device.

### Requirements

1. Module builds as a self-contained tarball: `<id>-module.tar.gz`
2. Tarball extracts to a folder matching the module ID (e.g., `minijv/`)
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

4. **Update the catalog** in `move-anything/module-catalog.json` (if not already listed):
   ```json
   {
     "id": "your-module",
     "name": "Your Module",
     "description": "What it does",
     "author": "Your Name",
     "component_type": "sound_generator",
     "github_repo": "username/move-anything-yourmodule",
     "default_branch": "main",
     "asset_name": "your-module-module.tar.gz",
     "min_host_version": "0.3.0",
     "requires": "Optional: external assets needed (e.g. sample files, ROMs)"
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

      - name: Update release.json
        run: |
          VERSION="${GITHUB_REF_NAME#v}"
          git fetch origin main
          git checkout -f main
          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"

          cat > release.json << EOF
          {
            "version": "${VERSION}",
            "download_url": "https://github.com/${{ github.repository }}/releases/download/${{ github.ref_name }}/${{ github.event.repository.name }}-module.tar.gz"
          }
          EOF

          git add release.json
          git commit -m "chore: update release.json for ${{ github.ref_name }}" || echo "No changes to commit"
          git push origin main
```

### Catalog Entry Schema (v2)

Each module in `module-catalog.json`:

```json
{
  "id": "module-id",
  "name": "Display Name",
  "description": "Short description",
  "author": "Author Name",
  "component_type": "sound_generator|audio_fx|midi_fx|overtake|utility|tool",
  "github_repo": "username/repo-name",
  "default_branch": "main",
  "asset_name": "module-id-module.tar.gz",
  "min_host_version": "0.3.0",
  "requires": "Optional: external assets needed (e.g. ROM files, .sf2 soundfonts)"
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `id` | Yes | Module ID (lowercase hyphenated) |
| `name` | Yes | Display name |
| `description` | Yes | Short description |
| `author` | Yes | Author name |
| `component_type` | Yes | `sound_generator`, `audio_fx`, `midi_fx`, `overtake`, `utility`, `tool` |
| `github_repo` | Yes | GitHub `owner/repo` |
| `default_branch` | Yes | Branch to fetch `release.json` from (usually `main`) |
| `asset_name` | Yes | Expected tarball filename |
| `min_host_version` | Yes | Minimum compatible host version |
| `requires` | No | User-facing note about required external assets |

### release.json

Each module repo must have a `release.json` on its main branch. The Module Store fetches this file (not the GitHub releases API) to determine the latest version and download URL.

```json
{
  "version": "0.2.0",
  "download_url": "https://github.com/username/move-anything-mymodule/releases/download/v0.2.0/mymodule-module.tar.gz"
}
```

Optional fields: `install_path`, `name`, `description`, `requires`, `post_install`, `repo_url`. Fields like `name`, `description`, and `requires` in `release.json` override their catalog equivalents.

The release workflow should auto-update `release.json` on each tagged release (see the workflow template above for an example).

### How the Module Store Works

1. Fetches `module-catalog.json` from the main branch
2. For each module, fetches `release.json` from the module's GitHub repo (on `default_branch`)
3. Compares `release.json` version to installed version
4. Downloads tarball from `release.json`'s `download_url`
5. Extracts tarball to category subdirectory (e.g., `modules/sound_generators/<id>/`)

### Component Types

| Type | Description |
|------|-------------|
| `sound_generator` | Synthesizers and samplers that produce audio |
| `audio_fx` | Audio effects that process audio |
| `midi_fx` | MIDI effects that transform MIDI |
| `overtake` | Overtake modules (full UI control) |
| `utility` | Utility modules |

## Host Updates

The Schwung host can also be updated via the Module Store. When an update is available, "Update Host" appears at the top of the Module Store category list.

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
   gh release create v1.0.1 schwung.tar.gz --title "v1.0.1" --notes "Release notes here"
   ```

4. **Update the catalog** in `module-catalog.json`:
   ```json
   {
     "host": {
       "name": "Schwung",
       "github_repo": "charlesvestal/schwung",
       "asset_name": "schwung.tar.gz",
       "latest_version": "1.0.1",
       "min_host_version": "1.0.0"
     }
   }
   ```

5. **Push the catalog update**:
   ```bash
   git add module-catalog.json
   git commit -m "Update host to v1.0.1"
   git push
   ```

### How Host Updates Work

1. Module Store fetches `module-catalog.json` from the main branch
2. Fetches `release.json` from the host repo for the latest version and download URL
3. Compares to installed version in `/data/UserData/schwung/host/version.txt`
4. If different, shows "Update Host" option with version numbers
5. Update downloads the tarball and extracts over the existing installation
6. User must restart Schwung for changes to take effect

### Catalog Location

The Module Store fetches the catalog from:
```
https://raw.githubusercontent.com/charlesvestal/schwung/main/module-catalog.json
```
