# Shadow Instrument - Status

**Branch:** `main` (merged from `feature/shadow-instrument-poc`)
**Date:** 2026-01-23
**Status:** ✅ FULLY WORKING - Audio, MIDI routing, shadow UI display, track selection, knob control, Master FX, parameter hierarchy editor, and in-UI module store all operational

## Goal

Run custom signal chain patches alongside stock Ableton Move, mixing their audio output with Move's audio through the SPI mailbox.

## Architecture

```
Stock Move (ioctl) → Shim (LD_PRELOAD) → SPI Mailbox
                         ↓
                In-process chain DSP
                         ↓
                 Shadow UI host (QuickJS)
```

### Shared Code Structure

Shadow mode shares code with the main Move Anything host to maintain consistency:

**JavaScript (src/shared/):**
- `constants.mjs` - MIDI CC/note mappings (MoveKnob1, MoveBack, etc.)
- `chain_ui_views.mjs` - Display constants (SCREEN_WIDTH, LIST_TOP_Y, etc.)
- `input_filter.mjs` - MIDI utilities (decodeDelta)
- `menu_layout.mjs` - UI primitives (drawMenuHeader, drawMenuFooter, overlay system)

**C Headers (src/host/):**
- `shadow_constants.h` - Shared memory names, buffer sizes, struct definitions
- `js_display.h/c` - Display primitives (set_pixel, print, font loading)

### In-Process Shadow Mode (Autostart, Current)

The shim loads and runs the chain DSP **inside MoveOriginal** without the
separate `shadow_poc` process. This mode autostarts when the shim sees the SPI
mailbox `mmap()` and mixes the chain audio directly into the mailbox.

**MIDI routing:** Channel **5–8** are routed to four independent chain instances:
5 → Slot A, 6 → Slot B, 7 → Slot C, 8 → Slot D.

**Config:** `/data/UserData/move-anything/shadow_chain_config.json`

Example:
```json
{
  "patches": [
    { "name": "SF2 + Freeverb (Preset 1)", "channel": 5 },
    { "name": "DX7 + Freeverb", "channel": 6 },
    { "name": "OB-Xd + Freeverb", "channel": 7 },
    { "name": "JV-880 + Freeverb", "channel": 8 }
  ]
}
```

### Components

1. **move_anything_shim.so** - LD_PRELOAD library that:
   - Hooks `mmap()` to capture mailbox address
   - Hooks `ioctl()` to intercept SPI communication
   - Creates shared memory segments for IPC
   - Forwards MIDI from mailbox to shadow process
   - Mixes shadow audio into mailbox audio output

2. **chain dsp.so (in-process)** - Loaded directly by the shim:
   - Scans chain patches in `modules/chain/patches`
   - Loads selected patches for each slot
   - Renders audio blocks inline

3. **shadow_ui** - Separate QuickJS host (no SPI) that:
   - Renders shadow UI into `/move-shadow-display`
   - Reads MIDI from `/move-shadow-midi`
   - Lets you browse patches per slot and request patch changes

### Shared Memory Segments

| Segment | Size | Purpose |
|---------|------|---------|
| `/move-shadow-audio` | 1536 bytes | Triple-buffered audio output (3 × 512) |
| `/move-shadow-movein` | 512 bytes | Move's audio for shadow to read |
| `/move-shadow-midi` | 256 bytes | MIDI from shim to chain DSP |
| `/move-shadow-ui-midi` | 256 bytes | MIDI from shim to shadow UI |
| `/move-shadow-display` | 1024 bytes | Shadow UI display buffer |
| `/move-shadow-control` | 64 bytes | Control flags + UI patch requests |
| `/move-shadow-ui` | 512 bytes | Slot labels + patch names for UI |
| `/move-shadow-param` | 2176 bytes | Parameter get/set requests (increased for large ui_hierarchy JSON) |

Segment names and buffer sizes are defined in `src/host/shadow_constants.h`.

## What Works

- ✅ Shim hooks mmap/ioctl successfully
- ✅ Chain module loading (patches load and play)
- ✅ Audio renders cleanly (in-process)
- ✅ In-process chain autostarts and routes MIDI to channels 5–8
- ✅ Shadow audio mixes with stock Move audio in the mailbox
- ✅ Shadow UI autostarts and renders patch list
- ✅ Jog + jog press navigate slot list and patch browser
- ✅ Most Move controls pass through while shadow UI is active (only jog, click, back blocked)
- ✅ Track buttons (40-43) select shadow slots while in shadow UI
- ✅ Shift+Volume+Track jumps directly to slot settings (works from Move or Shadow UI)
- ✅ Shift+Volume+Menu jumps directly to Master FX settings
- ✅ Knobs (71-78) control focused slot parameters with overlay feedback
- ✅ D-Bus integration shows Move volume on shadow display
- ✅ Master FX chain (4 slots for global effects processing)
- ✅ Hierarchy parameter editor for deep module parameter access
- ✅ ui_hierarchy support for modules to expose navigable parameter trees
- ✅ chain_params support for quick knob mappings
- ✅ Shadow UI Store Picker - install/update/remove modules without leaving shadow mode

