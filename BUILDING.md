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
    chain/                   # Signal Chain module
      audio_fx/              # Audio FX plugins (Freeverb)
      sound_generators/      # Sound generators (Line In)
      midi_fx/               # MIDI FX (Chord, Arp)
      patches/               # Chain patches
    controller/              # MIDI controller
    store/                   # Module Store

move-anything.tar.gz         # Deployable package
```

**Note:** Sound generators (SF2, DX7, JV-880, OB-Xd) and audio effects (CloudSeed, PSX Verb, etc.) are external modules installed via the Module Store, not built with the host.

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

## Releasing

### Version Numbering

Version is stored in `src/host/version.txt`. Follow semantic versioning:
- **Patch** (0.1.2 → 0.1.3): Bug fixes, minor changes
- **Minor** (0.1.x → 0.2.0): New features, backward compatible
- **Major** (0.x.x → 1.0.0): Breaking changes

### Creating a Release

1. **Update versions**
   ```bash
   # Host version
   echo "0.1.3" > src/host/version.txt

   # Built-in module versions (keep in sync with host)
   # Edit these files and update "version" field:
   #   src/modules/chain/module.json
   #   src/modules/controller/module.json
   #   src/modules/store/module.json

   # Module catalog (for backward compatibility with older hosts)
   # Edit module-catalog.json: update host.latest_version and host.download_url
   ```

2. **Build and test**
   ```bash
   ./scripts/build.sh
   ./scripts/install.sh local
   # Test on device
   ```

3. **Commit and tag**
   ```bash
   git add -A
   git commit -m "fix: description of changes"
   git tag v0.1.3
   git push origin main --tags
   ```

4. **Create GitHub release**
   ```bash
   gh release create v0.1.3 ./move-anything.tar.gz \
       --repo charlesvestal/move-anything \
       --title "v0.1.3" \
       --notes "Release notes here"
   ```

   **Note:** The `--repo charlesvestal/move-anything` flag is required because this repo has multiple remotes configured.

### Automated Releases

GitHub Actions will automatically create a release when you push a tag matching `v*`. See `.github/workflows/release.yml`.

## Architecture

- **Target**: Ableton Move (aarch64 Linux, glibc)
- **Audio**: 44.1kHz, 128-sample blocks (~3ms latency)
- **Host**: Statically links QuickJS
- **Modules**: Loaded via dlopen()
