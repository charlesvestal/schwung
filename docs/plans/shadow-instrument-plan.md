# Shadow Instrument Implementation Plan

## Overview

Add a "shadow mode" to Move Anything that runs alongside stock Move, allowing custom synths (Dexed, OB-Xd, SF2, etc.) to layer with native Move sounds. The existing fourtrack module provides the multi-track engine.

## What Works Without Knobs

- **Notes**: Move sends notes on channels 1-4 per track → shadow routes to 4 tracks
- **Velocity/Aftertouch**: Passed through
- **Audio mixing**: Shadow audio layers with stock Move audio
- **Display switching**: Toggle to shadow UI for patch selection
- **Patch persistence**: Each track remembers its assigned patch

## Architecture

```
Stock Move ──┬──► Audio ──────┬──► MIXED ──► Speakers
             │                │
Shadow DSP ──┴──► Audio ──────┘
     ▲
     │
     └── MIDI (notes on ch 1-4) from shim
```

---

## Phase 0: Proof of Concept

**Goal**: Validate the core architecture with the simplest possible setup

### What We're Proving

1. Shim can intercept ioctl and mix external audio into the mailbox
2. A separate process can receive MIDI from the shim via shared memory
3. That process can render a synth and write audio back
4. Display can be toggled between stock Move and shadow
5. **Stock Move keeps running normally throughout**

### Minimal Components

```
┌─────────────────────────────────────────────────────────────┐
│                      PROOF OF CONCEPT                       │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  SHIM (extended)                                            │
│  ├── Creates 3 shm segments: audio, midi, control           │
│  ├── In ioctl: mix shadow_audio into mailbox                │
│  ├── In ioctl: copy mailbox MIDI to shadow_midi             │
│  ├── In ioctl: if display_mode, swap in shadow_display      │
│  └── In midi_monitor: detect Shift+Vol+Knob1 → toggle mode  │
│                                                             │
│  SHADOW PROCESS (new, simple)                               │
│  ├── Opens shm segments                                     │
│  ├── Loads ONE synth (e.g., SF2 with a piano soundfont)     │
│  ├── Reads MIDI from shm, sends to synth                    │
│  ├── Renders synth audio to shm                             │
│  └── Renders simple display: "SHADOW MODE - SF2 Piano"      │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### Hotkey Mappings

| Hotkey | Action | MIDI Detection |
|--------|--------|----------------|
| Shift + Vol + Knob 8 | Launch Move Anything (replace stock) | CC 49=127 + Note 8=127 + Note 7=127 |
| Shift + Vol + Knob 1 | Toggle shadow mode | CC 49=127 + Note 8=127 + Note 0=127 |

Knob touch notes: Knob 1 = Note 0, Knob 2 = Note 1, ..., Knob 8 = Note 7, Volume = Note 8, Wheel = Note 9

### Success Criteria

- [ ] Boot Move → stock Move works normally
- [ ] Play pads → hear Move's sounds
- [ ] Shadow process running in background (no audio yet until MIDI arrives)
- [ ] Play pads → hear Move's sounds AND shadow synth layered
- [ ] Press Shift+Vol+Knob1 → display shows "SHADOW MODE"
- [ ] Press Shift+Vol+Knob1 again → display returns to stock Move
- [ ] Throughout all of this, stock Move never crashes or glitches

### Test Sequence

1. **Install modified shim** (with shm + audio mixing + mode toggle)
2. **Start shadow process manually** via SSH: `./shadow_poc &`
3. **Boot Move normally** (or it's already running)
4. **Play some pads** on any track
5. **Listen**: should hear Move sound + shadow piano layered
6. **Toggle display**: Shift + Volume knob + Knob 1 touch
7. **See shadow UI**: simple text saying what's loaded
8. **Toggle back**: same hotkey
9. **Verify**: Move still works, no glitches

### Files for POC

```
src/
├── move_anything_shim.c      # MODIFY: add shm, mixing, mode toggle
└── shadow/
    └── shadow_poc.c          # NEW: minimal shadow process

scripts/
└── build_shadow_poc.sh       # NEW: build just the POC binary
```

### POC Shadow Process (Pseudocode)

```c
// shadow_poc.c - Minimal proof of concept

#include "plugin_api_v1.h"
#include <sys/mman.h>