## Audio Status

Audio is now clean and stable by running the chain DSP **in-process** inside the shim.
This eliminates IPC timing drift entirely.

## Files Modified

- `src/move_anything_shim.c` - In-process chain loader and routing
- `src/modules/chain/patches/*.json` - Shadow test patches

## How to Test

```bash
# On Move, with shim installed:
cd /opt/move && LD_PRELOAD=/usr/lib/move-anything-shim.so ./Move &

# Set track MIDI channel to 5–8 and play pads
```

Shadow UI Navigation:
- **Toggle display**: Shift + Volume touch + Knob 1 touch
- **Shift+Vol+Track**: Jump directly to slot settings (from anywhere)
- **Shift+Vol+Menu**: Jump directly to Master FX settings (from anywhere)
- **Jog wheel**: Navigate menus, adjust values
- **Jog click**: Enter/confirm selection
- **Back**: Exit to Move (from slot detail or Master FX view)
- **Track buttons (1-4)**: Switch to that slot's settings (from any shadow view)

Knob Control (Shadow UI mode):
- Knobs 1-8 control parameters of the focused slot
- Overlay shows slot name, parameter name, and value during adjustment
- Slot focus follows track selection (selected slot = playback + knob target)
- Touch a knob (without turning) to peek at current value

Knob Control (Move mode with Shift held):
- Shift + turn knob: Adjusts shadow slot parameter, shows overlay
- Shift + touch knob: Peeks at current value without changing it
- Overlay stays visible while finger touches knob, fades after release
- Velocity-based acceleration: slow turns for fine control, fast turns for sweeps

## Extending Shadow Mode

Shadow mode uses the same chain module and patches as standalone Move Anything. **Any module installed via Module Store is automatically available in shadow mode** - no recompilation needed.

### Adding New Synths/Effects

1. **Install the module** via Module Store or copy to `/data/UserData/move-anything/modules/`
2. **Create a chain patch** in `modules/chain/patches/` referencing the module
3. **Select the patch** in the shadow UI

Example patch using an external module:

```json
{
    "name": "JV-880 + CloudSeed",
    "version": 1,
    "chain": {
        "synth": { "module": "jv880" },
        "audio_fx": [
            { "type": "cloudseed", "params": { "wet": 0.3 } }
        ]
    }
}
```

### Module Requirements

Modules must declare chainable capabilities in `module.json`:

```json
{
    "capabilities": {
        "chainable": true,
        "component_type": "sound_generator"
    }
}
```

### Future Enhancements

- **Per-patch default channel override**: Allow patches to specify a preferred receive channel (e.g., JV-880 expects channel 1), which the shadow chain would use when loading instead of the slot's default channel 5-8.

