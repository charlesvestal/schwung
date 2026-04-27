# Midiverb Module — Design

Date: 2026-04-27
Status: Design (pre-implementation)
Repo: `schwung-midiverb` (new, sibling to `schwung-mverb`)
Module ID: `midiverb`
Component type: `audio_fx`
License: GPL-3.0 (matches upstream)

## Overview

A Schwung audio-FX module that emulates the Alesis **Midiverb**, **Midifex**,
and **Midiverb II** rack reverbs (early-80s 16-bit DASP DSP). Based on
[thement/midiverb_emulator](https://github.com/thement/midiverb_emulator).

Two paths exist upstream:

- **Path A — decompiled** (ROM-free): pre-decompiled C effect functions,
  bundled with the upstream repo, all three units. Caveats vs. hardware:
  no flanger LFO triggers, no input/output filter modeling.
- **Path B — ROM interpreter** (cycle-accurate): runs original effect
  ROMs through the upstream `dasp16` µ-code interpreter. Requires the
  user to supply ROM dumps.

**v1 ships Path A only.** Path B is a planned v0.2 follow-on, additive
(no breaking changes to the param surface).

## Repo layout

```
schwung-midiverb/
  src/
    module.json              # id=midiverb, component_type=audio_fx, chainable=true
    ui.js                    # Shadow UI / chain UI
    ui_chain.js              # Chain UI shim
    dsp/
      plugin.cpp             # plugin_api_v2 wrapper
      midiverb_core.c        # Glue around upstream decompiled-*.h + dasp16.h
      decompiled-midiverb.h  # Vendored from upstream (GPL-3)
      decompiled-midifex.h
      decompiled-midiverb2.h
      names-midiverb.h
      names-midifex.h
      names-midiverb2.h
      dasp16.h
      lfo.h
      utils.h
      resampler.h            # 44.1 ↔ 23.4 kHz polyphase
      THIRD_PARTY_LICENSES   # Upstream GPL-3 notice
  scripts/
    build.sh                 # Cross-compile via Docker (matches sibling modules)
    install.sh
    Dockerfile
    gen_resampler.py         # Generates polyphase coefficient tables
  release.json
  README.md
  CLAUDE.md
  LICENSE                    # GPL-3
  .github/workflows/release.yml
```

Upstream sources are **vendored** (not a git submodule) so we can carry
small modifications: stripping the WAV CLI, removing libsndfile, and
inlining the decompiled tables behind a unit/program dispatch table.

## DSP plugin

Uses **plugin_api_v2** (multi-instance — required for chain integration).

### Per-instance state (`midiverb_instance_t`)

- `unit` — enum `{MIDIVERB, MIDIFEX, MIDIVERB2}`, default `MIDIVERB`
- `program` — `0..62` (Midiverb / Midifex) or `0..98` (Midiverb II)
- `mix` — float `0..1`, default `0.35`
- `feedback` — float `0..0.95`, default `0.3` (upstream `-f`)
- `input_gain` — float `0..2`, default `1.0`
- `output_gain` — float `0..2`, default `1.0`
- `predelay_ms` — float `0..200`
- `low_cut_hz` — float `20..1000` (input HPF, one-pole)
- `high_cut_hz` — float `1000..20000` (wet-path LPF, one-pole)
- `width` — float `0..1.5` (wet stereo M/S width)
- `dasp16_state_t dsp` — interpreter scratch state
- `effect_fn` — function pointer set on unit/program change
- Resampler state in/out (see SRC section)
- `pending_unit`, `pending_program` — written by `set_param` (non-RT),
  picked up at start of `render_block` so DSP state is never reinitialized
  mid-block

### Effect dispatch

Per-unit table of `void (*)(dasp16_state_t*, int16_t in_l, int16_t in_r,
int16_t *out_l, int16_t *out_r)` from the decompiled headers. Unit/program
selection swaps the pointer and `memset`s `dasp16_state_t` (cheap).

### Realtime safety

- No allocations, no file I/O, no logs in `render_block`
- Unit/program change = pointer swap + struct memset on the audio thread
- All upstream code is plain C arithmetic on stack/struct buffers

### `chain_params` (returned via `get_param`)

```json
[
  {"key":"unit","name":"Unit","type":"enum","options":["Midiverb","Midifex","Midiverb II"]},
  {"key":"program","name":"Program","type":"int","min":0,"max":62},
  {"key":"mix","name":"Mix","type":"float","min":0,"max":1,"step":0.01},
  {"key":"feedback","name":"Feedback","type":"float","min":0,"max":0.95,"step":0.01},
  {"key":"input_gain","name":"Input Gain","type":"float","min":0,"max":2,"step":0.01,"unit":"x"},
  {"key":"output_gain","name":"Output Gain","type":"float","min":0,"max":2,"step":0.01,"unit":"x"},
  {"key":"predelay_ms","name":"Pre-delay","type":"float","min":0,"max":200,"step":1,"unit":"ms"},
  {"key":"low_cut_hz","name":"Low Cut","type":"float","min":20,"max":1000,"step":1,"unit":"Hz"},
  {"key":"high_cut_hz","name":"High Cut","type":"float","min":1000,"max":20000,"step":50,"unit":"Hz"},
  {"key":"width","name":"Width","type":"float","min":0,"max":1.5,"step":0.01}
]
```

`program`'s `max` is reported as `62` for Midiverb / Midifex and `98` for
Midiverb II; the UI re-reads `chain_params` after a unit switch.

## Sample-rate conversion

44100 / 23400 = 1.88461… — irrational, no integer ratio. We use a
self-contained polyphase windowed-sinc FIR resampler (no libsamplerate
dependency).

```
Move 44100 Hz, 128 frames in
        │
        ▼
[downsampler 44.1→23.4]   ratio 0.5306, ~68 samples per 128 in
        │
        ▼
[Midiverb DSP loop]       process N samples through effect_fn
        │
        ▼
[upsampler 23.4→44.1]     ratio 1.8846, ~128 samples out
        │
        ▼
Move 44100 Hz, 128 frames out
```

### Resampler design

- 32-tap polyphase windowed-sinc FIR, Kaiser window (β≈8.6, ~80 dB
  stopband attenuation)
- Coefficient tables pre-computed per direction (~4 KB total, generated
  by `scripts/gen_resampler.py`, committed as `static const` in
  `resampler.h`)
- Fractional position via 64-bit fixed-point phase accumulator (no
  `double` math in audio path)
- Per-instance state: input ring buffer (≥ filter length + 1 block),
  output buffer, phase accumulator. Sized so worst-case "produce 1 extra
  sample this block" needs no conditionals in the inner loop
- Block alignment is not required at 23.4 kHz — downsampler emits
  whatever it produces (~60–69 samples), DSP processes that many,
  upsampler emits exactly 128 samples back, phase carries across blocks
- Latency: ~16 samples each direction at 23.4 kHz → ~1.4 ms total

### CPU budget

~32 muladds × ~200 samples × 2 channels per block ≈ 26k ops per block on
the resamplers. DASP DSP itself is delay-line work (a few muladds per
sample). Total well within Move's audio-thread budget.

## UI

Osirus pattern: jog wheel scrolls **program** at root (the main
browsable thing). Unit selection lives one level deeper.

### `ui_hierarchy` (returned via `get_param`)

```json
{
  "modes": null,
  "levels": {
    "root": {
      "label": "Midiverb",
      "list_param": "program",
      "count_param": "program_count",
      "name_param": "program_name",
      "knobs": [
        "mix", "feedback", "input_gain", "output_gain",
        "predelay_ms", "low_cut_hz", "high_cut_hz", "width"
      ],
      "params": [
        {"key": "mix", "label": "Mix"},
        {"key": "feedback", "label": "Feedback"},
        {"key": "input_gain", "label": "Input Gain"},
        {"key": "output_gain", "label": "Output Gain"},
        {"key": "predelay_ms", "label": "Pre-delay"},
        {"key": "low_cut_hz", "label": "Low Cut"},
        {"key": "high_cut_hz", "label": "High Cut"},
        {"key": "width", "label": "Width"},
        {"level": "unit", "label": "Unit"}
      ]
    },
    "unit": {
      "label": "Unit",
      "items_param": "unit_list",
      "select_param": "unit",
      "knobs": [],
      "params": []
    }
  }
}
```

### Supporting `get_param` responses

- `program_count` → `"63"` or `"99"` per current unit
- `program_name` → e.g. `"Large Hall"` (from `names-*.h`)
- `unit_list` → `["Midiverb","Midifex","Midiverb II"]`
- Each editable param returns a formatted display string (e.g. `mix` →
  `"35%"`, `predelay_ms` → `"45 ms"`)

### Knob layout (root, knobs 1–8)

1. Mix
2. Feedback
3. Input Gain
4. Output Gain
5. Pre-delay
6. Low Cut
7. High Cut
8. Width

Jog wheel = program scroll (Osirus-style).

### Unit switch behavior

When the user picks a different unit in the sub-menu:
1. DSP clamps `program` to the new unit's max
2. DSP `memset`s `dasp16_state_t` (clean tail)
3. UI re-reads `chain_params` so the program range updates (62 vs 98)

### Chain UI

`ui_chain.js` is the standard Signal Chain UI shim. The generic chain
renderer handles the hierarchy — no bespoke full-screen UI.

## Build, release, catalog

### Build

`scripts/build.sh` cross-compiles via Docker (matches `schwung-mverb`,
`move-anything-cloudseed` patterns):

- Compile `dsp/*.c` + vendored upstream headers into `dsp.so` with
  `-O3 -ffast-math -fPIC`
- No external libs (upstream WAV CLI / libsndfile stripped)
- Package `dist/midiverb/{module.json, ui.js, ui_chain.js, dsp.so,
  THIRD_PARTY_LICENSES}` → `dist/midiverb-module.tar.gz`

### Release workflow

`.github/workflows/release.yml` — standard tag-triggered workflow,
auto-updates `release.json` with new version + tarball URL on tag push.

### Catalog entry

Added to `module-catalog.json` in the main `schwung` repo on release:

```json
{
  "id": "midiverb",
  "name": "Midiverb",
  "description": "Alesis Midiverb / Midifex / Midiverb II emulator (lo-fi 23.4 kHz reverb)",
  "author": "thement (DSP), Schwung port",
  "component_type": "audio_fx",
  "github_repo": "charlesvestal/schwung-midiverb",
  "default_branch": "main",
  "asset_name": "midiverb-module.tar.gz",
  "min_host_version": "<current host version at v1 release>"
}
```

No `requires` field — Path A ships fully self-contained.

## Path B (ROM mode) — v0.2 follow-on

Additive, no breaking changes to v1 param surface:

- New param `rom_mode` enum `["Decompiled","ROM"]`, default `"Decompiled"`
- ROM mode: DSP looks for ROMs at
  `/data/UserData/schwung/midiverb-roms/{midiverb,midifex,midiverb2}.rom`
  and runs the cycle-accurate `dasp16` interpreter on those instead of
  the decompiled C
- If ROMs missing in ROM mode: silently fall back to decompiled, surface
  a `rom_status` string param the UI shows
- Adds LFO triggers + input/output filter modeling that decompiled mode
  lacks
- Ship `docs/ROM-INSTALLATION.md` with expected MD5s and a one-line
  `scp` instruction (mirrors the JV-880 ROM-install pattern)
- Catalog entry adds
  `"requires": "Optional: original Midiverb/Midifex/Midiverb II ROM dumps for ROM mode"`

## Open items (non-blocking)

- Decide jog-click behavior at root (no-op for v1, possibly "favorites
  toggle" later)
- Confirm exact `min_host_version` at v1 release time
- Profile CPU on-device after first build to confirm headroom under
  Link Audio + master FX load
