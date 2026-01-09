# Building Move Anything

Move Anything must be cross-compiled for the Ableton Move's ARM64 processor (aarch64 Linux).

## Quick Start

```bash
./scripts/build.sh
```

This builds everything and creates `move-anything.tar.gz`. The build script automatically uses Docker for cross-compilation if needed.

Requirements: Docker Desktop (macOS/Windows) or Docker Engine (Linux)

## Manual Build (without Docker)

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
    m8/                      # M8 LPP emulator
    controller/              # MIDI controller
    chain/                   # Signal Chain module
      audio_fx/              # Audio FX plugins (Freeverb)
      sound_generators/      # Sound generators (Line In)
      patches/               # Chain patches

move-anything.tar.gz         # Deployable package
```

## Troubleshooting

**"libquickjs.a not found"**
```bash
cd libs/quickjs/quickjs-2025-04-26
CC=aarch64-linux-gnu-gcc make libquickjs.a
```

**Missing font.png/font.png.dat**

The host falls back to the bitmap font when the system TTF isn't available, and `package.sh` expects these files. Generate them with:

```bash
python3 -m pip install pillow
python3 scripts/generate_font.py build/font.png
python3 - <<'PY'
from importlib.util import spec_from_file_location, module_from_spec
from pathlib import Path

spec = spec_from_file_location("generate_font", "scripts/generate_font.py")
mod = module_from_spec(spec)
spec.loader.exec_module(mod)
Path("build/font.png.dat").write_text(mod.CHARS + "\n")
PY
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
