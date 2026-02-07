# Architecture: How Move Anything Works

This document explains how Move Anything loads third-party code onto Ableton Move hardware.

## Overview

Move Anything uses a four-layer approach to run custom code on Move:

```
┌─────────────────────────────────────────────────────────────┐
│  1. Installation Bootstrap                                  │
│     install.sh deploys files and replaces /opt/move/Move    │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  2. LD_PRELOAD Shim                                         │
│     Intercepts system calls, monitors for hotkey combo      │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  3. Host Runtime                                            │
│     Takes over hardware, runs QuickJS, manages modules      │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  4. Module Loading                                          │
│     dlopen() loads native DSP plugins, JS handles UI        │
└─────────────────────────────────────────────────────────────┘
```

## Layer 1: Installation Bootstrap

The `scripts/install.sh` script connects to Move via SSH (at `move.local`) and performs these steps:

1. **Deploy files** to `/data/UserData/move-anything/`:
   - `move-anything` (host binary)
   - `move-anything-shim.so` (LD_PRELOAD library)
   - `shim-entrypoint.sh` and `start.sh` (launcher scripts)
   - `host/`, `shared/`, `modules/` directories

2. **Backup the original Move binary**:
   ```
   /opt/move/Move → /opt/move/MoveOriginal
   ```

3. **Install the shim entrypoint** as the new `/opt/move/Move`:
   ```bash
   LD_PRELOAD=move-anything-shim.so /opt/move/MoveOriginal
   ```

4. **Copy shim library** to `/usr/lib/move-anything-shim.so`

After installation, when Move boots normally, it runs the shim entrypoint instead of the original binary.

## Layer 2: LD_PRELOAD Shim

The shim (`src/move_anything_shim.c`) is loaded via `LD_PRELOAD` before the original Move binary runs. It intercepts two system calls:

### `mmap()` Interception

The shim hooks `mmap()` to capture the address of the 4KB shared memory region used for SPI communication with the control surface:

```c
void *mmap(void *addr, size_t length, ...) {
    void *result = real_mmap(addr, length, ...);
    if (length == 4096) {
        global_mmap_addr = result;  // Capture SPI mailbox address
    }
    return result;
}
```

### `ioctl()` Interception and MIDI Monitoring

The shim hooks `ioctl()` to monitor MIDI messages from the hardware on every SPI transaction:

```c
int ioctl(int fd, unsigned long request, char *argp) {
    midi_monitor();  // Check for hotkey combo
    return real_ioctl(fd, request, argp);
}
```

The `midi_monitor()` function reads incoming MIDI from the shared memory mailbox and watches for a specific combination:

- **Shift held** (CC 49 = 127)
- **Volume knob touched** (Note 8 on)
- **Knob 8 touched** (Note 7 on)

When all three are active simultaneously, the shim:

1. Forks a child process
2. Closes all file descriptors (releasing `/dev/ablspi0.0`)
3. Executes `/data/UserData/move-anything/start.sh`
4. Kills the parent (original Move) process

## Layer 3: Host Runtime

The `start.sh` script:

1. Kills any remaining Move processes (`MoveLauncher`, `Move`, etc.)
2. Launches the Move Anything host binary:
   ```bash
   ./move-anything ./host/menu_ui.js
   ```

The host runtime (`src/move_anything.c`):

1. **Opens `/dev/ablspi0.0`** directly for hardware communication
2. **Maps the SPI mailbox** (4KB shared memory for display, MIDI, audio)
3. **Embeds QuickJS** for JavaScript execution
4. **Loads the menu UI** (`host/menu_ui.js`)
5. **Initializes the Module Manager** for plugin discovery

### Main Loop

The host runs a main loop that:

- Renders the display buffer to hardware
- Processes incoming MIDI from pads/knobs/buttons
- Calls JavaScript `tick()` function (~60fps)
- Routes audio through loaded DSP plugins

## Hardware Communication: The SPI Mailbox

All communication with Move hardware happens through a 4KB memory-mapped region accessed via `/dev/ablspi0.0`. The host calls `ioctl()` to trigger SPI transactions that exchange data with the control surface.

### Memory Layout

```
Offset    Size    Direction   Purpose
──────────────────────────────────────────────────────
0x000     256     Out         MIDI to hardware (LEDs, etc.)
0x100     512     Out         Audio output (L/R interleaved)
0x300     1280    Out         Display framebuffer
──────────────────────────────────────────────────────
0x800     256     In          MIDI from hardware (pads, knobs)
0x900     512     In          Audio input (L/R interleaved)
0xB00     1280    In          (unused)
```

