# Schwung 2.0 Architecture Design

**Date:** 2026-03-13
**Status:** Draft
**Authors:** Charles Vestal + Claude

## Overview

Schwung 1.x grew organically from experiments into a 62,000-line framework running 31 modules on Ableton Move hardware. It works, but three god objects (shadow_ui.js at 10,969 lines, chain_host.c at 8,037 lines, schwung_shim.c at 5,142 lines) account for 39% of the codebase and implement the same features 2-3 times independently.

2.0 is a clean-room restructure that:
- Replaces god objects with focused domains
- Defines a module contract so modules declare what they need and get behavior for free
- Makes every domain unit-testable without hardware
- Rewrites the LD_PRELOAD shim from scratch using Ableton's GPL-2.0 `ablspi` library as reference (no bobbydigitales code)
- Lives in a new repo (`move-anything-2`) alongside the working 1.x system

**Shadow mode is the only mode.** Standalone mode (removed in 1.x as of March 2026) does not return. Schwung always coexists with stock Move via the LD_PRELOAD shim. There is no "replace MoveEngine" mode.

## Why a New Repo

- **Testing.** Domains are pure C/JS modules with clean interfaces. Unit tests run on Mac, no hardware needed.
- **No dark period.** 1.x keeps running on hardware while 2.0 is built and tested.
- **Clean git history.** No "refactor shadow_ui.js part 37 of 94" commits.
- **Module porting is the same either way.** Every module needs params.json and v3 API changes regardless of approach.

The hardware interface (shim, audio mailbox, MIDI buffers, display protocol) is rewritten from scratch in Phase 0.5 using Ableton's official `ablspi` library as the authoritative reference for buffer layout, struct definitions, and protocol constants. No code from the bobbydigitales fork is used. Battle-tested domain knowledge (cable routing, LED limits, display protocol) is carried forward in documentation form.

---

## Section 1: Domain Map

### Current State

Three god objects talking through shared memory, each reimplementing patch management, parameter editing, module loading, and knob handling independently.

```
Shadow UI (10,969 lines, does everything)
    ↕ shared memory
Chain Host (8,037 lines, does everything)
    ↕ dlopen
Shim (5,142 lines, does everything)
```

### 2.0 Architecture

24 focused domains organized in 6 layers. Each domain has exactly one implementation. No feature is implemented twice.

```
Hardware I/O
    ├── midi_in                  Raw cable 0/2, device detection, MPE detection
    ├── midi_out                 External USB out, internal loopback,
    │                            device protocol support, connection state
    ├── led_manager              LED state, batching, caching, grid abstraction,
    │                            progressive init for overtake
    └── screen_manager           Framebuffer, dirty regions, layer compositing

Input
    ├── input_manager            Raw MIDI → clean events (pad, knob, jog,
    │                            buttons, shift, transport, pressure/aftertouch)
    └── knob_engine              Acceleration math, type-aware stepping,
                                 parameter banking (8-knob pages)

Audio
    ├── audio_engine             Buffer pool, render pipeline, per-slot mix,
    │                            master bus, master volume, format config
    │                            (sample rate / frames / bit depth — queryable)
    ├── audio_input              Source selection (resample/line-in/mic),
    │                            circular capture buffer, quantized sampler,
    │                            skipback, module audio feeds, WAV writer,
    │                            clock-aligned recording
    ├── track_audio              Per-track taps from Move's mixer (Link Audio)
    └── signal_chain             Per-slot: midi_fx → synth → audio_fx
                                 Master: master_fx on mixed bus
                                 MIDI routing to audio FX that declare midi_in
                                 Audio input routing from declarations

Timing
    └── clock_manager            Tempo (internal BPM / external MIDI clock),
                                 transport state (play/stop/continue),
                                 bar/beat/tick position, tap tempo,
                                 subscribers: arp, LFOs, quantized sampler

Modulation
    └── modulation               LFOs, mod matrix, clock-synced,
                                 routes to param_system targets

Core Logic
    ├── midi_router              Channel mapping, slot dispatch, queue mgmt
    ├── param_system             Metadata registry (static AND dynamic),
    │                            get/set with validation, formatting,
    │                            knob binding, change notification,
    │                            blocking delivery mode, banking/paging
    ├── slot_manager             Volume, mute, solo, channel config (receive/forward),
    │                            active slot, signal chain assignment,
    │                            session lifecycle (active/background/unloaded)
    ├── module_registry          Discovery, loading, capabilities, lifecycle,
    │                            progressive loading states, asset requirements,
    │                            module-private storage, sub-plugin hosting
    ├── patch_store              Load/save/list patches, state versioning
    └── task_manager             Background tasks, long-running services,
                                 downloads, progress, cancellation

Host Services
    ├── module_store             Catalog fetch, download, install, update, remove
    ├── settings                 Preferences, velocity curves, aftertouch,
    │                            display settings
    ├── screen_reader            Observes param_system, menu_toolkit, input_manager —
    │                            auto-announces changes, zero module code needed
    └── help                     Per-module help, consistent rendering

System Services
    ├── update_manager           Host self-update, integrity check, restart, rollback
    ├── logger                   Unified log (debug.log), levels, rotation,
    │                            structured output, C + JS sources
    ├── health_monitor           Watchdog (hung modules, audio underruns, MIDI overflow),
    │                            auto-recovery, disk space monitoring, diagnostics
    └── feature_config           Feature flags (features.json), queried by domains

UI Layer
    └── menu_toolkit             Menus, overlays, preset browser, param editor,
                                 parameter bank navigation, renders into screen_manager,
                                 subscribes to input_manager events
```

### Dependency Flow (No Cycles)

```
Hardware I/O  →  Input / Audio  →  Core Logic  →  UI Layer
                                        ↑
                                   module_registry
                                        ↑
                                  External Modules
```

### What Modules Access by Type

| Module type | Domains accessed |
|---|---|
| Sound generator | `audio_engine` (render buffer), `param_system`, optionally `audio_input` |
| Audio FX | `audio_engine` (process buffer), `param_system`, optionally `midi_router`, `audio_input` |
| MIDI FX | `midi_router` (transform events), `param_system`, `clock_manager` |
| Performance FX | `audio_input` (live feed), `audio_engine`, `param_system`, `input_manager`, `led_manager` |
| Overtake | Everything — `input_manager`, `led_manager`, `screen_manager`, `midi_in/out`. Shim suppresses Move's display and routes all MIDI (cable 0 + cable 2+) to the overtake module. Progressive LED init via `led_manager` batching. |
| Tool | `menu_toolkit`, `screen_manager`, `input_manager`, optionally `audio_engine`, `param_system`. Runs within shadow UI (not overtake). Accessed via Tools menu. Can be `backgroundable` (Song Mode, WaveEdit). |

