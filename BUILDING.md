# Building Move Anything

Move Anything must be cross-compiled for the Ableton Move's ARM64 processor (aarch64 Linux).

## Quick Start

```bash
./scripts/build.sh
```

This builds everything and creates `move-anything.tar.gz`. The build script automatically uses Docker for cross-compilation if needed.

Requirements: Docker Desktop (macOS/Windows) or Docker Engine (Linux)

Build without screen reader (skip D-Bus/Flite dependencies):
```bash
DISABLE_SCREEN_READER=1 ./scripts/build.sh
```
This disables D-Bus-driven accessibility/sync features (screen reader announcements, track-volume D-Bus sync, and set-tempo detection from D-Bus text).

## Verify Disabled Screen Reader Build (Smoke Test)

Use this when validating a fresh environment that does not have D-Bus/Flite dev headers installed.

```bash
# Optional: return device to stock state first
MOVE_FORCE_UNINSTALL=1 ./scripts/uninstall.sh

# Build shim without dbus/flite
DISABLE_SCREEN_READER=1 ./scripts/build.sh

# Install fresh build
./scripts/install.sh local --skip-confirmation --skip-modules
```

Expected install output includes:
- `Screen reader runtime not bundled; skipping Flite deployment.`
- `Screen Reader: disabled`

Runtime checks on device:
```bash
# Shim should still be active
ssh root@move.local 'pid=$(pidof MoveOriginal | awk "{print \$1}"); tr "\0" "\n" < /proc/$pid/environ | grep "^LD_PRELOAD="'

# No Flite runtime links should be installed
ssh root@move.local 'ls -1 /usr/lib/libflite*.so* 2>/dev/null || echo absent'
```

Optional hardening check:
```bash
# Force "screen reader on" state file, restart Move, confirm no crash loop
ssh ableton@move.local 'echo 1 > /data/UserData/move-anything/config/screen_reader_state.txt'
ssh root@move.local '/etc/init.d/move stop >/dev/null 2>&1 || true; sleep 1; /etc/init.d/move start >/dev/null 2>&1'
ssh root@move.local 'pid1=$(pidof MoveOriginal | awk "{print \$1}"); sleep 10; pid2=$(pidof MoveOriginal | awk "{print \$1}"); sleep 10; pid3=$(pidof MoveOriginal | awk "{print \$1}"); echo "$pid1 -> $pid2 -> $pid3"'
```
PIDs should remain stable (no repeated restarts).

## Bootstrap Dependencies (Debian/Ubuntu)

For a fresh Linux machine, bootstrap dependencies and QuickJS first:

```bash
./scripts/bootstrap-build-deps.sh
CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh
```

Without screen reader dependencies:

```bash
./scripts/bootstrap-build-deps.sh --no-screen-reader
DISABLE_SCREEN_READER=1 CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh
```

## Manual Build (without Docker)

### Ubuntu/Debian

```bash
./scripts/bootstrap-build-deps.sh

# Build QuickJS
# (handled by bootstrap script; manual fallback shown below if needed)

# Build project
CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh
# Or build without screen reader:
# DISABLE_SCREEN_READER=1 CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh
./scripts/package.sh
```

### macOS (via Homebrew)

```bash
brew tap messense/macos-cross-toolchains
brew install aarch64-unknown-linux-gnu

cd libs/quickjs/quickjs-2025-04-26
CC=aarch64-unknown-linux-gnu-gcc AR=aarch64-unknown-linux-gnu-ar make libquickjs.a
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
  patches/                   # Chain patches
  modules/
    chain/                   # Signal Chain module
      audio_fx/              # Audio FX plugins (Freeverb)
      sound_generators/      # Sound generators (Line In)
      midi_fx/               # MIDI FX (Chord, Arp)
    controller/              # MIDI controller
    store/                   # Module Store

move-anything.tar.gz         # Deployable package
```

**Note:** Sound generators (SF2, Dexed, Mini-JV, OB-Xd) and audio effects (CloudSeed, PSX Verb, etc.) are external modules installed via the Module Store, not built with the host.