### Display (128x64 1-bit)

The display is a 128x64 pixel monochrome screen. The host maintains a `screen_buffer[128*64]` in memory:

```c
unsigned char screen_buffer[128*64];  // 1 byte per pixel (0 or 1)
```

JavaScript draws to this buffer via functions like `set_pixel()`, `print()`, `draw_rect()`.

**Flushing to hardware:**

The screen is packed from 8-bit-per-pixel to 1-bit-per-pixel and sent in 6 slices (172 bytes each) to fit the mailbox:

```c
void push_screen(int sync) {
    // Pack 8 vertical pixels into 1 byte
    for (int y = 0; y < 64/8; y++) {
        for (int x = 0; x < 128; x++) {
            unsigned char packed = 0;
            for (int j = 0; j < 8; j++) {
                packed |= screen_buffer[y*128*8 + x + j*128] << j;
            }
            packed_buffer[i++] = packed;
        }
    }
    // Write slice to mailbox at offset 84
    memcpy(mapped_memory + 84, packed_buffer + slice*172, 172);
}
```

Display refresh is rate-limited to ~11Hz to avoid flicker.

### Audio Buffers

Audio runs at 44100 Hz with 128-frame blocks (~2.9ms latency).

**Output:** DSP plugins render to a buffer, which the host copies to the mailbox:

```c
void mm_render_block(module_manager_t *mm) {
    // Plugin renders 128 stereo frames
    mm->plugin->render_block(mm->audio_out_buffer, 128);

    // Apply host volume
    if (mm->host_volume < 100) {
        for (int i = 0; i < 256; i++) {
            mm->audio_out_buffer[i] = (mm->audio_out_buffer[i] * mm->host_volume) / 100;
        }
    }

    // Copy to mailbox at offset 0x100 (256)
    memcpy(mapped_memory + MOVE_AUDIO_OUT_OFFSET, mm->audio_out_buffer, 512);
}
```

**Input:** Plugins can read audio input directly from the mailbox:

```c
// In plugin code:
int16_t *audio_in = (int16_t *)(host->mapped_memory + host->audio_in_offset);
// audio_in contains 128 stereo samples: [L0, R0, L1, R1, ...]
```

**Format:** Stereo interleaved int16 (little-endian), range -32768 to +32767.

### Native Sampler Bridge (Shim)

The shim can bridge Move Everything's mixed output into Move's native sampler input path when native sampling is in use. This is controlled by the Master FX setting `Resample Src` (`Off`, `Replace`).

In `Replace` mode, the bridge writes a snapshot of the combined Move + Move Everything mix into native `AUDIO_IN`. The snapshot tap point is:
- after slot mix and master FX
- before master-volume attenuation

This keeps native resample capture aligned with the pre-master mix and avoids coupling capture level to transient master-volume reads.

The shim still tracks native sampler-source announcements for diagnostics/compatibility, but `Replace` intentionally applies continuously to avoid dropouts during route/source transitions.

For practical use, `Replace` with sampler source set to `Line In` and monitoring set to `Off` is recommended. Other routing/monitoring combinations can cause feedback loops.

### MIDI

**Incoming MIDI** (from pads, knobs, buttons) arrives at offset 0x800 (2048) as USB-MIDI packets:

```c
// 4-byte USB-MIDI packet format:
// [cable<<4 | CIN, status, data1, data2]

for (int i = 2048; i < 2048+256; i += 4) {
    uint8_t *packet = &mapped_memory[i];
    uint8_t cable = packet[0] >> 4;
    uint8_t status = packet[1];
    uint8_t data1 = packet[2];
    uint8_t data2 = packet[3];

    if (cable == 0) {
        // Internal: pads, knobs, buttons
        onMidiMessageInternal([status, data1, data2]);
    } else if (cable == 2) {
        // External: USB-A connected devices
        onMidiMessageExternal([status, data1, data2]);
    }
}
```

**Outgoing MIDI** (to LEDs, external devices) is written to offset 0x000:

```c
void queueMidiSend(uint8_t cable, uint8_t cin, uint8_t *msg) {
    mapped_memory[outgoing_midi_counter*4 + 0] = (cable << 4) | cin;
    mapped_memory[outgoing_midi_counter*4 + 1] = msg[0];
    mapped_memory[outgoing_midi_counter*4 + 2] = msg[1];
    mapped_memory[outgoing_midi_counter*4 + 3] = msg[2];
    outgoing_midi_counter++;
}
```

