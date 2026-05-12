# Organelle Port — Design

**Date**: 2026-05-12
**Status**: Design

## Goal

Run Critter & Guitari Organelle Pure Data patches on Move as a Schwung shadow-slot sound generator. New external module `move-anything-organelle` embeds `libpd`, translates the Organelle screen/knob/encoder conventions to Move controls, and loads patches from a shared on-device folder.

## Scope

**In scope (v1)**:
- Sound-generator chainable module loaded into a Move shadow slot.
- libpd multi-instance DSP plugin (Plugin API v2).
- Vanilla Pd objects only — no externals. Many "Mother" and patchstorage community patches will work; externals-using patches fail to load with a clear error.
- Bundled starter patch set + user-droppable folder.
- Full MIDI bridge (in/out, including external USB cable-2).
- Audio in (`[adc~]`) routed from the chain mixer.
- Per-slot state persistence (patch + slot params).
- Tempo injection via `[r clock]`.

**Out of scope (deferred)**:
- A separate overtake module accessed via Tools menu (the slot UI already provides the full-screen patch experience).
- Externals: `freeverb~`, `else`, `cyclone`, `mi/`, etc. Architecture leaves room to add them.
- ORAC. Separate, much larger project.
- An in-device patchstorage.com downloader.

## Control mappings

| Organelle | Move |
|---|---|
| Knob 1–4 (0–1023) | Knob 1–4 (CC 71–74), scaled ×8.05 |
| Volume knob (`r volume`) | Slot volume (mirrored read-only into Pd) |
| Encoder turn | Jog wheel (CC 14) |
| Encoder click | Jog click (CC 3) |
| Aux button (`r auxKey`) | Knob 7 capacitive touch |
| Foot switch (`r fs`) | Knob 8 capacitive touch |
| 25-key keybed | Slot's incoming MIDI notes (passthrough in synth-module mode; per-slot Octave param for nudging) |
| MIDI in/out | Full bridge — slot MIDI (incl. external USB cable-2) → Pd `[notein]/[ctlin]/[bendin]`; Pd `[noteout]/[ctlout]` → slot MIDI out |
| 128×64 OLED | Move's 128×64 framebuffer (direct, same dimensions) |

Aux and foot-switch sources are configurable per slot (default Knob7/Knob8 touch, can be set to Off).

## Architecture

### Repo layout

```
move-anything-organelle/
  src/
    dsp/
      pd_host.cpp           # libpd wrapper, instance API v2
      screen_translator.cpp # parses [s oled] msgs → ring buffer
      patch_loader.cpp      # patch folder discovery
    module/
      module.json           # component_type: sound_generator
      ui_chain.js           # slot UI (patch browser + screen passthrough)
  libs/
    libpd/                  # vendored submodule, multi-instance build
  patches/                  # bundled starter set (~6–10 patches)
  scripts/
    Dockerfile
    build.sh
    install.sh
  .github/workflows/release.yml
```

Installs to `modules/sound_generators/organelle/` on device. Shared on-device patch store: `/data/UserData/schwung/organelle-patches/`.

### DSP plugin (Plugin API v2)

- `pdinstance_new()` per instance; `libpd_set_instance()` guard before every libpd call.
- `libpd_init_audio(2, 2, 44100)` — stereo in (`adc~`) and out.
- `libpd_openfile("main.pd", patch_dir)` on `set_param("patch_path", ...)`. Patch loading happens on the calling thread, not the audio thread.
- `render_block(out_lr, 128)` converts int16 line-in → float, calls `libpd_process_short(2 ticks, in, out)`, converts back. If `audio_in_enable=Off` or no upstream audio, feeds zeros.
- `on_midi` dispatches to `libpd_noteon/_cc/_pgm/_bend` by status nibble. Channel from low nibble. Cable-2 routes the same way.

#### Parameter dispatch (`set_param`)

| Key | Pd target | Notes |
|---|---|---|
| `knob1..4` | `libpd_float("knobN", val × 1023/127)` | Live knob values |
| `volume` | `libpd_float("volume", slot_vol × 1023)` | Mirrored from slot volume |
| `aux` | `libpd_float("auxKey", 0\|1)` | From knob 7 touch |
| `fs` | `libpd_float("fs", 0\|1)` | From knob 8 touch |
| `encoderInput` | `libpd_float("encoderInput", ±1)` | Jog turn delta |
| `encoderButton` | `libpd_float("encoderButton", 0\|1)` | Jog click |
| `patch_path` | unload current, `libpd_openfile`, `libpd_bang("loadbang")` | |
| `state` | bulk restore JSON blob | Slot save/restore |
| `chain_tempo` | `libpd_float("clock", bpm)` | Per-tick if changed |

#### Screen pipeline

- `libpd_bind("oled")` + `libpd_set_message_hook(...)` on the C side.
- Each `[s oled <cmd> <args…>]` parsed into a struct, pushed onto a lock-free SPSC ring (~1KB).
- JS pulls via `get_param("screen_ops")` once per tick — returns up to N parsed ops as JSON.
- No realtime allocation, no JSON parsing in the audio thread (JSON is serialized on the JS-thread side of the ring on read).
- On overflow, drop oldest ops; patches re-emit on next frame.

Supported `[s oled]` commands (Organelle convention):
- `gFillArea x y w h c`
- `gLine x1 y1 x2 y2 c`
- `gBox x y w h c`
- `gInvertArea x y w h`
- `gSetPixel x y c`
- `gPrintln x y size c text…` (sizes 8 and 16)
- `gFlip` (end-of-frame, triggers display flush)

### JS UI shim (`ui_chain.js`)