### Domain Design Rationale

Each domain was pressure-tested against real modules:

- **knob_engine** — today 5 independent implementations with different acceleration curves (shadow_ui 1x-4x at 50-250ms, menu_nav 1x-10x at 25-150ms, chain_host 1x-4x, chain/ui.js none, controller none). One engine fixes this.
- **param_system with dynamic registration** — CLAP discovers 498 parameters at runtime. Static declarations aren't enough.
- **param_system with blocking delivery** — Performance FX needs guaranteed punch-in/out delivery. Fire-and-forget drops critical events.
- **signal_chain with MIDI-to-audio-FX routing** — Ducker is an audio FX that triggers from MIDI. Today discovered via `dlsym` hack, no declared capability.
- **audio_input with declared sources** — Vocoder reads hardware line-in directly from mailbox offsets. Modules should declare `audio.inputs: ["hardware_input"]` and receive a clean pointer.
- **clock_manager** — Arpeggiator, LFOs, quantized sampler, and Performance FX repeat effects all need tempo/transport. No shared clock exists today.
- **module_registry with sessions** — WaveEdit, Performance FX, Song Mode need to keep running (audio + MIDI) when the user navigates away. Today modules are binary: loaded or unloaded.
- **task_manager** — Module Store downloads block tick(). Any operation longer than one tick should be a managed background task.
- **screen_reader as observer** — Today every module manually calls announce(). If it doesn't, screen reader is silent. 2.0 auto-announces from param_system and menu_toolkit data.
- **led_manager with batching** — Overtake modules need to set all 32 pad LEDs + step LEDs + track LEDs on init, but the SPI buffer holds only 20 MIDI OUT messages per transfer. Progressive init (8 LEDs/frame) is mandatory. Today each overtake module implements its own batching. `led_manager` handles this once.
- **module_registry with tool lifecycle** — Tool modules run within the shadow UI menu system, not as overtake. They need consistent loading, input routing, and the ability to exit back to the tools menu. Today `host_exit_module()` and tool menu navigation are ad-hoc. `module_registry` standardizes this.
- **No standalone mode** — Standalone mode was removed from 1.x (March 2026). 2.0 is shadow-only. Every module type (sound generator, audio FX, MIDI FX, overtake, tool) runs within the shadow mode shim architecture. There is no mode where Schwung replaces MoveEngine.

---

## Section 2: Module Contract

### Module Types

Every module is exactly one of these. The type determines which domain APIs the module gets access to and which callbacks it must implement.

| Type | What it does | Implements |
|---|---|---|
| `sound_generator` | Receives MIDI, renders audio | `render_block`, `on_midi` |
| `audio_fx` | Processes audio buffer in-place | `process_block`, optionally `on_midi` |
| `midi_fx` | Transforms MIDI events | `process_midi`, `tick` |
| `overtake` | Full UI/LED/input control | `init`, `tick`, `on_input`, `on_midi` |
| `tool` | Menu-driven utility UI | `init`, `tick`, `on_input` |

### Module Declaration (module.json 2.0)

```json
{
  "id": "my-synth",
  "name": "My Synth",
  "version": "1.0.0",
  "type": "sound_generator",

  "audio": {
    "inputs": [],
    "outputs": ["stereo"]
  },

  "midi": {
    "in": true,
    "out": false,
    "mpe": false,
    "clock": false
  },

  "params": "params.json",

  "presets": {
    "browsable": true,
    "banks": true
  },

  "assets": ["roms/rom1.bin"],

  "session": {
    "backgroundable": false
  }
}
```

Key fields:

- **`type`** — determines domain access and required callbacks
- **`audio.inputs`** — declares what audio sources the module reads: `"chain_audio"`, `"hardware_input"`, `"resample"`, `"track_audio"`. Framework routes accordingly. No raw mailbox offsets.
- **`midi.in` on audio_fx** — Ducker declares this and gets MIDI routed to it. No more `dlsym` discovery.
- **`params`** — points to params.json (static) or `"dynamic"` for runtime registration (CLAP).
- **`presets`** — if declared, framework handles jog wheel browsing with consistent debouncing, wrapping, and display.
- **`assets`** — required files. `module_registry` checks they exist before loading.
- **`session`** — if `backgroundable: true`, module persists when user navigates away. Framework routes audio/MIDI and shows status indicator.

### Unified Parameter File (params.json)

Replaces both `chain_params` (type metadata) AND `ui_hierarchy` (menu structure, knob mapping) with a single file. A parameter's group, knob position, type, range, and display are all part of its definition.

```json
{
  "modes": {
    "param": "mode",
    "options": ["Patch", "Performance"]
  },

  "presets": {
    "param": "preset",
    "count_param": "preset_count",
    "name_param": "preset_name",
    "bank_param": "soundfont_index",
    "bank_list_param": "soundfont_list",
    "bank_label": "Soundfont"
  },

  "groups": [
    {
      "id": "oscillator",
      "label": "Oscillator",
      "params": [
        {
          "key": "algorithm",
          "label": "Algorithm",
          "type": "enum",
          "options": ["CSAW", "Morph", "Pluck", "Chord"],
          "default": "CSAW",
          "knob": 1
        },
        {
          "key": "timbre",
          "label": "Timbre",
          "type": "float",
          "min": 0, "max": 1, "step": 0.01,
          "default": 0.5,
          "knob": 2
        }
      ]
    },
    {
      "id": "envelope",
      "label": "Envelope",
      "params": [
        {
          "key": "attack",
          "label": "Attack",
          "type": "float",
          "min": 0, "max": 1, "step": 0.01,
          "default": 0.1,
          "knob": 1
        }
      ]
    }
  ]
}
```

Three navigation concepts:

1. **Modes** — optional top-level context switch (e.g., Patch vs Performance in JV880). Changes which presets/params are visible.
2. **Presets** — optional browsable list, with optional banks. Framework handles debouncing, wrapping, display. One-level (Braids) or two-level (SF2 soundfont → preset, JV880 bank → patch).
3. **Groups** — parameter groups for editing. Knobs mapped per-group via `"knob": N`. Framework renders the UI, modules just respond to `on_param_changed`.

### What a Module Gets for Free

By declaring params.json and module.json, the framework automatically provides:
- Parameter storage, validation (rejects out-of-range), and formatting
- Knob mapping with consistent acceleration and parameter banking
- Preset browsing with debouncing and display
- State serialization (auto-reads all declared params, saves as JSON)
- UI rendering in shadow mode (menus, overlays, knob feedback)
- Audio input routing (clean pointers via audio_buffers_t)
- MIDI dispatch (clean events, no cable filtering)
- Screen reader announcements (auto-generated from param changes)
- Help integration