### SPI Transaction Cycle

Each iteration of the main loop:

1. JavaScript `tick()` updates display buffer and queues MIDI
2. `push_screen()` writes display slice to mailbox
3. `flush_pending_leds()` writes LED MIDI to mailbox
4. DSP `render_block()` writes audio to mailbox
5. `ioctl()` triggers SPI exchange with hardware
6. Host reads incoming MIDI from mailbox and dispatches to JS/DSP

## Layer 4: Module Loading

Modules live in `/data/UserData/move-anything/modules/<id>/`. Each module contains:

| File | Purpose |
|------|---------|
| `module.json` | Metadata (id, name, version, capabilities) |
| `ui.js` | JavaScript UI (optional) |
| `dsp.so` | Native ARM shared library (optional) |

### Module Discovery

The Module Manager (`src/host/module_manager.c`) scans the modules directory:

```c
void mm_scan_modules(module_manager_t *mm) {
    // For each subdirectory in modules/
    //   Parse module.json
    //   Store module_info_t with paths to ui.js and dsp.so
}
```

### Loading a Module

When a module is selected:

1. **Unload any current module** (call `on_unload`, `dlclose`)

2. **Load the DSP plugin** (if present):
   ```c
   mm->dsp_handle = dlopen(info->dsp_path, RTLD_NOW | RTLD_LOCAL);
   ```

3. **Get the init function**:
   ```c
   move_plugin_init_v1_fn init_fn = dlsym(mm->dsp_handle, "move_plugin_init_v1");
   ```

4. **Initialize the plugin** (passes host API with audio/MIDI callbacks):
   ```c
   mm->plugin = init_fn(&mm->host_api);
   ```

5. **Call `on_load`** with the module directory and JSON defaults

6. **Load the JavaScript UI** via QuickJS

### Plugin API v1

Native DSP plugins implement this interface (`src/host/plugin_api_v1.h`):

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

Audio specs:
- Sample rate: 44100 Hz
- Block size: 128 frames (~3ms latency)
- Format: Stereo interleaved int16

## Module Store and External Modules

The Module Store (built-in module) enables installing third-party modules:

### Catalog

The store fetches a JSON catalog from GitHub:
```
https://raw.githubusercontent.com/charlesvestal/move-anything/main/module-catalog.json
```

Example catalog entry:
```json
{
  "id": "dexed",
  "name": "Dexed",
  "description": "Dexed FM synth with DX7-compatible sysex support",
  "component_type": "sound_generator",
  "latest_version": "0.1.0",
  "download_url": "https://github.com/charlesvestal/move-anything-dx7/releases/download/v0.1.0/dexed-module.tar.gz"
}
```

### Installation Flow

1. User selects a module in the store
2. Store downloads the tarball via `curl`
3. Extracts to `/data/UserData/move-anything/modules/<id>/`
4. Module appears in the main menu immediately

## Returning to Stock Move

When the user exits Move Anything (Shift + Jog click, or "Return to Move" in settings):

1. Host runtime exits
2. `start.sh` restarts `/opt/move/MoveLauncher`
3. MoveLauncher starts the shim entrypoint
4. Shim runs the original Move with MIDI monitoring active

The cycle continues until the hotkey combo is pressed again.

## Shadow Mode

Shadow Mode is an alternative operating mode that runs Move Anything's audio engine **alongside** stock Move, rather than replacing it. This allows users to layer additional synths and effects over Move's native instruments.

### Activation

Shadow Mode is activated with a different hotkey combo:
- **Shift held** (CC 49 = 127)
- **Volume knob touched** (Note 8 on)
- **Knob 1 touched** (Note 0 on)

### Architecture

Unlike the full host takeover, Shadow Mode uses the shim's in-process capabilities:

```
┌─────────────────────────────────────────────────────────────┐
│  Stock Move (MoveOriginal)                                  │
│  - Continues running normally                               │
│  - Handles its own instruments and sequencer                │
└─────────────────────────────────────────────────────────────┘
                              ↕ (shim intercepts ioctl)
┌─────────────────────────────────────────────────────────────┐
│  LD_PRELOAD Shim (in-process)                               │
│  - Filters MIDI: blocks some controls, passes others        │
│  - Loads chain DSP plugins via dlopen                       │
│  - Mixes shadow audio with Move's audio output              │
│  - Manages shared memory for UI communication               │
└─────────────────────────────────────────────────────────────┘
                              ↕ (shared memory)
┌─────────────────────────────────────────────────────────────┐
│  Shadow UI Process (separate)                               │
│  - Runs shadow_ui.js via QuickJS                            │
│  - Draws overlay on display                                 │
│  - Handles jog/back/knob navigation                         │
└─────────────────────────────────────────────────────────────┘
```