Two display states, toggled by whether `current_patch_path` is set:

**State A: Patch browser** (no patch loaded)
- Scans `/data/UserData/schwung/organelle-patches/` for folders with `main.pd`.
- Standard `menu_layout.mjs` list.
- Jog turn → move selection. Jog click → `set_param("patch_path", folder)`, enter State B.
- Back → exit slot editor.
- Footer shows patch count and selected patch's category/author from `metadata.json` if present.

**State B: Patch running**
- Each `tick()`: pull `screen_ops` JSON, execute against Move's draw API (`rect`, `line`, `pixel`, `invertArea`, `print`).
- `gFlip` triggers `host_flush_display()`.
- Back button → unload patch, return to State A.

**Input routing (both states)**:
- `onMidiMessageInternal`: filter via `shouldFilterMessage`.
  - Knob CCs 71–74 → `set_param("knob1..4")`.
  - Knob touches note 7 → `set_param("aux", on/off)`. Note 8 → `set_param("fs", on/off)`.
  - Jog (CC 14, CC 3) → State A: browser nav. State B: `set_param("encoderInput"/"encoderButton")`.
- Pad/note MIDI: not re-dispatched from JS. It already arrives in the chain mixer → `on_midi` in C.
- External USB MIDI: same path; full bridge per design.

### Slot params and persistence

`chain_params` exposed:

```json
[
  {"key":"patch_path","name":"Patch","type":"enum","options":["<dynamic>"]},
  {"key":"octave_transpose","name":"Octave","type":"int","min":-4,"max":4,"default":0},
  {"key":"audio_in_enable","name":"Line In to Pd","type":"enum","options":["Off","On"],"default":"On"},
  {"key":"gain","name":"Output Gain","type":"float","min":0,"max":1.5,"step":0.01,"default":1.0},
  {"key":"aux_source","name":"Aux Button","type":"enum","options":["Knob7 Touch","Off"],"default":"Knob7 Touch"},
  {"key":"fs_source","name":"Foot Switch","type":"enum","options":["Knob8 Touch","Off"],"default":"Knob8 Touch"}
]
```

`ui_hierarchy` is minimal — slot UI is owned by `ui_chain.js`; the hierarchy just lists the params above for the slot-settings menu. Knobs 1–4 are live patch knobs, not slot-settings entries.

**State persistence**: `get_param("state")` returns `{patch_path, octave, audio_in_enable, gain, aux_source, fs_source}`. `set_param("state", json)` restores them (triggers patch load). Pd patches don't have own persistent state — known Organelle limitation, accepted.

**Patch metadata sidecar** (optional, per patch folder): `metadata.json` with `{name, author, description, category, knob_labels}`. Used by the patch-browser footer. Absent → fall back to folder name and `info.txt` if present.

**Audio-in declaration**: `module.json` sets `capabilities.audio_in: true`. Triggers Schwung's feedback-protection gate on load (speaker active + no line-in cable → warning modal).

## Build & distribution

- Docker-based ARM64 cross-compile, matching every other Schwung module.
- libpd built with `-DPD_MULTI=ON -DPD_UTILS=OFF -DPD_EXTRA=ON`. Statically linked into `dsp.so`. ~1.5–2 MB stripped.
- Patch bundle copied to `dist/organelle/patches/`. On install, copied to `/data/UserData/schwung/organelle-patches/` only if that folder is empty (preserves user patches across re-installs).
- Starter set: C&G Mother stock patches that are vanilla Pd + a few vanilla patchstorage favorites. Target <2 MB total.
- Standard Schwung release flow: bump `module.json` version, tag `v0.1.0`, GitHub Actions builds tarball and updates `release.json`. Add to `schwung/module-catalog.json` with `requires: "Bundled patches included; drop more into /data/UserData/schwung/organelle-patches/ via SFTP"`.

## Risks & open questions

1. **CPU budget**. Vanilla patches should be fine on Move's ARM (similar ballpark to Organelle-M's CM3+), but realtime budget is tighter (~900 µs/frame after SPI transfer). Mitigation: start with a light starter set; flag heavy patches in `metadata.json`.

2. **libpd realtime cleanliness**. libpd allocates on patch open/close, not in the process loop. Patches using `[delread~]` with dynamic delay times can allocate. Mitigation: load patches outside the audio thread (on `set_param("patch_path")`, before `render_block` sees them).

3. **Externals deferred but inevitable**. The moment a popular patch wants `[freeverb~]` or `else/`, we'll need to add externals support. Add the build hooks and install dirs now (`libs/externals/`, install to `<patch_dir>/extra/` or shared `/data/UserData/schwung/organelle-externals/`) so adding one later is mechanical.

4. **Foot-switch ergonomics unproven**. Knob 8 capacitive touch as foot switch sounds clean but may feel weird (accidental touches, latency). Validate with a test patch before committing.

5. **Screen op rate**. Some patches redraw every frame. 1 KB ring might overflow under bursts. Drop-oldest fallback is documented; measure on real patches.

6. **External lib license**. libpd is BSD; safe. Patches we bundle — confirm each is GPL/CC/permissive before shipping. C&G's Mother patches license needs checking (likely OK, they encourage redistribution).

## v2 candidates (not v1)

- Separate `organelle-launcher` overtake module (Tools-menu entry) sharing the same DSP and patch store.
- In-device patchstorage.com browser.
- Externals: `freeverb~`, `else`, `cyclone`, `mi/` (Mutable in Pd).
- Knob-label display in slot settings menu (pull from patch's `[s knob1Label]` messages).
- ORAC (its own design effort).