See [MODULES.md](MODULES.md#shadow-mode-integration) for complete documentation.

## Conclusion

In-process shadow DSP is the working path: it autostarts in the shim, renders
clean audio, and mixes with stock Move output. MIDI is gated to channels 5–8 to
avoid UI events. The shadow UI runs in its own process and can swap patches
per slot without stopping stock Move.

## Notes (2026-01-23 - Shadow UI Store Picker)

**New Feature:** Install, update, and remove modules directly from Shadow UI without switching to Move Anything mode.

**How it works:**
- When selecting a synth or effect slot, "[Get more...]" option opens the store picker
- Browse modules by category (Sound Generators, Audio FX, MIDI FX)
- See installed status, available updates, and version info
- Install/Update/Remove modules with real-time progress display
- Newly installed modules are immediately available for use

**Implementation:**
- Shared `store_utils.mjs` provides catalog fetching, install/remove functions
- Shadow UI uses `host_http_download`, `host_extract_tar` for module operations
- Consistent UX with shared `menu_layout.mjs` components (headers, footers, list)
- Module list rescans filesystem after install (no reboot required)

## Notes (2026-01-20 - Track Selection & Knob Control)

**Track Button Slot Selection:**
- Track buttons (CC 40-43) select shadow slots 1-4 while shadow UI is active
- Track buttons are in reverse order: CC43=Track1→slot0, CC42=Track2→slot1, etc.
- Selected slot is marked with `*` asterisk in the slot list
- Selected slot becomes the target for knob parameter control

**Shift+Volume+Track Shortcut:**
- Holding Shift + touching Volume knob + pressing Track button jumps directly to slot settings
- Useful for quickly accessing slot configuration without navigating through menus
- Uses shared memory flags to communicate between shim and shadow UI process

**Knob Parameter Control:**
- Knobs 1-8 (CC 71-78) are routed to the focused slot's chain DSP
- Chain DSP processes knob CCs via its knob mapping infrastructure
- Shadow UI shows overlay with slot/patch info, parameter name, and value
- Knobs are blocked from Move when shadow mode is active

**D-Bus Volume Display:**
- Shadow shim monitors D-Bus for Move volume changes
- Current volume is displayed in shadow UI header
- Uses org.freedesktop.DBus.Properties interface

## Notes (2026-01-20 - Capture Rules)

**New Feature:** Slots can now capture specific Move controls exclusively. When a slot is focused, captured controls are blocked from Move and routed to the slot's DSP.

**Capture Rules Definition:**
- Chain patches define capture in the patch JSON
- Master FX defines capture in module.json capabilities

**Example patch with capture:**
```json
{
    "name": "Performance Effect",
    "audio_fx": [{ "type": "perfverb" }],
    "capture": {
        "groups": ["steps"]
    }
}
```

**Control Group Aliases:**
| Alias | Type | Values | Description |
|-------|------|--------|-------------|
| `pads` | notes | 68-99 | 32 performance pads |
| `steps` | notes | 16-31 | 16 step sequencer buttons |
| `tracks` | CCs | 40-43 | 4 track buttons |
| `knobs` | CCs | 71-78 | 8 encoders |
| `jog` | CC | 14 | Main encoder |

**Granular capture:**
```json
{
    "capture": {
        "groups": ["steps"],
        "notes": [60, 61, 62],
        "note_ranges": [[68, 75]],
        "ccs": [118, 119]
    }
}
```

**Audio FX MIDI:** The `audio_fx_api_v2` now includes an optional `on_midi` callback. FX modules that implement it will receive captured MIDI when focused as Master FX.

## Notes (2026-01-19 - Display Animation FIXED)

**Root Cause:** The display slice protocol requires a 7-phase cycle, not 6:
- Phase 0: Zero slice area with sync=0 (signals start of new frame)
- Phases 1-6: Write slices 0-5 with sync=1-6

**The Fix:**
1. Added DISPLAY_OFFSET (768) write for full framebuffer
2. Implemented proper 7-phase slice protocol with zero phase
3. Write BEFORE ioctl to ensure our content is sent (not Move's)
4. Removed seed pattern that was corrupting shared memory

**Debugging Process:**
- Added incrementing debug bytes to verify write path → saw flickering
- This confirmed hardware reads our writes, issue was content/protocol
- Matching move_anything's exact protocol fixed the animation

## Notes (2026-01-20 - Knob Improvements)

**Velocity-Based Acceleration:**
- Knob turns now use time-based acceleration (1x-8x multiplier based on turn speed)
- Base step reduced from 0.05 to 0.0015 for fine control (~600 clicks for full 0-1 range at slow speed)
- Fast turns sweep through values quickly, slow turns allow precise adjustment
- Separate acceleration cap for int parameters (3x max) to prevent jumping

**Touch-to-Peek:**
- Shift + touch knob (without turning) shows current parameter value
- Overlay stays visible while finger touches the knob
- On finger release, overlay fades after normal timeout
- Works in Move mode (with Shift held) via shim interception

**Int Parameter Type Detection:**
- V2 API (shadow mode) now properly detects int vs float parameter types
- Reads chain_params from module.json to get type/min/max info
- Int parameters display as integers, float parameters as percentages

**Throttled Display Updates:**
- Shadow UI throttles overlay value refresh to once per frame
- Prevents display lag when turning knobs quickly
- Actual DSP parameter updates smoothly; only display sampling is throttled

## Notes (2026-01-19 - Earlier)

- **Fixed jog input not updating display live**: Root cause was timing of `shadow_capture_midi_for_ui()`. It was called AFTER the hardware ioctl, but the ioctl transaction clears the MIDI_IN buffer. Moved the call to BEFORE the ioctl (alongside `midi_monitor()` which correctly reads MIDI before the buffer is cleared). The deduplication removal from earlier was not the actual fix.

## Notes (2026-01-17)

- Shadow UI renders but jog input does not update the highlight live; selection only appears to change after toggling the UI off/on.
- Added shadow UI resiliency: PID file + watchdog relaunch in shim, and install script now kills `shadow_ui` to avoid multiple instances.
- Shadow UI now accepts any MIDI cable for internal events, but jog still does not redraw.
- Captures during jog show no obvious `0xB0` CC packets in MIDI_IN/OUT regions; `usb_midi.log` shows "OTHER" bytes near offset 4088, suggesting jog data may be elsewhere or encoded.
- Next steps: run `mailbox_diff_on` during jog to locate changing bytes; consider SPI/XMOS trace if jog data is outside mailbox. Logs to check:
  - `/data/UserData/move-anything/shadow_ui_midi_capture.log`
  - `/data/UserData/move-anything/usb_midi.log`
  - `/data/UserData/move-anything/midi_region.log`