### Shared Memory Layout

Shadow Mode uses several shared memory regions for IPC:

| Region | Purpose |
|--------|---------|
| `/shadow_control` | Control flags, slot selection, request IDs |
| `/shadow_ui_midi` | MIDI messages forwarded to shadow UI |
| `/shadow_display` | Display buffer for overlay rendering |
| `/shadow_ui_state` | Slot state (names, channels, active status) |
| `/shadow_param` | Parameter read/write requests |

### MIDI Cables

Move uses USB-MIDI cable numbers to separate different MIDI streams:

| Cable | Direction | Purpose |
|-------|-----------|---------|
| 0 | In | Move hardware controls (pads, knobs, buttons) |
| 0 | Out | Move UI events (filtered from shadow MIDI output) |
| 2 | Out | Track MIDI output (routed to shadow synths) |
| 15 | Both | Special/system messages |

**Important:** When processing outgoing MIDI from Move (for shadow synth input), the shim filters cable 0 to prevent Move's UI events from triggering shadow synths. Only cable 2 (track MIDI output) is routed to shadow slots.

### MIDI Routing

The shim filters incoming MIDI based on control type:

**Blocked from Move (intercepted for Shadow UI):**
- CC 14 (jog wheel) - shadow UI navigation
- CC 3 (jog click) - shadow UI selection
- CC 51 (back button) - shadow UI back navigation
- CC 71-78 (knobs 1-8) - routed to focused slot's DSP
- Notes 0-9 (knob touches) - filtered to avoid confusing Move

**Forwarded to Shadow UI AND passed to Move:**
- CC 40-43 (track buttons) - switch shadow slots while also switching Move tracks

**Passed through to Move (not intercepted):**
- All other CCs: transport (play, record), editing (loop, mute, copy, delete), navigation (arrows), shift
- All notes: pads (68-99), steps (16-31)
- Aftertouch, pitch bend

### Chain Slot System

Shadow Mode supports 4 independent chain slots, each with:
- A loaded patch (synth + effects chain)
- A receive channel (MIDI channel to listen on, default 1-4)
- A forward channel (channel remapping for synths that need specific channels)
- Per-slot volume control
- Independent knob mappings
- State persistence (synth, audio FX, MIDI FX states saved/restored)

```c
typedef struct {
    void *instance;           // Chain DSP instance
    int channel;              // Receive channel (0-15, 0-based)
    int active;               // Slot has loaded patch
    float volume;             // 0.0 to 1.0, applied to audio output
    int forward_channel;      // -1 = auto (use receive channel), 0-15 = specific channel
    char patch_name[64];      // Currently loaded patch name
    shadow_capture_rules_t capture;  // MIDI controls this slot captures
} shadow_chain_slot_t;
```

**Forward Channel:** Some synths (like Mini-JV) need MIDI on a specific channel regardless of which slot they're in. The `forward_channel` setting remaps MIDI before sending to the synth. When set to -1 (auto), MIDI passes through unchanged on the receive channel.

**State Persistence:** When saving slot configuration, the system queries and stores:
- Synth state via `synth:get_state`
- Audio FX state via `audio_fx_<n>:get_state`
- MIDI FX state via `midi_fx_<type>:get_state`

States are restored when the patch is reloaded.

### Capture Rules

Slots can define capture rules that intercept specific Move controls:

```c
typedef struct {
    uint8_t notes[16];   // bitmap: 128 notes
    uint8_t ccs[16];     // bitmap: 128 CCs
} shadow_capture_rules_t;
```

When a slot is focused and has capture rules:
1. Captured notes/CCs are blocked from reaching Move
2. Captured MIDI is routed to the focused slot's DSP via `on_midi`
3. Non-captured MIDI follows normal passthrough behavior

Capture rules are parsed from patch JSON (`"capture"` field) or module.json capabilities (for Master FX).

### Knob Control Routing

Knobs 1-8 (CC 71-78) control the currently focused slot:

1. Shadow UI tracks `selectedSlot` (0-3)
2. JS calls `shadow_set_focused_slot(slot)` to update shared memory
3. Shim reads `shadow_control->ui_slot`
4. Knob CCs are routed to that slot's chain DSP instance
5. Chain DSP applies knob mappings defined in the patch JSON

