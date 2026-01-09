# Module Development Guide

Modules are self-contained packages that extend Move Anything with new functionality.

## Module Structure

```
src/modules/your-module/
  module.json       # Required: module metadata
  ui.js             # Optional: JavaScript UI
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
    "api_version": 1
}
```

Required fields: `id`, `name`, `version`, `api_version`
Optional fields: `description`, `author`, `ui`, `dsp`

### Capabilities

Add capability flags to enable special module behaviors:

```json
{
    "id": "your-module",
    "name": "Your Module",
    "version": "1.0.0",
    "api_version": 1,
    "audio_out": true,
    "midi_in": true,
    "claims_master_knob": true
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
| `raw_midi` | Skip host MIDI transforms (velocity curve, aftertouch filter) |

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
| `input_filter.mjs` | Capacitive touch filtering |
| `midi_messages.mjs` | MIDI helper functions |
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

The Signal Chain module allows combining sound generators, MIDI effects, and audio effects into configurable patches.

### Chain Structure

```
[Input] → [MIDI FX] → [Sound Generator] → [Audio FX] → [Output]
```

### Available Components

| Type | Components |
|------|------------|
| Sound Generators | SF2, DX7, Line In |
| MIDI Effects | Chord generator (major, minor, power, octave), Arpeggiator (up, down, updown, random) |
| Audio Effects | Freeverb (reverb) |

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
