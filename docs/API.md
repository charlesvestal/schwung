# Move Hardware API Reference

This document describes the JavaScript API available for developing Move Anything modules.

## Display

The Move has a 128x64 pixel 1-bit (black/white) display.

### Functions

```javascript
clear_screen()                    // Clear display to black
print(x, y, text, color)          // Draw text at position (color: 0=black, 1=white)
set_pixel(x, y, value)            // Set pixel at position (value: 0=black, 1=white)
draw_rect(x, y, w, h, value)      // Draw rectangle outline
fill_rect(x, y, w, h, value)      // Draw filled rectangle
```

## MIDI

### Sending MIDI

```javascript
// Send to external USB-A port
move_midi_external_send([cable, status, data1, data2])

// Send to internal Move hardware (LEDs, etc.)
move_midi_internal_send([type, status, note, value])
```

Cable numbers for external send: `0x00`-`0x0F` (shifted left 4 bits in real MIDI)

Type codes for internal send:
- `0x09` - Note messages (for LED control)
- `0x0b` - CC messages (for LED control)

### Receiving MIDI

```javascript
// Called when MIDI arrives from external USB-A devices
globalThis.onMidiMessageExternal = function(data) {
    // data = [status, data1, data2] or [status, data1, data2, data3...]
}

// Called when MIDI arrives from Move hardware (pads, knobs, buttons)
globalThis.onMidiMessageInternal = function(data) {
    // data = [status, data1, data2]
}
```

## Module Lifecycle

```javascript
// Called once when module loads
globalThis.init = function() {
    // Initialize state, set up LEDs, etc.
}

// Called every frame (~60fps)
globalThis.tick = function() {
    // Update display, handle animations
}
```

## Menu Layout Helpers

For list-based screens (title/list/footer), import the shared menu layout helpers:

```javascript
import {
    drawMenuHeader,
    drawMenuList,
    drawMenuFooter,
    menuLayoutDefaults
} from '../../shared/menu_layout.mjs';
```

## Encoder Utilities

### Delta Decoding

```javascript
import { decodeDelta, decodeAcceleratedDelta } from '../../shared/input_filter.mjs';

// Decode jog wheel CC value to simple ±1 delta
const delta = decodeDelta(value);  // -1, 0, or 1

// Decode with velocity-sensitive acceleration (for value editing)
const accelDelta = decodeAcceleratedDelta(value, encoderId);  // -10 to +10
```

**Acceleration behavior:**
- Slow turns (>150ms): step = 1
- Fast turns (<25ms): step = 10
- In between: interpolated

### LED Control

```javascript
import { setLED, setButtonLED, clearAllLEDs } from '../../shared/input_filter.mjs';

setLED(note, color);        // Set pad/step LED (uses note messages)
setButtonLED(cc, color);    // Set button LED (uses CC messages)
clearAllLEDs();             // Clear all LEDs
```

LED values are cached to avoid redundant MIDI sends.

## Host Functions (Module Management)