int main() {
    // 1. Open shared memory
    int fd_audio = shm_open("/move-shadow-audio", O_RDWR, 0);
    int fd_midi = shm_open("/move-shadow-midi", O_RDONLY, 0);
    int fd_ctrl = shm_open("/move-shadow-control", O_RDWR, 0);

    int16_t *audio_shm = mmap(..., fd_audio, ...);
    uint8_t *midi_shm = mmap(..., fd_midi, ...);
    shadow_control_t *ctrl = mmap(..., fd_ctrl, ...);

    // 2. Load a simple synth (SF2 with built-in piano)
    void *sf2_handle = dlopen("modules/sf2/dsp.so", RTLD_NOW);
    plugin_api_v1_t *synth = init_plugin(sf2_handle);
    synth->on_load("/data/UserData/move-anything/modules/sf2", NULL);
    synth->set_param("soundfont_path", "/path/to/piano.sf2");

    // 3. Signal ready
    ctrl->shadow_ready = 1;
    printf("Shadow POC ready\n");

    // 4. Main loop
    int16_t render_buf[256];
    uint8_t last_midi[256];

    while (!ctrl->should_exit) {
        // Check for new MIDI (simple: compare to last)
        if (memcmp(midi_shm, last_midi, 256) != 0) {
            memcpy(last_midi, midi_shm, 256);

            // Process MIDI packets
            for (int i = 0; i < 256; i += 4) {
                if (midi_shm[i] == 0) continue;
                uint8_t *pkt = &midi_shm[i];
                // Forward to synth (bytes 1-3 are the MIDI message)
                synth->on_midi(&pkt[1], 3, 0);
            }
        }

        // Render audio
        synth->render_block(render_buf, 128);

        // Write to shared memory
        memcpy(audio_shm, render_buf, 512);

        // Pace loop (~3ms to match hardware block rate)
        usleep(2900);
    }

    synth->on_unload();
    dlclose(sf2_handle);
    return 0;
}
```

### POC Display (Simple)

When `ctrl->display_mode == 1`, shim writes this to mailbox display region:

```
┌────────────────────────────────────┐
│         SHADOW MODE                │
│                                    │
│    SF2 Piano Loaded                │
│                                    │
│    Playing on all channels         │
│                                    │
│  Shift+Vol+Knob1: Return to Move   │
└────────────────────────────────────┘
```

For POC, this can be a hardcoded bitmap or simple text render in the shim itself.

### What POC Does NOT Include

- Multiple tracks/patches (just one synth for all notes)
- Patch browser UI
- Config persistence
- Auto-launch (manual start via SSH)
- Graceful error handling

### After POC Success

Once POC validates the architecture, proceed to Phase 1-6 for full implementation with fourtrack engine, multi-track routing, proper UI, etc.

---

## Phase 1: Shim Broker Foundation

**Goal**: Shim can mix shadow audio and forward MIDI via shared memory

### 1.1 Shared Memory Setup

File: `src/move_anything_shim.c`

```c
// Add to shim initialization (in mmap hook, after first detection)
void init_shadow_shm() {
    // Create shared memory segments
    shm_audio_fd = shm_open("/move-shadow-audio", O_CREAT|O_RDWR, 0666);
    ftruncate(shm_audio_fd, 512);  // 128 frames * 2 channels * 2 bytes
    shadow_audio = mmap(...);

    shm_midi_fd = shm_open("/move-shadow-midi", O_CREAT|O_RDWR, 0666);
    ftruncate(shm_midi_fd, 256);
    shadow_midi = mmap(...);

    shm_control_fd = shm_open("/move-shadow-control", O_CREAT|O_RDWR, 0666);
    ftruncate(shm_control_fd, 64);
    shadow_control = mmap(...);

    memset(shadow_control, 0, 64);
}
```

### 1.2 Audio Mixing in ioctl Hook

```c
int ioctl(int fd, unsigned long request, char *argp) {
    // Mix shadow audio BEFORE hardware transaction
    if (shadow_control && shadow_control->shadow_ready) {
        int16_t *mailbox = (int16_t*)(global_mmap_addr + 256);
        int16_t *shadow = (int16_t*)shadow_audio;

        for (int i = 0; i < 256; i++) {
            int32_t mixed = (int32_t)mailbox[i] + (int32_t)shadow[i];
            mailbox[i] = (mixed > 32767) ? 32767 :
                         (mixed < -32768) ? -32768 : mixed;
        }
    }

    int result = real_ioctl(fd, request, argp);

    // Copy incoming MIDI to shadow after transaction
    if (shadow_midi) {
        memcpy(shadow_midi, global_mmap_addr + 2048, 256);
    }

    return result;
}
```

### 1.3 Deliverable

- [ ] Shared memory creation/cleanup in shim
- [ ] Audio mixing in ioctl hook
- [ ] MIDI forwarding to shm after ioctl
- [ ] Test: dummy shadow process writes sine wave, hear it mixed with Move

---

## Phase 2: Shadow DSP Process

**Goal**: Standalone process that renders fourtrack audio to shared memory

### 2.1 Create shadow_dsp binary

File: `src/shadow/shadow_dsp.c`

```c
int main() {
    // Open shared memory (created by shim)
    shadow_audio = open_shm("/move-shadow-audio");
    shadow_midi = open_shm("/move-shadow-midi");
    shadow_control = open_shm("/move-shadow-control");

    // Initialize fourtrack engine
    fourtrack_init();

    // Signal ready
    shadow_control->shadow_ready = 1;

    // Main loop - sync to shim's ~3ms blocks
    while (running) {
        // Process MIDI from shm
        process_midi(shadow_midi);

        // Render audio
        fourtrack_render(audio_buffer, 128);

        // Write to shm
        memcpy(shadow_audio, audio_buffer, 512);

        // Pace to ~3ms
        usleep(2900);
    }
}
```

### 2.2 Extract fourtrack core

The fourtrack module has the engine we need. Options:
- A) Link fourtrack.c directly into shadow_dsp
- B) Extract shared library
- C) Copy/adapt relevant functions

Recommend (A) for simplicity.

### 2.3 MIDI Channel Routing

Fourtrack already has `MIDI_ROUTING_SPLIT_CHANNELS`:
- Channel 1 → Track 1
- Channel 2 → Track 2
- Channel 3 → Track 3
- Channel 4 → Track 4

Just ensure this mode is enabled by default in shadow.

### 2.4 Deliverable

- [ ] shadow_dsp.c main loop
- [ ] Link fourtrack rendering code
- [ ] MIDI processing from shm
- [ ] Audio output to shm
- [ ] Test: load Dexed patch, play Move pads, hear Dexed layered

---

## Phase 3: Display Switching

**Goal**: Toggle display between stock Move and shadow UI

### 3.1 Add display shm

```c
shm_display_fd = shm_open("/move-shadow-display", O_CREAT|O_RDWR, 0666);
ftruncate(shm_display_fd, 1024);  // 128x64 @ 1bpp
shadow_display = mmap(...);
```

### 3.2 Mode toggle hotkey

In shim's `midi_monitor()`, add detection for Knob 1 touch (Note 0):

```c
// Add variable at top of file
int knob1touched = 0;

