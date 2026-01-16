# Audio FX Modules Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Create 4 new audio effect modules as separate repositories: CloudSeed reverb, PSXVerb reverb, TapeScam saturation, and RE-201 style delay.

**Architecture:** Each module is a standalone git repo following the move-anything-jv880 pattern. Effects implement `audio_fx_api_v1` and install to `modules/chain/audio_fx/<name>/` on the device. All use Docker-based cross-compilation for ARM64.

**Tech Stack:** C/C++, audio_fx_api_v1, Docker cross-compilation, MIT license

---

## Repository Structure (All 4 Modules)

Each repo follows this structure:
```
move-anything-<name>/
├── LICENSE                 # MIT license
├── README.md
├── CLAUDE.md               # Claude instructions
├── scripts/
│   ├── Dockerfile          # Cross-compilation (copy from jv880)
│   ├── build.sh            # Build script
│   └── install.sh          # Deploy to Move
├── src/
│   ├── module.json         # Module metadata
│   ├── dsp/
│   │   ├── <name>.c        # Main DSP implementation
│   │   └── audio_fx_api_v1.h  # API header (copy from main repo)
│   └── chain_patches/      # Optional preset chains
├── build/                  # Build output (gitignored)
└── dist/                   # Distribution (gitignored)
```

---

## Task 1: Create move-anything-cloudseed Repository

**Goal:** Port CloudSeedCore reverb algorithm to audio_fx_api_v1.

### Step 1.1: Initialize repository

```bash
cd /Volumes/ExtFS/charlesvestal/github/move-everything-parent
mkdir move-anything-cloudseed
cd move-anything-cloudseed
git init
```

### Step 1.2: Create LICENSE file

Create: `LICENSE`

```
MIT License

Copyright (c) 2024 Ghost Note Audio (CloudSeedCore algorithm)
Copyright (c) 2025 Charles Vestal (Move Anything port)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Step 1.3: Create directory structure

```bash
mkdir -p scripts src/dsp src/chain_patches
```

### Step 1.4: Copy build infrastructure from jv880

```bash
cp ../move-anything-jv880/scripts/Dockerfile scripts/
```

### Step 1.5: Create build.sh

Create: `scripts/build.sh`

```bash
#!/usr/bin/env bash
# Build CloudSeed module for Move Anything (ARM64)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== CloudSeed Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building CloudSeed Module ==="
echo "Cross prefix: $CROSS_PREFIX"

mkdir -p build dist/cloudseed

# Compile DSP plugin
echo "Compiling DSP plugin..."
${CROSS_PREFIX}gcc -O3 -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    src/dsp/cloudseed.c \
    -o build/cloudseed.so \
    -Isrc/dsp \
    -lm

# Package
echo "Packaging..."
cat src/module.json > dist/cloudseed/module.json
cat build/cloudseed.so > dist/cloudseed/cloudseed.so
chmod +x dist/cloudseed/cloudseed.so

echo ""
echo "=== Build Complete ==="
echo "Output: dist/cloudseed/"
```

### Step 1.6: Create install.sh

Create: `scripts/install.sh`

```bash
#!/bin/bash
# Install CloudSeed module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/cloudseed" ]; then
    echo "Error: dist/cloudseed not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing CloudSeed Module ==="

# Deploy audio FX to chain directory
echo "Copying module to Move..."
scp -r dist/cloudseed ableton@move.local:/data/UserData/move-anything/modules/chain/audio_fx/

