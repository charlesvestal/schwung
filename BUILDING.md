# Building Move Anything

Move Anything must be cross-compiled for the Ableton Move's ARM64 processor (aarch64 Linux).

## Quick Start (Docker)

```bash
./scripts/build-docker.sh
```

This builds everything and creates `move-anything.tar.gz`.

Requirements: Docker Desktop (macOS/Windows) or Docker Engine (Linux)

> **OrbStack Users**: The build script uses `docker cp` instead of volume mounts to avoid file corruption issues with OrbStack on macOS.

## Manual Build

### Ubuntu/Debian

```bash
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu make

# Build QuickJS
cd libs/quickjs/quickjs-2025-04-26
CC=aarch64-linux-gnu-gcc make libquickjs.a
cd ../../..

# Build project
CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh
./scripts/package.sh
```

### macOS (via Homebrew)

```bash
brew tap messense/macos-cross-toolchains
brew install aarch64-unknown-linux-gnu

cd libs/quickjs/quickjs-2025-04-26
CC=aarch64-unknown-linux-gnu-gcc make libquickjs.a
cd ../../..

CROSS_PREFIX=aarch64-unknown-linux-gnu- ./scripts/build.sh
./scripts/package.sh
```

## Deployment

After building, the tarball is at the repo root: `move-anything.tar.gz`

### Install from local build

```bash
./scripts/install.sh local
```

### Manual deployment

```bash
scp move-anything.tar.gz ableton@move.local:~/
ssh ableton@move.local 'tar -xf move-anything.tar.gz'
```

## Build Outputs

```
build/
  move-anything              # Host binary
  move-anything-shim.so      # LD_PRELOAD shim
  host/menu_ui.js            # Host menu
  shared/                    # Shared JS utilities
  modules/
    sf2/                     # SF2 synth module
    dx7/                     # DX7 FM synth module
    sequencer/               # Step sequencer module
    m8/                      # M8 LPP emulator
    controller/              # MIDI controller

move-anything.tar.gz         # Deployable package (~588KB)
```

## Troubleshooting

**"libquickjs.a not found"**
```bash
cd libs/quickjs/quickjs-2025-04-26
CC=aarch64-linux-gnu-gcc make libquickjs.a
```

**Verify binary architecture**
```bash
file build/move-anything
# Should show: ELF 64-bit LSB executable, ARM aarch64
```

**SSH connection issues**
Add your public key at http://move.local/development/ssh

## Module Development

Rebuild a single module:

```bash
aarch64-linux-gnu-gcc -g -O3 -shared -fPIC \
    src/modules/sf2/dsp/sf2_plugin.c \
    -o build/modules/sf2/dsp.so \
    -Isrc -Isrc/modules/sf2/dsp -lm

scp build/modules/sf2/dsp.so ableton@move.local:~/move-anything/modules/sf2/
```

## Architecture

- **Target**: Ableton Move (aarch64 Linux, glibc)
- **Audio**: 44.1kHz, 128-sample blocks (~3ms latency)
- **Host**: Statically links QuickJS
- **Modules**: Loaded via dlopen()