// In midi_monitor(), add detection (similar to knob8touched)
if (midi_0 == 0x90 && midi_1 == 0x00) {  // Note 0 = Knob 1 touch
    if (midi_2 == 0x7f) {
        knob1touched = 1;
    } else {
        knob1touched = 0;
    }
}

// Add toggle logic (AFTER the existing Move Anything launch check)
if (shiftHeld && volumeTouched && knob1touched && !shadowModeDebounce) {
    shadow_control->display_mode = !shadow_control->display_mode;
    shadowModeDebounce = 1;
    printf("Shadow mode toggled: %d\n", shadow_control->display_mode);
}

// Reset debounce when shift released
if (!shiftHeld) {
    shadowModeDebounce = 0;
}
```

### 3.3 Display swap in ioctl

```c
if (shadow_control->display_mode && shadow_control->shadow_ready) {
    memcpy(global_mmap_addr + DISPLAY_OFFSET, shadow_display, 1024);
}
```

### 3.4 Shadow display rendering

Adapt fourtrack's UI or create minimal UI:
```
┌────────────────────────────────────┐
│ SHADOW                             │
│ ─────────────────────────────────  │
│ 1: Dexed E.Piano      [ch1]          │
│ 2: OB-Xd Pad        [ch2]          │
│ 3: SF2 Piano        [ch3]          │
│ 4: Mini-JV Strings   [ch4]          │
│                                    │
│ Shift+Vol+Jog: Return to Move      │
└────────────────────────────────────┘
```

### 3.5 Deliverable

- [ ] Display shm segment
- [ ] Mode toggle hotkey detection
- [ ] Display swap in ioctl hook
- [ ] Basic shadow UI rendering
- [ ] Test: toggle displays, see shadow UI, toggle back

---

## Phase 4: Patch Configuration

**Goal**: Load/save patch assignments per track

### 4.1 Config file

File: `/data/UserData/move-anything/shadow_config.json`

```json
{
    "tracks": [
        {"patch": "dx7_epiano", "level": 1.0, "pan": 0.0},
        {"patch": "obxd_pad", "level": 0.8, "pan": 0.0},
        {"patch": "sf2_piano", "level": 1.0, "pan": 0.0},
        {"patch": "jv880_strings", "level": 0.7, "pan": 0.0}
    ]
}
```

### 4.2 Patch browser (when in shadow display mode)

- Jog wheel: navigate patches
- Jog click: select patch for current track
- Track buttons (40-43): select track to configure

### 4.3 Deliverable

- [ ] Config file load/save
- [ ] Patch browser in shadow UI
- [ ] Track selection via track buttons
- [ ] Persist changes on patch select

---

## Phase 5: Startup Integration

**Goal**: Shadow process starts automatically

### 5.1 Launch from shim

When shim detects first mmap (Move is starting):
```c
if (!shadow_launched && shadow_enabled_in_config) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/data/UserData/move-anything/shadow_dsp", "shadow_dsp", NULL);
    }
    shadow_launched = 1;
}
```

### 5.2 Clean shutdown

When shim detects Move exit (or on SIGTERM):
```c
shadow_control->should_exit = 1;
// Wait briefly for shadow to clean up
usleep(100000);
shm_unlink("/move-shadow-audio");
// etc.
```

### 5.3 Deliverable

- [ ] Auto-launch shadow_dsp from shim
- [ ] Graceful shutdown signaling
- [ ] Recovery if shadow crashes

---

## Phase 6: Polish

### 6.1 Visual feedback
- LED flash on mode toggle
- Track LEDs show which tracks have patches loaded

### 6.2 Level control
- In shadow mode, volume knob controls shadow master level
- Or: dedicated shadow level in config

### 6.3 Mute per track
- Track button hold = mute that shadow track
- Visual feedback on display

### 6.4 Deliverable

- [ ] Mode switch LED feedback
- [ ] Shadow volume control
- [ ] Per-track mute
- [ ] Documentation

---

## File Changes Summary

### New Files
- `src/shadow/shadow_dsp.c` - Shadow DSP process
- `src/shadow/shadow_ui.c` - Display rendering for shadow
- `docs/SHADOW.md` - User documentation

### Modified Files
- `src/move_anything_shim.c` - Add broker functionality
- `scripts/build.sh` - Build shadow_dsp binary
- `scripts/install.sh` - Deploy shadow_dsp

### Build Output
- `build/shadow_dsp` - Shadow DSP binary (ARM64)

---

## Testing Checklist

### Phase 1
- [ ] Shim creates shm on Move boot
- [ ] Dummy process can write audio to shm
- [ ] Mixed audio audible through speakers

### Phase 2
- [ ] Shadow process starts and connects to shm
- [ ] MIDI notes reach shadow tracks (verify with logging)
- [ ] Dexed/OB-Xd sounds play when Move pads pressed

### Phase 3
- [ ] Hotkey toggles display
- [ ] Shadow UI visible
- [ ] Return to Move UI works

### Phase 4
- [ ] Patches load on startup
- [ ] Patch browser works
- [ ] Config persists across reboot

### Phase 5
- [ ] Shadow auto-starts with Move
- [ ] Clean shutdown on Move exit
- [ ] Recovery from shadow crash

---

## Timeline Estimate

| Phase | Complexity | Dependencies |
|-------|------------|--------------|
| 1. Shim Broker | Medium | None |
| 2. Shadow DSP | Medium | Phase 1 |
| 3. Display Switch | Low | Phase 1 |
| 4. Patch Config | Low | Phase 2, 3 |
| 5. Startup | Low | Phase 2 |
| 6. Polish | Low | All above |

Phases 1-2 are the core - once those work, you have layered sounds.
Phases 3-6 are UX improvements.

---

## Open Questions

1. **Should shadow audio be mutable from stock mode?** (e.g., track mute button affects shadow too?)
2. **What if user wants ONLY shadow sounds?** (mute stock Move somehow?)
3. **How to handle if shadow crashes mid-performance?** (graceful degradation)

---

## Future Enhancements (Out of Scope)

- Knob control in shadow mode (requires CC interception)
- Recording shadow output separately
- MIDI FX (arp, chord) in shadow
- Multiple shadow patch sets (scenes)