### C API: module_api_v3 (Sound Generators)

```c
typedef struct audio_buffers {
    int16_t *chain_audio;      // NULL if not requested
    int16_t *hardware_input;   // NULL if not requested
    int16_t *resample;         // NULL if not requested
    int16_t *track_audio[4];   // NULL if not requested
    int frames;
    int sample_rate;
} audio_buffers_t;

typedef struct module_config {
    int sample_rate;
    int frames_per_block;
    const char *module_dir;
    const char *data_dir;       // module-private storage
    void (*log)(const char *msg);
} module_config_t;

typedef struct module_api_v3 {
    uint32_t api_version;       // 3

    // Lifecycle
    void* (*create)(const module_config_t *config);
    void  (*destroy)(void *instance);

    // Audio
    void  (*render_block)(void *instance,
                          const audio_buffers_t *inputs,
                          int16_t *out_lr, int frames);

    // MIDI
    void  (*on_midi)(void *instance, const uint8_t *msg, int len, int source);

    // Params — framework owns storage, module gets notified of changes
    void  (*on_param_changed)(void *instance, const char *key, const char *value);

    // State — optional override. If NULL, framework auto-serializes
    // all params from params.json.
    int   (*get_state)(void *instance, char *buf, int buf_len);
    void  (*set_state)(void *instance, const char *json);

    // Session — optional. For backgroundable modules.
    void  (*on_background)(void *instance);
    void  (*on_foreground)(void *instance);
    int   (*get_status)(void *instance, char *buf, int buf_len);
} module_api_v3_t;

// Entry point
extern "C" module_api_v3_t* move_plugin_init_v3(const module_config_t *config);
```

### C API: audio_fx_api_v3

```c
typedef struct audio_fx_api_v3 {
    uint32_t api_version;
    void* (*create)(const module_config_t *config);
    void  (*destroy)(void *instance);
    void  (*process_block)(void *instance,
                           const audio_buffers_t *inputs,
                           int16_t *audio_inout, int frames);
    void  (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void  (*on_param_changed)(void *instance, const char *key, const char *value);
    int   (*get_state)(void *instance, char *buf, int buf_len);
    void  (*set_state)(void *instance, const char *json);
} audio_fx_api_v3_t;
```

### C API: midi_fx_api_v2

```c
typedef struct midi_fx_api_v2 {
    uint32_t api_version;
    void* (*create)(const module_config_t *config);
    void  (*destroy)(void *instance);
    int   (*process_midi)(void *instance,
                          const uint8_t *in_msg, int in_len,
                          uint8_t *out_msgs, int max_out);
    int   (*tick)(void *instance, int frames, int sample_rate,
                  uint8_t *out_msgs, int max_out);
    void  (*on_param_changed)(void *instance, const char *key, const char *value);
    int   (*get_state)(void *instance, char *buf, int buf_len);
    void  (*set_state)(void *instance, const char *json);
} midi_fx_api_v2_t;
```

### Key Differences from v2/v1 API

| Aspect | v1/v2 (today) | v3 (2.0) |
|---|---|---|
| Parameter storage | Module owns, strcmp chains | Framework owns, module gets `on_param_changed` |
| Parameter metadata | Hardcoded JSON strings in C | `params.json` file or dynamic registration |
| State serialization | Manual `get_param("state")` JSON | Automatic from declared params (override if needed) |
| Audio format | Hardcoded 44100/128/int16 | Passed in `module_config_t` and `audio_buffers_t` |
| Audio inputs | Raw mailbox offset reads | Declared in module.json, clean pointers in `audio_buffers_t` |
| MIDI on audio FX | `dlsym("move_audio_fx_on_midi")` hack | Declared `midi.in: true`, `on_midi` callback |
| UI hierarchy | Hardcoded JSON string in C | Part of `params.json` groups |

---

## Section 3: Compatibility Adapter

A thin C layer (`compat_v2.c`) wraps v2 modules to look like v3 to the framework. This allows incremental migration — the new framework works with old modules immediately.

### How It Works

When `module_registry` loads a module and sees `api_version: 2` in module.json, it wraps it:

```c
module_api_v3_t* compat_wrap_v2_plugin(plugin_api_v2_t *v2,
                                        const char *module_dir);
audio_fx_api_v3_t* compat_wrap_v2_audio_fx(audio_fx_api_v2_t *v2, ...);
midi_fx_api_v3_t*  compat_wrap_v1_midi_fx(midi_fx_api_v1_t *v1, ...);
```

### Adapter Behavior

| v3 callback | Adapter delegates to |
|---|---|
| `create()` | `v2->create_instance(module_dir, json)` |
| `destroy()` | `v2->destroy_instance(inst)` |
| `render_block(bufs, out)` | Ignores `audio_buffers_t`, calls `v2->render_block(inst, out, frames)` |
| `on_midi(msg)` | `v2->on_midi(inst, msg, len, source)` |
| `on_param_changed(k, v)` | `v2->set_param(inst, key, val)` |
| `get_state()` | `v2->get_param(inst, "state", buf, len)` |
| `set_state(json)` | `v2->set_param(inst, "state", json)` |

For parameter metadata, the adapter calls `v2->get_param(inst, "chain_params", ...)` and `v2->get_param(inst, "ui_hierarchy", ...)`, parses the JSON, and registers with `param_system`.

The ducker's `dlsym("move_audio_fx_on_midi")` hack is also handled here — the adapter checks for the symbol and wires it to `on_midi` if found.

### What the Adapter Can't Do

- **audio_buffers_t routing** — v2 modules read mailbox directly. Still works, but they don't get clean audio routing.
- **Automatic state serialization** — v2 modules with custom `get_param("state")` keep their own serialization.
- **Full param_system validation** — adapter registers params from legacy JSON, but v2 modules can still accept any value.

### When to Delete

After all modules are ported to v3, delete `compat_v2.c`. No legacy code lives inside the new architecture permanently.

---

## Section 4: Build Plan

Every phase produces a deployable, working system. No dark period.

### Phase 0: Clear the Decks

Before any new architecture, remove dead weight from the existing codebase:

- Remove standalone chain/ui.js code (~430 lines of dead code)
- Remove unused color constants from constants.mjs (~154 exports)
- Resolve 2 stale TODOs
- Clean up SHADOW_HOTKEY_DEBUG dead code

Single PR. Everything still works after.

### Phase 0.5: Clean-Room Shim Using ablspi Types and Constants

