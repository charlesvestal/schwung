# Move Hardware API Reference

This document describes the JavaScript API available for developing Move Anything modules.

## Display

The Move has a 128x64 pixel 1-bit (black/white) display.

### Functions

```javascript
clear_screen()              // Clear display to black
print(x, y, text, scale)    // Draw text at position with scale (1-4)
draw_image(x, y, path)      // Draw PNG image at position
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

## Host Functions (Module Management)

```javascript
host_list_modules()           // Returns [{id, name, version}, ...]
host_load_module(id_or_index) // Load a module by ID or index
host_load_ui_module(path)     // Load a UI module file (for chain UI shims)
host_unload_module()          // Unload current module
host_return_to_menu()         // Unload module and return to host menu UI
host_module_set_param(k, v)   // Set DSP parameter
host_module_get_param(k)      // Get DSP parameter
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
```

`host_module_send_midi` accepts a 3-byte array `[status, data1, data2]` and an optional `source` (`"internal"`, `"external"`, or `"host"`).

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
- Notes 40-43: 4 track buttons (row 4 to row 1)

### Control CCs
| CC  | Control           | Notes                          |
|-----|-------------------|--------------------------------|
| 3   | Jog wheel click   | 127 = pressed                  |
| 14  | Jog wheel rotate  | 1 = CW, 127 = CCW              |
| 49  | Shift             | 127 = held                     |
| 50  | Menu              |                                |
| 51  | Back              |                                |
| 52  | Capture           |                                |
| 54  | Down arrow        |                                |
| 55  | Up arrow          |                                |
| 62  | Left arrow        |                                |
| 63  | Right arrow       |                                |
| 71-78 | Knob LEDs       | 8 knobs                        |
| 79  | Master volume     | Relative encoder (1-63 CW, 65-127 CCW) |
| 85  | Play              |                                |
| 86  | Record            |                                |
| 88  | Mute              |                                |

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

Common color values for pad LEDs:
```javascript
const Black = 0x00;
const White = 0x7a;
const LightGrey = 0x7c;
const Red = 0x7f;
const Blue = 0x5f;
```