```javascript
host_list_modules()           // Returns [{id, name, version}, ...]
host_load_module(id_or_index) // Load a module by ID or index
host_load_ui_module(path)     // Load a UI module file (for chain UI shims)
host_unload_module()          // Unload current module
host_return_to_menu()         // Unload module and return to host menu UI
host_module_set_param(k, v)   // Set DSP parameter
host_module_get_param(k)      // Get DSP parameter
host_module_get_error()       // Get error message from last module operation
host_module_send_midi(msg, source) // Send MIDI into current DSP module
host_is_module_loaded()       // Returns bool
host_get_current_module()     // Returns {id, name, version, ui_script} or null
host_rescan_modules()         // Rescan modules directory, returns count

// Host volume control
host_get_volume()             // Returns volume (0-100)
host_set_volume(vol)          // Set host output volume (0-100)

// Host settings
host_get_setting(key)         // Get setting value (velocity_curve, aftertouch_enabled, aftertouch_deadzone)
host_set_setting(key, val)    // Set setting value
host_save_settings()          // Save settings to disk
host_reload_settings()        // Reload settings from disk

// Display control
host_flush_display()          // Force immediate display update
host_set_refresh_rate(hz)     // Set display refresh rate (default ~11Hz)
host_get_refresh_rate()       // Get current refresh rate

// File system utilities (used by Module Store)
host_file_exists(path)        // Returns bool - check if file/directory exists
host_read_file(path)          // Returns file contents as string, or null on error
host_write_file(path, content) // Write string content to file, returns bool
host_http_download(url, dest) // Download URL to dest path, returns bool
host_extract_tar(tarball, dir) // Extract .tar.gz to directory, returns bool
host_extract_tar_strip(tarball, dir, strip) // Extract with --strip-components
host_ensure_dir(path)         // Create directory if it doesn't exist, returns bool
host_remove_dir(path)         // Recursively remove directory, returns bool

// Screen reader
host_announce_screenreader(text) // Speak text via TTS (if screen reader enabled)
```

`host_module_send_midi` accepts a 3-byte array `[status, data1, data2]` and an optional `source` (`"internal"`, `"external"`, or `"host"`).
`host_load_ui_module` returns a boolean and loads the file as an ES module without invoking `globalThis.init`.

## Utility Functions

```javascript
exit()                        // Exit Move Anything, return to stock Move
console.log(msg)              // Log to /data/UserData/move-anything/move-anything.log
```

## Move Hardware MIDI Mapping

### Pads
- Notes 68-99: 32 pads (bottom-left to top-right, 4 rows of 8)

### Sequencer Steps
- Notes 16-31: 16 step buttons

### Track Selectors
- CCs 40-43: 4 track buttons (reversed: CC43=Track1, CC40=Track4)

### Control CCs
| CC  | Control           | Notes                          |
|-----|-------------------|--------------------------------|
| 3   | Jog wheel click   | 127 = pressed                  |
| 14  | Jog wheel rotate  | 1-63 = CW, 65-127 = CCW        |
| 49  | Shift             | 127 = held                     |
| 50  | Menu              |                                |
| 51  | Back              |                                |
| 52  | Capture           |                                |
| 54  | Down arrow        |                                |
| 55  | Up arrow          |                                |
| 62  | Left arrow        |                                |
| 63  | Right arrow       |                                |
| 71-78 | Knobs 1-8       | Relative encoder (1-63 CW, 65-127 CCW) |
| 79  | Master volume     | Relative encoder (1-63 CW, 65-127 CCW) |
| 85  | Play              |                                |
| 86  | Record            |                                |
| 88  | Mute              |                                |
| 118 | Record button     | LED: RGB (use Note for input)  |
| 119 | Delete            |                                |

### Knob Touch (Capacitive)
Notes 0-9 are generated when knobs are touched. Filter these if you don't need them:
```javascript
if (data[1] < 10) return;  // Ignore capacitive touch
```
When `raw_midi` is false, the host filters knob-touch notes from internal MIDI before modules receive them.

## Host Volume Control

The volume knob (CC 79) controls host-level output volume by default:
- Volume is applied after DSP rendering, before audio output
- Range: 0-100 (default: 100)
- Displayed in host menu UI
- Use `host_get_volume()` and `host_set_volume()` to read/write from JS

Modules can claim the volume knob for their own use by setting `"claims_master_knob": true` in module.json. When claimed, the host passes CC 79 through to the module instead of adjusting volume.

## Host Input Settings

The host provides MIDI input processing that can be configured from the Settings menu:

### Velocity Curve

Applied to Note On messages before forwarding to modules:

| Curve | Behavior |
|-------|----------|
| `linear` | No transform (default) |
| `soft` | Boost low velocities: `64 + (velocity / 2)` |
| `hard` | Exponential curve: `(velocity * velocity) / 127` |
| `full` | Always 127 |