**Goal:** Rewrite the LD_PRELOAD shim from scratch, without referencing any code from the bobbydigitales fork. The new shim uses Ableton's GPL-2.0 `ablspi` library as its authoritative reference for buffer layout, struct definitions, and protocol constants.

**Background:** The `ablspi` library (obtained via GPL request from the jack2 Move driver, `SPDX-License-Identifier: GPL-2.0-or-later`) documents the complete SPI protocol for Move hardware. While the library itself is designed for direct device ownership (exclusive `/dev/ablspi0.0` access), it provides everything needed to write a correct LD_PRELOAD shim: struct layouts, buffer offsets, ioctl commands, MIDI encoding, display protocol, and hardware limits.

**Why LD_PRELOAD (shadow mailbox coexistence):**
Schwung MUST coexist with stock Move. Shadow mode — where our shim intercepts MoveEngine's SPI transactions, reads Move's audio/MIDI, mixes in shadow slot audio, and writes back — is the core architecture. Users get stock Move pads, clips, sampling, AND Schwung synths/FX simultaneously. This is non-negotiable.

**What ablspi provides as reference (not called at runtime):**
- **Struct definitions** — `AblSpiMidiMessage`, `AblSpiUsbMidiMessage`, `AblSpiMidiEvent` (replaces our hand-rolled buffer parsing)
- **Buffer layout constants** — output MIDI at 0, output audio at 256, output display status at 80, output display data at 84, input data at 2048, input audio at 2304, frame size 768
- **Ioctl command enum** — `ABLSPI_FILL_TX_BUFFER` through `ABLSPI_GET_SPEED` (replaces our reverse-engineered ioctl numbers)
- **MIDI limits** — 20 out per transfer (`ABLSPI_MAX_MIDI_OUT_PER_TRANSFER`), 31 in per transfer (`ABLSPI_MAX_MIDI_IN_PER_TRANSFER`)
- **`encodeAsUsbMidi()`** — USB-MIDI CIN encoding (can be used directly in our shim)
- **Display protocol** — 6-chunk transfer, 172 bytes per chunk, double-buffered with dirty flag, status word handshake
- **Audio format** — 128 frames (`ABLSPI_AUDIO_BUFFER_SIZE`), 44100 Hz, int16 interleaved stereo
- **SPI frequency** — 20 MHz
- **Cable assignments** — cable 0 = internal hardware, cable 2 = external USB MIDI, cable 14 = system events, cable 15 = SPI protocol events, cables 2-10 all treated as external

