# OB-Xd Module Design

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Port OB-Xd virtual analog synthesizer as a separate GPL-licensed module for Move Anything.

**Architecture:** Extract OB-Xd Engine (C++ DSP core), wrap with plugin_api_v1, distribute as standalone repo.

**Tech Stack:** C++14, ARM64 cross-compilation, GitHub Actions for releases.

---

## Repository Structure

```
github.com/charlesvestal/move-anything-obxd/
├── LICENSE                     # GPL-3.0
├── README.md
├── src/
│   ├── module.json             # Move Anything module metadata
│   ├── ui.js                   # JS UI (display, knob handling, bank switching)
│   └── dsp/
│       ├── obxd_plugin.cpp     # Plugin wrapper (implements plugin_api_v1)
│       └── Engine/             # OB-Xd DSP core (extracted from upstream)
│           ├── SynthEngine.h
│           ├── Motherboard.h
│           ├── ObxdVoice.h
│           ├── Filter.h
│           ├── ObxdOscillatorB.h
│           ├── AdsrEnvelope.h
│           ├── Lfo.h
│           └── ...
├── patches/
│   └── factory.fxb             # Bundled factory presets
├── scripts/
│   ├── build.sh                # Cross-compile for Move ARM64
│   └── install.sh              # Deploy to Move device
└── .github/
    └── workflows/
        └── build.yml           # CI/release builds
```

## License Strategy

- Main move-anything repo: CC BY-NC-SA 4.0 (unchanged)
- This module: GPL-3.0 (required by OB-Xd upstream)
- Distributed separately, never bundled with main repo
- Users choose to install GPL modules on their own device

## DSP Architecture

### Voice Configuration
- 4 voices (reduced from 8 for ARM CPU headroom)
- No oversampling initially (can add 2x later if aliasing is audible)
- Sample rate: 44100 Hz
- Block size: 128 frames

### Tempo Handling
```cpp
static float g_tempo_bpm = 120.0f;  // Default until host provides

static void plugin_set_param(const char *key, const char *val) {
    if (strcmp(key, "tempo") == 0) {
        g_tempo_bpm = atof(val);
        g_synth.setTempo(g_tempo_bpm);
    }
}
```

Keep OB-Xd tempo/sync code intact, default to 120 BPM. Future host tempo support will just work.

### JUCE Removal
- Replace `juce::float_Pi` with `M_PI`
- Remove `AudioPlayHead` references (use default tempo)
- Remove JUCE macros (`JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR`)
- Remove `#include "../PluginProcessor.h"`

## Parameter Banks

Left/Right arrows switch between banks. 8 knobs per bank.

### Bank 0: Filter
| Knob | Parameter | Key |
|------|-----------|-----|
| 1 | Cutoff | `cutoff` |
| 2 | Resonance | `resonance` |
| 3 | Filter Env Amt | `filter_env` |
| 4 | Key Track | `key_track` |
| 5 | Attack | `attack` |
| 6 | Decay | `decay` |
| 7 | Sustain | `sustain` |
| 8 | Release | `release` |

### Bank 1: Oscillators
| Knob | Parameter | Key |
|------|-----------|-----|
| 1 | Osc1 Wave | `osc1_wave` |
| 2 | Osc1 PW | `osc1_pw` |
| 3 | Osc2 Wave | `osc2_wave` |
| 4 | Osc2 PW | `osc2_pw` |
| 5 | Osc2 Detune | `osc2_detune` |
| 6 | Osc Mix | `osc_mix` |
| 7 | Osc2 Pitch | `osc2_pitch` |
| 8 | Noise Level | `noise` |

### Bank 2: Modulation
| Knob | Parameter | Key |
|------|-----------|-----|
| 1 | LFO Rate | `lfo_rate` |
| 2 | LFO Wave | `lfo_wave` |
| 3 | LFO → Cutoff | `lfo_cutoff` |
| 4 | LFO → Pitch | `lfo_pitch` |
| 5 | LFO → PW | `lfo_pw` |
| 6 | Vibrato | `vibrato` |
| 7 | Unison | `unison` |
| 8 | Portamento | `portamento` |

## UI Layout

6-line display (128x64 pixels):

```
┌────────────────────────┐
│ OB-Xd  [Filter]    01  │  Line 1: Module, bank, preset #
│ Brass Lead             │  Line 2: Preset name
│ Cut Res Env Key        │  Line 3: Params 1-4 names
│ 64  32  50  0          │  Line 4: Params 1-4 values
│ Atk Dec Sus Rel        │  Line 5: Params 5-8 names
│ 20  45  80  30         │  Line 6: Params 5-8 values
└────────────────────────┘
```

### Controls
- Jog wheel: Browse presets
- Left/Right: Switch parameter bank
- Knobs 1-8: Adjust current bank's parameters
- Up/Down: Octave transpose

## Build System

### build.sh
```bash
#!/bin/bash
set -e
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

mkdir -p build dist/obxd

${CROSS_PREFIX}g++ -g -O3 -shared -fPIC -std=c++14 \
    src/dsp/obxd_plugin.cpp \
    -o build/dsp.so \
    -Isrc/dsp \
    -lm

cp src/module.json src/ui.js dist/obxd/
cp build/dsp.so dist/obxd/
cp -r patches dist/obxd/

echo "Build complete: dist/obxd/"
```

### install.sh
```bash
#!/bin/bash
set -e
scp -r dist/obxd ableton@move.local:/data/UserData/move-anything/modules/
echo "Installed to Move"
```

## User Install Paths

### Pre-built binary (recommended)
```bash
curl -L https://github.com/charlesvestal/move-anything-obxd/releases/latest/download/obxd-module.tar.gz | \
  ssh ableton@move.local 'tar -xz -C /data/UserData/move-anything/modules/'
```

### Build from source
```bash
git clone https://github.com/charlesvestal/move-anything-obxd
cd move-anything-obxd
./scripts/build.sh
./scripts/install.sh
```

## GitHub Actions

`.github/workflows/build.yml` builds ARM64 binary on each tag and attaches to GitHub Release.

## Implementation Tasks

1. Create new repo `move-anything-obxd`
2. Add GPL-3.0 LICENSE
3. Extract OB-Xd Engine/ directory, strip JUCE
4. Write obxd_plugin.cpp wrapper
5. Write module.json
6. Write ui.js with bank switching
7. Bundle factory.fxb presets
8. Create build.sh and install.sh
9. Set up GitHub Actions
10. Test on Move hardware
11. Update main repo README with optional module link

## Credits

- OB-Xd by reales (GPL-3.0): https://github.com/reales/OB-Xd
- Original Oberheim OB-X design by Tom Oberheim