### Aftertouch

- **Enabled/Disabled**: When disabled, aftertouch messages are dropped
- **Dead Zone (0-50)**: Values below threshold become 0, reducing accidental triggers

### Settings Access

```javascript
// Get current velocity curve
let curve = host_get_setting('velocity_curve');  // 'linear', 'soft', 'hard', 'full'

// Get aftertouch settings
let atEnabled = host_get_setting('aftertouch_enabled');  // 0 or 1
let atDeadzone = host_get_setting('aftertouch_deadzone');  // 0-50

// Modify settings
host_set_setting('velocity_curve', 'hard');
host_set_setting('aftertouch_deadzone', 15);
host_save_settings();  // Persist to disk
```

### Raw MIDI Mode

Modules that need unprocessed MIDI input can opt out of transforms by setting `"raw_midi": true` in module.json (this also disables the host knob-touch filter).

### Raw UI Mode

Modules that want full control of UI input can opt out of host Back-to-menu handling by setting `"raw_ui": true` in module.json.
Some modules (like Signal Chain) also use `raw_midi` to bypass their own MIDI filters (for example, allowing knob-touch notes through).

## Overtake Mode

Overtake modules take full control of Move's UI. The host provides special handling for these modules.

### LED Buffer Constraints

The MIDI output buffer holds approximately 64 USB-MIDI packets. Sending more than ~60 LED commands in a single frame causes buffer overflow. Use progressive LED handling:

```javascript
const LEDS_PER_FRAME = 8;
let ledInitIndex = 0;
let ledInitPending = true;

function setupLedBatch() {
    const ledsToSet = [...]; // All LEDs you want to set
    const start = ledInitIndex;
    const end = Math.min(start + LEDS_PER_FRAME, ledsToSet.length);

    for (let i = start; i < end; i++) {
        setLED(ledsToSet[i].note, ledsToSet[i].color);
    }

    ledInitIndex = end;
    if (ledInitIndex >= ledsToSet.length) {
        ledInitPending = false;
    }
}

globalThis.tick = function() {
    if (ledInitPending) setupLedBatch();
    drawUI();
};
```

### Host-Level Escape

The host provides a built-in escape that works regardless of module behavior:

**Shift + Volume Touch + Jog Click**

The host tracks these inputs locally (independent of the shim) to ensure escape always works. Modules should not block or consume these inputs for other purposes.

### MIDI Routing in Overtake Mode

| Input | Behavior |
|-------|----------|
| Shift (CC 49) | Tracked by host for escape; passed to module |
| Volume touch (Note 8) | Tracked by host for escape; passed to module |
| Jog click (CC 3) | If Shift+Vol held, triggers escape; otherwise passed to module |
| All other MIDI | Passed directly to module |

Modules receive full MIDI access and can send to both internal (LEDs) and external (USB-A) targets.

## Audio (DSP Modules Only)

Audio is handled by native DSP plugins (`.so` files). See [MODULES.md](MODULES.md) for plugin API.

- Sample rate: 44100 Hz
- Block size: 128 frames
- Format: Stereo interleaved int16 (L0, R0, L1, R1, ...)

### Audio Input

Native plugins can access audio input through the host API:

```c
// In your plugin's render_block function:
int16_t *audio_in = (int16_t *)(host->mapped_memory + host->audio_in_offset);
// audio_in contains 128 stereo samples in interleaved format
```

Note: Audio input routing depends on the last selected input in the stock Move interface before launching Move Anything.

## LED Colors

Common color values for pad LEDs (from `constants.mjs`):
```javascript
const Black = 0;        // 0x00
const White = 120;      // 0x78
const LightGrey = 118;  // 0x76
const Red = 127;        // 0x7F
const Blue = 125;       // 0x7D
const BrightGreen = 8;  // 0x08
const BrightRed = 1;    // 0x01
```

See `src/shared/constants.mjs` for the full RGB color palette (0-127).