**Architecture:** Same shadow mailbox technique as 1.x, but clean-room implementation:
1. LD_PRELOAD shim intercepts `open()` / `ioctl()` / `mmap()` on `/dev/ablspi0.0`
2. Shim gets a reference to MoveEngine's mmap'd SPI buffer
3. On each `ABLSPI_WAIT_AND_SEND_MESSAGE_WITH_SIZE` ioctl (the transfer trigger):
   - Read Move's audio output from the buffer, mix with shadow slot audio
   - Read MIDI IN (cable 0 for controls, cable 2+ for external), route to shadow slots
   - Write shadow MIDI OUT (LEDs, external MIDI) respecting the 20-message limit
   - Handle display (shadow UI when active, pass through Move's display otherwise)
4. Shadow audio rendered in a separate host process, exchanged via shared memory

**Overtake module handling in the shim:**
Overtake modules (Controller, M8, SID Control) take full control of Move's UI. The shim must:
- Suppress Move's display output (zero the display region in Move's outbound buffer)
- Route ALL MIDI IN to the overtake module — both cable 0 (internal hardware) and cables 2+ (external USB)
- Give the overtake module full MIDI OUT budget (all 20 slots per transfer for LEDs + external MIDI)
- Handle the progressive LED init sequence (overtake modules can't blast 64+ LED commands at once)
- Provide Shift+Vol+Jog Click as a host-level escape hatch (always intercepted by shim, never forwarded)
- On exit: progressively clear all LEDs, restore Move's display passthrough, return MIDI routing to shadow slots

**Tool module handling:**
Tool modules (File Browser, Song Mode, WAV Player) run within the shadow UI — they are NOT overtake. The shim treats them identically to shadow slot navigation. The `module_registry` and `menu_toolkit` domains handle tool lifecycle (loading, tick, input routing). Tools that declare `backgroundable: true` (Song Mode, WaveEdit) continue receiving audio/MIDI ticks when the user navigates away, managed by `module_registry` sessions.

**What changes from 1.x shim:**
- Written from scratch — no bobbydigitales code copied or referenced
- Uses `AblSpiUsbMidiMessage` structs instead of raw byte offset arithmetic
- Uses `AblSpiMidiEvent` for timestamped MIDI with proper CIN-based size detection
- Enforces `ABLSPI_MAX_MIDI_OUT_PER_TRANSFER = 20` limit explicitly (fixes LED overflow crashes)
- Uses `encodeAsUsbMidi()` for outbound MIDI encoding
- Uses named ioctl constants from the enum instead of magic numbers
- Handles EINTR on ioctl wait (as ablspi does)
- Properly handles cables 2-10 for multi-device external USB MIDI

**Steps:**
1. Vendor `ablspi.h` into `src/hal/` as a header-only reference (structs, constants, ioctl enum)
2. Optionally vendor `ablspi.c` for `encodeAsUsbMidi()` and interleave/deinterleave helpers
3. Write `src/hal/shim.c` from scratch — LD_PRELOAD hooks for open/ioctl/mmap using ablspi types
4. Write `src/hal/shadow_mailbox.c` — shadow audio mixing, MIDI routing, display multiplexing
5. Write `src/hal/hal.h` — abstract interface consumed by domains above
6. Prototype: shadow audio passthrough (Move plays, shadow slot adds a sine wave)
7. Prototype: MIDI routing (pad press reaches shadow slot)
8. Prototype: display switching (shift+vol shows shadow UI)
9. Prototype: LED control (shadow slot sets pad colors)

**Key constraint:** Zero reference to bobbydigitales's shim code. The shim is written from scratch using only:
- The `ablspi` library headers and source (GPL-2.0, from Ableton via jack2 Move driver)
- Standard Linux LD_PRELOAD / ioctl / mmap techniques (public knowledge)
- Our own domain knowledge of Move hardware behavior
- The CLAUDE.md documentation we've written ourselves

### Phase 1: Foundation Domains

Build the domains that everything else depends on. New code in the new repo, tested on Mac.

**1a. knob_engine** — the biggest pain point today.
- One acceleration curve (1x-4x, 50ms-250ms thresholds)
- One delta→value function, type-aware (float step, int step, enum ±1)
- C implementation (canonical) + JS bindings
- Unit tests on Mac

**1b. param_system** — enables everything after.
- Reads params.json
- Stores values, validates on set (rejects out-of-range)
- Change notification callbacks
- Blocking delivery mode
- Dynamic registration (for CLAP)
- Unit tests on Mac

**1c. clock_manager** — needed by arp, sampler, modulation.
- Internal BPM + external MIDI clock (24 PPQN)
- Transport state (play/stop/continue)
- Bar/beat/tick position
- Subscriber pattern
- Unit tests on Mac

**1d. input_manager** — replaces per-module MIDI parsing.
- Digests cable 0 raw MIDI → clean events
- Pad down/up with velocity, knob turn (decoded + accelerated via knob_engine), jog, buttons, shift state, transport
- Event subscription pattern
- Unit tests on Mac

### Phase 2: Module Contract + Adapter

**2a. Define module_api_v3** — the C API.
- `module_config_t` with audio format, paths
- `audio_buffers_t` for declared inputs
- `on_param_changed` instead of set_param/get_param
- Optional `get_state`/`set_state`

**2b. Build compat_v2.c** — the adapter.
- Wraps v2 plugins as v3
- Reads legacy chain_params/ui_hierarchy JSON
- Registers with param_system
- All 31 modules work unmodified

**2c. Port 3 pilot modules** — prove the contract works.
- Braids (simple synth, already uses param_helper)
- Ducker (simple FX, tests midi_in declaration)
- SF2 (tests banks + presets)
- Iterate on contract if anything doesn't fit

### Phase 3: Signal Chain + Audio Domains

**3a. signal_chain** — extracted from chain_host.c.
- midi_fx → synth → audio_fx pipeline
- MIDI routing to FX that declare midi_in
- Audio input routing from module declarations

**3b. audio_engine** — extracted from chain_host.c + shim.
- Buffer pool
- Per-slot mixing (volume, mute, solo from slot_manager)
- Master mix + master volume
- Format config (queryable)

**3c. audio_input** — extracted from shim.
- Circular capture buffer (shared by skipback + quantized sampler)
- Source selection (resample / hardware input / Move input)
- Quantized recording (clock-aligned via clock_manager)
- Skipback (dump last 30 seconds)
- Module audio feeds
- WAV writer (background thread)

**3d. slot_manager** — extracted from shadow_ui + shim.
- Per-slot: volume, mute, solo, receive/forward channel
- Signal chain assignment per slot
- Session lifecycle (active / backgrounded / unloaded)

### Phase 4: UI Layer

**4a. screen_manager** — owns the framebuffer.
- Dirty region tracking
- Layer compositing (module content + overlay)

**4b. led_manager** — consolidates LED handling.
- Batching (respect 60-packet buffer limit)
- Caching (don't resend unchanged LEDs)
- Progressive init for overtake modules
- Grid abstraction (pad index ↔ note number)

**4c. menu_toolkit** — unifies all menu rendering.
- Parameter editor (one implementation, replaces 4)
- Preset browser (one implementation, replaces 3)
- Knob overlay (one implementation, replaces 3)
- Renders from param_system + slot_manager data into screen_manager

**4d. screen_reader** — auto-announce from domains.
- Observes param_system, menu_toolkit, input_manager, module_registry, task_manager, slot_manager
- Zero module code needed

### Phase 5: Port All Modules

See Section 5 for detailed migration plan and per-module instructions.

### Phase 6: System Services + Cleanup

**6a. Host services** — module_store, settings, help (always-on, not modules).
**6b. System services** — logger, health_monitor, update_manager, feature_config.
**6c. Delete compat_v2.c** — all modules ported, adapter no longer needed.
**6d. Modulation** — LFOs, mod matrix (routes to param_system targets).
**6e. Build infrastructure** — shared Dockerfile, reusable GitHub Actions workflow, module template repo.

### Deployability at Each Phase

| After | State |
|---|---|
| Phase 0 | Same system, less dead code |
| Phase 0.5 | Clean-room shim using ablspi types, shadow mode coexistence preserved |
| Phase 1 | Same system, knobs feel consistent everywhere |
| Phase 2 | New framework running, 3 modules on v3 API, rest via adapter |
| Phase 3 | God objects broken up, all modules still work |
| Phase 4 | UI unified, one rendering path |
| Phase 5 | All modules on v3, adapter deleted |
| Phase 6 | Full 2.0 with system services |

---

## Section 5: Module Migration Plan

### Migration Steps: Sound Generator

Using Braids as the example.

**Before (v2):**
```c
// 60+ lines of strcmp chains
int get_param(void *inst, const char *key, char *buf, int len) {
    if (strcmp(key, "chain_params") == 0) {
        // 40-line hardcoded JSON string
    }
    if (strcmp(key, "ui_hierarchy") == 0) {
        // 30-line hardcoded JSON string
    }
    if (strcmp(key, "state") == 0) {
        // manually snprintf every param into JSON
    }
    if (strcmp(key, "timbre") == 0) {
        snprintf(buf, len, "%.4f", inst->timbre);
    }
    // ... repeat for every param
}

void set_param(void *inst, const char *key, const char *val) {
    if (strcmp(key, "timbre") == 0) {
        inst->timbre = atof(val);
    }
    if (strcmp(key, "state") == 0) {
        // manually parse JSON, extract every param
    }
    // ... repeat for every param
}
```

**After (v3):**
```c
void on_param_changed(void *inst, const char *key, const char *val) {
    braids_instance_t *b = (braids_instance_t *)inst;
    if (strcmp(key, "timbre") == 0)     b->timbre = atof(val);
    if (strcmp(key, "color") == 0)      b->color = atof(val);
    if (strcmp(key, "algorithm") == 0)  b->algorithm = atoi(val);
}
```

**Steps:**
1. Create `src/params.json` from hardcoded `chain_params` + `ui_hierarchy` strings
2. Delete `get_param()` entirely — framework handles it
3. Replace `set_param()` with `on_param_changed()` — keep only lines that update DSP state
4. Delete `get_param("state")` / `set_param("state")` — framework auto-serializes
5. Change `module.json`: `api_version` 2 → 3, add `"params": "params.json"`
6. Update entry point: `move_plugin_init_v2` → `move_plugin_init_v3`, accept `module_config_t`
7. Delete local copy of `plugin_api_v1.h`
8. Build. Test.

**Lines deleted:** ~100-150. **Lines added:** ~10. **Time:** ~30 minutes.

### Migration Steps: Audio FX with MIDI (Ducker)

Same as sound generator, plus:
- Add `"midi": { "in": true }` to module.json
- `on_midi` callback is now declared, not discovered via dlsym
- Delete the `move_audio_fx_on_midi` export wrapper

### Migration Steps: Complex Module (JV880)

JV880 has state beyond declared params (NVRAM hex dump, emulator state):
1. Create `params.json` for exposed params (octave_transpose, mode, performance, etc.)
2. Delete `get_param` strcmp chains for standard params
3. **Keep** `get_state`/`set_state` overrides for NVRAM data
4. Add `"assets": ["roms/rom1.bin", ...]` to module.json
5. `on_param_changed` handles mode switches, preset selection

**Time:** 2-4 hours (carefully separate framework-owned params from module-private state).

### Migration Steps: Dynamic Module (CLAP)

CLAP can't use static params.json:
1. `module.json`: `"params": "dynamic"`
2. After loading a CLAP plugin, call `param_system_register(inst, key, metadata)` for each parameter
3. After unloading, call `param_system_clear(inst)`
4. `on_param_changed` receives updates for registered params
5. Framework handles knob banking/paging automatically

### Migration Steps: Overtake Module (Controller, M8)

JS-only migration. Replace raw MIDI parsing with input_manager events:

**Before:**
```javascript
globalThis.onMidiMessageInternal = function(data) {
    if (isCapacitiveTouchMessage(data)) return;
    const status = data[0] & 0xF0;
    if (status === 0xB0) {
        const cc = data[1];
        if (cc === 49) shiftHeld = data[2] > 0;
        if (cc >= 71 && cc <= 78) {
            const delta = decodeDelta(data[2]);
            // manual knob handling...
        }
    }
    if (status === 0x90 && data[1] >= 68) {
        // manual pad handling...
    }
}
```

**After:**
```javascript
import { input } from '/shared/input_manager.mjs';

input.on('padDown', (padIndex, velocity) => { ... });
input.on('padUp', (padIndex) => { ... });
input.on('knobTurn', (knobIndex, delta) => { ... });
input.on('buttonDown', (button) => { ... });
input.on('shiftChanged', (held) => { ... });
```

### Migration Steps: Tool with Sessions (WaveEdit, Song Mode)

1. Add session config to module.json: `"session": { "backgroundable": true, "audio_while_backgrounded": true }`
2. Implement `on_background()` / `on_foreground()` callbacks
3. Implement `get_status()` for background indicator
4. Replace raw MIDI parsing with input_manager events

### Migration Checklist (All Modules)

```
□ Create params.json (from chain_params + ui_hierarchy)
□ Update module.json (api_version, type, audio, midi, params, presets, assets, session)
□ Delete get_param / set_param strcmp chains
□ Add on_param_changed callback
□ Delete manual state serialization (or keep as override for complex modules)
□ Delete local plugin_api_v1.h / audio_fx_api_v2.h copies
□ Replace host_api_v1_t with module_config_t
□ Use audio_buffers_t instead of raw mailbox offsets
□ Replace raw MIDI parsing with input_manager (JS modules)
□ Test on hardware
□ Update help.json if needed
```

### Migration Order

Risk-based: start with lowest risk / highest learning, end with most complex.

**Week 1 — Simple synths + simple FX (prove the pattern):**
- Braids, NuSaw, Moog (param_helper already, trivial ports)
- Ducker, Gate (simple FX, tests midi_in declaration)

**Week 2 — Remaining simple modules:**
- CloudSeed, PSXVerb, Tapescam, SpaceDelay, JunoChorus (simple FX, no MIDI)
- Chiptune, OB-Xd, Hera (simple synths)

**Week 3 — Moderate complexity:**
- SF2 (tests bank + preset two-level browsing)
- REX, SH-101, Bristol (moderate synths)
- Vocoder, NAM, KeyDetect (tests audio input declarations)

**Week 4 — High complexity:**
- Surge, DX7 (large param sets, MPE)
- JV880 (progressive loading, ROMs, NVRAM, custom state)
- Virus (complex emulator)
- CLAP (dynamic params, sub-plugin hosting)

**Week 5 — Overtake + tools (JS migration):**
- Controller, M8, SID Control (overtake modules)
- Performance FX (overtake + audio + sessions)
- Song Mode, WaveEdit (tools + sessions)

**Week 6 — Network + remaining:**
- RadioGarden, WebStream, AirPlay, PipeWire (network modules)
- Four Track, AutoSample, SampleRobot (remaining tools)

---

## Section 6: Project Structure

```
move-anything-2/
├── src/
│   ├── hal/                        Hardware Abstraction Layer (clean-room, no bobbydigitales code)
│   │   ├── hal.h                   Abstract interface (audio/MIDI/display callbacks)
│   │   ├── shim.c                  LD_PRELOAD hooks (open/ioctl/mmap interception)
│   │   ├── shadow_mailbox.c        Shadow audio mixing, MIDI routing, display mux
│   │   └── ablspi/                 Vendored GPL-2.0 library from Ableton (types + helpers)
│   │       ├── ablspi.h            Structs, constants, ioctl enum, MIDI limits
│   │       └── ablspi.c            encodeAsUsbMidi(), interleave/deinterleave helpers
│   │
│   ├── domains/                    The 24 domains
│   │   ├── midi_in/
│   │   ├── midi_out/
│   │   ├── led_manager/
│   │   ├── screen_manager/
│   │   ├── input_manager/
│   │   ├── knob_engine/
│   │   ├── audio_engine/
│   │   ├── audio_input/
│   │   ├── track_audio/
│   │   ├── signal_chain/
│   │   ├── clock_manager/
│   │   ├── modulation/
│   │   ├── midi_router/
│   │   ├── param_system/
│   │   ├── slot_manager/
│   │   ├── module_registry/
│   │   ├── patch_store/
│   │   └── task_manager/
│   │
│   ├── services/                   Host + system services
│   │   ├── module_store/
│   │   ├── settings/
│   │   ├── screen_reader/
│   │   ├── help/
│   │   ├── update_manager/
│   │   ├── logger/
│   │   ├── health_monitor/
│   │   └── feature_config/
│   │
│   ├── ui/                         JS UI layer
│   │   ├── menu_toolkit/
│   │   ├── shadow_ui.js            Thin orchestrator (~500 lines)
│   │   └── overlays/
│   │
│   ├── shared/                     JS utilities for modules
│   │   ├── input_manager.mjs
│   │   ├── knob_engine.mjs
│   │   ├── sound_generator_ui.mjs
│   │   └── ...
│   │
│   ├── api/                        Module contract
│   │   ├── module_api_v3.h
│   │   ├── audio_fx_api_v3.h
│   │   ├── midi_fx_api_v2.h
│   │   ├── module_config.h
│   │   └── audio_buffers.h
│   │
│   ├── compat/                     Compatibility adapter
│   │   └── compat_v2.c            Wraps v2 modules as v3
│   │
│   ├── modules/                    Built-in modules
│   │   ├── audio_fx/
│   │   │   └── freeverb/
│   │   └── midi_fx/
│   │       ├── arp/
│   │       └── chord/
│   │
│   └── host/                       Main runtime
│       └── main.c                  Bootstrap, wire domains together
│
├── tests/                          Unit tests (run on Mac, no hardware)
│   ├── test_knob_engine.c
│   ├── test_param_system.c
│   ├── test_clock_manager.c
│   ├── test_midi_router.c
│   ├── test_input_manager.c
│   ├── test_slot_manager.c
│   └── ...
│
├── docs/
│   ├── architecture.md
│   ├── module-guide.md
│   └── migration-guide.md
│
└── scripts/
    ├── build.sh
    ├── install.sh
    ├── test.sh
    └── new-module.sh               Scaffold a new module
```

---

## Section 7: Enforcement and Testing

Three layers ensure module developers follow the 2.0 paradigm: make the right thing easy, make the wrong thing hard, and gate releases on compliance.

### Make the Right Thing Easy: Module Scaffolding

```bash
./scripts/new-module.sh my-synth sound_generator
```

Generates a complete, buildable, test-passing module:

```
move-anything-my-synth/
├── src/
│   ├── module.json          ← pre-filled for type
│   ├── params.json          ← skeleton with one example group
│   ├── dsp/
│   │   └── plugin.c         ← v3 API skeleton with on_param_changed stub
│   └── ui.js                ← imports sound_generator_ui.mjs (if synth)
├── tests/
│   ├── test_params.c        ← auto-generated from params.json
│   ├── test_plugin.c        ← skeleton: create/destroy, param round-trip
│   └── run_tests.sh         ← runs on Mac, no hardware needed
├── scripts/
│   ├── build.sh             ← shared template, sources common build lib
│   ├── install.sh
│   └── Dockerfile
├── .github/
│   └── workflows/
│       ├── test.yml          ← runs on every push
│       └── release.yml       ← runs on tag, gates on tests passing
└── CLAUDE.md
```

The path of least resistance is compliance. A scaffolded module builds, passes tests, and has correct structure from minute one.

### Auto-Generated Tests from params.json

Since params.json declares all parameters with types, ranges, and defaults, tests are generated automatically at build time. The developer writes zero test code for basic compliance.

**Auto-generated test: parameter round-trip**
```c
void test_param_round_trip() {
    void *inst = create_instance(&test_config);

    // For each param in params.json:
    set_and_verify(inst, "timbre", "0.5", "float in range");
    set_and_verify(inst, "timbre", "0.0", "float at min");
    set_and_verify(inst, "timbre", "1.0", "float at max");
    set_and_verify(inst, "algorithm", "CSAW", "valid enum");

    destroy_instance(inst);
}
```

**Auto-generated test: bounds checking**
```c
void test_param_bounds() {
    void *inst = create_instance(&test_config);

    // timbre: min=0, max=1
    set_param(inst, "timbre", "1.5");
    float val = get_param_float(inst, "timbre");
    ASSERT(val <= 1.0, "timbre should clamp to max");

    set_param(inst, "timbre", "-0.5");
    val = get_param_float(inst, "timbre");
    ASSERT(val >= 0.0, "timbre should clamp to min");

    destroy_instance(inst);
}
```

**Auto-generated test: state serialization round-trip**
```c
void test_state_round_trip() {
    void *inst = create_instance(&test_config);

    // Set every param to a non-default value
    set_param(inst, "timbre", "0.73");
    set_param(inst, "color", "0.42");
    set_param(inst, "algorithm", "Morph");

    // Serialize
    char state[4096];
    get_state(inst, state, sizeof(state));

    // Create fresh instance, restore
    void *inst2 = create_instance(&test_config);
    set_state(inst2, state);

    // Verify all params match
    ASSERT_PARAM_EQ(inst2, "timbre", "0.73");
    ASSERT_PARAM_EQ(inst2, "color", "0.42");
    ASSERT_PARAM_EQ(inst2, "algorithm", "Morph");

    destroy_instance(inst);
    destroy_instance(inst2);
}
```

**Auto-generated test: lifecycle**
```c
void test_lifecycle() {
    for (int i = 0; i < 10; i++) {
        void *inst = create_instance(&test_config);
        ASSERT(inst != NULL, "create should succeed");
        destroy_instance(inst);
    }
}
```

All auto-generated tests run on Mac — no hardware, no audio, no MIDI. Developers can add custom tests beyond the auto-generated ones.

### Make the Wrong Thing Hard: Framework Validation

The framework validates modules at load time:

```c
module_error_t err = validate_module(module_dir);

switch (err) {
    case MODULE_OK:                    break;
    case MODULE_MISSING_PARAMS_JSON:   log("no params.json"); refuse_load();
    case MODULE_INVALID_PARAMS:        log("params.json schema error"); refuse_load();
    case MODULE_MISSING_ENTRY_POINT:   log("no move_plugin_init_v3"); refuse_load();
    case MODULE_MISSING_ASSETS:        log("required ROM files missing"); refuse_load();
    case MODULE_API_VERSION_UNKNOWN:   log("unknown api_version"); refuse_load();
}
```

If it doesn't have params.json, it doesn't load. No exceptions (except via compat_v2 adapter during migration).

### params.json Schema Validation

A JSON schema that the build system and framework both validate against:

- `float` params must declare `min`, `max`, `step`
- `enum` params must declare `options` array
- `knob` values must be 1-8
- `key` must be unique across all groups
- `groups` must have `id` and `label`

Schema errors caught at build time, not at runtime on hardware.

### Build Script Validation

The shared build template runs validation before creating the tarball:

```bash
validate_module() {
    echo "=== Validating module ==="

    # params.json exists and is valid
    validate_params_json src/params.json || exit 1

    # module.json has required fields
    validate_module_json src/module.json || exit 1

    # v3 entry point exists in compiled .so
    check_symbol dist/$MODULE_ID/dsp.so "move_plugin_init_v3" || \
    check_symbol dist/$MODULE_ID/dsp.so "move_audio_fx_init_v3" || \
    check_symbol dist/$MODULE_ID/dsp.so "move_midi_fx_init_v2" || \
        { echo "ERROR: No v3 entry point found"; exit 1; }

    # Auto-generated tests pass
    run_tests || exit 1

    echo "=== Validation passed ==="
}
```

If tests fail, the tarball isn't created. No way to release a broken module.

### Gate Releases: CI Pipeline

```yaml
# .github/workflows/test.yml — runs on every push
name: Test
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Validate module.json
        run: ./scripts/validate-module-json.sh
      - name: Validate params.json
        run: ./scripts/validate-params-json.sh
      - name: Build for test (x86)
        run: ./scripts/build.sh --test
      - name: Run auto-generated tests
        run: ./tests/run_tests.sh
      - name: Run custom tests
        run: ./tests/run_custom_tests.sh
```

```yaml
# .github/workflows/release.yml — runs on tag push
name: Release
on:
  push:
    tags: ['v*']
jobs:
  release:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Run all tests
        run: ./scripts/build.sh --test && ./tests/run_tests.sh
      - name: Build for ARM
        run: ./scripts/build.sh
      - name: Validate tarball contents
        run: ./scripts/validate-tarball.sh
      - name: Create release
        uses: softprops/action-gh-release@v1
        with:
          files: dist/*-module.tar.gz
```

Tests gate the release. Tag push → tests run → if pass, release created. If fail, no release.

### Module Store Validation

The module store checks before listing:

- `release.json` exists on main branch
- `module.json` has `api_version: 3`
- `params.json` exists in tarball
- CI badge is green (optional but visible)
- `min_host_version` is compatible

Modules that don't meet the bar don't show up in the store.

### What Gets Tested Without Hardware

| Test | What it validates | Runs on |
|---|---|---|
| Schema validation | params.json and module.json are well-formed | Mac/CI |
| Param round-trip | Every declared param can be set and read back | Mac/CI |
| Param bounds | Out-of-range values are clamped or rejected | Mac/CI |
| State round-trip | Serialize → restore → all params match | Mac/CI |
| Lifecycle | create/destroy 10x without crash or leak | Mac/CI |
| Enum values | Every enum option is accepted | Mac/CI |
| Default values | Fresh instance has correct defaults | Mac/CI |
| Entry point | Correct v3 symbol exported from .so | Mac/CI |
| Audio smoke | render_block produces non-silence for basic input | Mac/CI |
| MIDI smoke | on_midi with note-on doesn't crash | Mac/CI |

### What Still Needs Hardware Testing

- Actual audio quality (does it sound right?)
- MIDI timing and latency
- Display rendering
- LED behavior
- Real-world preset browsing feel
- Integration with other slots

---

## Appendix A: Pressure Test Results

The domain map was validated against these modules:

| Module | Type | What it tested | Gaps found |
|---|---|---|---|
| Braids | Sound gen | Standard params + presets | None (clean fit) |
| CloudSeed | Audio FX | Standard audio processing | None (clean fit) |
| Surge | Sound gen | 300+ params, MPE | `midi_in` needs MPE detection |
| JV880 | Sound gen | ROMs, progressive loading, NVRAM | Asset management, progressive loading, module-private storage |
| Performance FX | Overtake + FX | Capture buffer, pad grid, blocking params, per-track audio | Blocking param delivery, track_audio, module capture buffers |
| Controller | Overtake | MIDI out, external device, 16 banks | Internal MIDI loopback for LED feedback |
| Arpeggiator | MIDI FX | Clock sync, tempo, transport | clock_manager domain needed |
| Vocoder | Audio FX | Dual audio input (chain + line-in) | Audio input declarations needed |
| CLAP | Audio FX + Gen | Dynamic params, sub-plugin hosting, 498 plugins | Dynamic param registration, parameter banking |
| Ducker | Audio FX | MIDI-triggered audio effect | Audio FX midi_in declaration (replaces dlsym hack) |
| M8 | Overtake | External USB device protocol, handshake | Device connection state management |

## Appendix B: What Gets Extracted vs Rewritten vs Deleted

### Rewritten from scratch using ablspi as reference (clean-room, no bobbydigitales code)

The LD_PRELOAD shim is rewritten from scratch. Ableton's GPL-2.0 `ablspi` library (obtained via GPL request from the jack2 Move driver) provides the authoritative struct definitions, buffer layout constants, ioctl commands, and MIDI encoding. No code from the bobbydigitales fork is referenced or used.

- Shim ioctl interception → rewritten using ablspi's ioctl enum (`ABLSPI_WAIT_AND_SEND_MESSAGE_WITH_SIZE` etc.)
- Audio mailbox layout → rewritten using ablspi's offset constants and interleave/deinterleave helpers
- MIDI buffer format → rewritten using `AblSpiUsbMidiMessage` structs, `encodeAsUsbMidi()`, CIN-based size detection
- SPI device communication → same LD_PRELOAD technique, but with documented constants instead of magic numbers
- Setuid/capabilities handling → still required (LD_PRELOAD into MoveEngine), but cleanly implemented
- LED packet limit → explicitly enforced via `ABLSPI_MAX_MIDI_OUT_PER_TRANSFER = 20`
- Display protocol → rewritten using ablspi's 6-chunk / 172-byte / double-buffer protocol
- MIDI limits → `ABLSPI_MAX_MIDI_IN_PER_TRANSFER = 31` enforced on input path

### Extracted (brought to new repo)

- All external DSP engines (third-party code: Surge, Dexed, JV880 emulator, etc.)
- Domain knowledge: cable routing rules (0 = internal, 2-10 = external, 14 = system, 15 = SPI protocol), pad/knob note/CC mappings, LED color protocol

### Rewritten (new, clean implementations)

- Parameter system (replaces strcmp chains across 31 modules)
- Knob engine (replaces 5 independent implementations)
- Signal chain orchestration (replaces chain_host.c monolith)
- Shadow UI (replaces 10,969-line god object with ~500-line thin orchestrator + menu_toolkit)
- Menu/UI rendering (replaces 4 independent parameter editors, 3 preset browsers)
- State serialization (automatic from params.json, replaces per-module JSON construction)
- Module loading (replaces 3 independent implementations)
- Patch management (replaces 3 independent implementations)
- MIDI dispatch (replaces per-module raw parsing)
- Clock/transport (new domain, replaces scattered BPM/clock handling)
- Audio input/capture (replaces scattered capture buffer management)
- Screen reader (observer pattern, replaces manual announce() calls)

### Deleted (dead code and duplication)

- Standalone chain/ui.js code (~430 lines)
- 154 unused color constant exports
- 5 redundant knob acceleration implementations
- 3 redundant patch management implementations
- 3 redundant module loading implementations
- 4 redundant parameter editing UIs
- 31 modules × ~100 lines of strcmp boilerplate = ~3,100 lines
- 14 local copies of plugin_api_v1.h
- All hardcoded chain_params / ui_hierarchy JSON strings in C
- compat_v2.c (after all modules ported)