# Install chain presets if they exist
if [ -d "src/chain_patches" ] && [ "$(ls -A src/chain_patches 2>/dev/null)" ]; then
    echo "Installing chain presets..."
    scp src/chain_patches/*.json ableton@move.local:/data/UserData/move-anything/modules/chain/patches/
fi

echo ""
echo "=== Install Complete ==="
echo "Module installed to: modules/chain/audio_fx/cloudseed/"
echo "Restart Move Anything to load the new module."
```

### Step 1.7: Create module.json

Create: `src/module.json`

```json
{
  "id": "cloudseed",
  "name": "CloudSeed",
  "version": "0.1.0",
  "description": "Algorithmic reverb based on CloudSeedCore",
  "author": "Ghost Note Audio / Charles Vestal",
  "dsp": "cloudseed.so",
  "api_version": 1,
  "capabilities": {
    "chainable": true,
    "component_type": "audio_fx",
    "chain_params": [
      {
        "key": "decay",
        "name": "Decay",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.5,
        "step": 0.05
      },
      {
        "key": "mix",
        "name": "Mix",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.3,
        "step": 0.05
      },
      {
        "key": "predelay",
        "name": "Pre-Delay",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.1,
        "step": 0.05
      },
      {
        "key": "diffusion",
        "name": "Diffusion",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.7,
        "step": 0.05
      }
    ]
  }
}
```

### Step 1.8: Copy audio_fx_api_v1.h

```bash
cp ../move-anything/src/host/audio_fx_api_v1.h src/dsp/
cp ../move-anything/src/host/plugin_api_v1.h src/dsp/
```

### Step 1.9: Create cloudseed.c DSP implementation

Create: `src/dsp/cloudseed.c`

This is the main implementation task. Port CloudSeedCore algorithm:
- Implement simplified allpass diffuser network
- Implement modulated delay lines for late reverb
- Implement low/high cut filters
- Connect to audio_fx_api_v1 interface

**Core algorithm (simplified):**
```c
// Input -> Predelay -> Diffuser -> Delay Network -> Output
// Feedback from delay network back to diffuser input
```

**Parameters:**
- `decay`: Feedback amount in delay network (0.0-1.0 -> 0.5-0.98)
- `mix`: Wet/dry mix
- `predelay`: Pre-delay time (0-100ms)
- `diffusion`: Allpass diffusion amount

### Step 1.10: Create CLAUDE.md

Create: `CLAUDE.md`

```markdown
# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

CloudSeed audio effect module for Move Anything - an algorithmic reverb based on Ghost Note Audio's CloudSeedCore.

## Build Commands

```bash
./scripts/build.sh      # Build with Docker
./scripts/install.sh    # Deploy to Move
```

## Architecture

Implements audio_fx_api_v1 for Signal Chain integration. Core algorithm:
- Allpass diffuser network for early reflections
- Modulated delay lines for late reverb tail
- Low/high cut filtering for tone shaping

## License

MIT (CloudSeedCore by Ghost Note Audio, port by Charles Vestal)
```

### Step 1.11: Make scripts executable and commit

```bash
chmod +x scripts/*.sh
git add .
git commit -m "feat: initial CloudSeed reverb module structure"
```

---

## Task 2: Create move-anything-psxverb Repository

**Goal:** Port PSXVerb from CVCHothouse to audio_fx_api_v1.

Follow same steps as Task 1 with these differences:

### Step 2.7: Create module.json

```json
{
  "id": "psxverb",
  "name": "PSX Verb",
  "version": "0.1.0",
  "description": "PlayStation 1 SPU reverb emulation",
  "author": "Charles Vestal",
  "dsp": "psxverb.so",
  "api_version": 1,
  "capabilities": {
    "chainable": true,
    "component_type": "audio_fx",
    "chain_params": [
      {
        "key": "preset",
        "name": "Preset",
        "type": "int",
        "min": 0,
        "max": 5,
        "default": 4,
        "labels": ["Room", "Studio S", "Studio M", "Studio L", "Hall", "Space Echo"]
      },
      {
        "key": "decay",
        "name": "Decay",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.7,
        "step": 0.05
      },
      {
        "key": "mix",
        "name": "Mix",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.35,
        "step": 0.05
      }
    ]
  }
}
```

### Step 2.9: Port PsxReverb.h

Port from CVCHothouse/PSXVerb:
- PsxReverb.h (core algorithm)
- PsxPreset.h (6 presets)
- WorkArea.h (circular buffer)

Adapt from 48kHz to 44.1kHz (adjust halfband filter or work at native rate).

---

## Task 3: Create move-anything-tapescam Repository

**Goal:** Port TapeScam from CVCHothouse to audio_fx_api_v1.

### Step 3.7: Create module.json

```json
{
  "id": "tapescam",
  "name": "Tape Scam",
  "version": "0.1.0",
  "description": "Tape saturation and degradation effect",
  "author": "Charles Vestal",
  "dsp": "tapescam.so",
  "api_version": 1,
  "capabilities": {
    "chainable": true,
    "component_type": "audio_fx",
    "chain_params": [
      {
        "key": "drive",
        "name": "Drive",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.3,
        "step": 0.05
      },
      {
        "key": "saturation",
        "name": "Saturation",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.5,
        "step": 0.05
      },
      {
        "key": "wobble",
        "name": "Wobble",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.2,
        "step": 0.05
      },
      {
        "key": "tone",
        "name": "Tone",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.5,
        "step": 0.05
      }
    ]
  }
}
```

### Step 3.9: Port TapeScam.cpp

Core DSP stages to implement:
1. Drive/gain stage
2. Tape saturation (soft clipping)
3. Wow/flutter (LFO modulated delay)
4. Tone shaping (lowpass filter)
5. Output level control

---

## Task 4: Create move-anything-delay Repository

**Goal:** Create RE-201 style tape delay from scratch.

### Step 4.7: Create module.json

```json
{
  "id": "spacecho",
  "name": "Space Echo",
  "version": "0.1.0",
  "description": "RE-201 style tape delay",
  "author": "Charles Vestal",
  "dsp": "spacecho.so",
  "api_version": 1,
  "capabilities": {
    "chainable": true,
    "component_type": "audio_fx",
    "chain_params": [
      {
        "key": "time",
        "name": "Time",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.4,
        "step": 0.05
      },
      {
        "key": "feedback",
        "name": "Feedback",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.4,
        "step": 0.05
      },
      {
        "key": "mix",
        "name": "Mix",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.35,
        "step": 0.05
      },
      {
        "key": "tone",
        "name": "Tone",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.6,
        "step": 0.05
      },
      {
        "key": "flutter",
        "name": "Flutter",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.15,
        "step": 0.05
      }
    ]
  }
}
```

### Step 4.9: Create spacecho.c DSP implementation

**Core components:**
1. **Delay line** - Circular buffer, max ~1 second at 44.1kHz
2. **Tape saturation** - Soft clipping on feedback path
3. **Tone filter** - Lowpass on feedback (tape darkening)
4. **Flutter** - LFO modulating delay read position
5. **Wow** - Slower LFO for pitch drift

**Algorithm:**
```c
// Each sample:
// 1. Read from delay line (with flutter modulation)
// 2. Apply tone filter to delayed signal
// 3. Mix dry + wet
// 4. Write input + (filtered_delay * feedback * saturation) to delay line
```

---

## Task 5: Build and Test All Modules

### Step 5.1: Build all modules

```bash
cd move-anything-cloudseed && ./scripts/build.sh
cd ../move-anything-psxverb && ./scripts/build.sh
cd ../move-anything-tapescam && ./scripts/build.sh
cd ../move-anything-delay && ./scripts/build.sh
```

### Step 5.2: Install all modules

```bash
cd move-anything-cloudseed && ./scripts/install.sh
cd ../move-anything-psxverb && ./scripts/install.sh
cd ../move-anything-tapescam && ./scripts/install.sh
cd ../move-anything-delay && ./scripts/install.sh
```

### Step 5.3: Test on device

1. Load Signal Chain module
2. Enter editor mode
3. Add each new audio FX
4. Verify parameters work
5. Verify audio processing

---

## Implementation Order

Recommended order based on complexity:

1. **TapeScam** (simplest - mostly filters and waveshaping)
2. **Space Echo** (delay line + filters)
3. **PSXVerb** (direct port from your code)
4. **CloudSeed** (most complex algorithm)

---

## License Summary

| Module | License | Attribution |
|--------|---------|-------------|
| CloudSeed | MIT | Ghost Note Audio (algorithm), Charles Vestal (port) |
| PSXVerb | MIT | Charles Vestal |
| TapeScam | MIT | Charles Vestal |
| Space Echo | MIT | Charles Vestal |
