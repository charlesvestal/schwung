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

**Note:** Sound generators (SF2, Dexed, Mini-JV, OB-Xd) and audio effects (CloudSeed, PSX Verb, etc.) are external modules installed via the Module Store, not built with the host.

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

## External Module Releases

External modules (SF2, Dexed, Mini-JV, etc.) are built and released separately. Each module repo has its own release workflow.

### Build Script Requirements

Each external module's `scripts/build.sh` must:

1. Build the module to `dist/<module-id>/`
2. Create a tarball at `dist/<module-id>-module.tar.gz`

Example tarball creation (add at end of build.sh, after packaging):
```bash
# Create tarball for release
cd dist
tar -czvf mymodule-module.tar.gz mymodule/
cd ..

echo "Tarball: dist/mymodule-module.tar.gz"
```

### Release Workflow

Each module should have `.github/workflows/release.yml`:
```yaml
name: Release

on:
  push:
    tags:
      - 'v*'

permissions:
  contents: write

jobs:
  build:
    runs-on: ubuntu-latest

    steps:
      - uses: actions/checkout@v4

      - name: Set up Docker Buildx
        uses: docker/setup-buildx-action@v3

      - name: Build with Docker
        run: |
          docker build -t module-builder -f scripts/Dockerfile .
          docker run --rm -v "$PWD:/build" -w /build module-builder ./scripts/build.sh

      - name: Create Release
        uses: softprops/action-gh-release@v1
        with:
          files: dist/<module-id>-module.tar.gz
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
```

### Version Management

1. Update `src/module.json` version before tagging
2. Create and push a version tag: `git tag v0.2.0 && git push --tags`
3. The release workflow builds and uploads the tarball automatically

### Module Store Integration

The Module Store fetches release info from GitHub API:
- Reads `tag_name` for version (strips `v` prefix)
- Looks for asset matching `<module-id>-module.tar.gz`
- If asset not found, shows version as "0.0.0"

The `module-catalog.json` in this repo lists all available modules. The store uses the `github_repo` and `asset_name` fields to locate releases.

## Plugin API Versions

The host supports two plugin APIs. **All new modules should use V2.**

### Plugin API v2 (Recommended)

V2 supports multiple instances and is required for Signal Chain integration.

```c
#include "host/plugin_api_v2.h"

typedef struct plugin_api_v2 {
    uint32_t api_version;              // Must be 2
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_lr, int frames);
} plugin_api_v2_t;

// Entry point - export this function
extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host);
```

### Plugin API v1 (Deprecated)

V1 is a singleton API - only one instance can exist. **Do not use for new modules.**

```c
typedef struct plugin_api_v1 {
    uint32_t api_version;              // Must be 1
    int (*on_load)(const char *module_dir, const char *json_defaults);
    void (*on_unload)(void);
    void (*on_midi)(const uint8_t *msg, int len, int source);
    void (*set_param)(const char *key, const char *val);
    int (*get_param)(const char *key, char *buf, int buf_len);
    void (*render_block)(int16_t *out_lr, int frames);
} plugin_api_v1_t;

// V1 entry point (deprecated)
extern "C" plugin_api_v1_t* move_plugin_init_v1(const host_api_v1_t *host);
```

### Migration from V1 to V2

1. Replace singleton state with instance struct
2. Update all functions to take `void *instance` as first parameter
3. Implement `create_instance()` and `destroy_instance()`
4. Export `move_plugin_init_v2()` instead of `move_plugin_init_v1()`

## Module Store and release.json

External modules can specify where they should be installed via `release.json`.

### release.json Format

```json
{
  "version": "0.2.1",
  "download_url": "https://github.com/user/repo/releases/download/v0.2.1/module.tar.gz"
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `version` | Yes | Semantic version (without `v` prefix) |
| `download_url` | Yes | Direct link to release tarball |

### Install Paths by Module Type

Installation paths are automatically determined by the `component_type` field in module.json:

| component_type | Extracts To |
|----------------|-------------|
| `sound_generator` | `modules/sound_generators/<id>/` |
| `audio_fx` | `modules/audio_fx/<id>/` |
| `midi_fx` | `modules/midi_fx/<id>/` |
| `utility` (or other) | `modules/<id>/` |

The Module Store reads `component_type` from the catalog and installs modules to the appropriate subdirectory automatically.

### GitHub Actions Workflow

The release workflow should preserve `install_path` when updating release.json:

```yaml
- name: Update release.json
  run: |
    VERSION="${GITHUB_REF_NAME#v}"
    INSTALL_PATH=$(jq -r '.install_path // empty' release.json 2>/dev/null || echo "")
    if [ -n "$INSTALL_PATH" ]; then
      cat > release.json << EOFJ
    {
      "version": "${VERSION}",
      "download_url": "https://github.com/${{ github.repository }}/releases/download/${{ github.ref_name }}/<id>-module.tar.gz",
      "install_path": "${INSTALL_PATH}"
    }
    EOFJ
    else
      cat > release.json << EOFJ
    {
      "version": "${VERSION}",
      "download_url": "https://github.com/${{ github.repository }}/releases/download/${{ github.ref_name }}/<id>-module.tar.gz"
    }
    EOFJ
    fi
```

## Architecture

- **Target**: Ableton Move (aarch64 Linux, glibc)
- **Audio**: 44.1kHz, 128-sample blocks (~3ms latency)
- **Host**: Statically links QuickJS
- **Modules**: Loaded via dlopen()