## Troubleshooting

**"libquickjs.a not found"**
```bash
cd libs/quickjs/quickjs-2025-04-26
CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar make libquickjs.a
```

**Missing font.png/font.png.dat**

`build/host/font.png` and `build/host/font.png.dat` are generated automatically by `build.sh` from `scripts/generate_font.py`. This script is the **single source of truth** for the bitmap font used on the host display, the shadow UI, and the OLED shim overlay.

If you need to regenerate the font manually (e.g. after editing `FONT` in `generate_font.py`):

```bash
python3 -m pip install pillow
python3 scripts/generate_font.py --deploy-png build/host/font.png
```

To preview the font as a grid image:
```bash
python3 scripts/generate_font.py --png font_preview.png
```

To print the C array for `overlay_font_5x7` in the shim:
```bash
python3 scripts/generate_font.py --c-array
```

> **Note:** The host no longer attempts to load the system TTF (`/opt/move/Fonts/unifont_jp-14.0.01.ttf`). The 5x7 bitmap font from `generate_font.py` is always used.

**Flite bundle verification failed**
```bash
# Recommended: use Docker build (auto-installs arm64 dependencies)
./scripts/build.sh

# If building manually, bootstrap deps (including Flite/dbus):
./scripts/bootstrap-build-deps.sh

# Or disable screen reader support in this build:
DISABLE_SCREEN_READER=1 ./scripts/build.sh
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

External modules (SF2, Dexed, Braids, etc.) are built and released separately. Each module repo has its own GitHub Actions workflow that automates the entire build-and-release pipeline.

### How It Works

When you push a version tag (e.g., `v0.2.0`), GitHub Actions automatically:

1. **Verifies** the tag version matches `src/module.json`
2. **Builds** the module via Docker cross-compilation
3. **Creates** a GitHub Release with the compiled tarball attached
4. **Updates** `release.json` on the main branch so the Module Store can find the new version

No manual release creation or file uploads needed — just tag and push.

### Build Script Requirements

Each external module's `scripts/build.sh` must:

1. Cross-compile DSP code for ARM64 (aarch64 Linux)
2. Package all files to `dist/<module-id>/`
3. Create a tarball at `dist/<module-id>-module.tar.gz`

Example tarball creation (at end of build.sh, after packaging):
```bash
# Create tarball for release
cd dist
tar -czvf mymodule-module.tar.gz mymodule/
cd ..

echo "Tarball: dist/mymodule-module.tar.gz"
```

The tarball must contain a single directory (`<module-id>/`) with all module files inside it.

### Release Workflow

Each module needs `.github/workflows/release.yml`. Here's the complete, production-ready workflow:

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

      # Step 1: Verify tag version matches module.json
      # Prevents releasing with mismatched versions
      - name: Verify version match
        run: |
          TAG_VERSION="${GITHUB_REF_NAME#v}"
          MODULE_VERSION=$(grep '"version"' src/module.json | head -1 | sed 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')
          if [ "$TAG_VERSION" != "$MODULE_VERSION" ]; then
            echo "ERROR: Tag version ($TAG_VERSION) does not match module.json version ($MODULE_VERSION)"
            echo "Please update src/module.json to version $TAG_VERSION before tagging."
            exit 1
          fi
          echo "Version check passed: $TAG_VERSION"

      # Step 2: Cross-compile for ARM64 via Docker
      - name: Set up Docker Buildx
        uses: docker/setup-buildx-action@v3

      - name: Build with Docker
        run: |
          docker build -t module-builder -f scripts/Dockerfile .
          docker run --rm -v "$PWD:/build" -w /build module-builder ./scripts/build.sh

      # Step 3: Create GitHub Release with tarball attached
      - name: Create Release
        uses: softprops/action-gh-release@v1
        with:
          files: dist/<module-id>-module.tar.gz
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}

      # Step 4: Update release.json on main branch
      # The Module Store reads this file to discover the latest version
      - name: Commit release.json
        run: |
          VERSION="${GITHUB_REF_NAME#v}"
          INSTALL_PATH=$(jq -r '.install_path // empty' release.json 2>/dev/null || echo "")

          git fetch origin main
          git checkout -f main
          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"

          if [ -n "$INSTALL_PATH" ]; then
            cat > release.json << EOF
          {
            "version": "${VERSION}",
            "download_url": "https://github.com/${{ github.repository }}/releases/download/${{ github.ref_name }}/<module-id>-module.tar.gz",
            "install_path": "${INSTALL_PATH}"
          }
          EOF
          else
            cat > release.json << EOF
          {
            "version": "${VERSION}",
            "download_url": "https://github.com/${{ github.repository }}/releases/download/${{ github.ref_name }}/<module-id>-module.tar.gz"
          }
          EOF
          fi

          git add release.json
          git commit -m "chore: update release.json for ${{ github.ref_name }}" || echo "No changes to commit"
          git push origin main
```