### Audio Mixing

The shim mixes shadow audio with Move's output post-ioctl:

```c
void shadow_inprocess_mix_audio(void) {
    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);
    int32_t mix[FRAMES_PER_BLOCK * 2];

    // Start with Move's audio
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        mix[i] = mailbox_audio[i];
    }

    // Add each active shadow slot's output
    for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
        if (!shadow_chain_slots[s].active) continue;
        plugin->render_block(shadow_chain_slots[s].instance, slot_buffer, FRAMES_PER_BLOCK);
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            mix[i] += slot_buffer[i];
        }
    }

    // Clamp and write back
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        mailbox_audio[i] = clamp16(mix[i]);
    }
}
```

### Display Overlay

The shadow UI process draws to a shared display buffer. The shim swaps this buffer into the mailbox just before each ioctl, overlaying shadow UI on top of Move's display when shadow mode is active.

### Configuration Persistence

Shadow slot configuration is saved to `/data/UserData/move-anything/shadow_chain_config.json`:

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

### Overtake Mode

Overtake mode is a special shadow mode variant where a module takes complete control of Move's UI. Unlike regular shadow mode (which overlays a custom UI), overtake modules fully replace Move's display and control all LEDs.

**Activation:** Accessed via the shadow UI menu (select an overtake module like MIDI Controller or M8).

**Lifecycle:**

```
Shadow UI selects overtake module
         ↓
Host clears all LEDs progressively
(20 LEDs per frame to avoid buffer overflow)
         ↓
"Loading..." displayed (~500ms)
         ↓
Module's init() called
         ↓
Module takes full control
(display, LEDs, MIDI input)
         ↓
Shift+Vol+Jog Click exits
         ↓
Returns to shadow UI
```

**Host-Level Escape:**

The host tracks shift (CC 49) and volume touch (Note 8) state locally within the shadow UI process. When Shift+Vol+Jog Click is detected, the host exits overtake mode before the module sees the jog click. This ensures escape always works regardless of module behavior.

```javascript
// In shadow_ui.js
let hostShiftHeld = false;
let hostVolumeKnobTouched = false;

// On MIDI input (before passing to module)
if (ccNumber === MoveShift) {
    hostShiftHeld = value === 127;
}
if (isNote && note === 8) {
    hostVolumeKnobTouched = velocity > 0;
}
if (ccNumber === MoveMainButton && value > 0) {
    if (hostShiftHeld && hostVolumeKnobTouched) {
        exitOvertakeMode();
        return; // Don't pass to module
    }
}
```

**LED Buffer Management:**

The MIDI output buffer (`SHADOW_MIDI_OUT_BUFFER_SIZE = 256 bytes`) holds 64 USB-MIDI packets. The host clears LEDs progressively:

| LED Type | Addressing | Count |
|----------|-----------|-------|
| Pads | Notes 68-99 | 32 |
| Steps | Notes 16-31 | 16 |
| Knob touch | Notes 0-7 | 8 |
| Step icons | CCs 16-31 | 16 |
| Buttons | Various CCs | ~20 |
| Knob indicators | CCs 71-78 | 8 |

Total: ~100 LEDs, cleared in batches of 20 per frame (~5 frames = ~80ms).

**Module Initialization:**

Modules should also use progressive LED setup to avoid buffer overflow:

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

## Security Considerations

- The shim runs with elevated privileges (setuid) to access hardware
- Modules are native ARM code with full system access
- Only install modules from trusted sources
- The Module Store only fetches from the official catalog

## Summary Diagram

```
┌─────────────────┐
│   Move boots    │
└────────┬────────┘
         ↓
┌─────────────────┐     ┌──────────────────┐
│ shim-entrypoint │────→│ MoveOriginal     │
│ (LD_PRELOAD)    │     │ (stock Move)     │
└────────┬────────┘     └──────────────────┘
         │
         │ monitors MIDI via ioctl hook
         ↓
┌─────────────────┐
│ Shift + Vol +   │
│ Knob8 detected  │
└────────┬────────┘
         ↓
┌─────────────────┐
│   start.sh      │
│   kills Move    │
└────────┬────────┘
         ↓
┌─────────────────┐
│ move-anything   │
│ (host runtime)  │
└────────┬────────┘
         │
         │ scans modules/, loads via dlopen
         ↓
┌─────────────────┐
│    Modules      │
│ (JS UI + DSP)   │
└─────────────────┘
```