Replace `<module-id>` with your module's actual ID (e.g., `braids`, `sf2`, `cloudseed`).

### Workflow Step-by-Step

**Step 1 — Version Verification:**
The workflow extracts the version from the git tag (stripping the `v` prefix) and compares it to the `version` field in `src/module.json`. If they don't match, the build fails immediately. This prevents accidentally releasing with a stale version number.

**Step 2 — Docker Build:**
Uses the module's `scripts/Dockerfile` to create a cross-compilation environment with the ARM64 toolchain, then runs `scripts/build.sh` inside the container. The build script compiles DSP code and creates the tarball at `dist/<module-id>-module.tar.gz`.

**Step 3 — GitHub Release:**
Creates a GitHub Release named after the tag and attaches the compiled tarball as a release asset. Uses `softprops/action-gh-release@v1`.

**Step 4 — release.json Update:**
Switches to the `main` branch and writes/updates `release.json` with the new version and download URL. This file is what the Module Store reads to discover available updates. If the repo already has an `install_path` in release.json (for legacy reasons), it's preserved.

### Releasing a New Version

```bash
# 1. Update version in module.json
#    Edit src/module.json and set "version": "0.2.0"

# 2. Commit the version bump
git add src/module.json
git commit -m "bump version to 0.2.0"

# 3. Tag and push
git tag v0.2.0
git push origin main --tags

# 4. (Automated) GitHub Actions builds, creates release, updates release.json

# 5. Add release notes after the release is created
gh release edit v0.2.0 --notes "$(cat <<'EOF'
- Brief description of changes
- One bullet per significant change
EOF
)"
```

The tag version (e.g., `v0.2.0`) **must** match the version in `src/module.json` (e.g., `"version": "0.2.0"`), or the build will fail.

### Module Store Integration

The Module Store fetches release info from GitHub API:
- Reads `tag_name` for version (strips `v` prefix)
- Looks for asset matching `<module-id>-module.tar.gz`
- If asset not found, shows version as "0.0.0"

The `module-catalog.json` in this repo lists all available modules. The store uses the `github_repo` and `asset_name` fields to locate releases.

### Variations

**Modules with git submodules** (e.g., NAM): Add `submodules: recursive` to the checkout step:
```yaml
- uses: actions/checkout@v4
  with:
    submodules: recursive
```

**Modules with multiple release assets** (e.g., webstream): List multiple files in the release step:
```yaml
- name: Create Release
  uses: softprops/action-gh-release@v1
  with:
    files: |
      dist/mymodule-module.tar.gz
      dist/mymodule-module-core.tar.gz
```

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

The release workflow automatically updates `release.json` on each release — see [Release Workflow](#release-workflow) above for the complete workflow including the `release.json` commit step.

## Architecture

- **Target**: Ableton Move (aarch64 Linux, glibc)
- **Audio**: 44.1kHz, 128-sample blocks (~3ms latency)
- **Host**: Statically links QuickJS
- **Modules**: Loaded via dlopen()
