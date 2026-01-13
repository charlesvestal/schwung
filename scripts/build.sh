#!/usr/bin/env bash
# Build Move Anything for Ableton Move (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Move Anything Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        "$IMAGE_NAME"

    echo ""
    echo "=== Done ==="
    echo "Output: $REPO_ROOT/move-anything.tar.gz"
    echo ""
    echo "To install on Move:"
    echo "  ./scripts/install.sh local"
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
set -x

cd "$REPO_ROOT"

# Clean and prepare
./scripts/clean.sh
mkdir -p ./build/
mkdir -p ./build/host/
mkdir -p ./build/shared/
# Built-in modules only (chain, controller, store)
# External modules (sf2, dx7, m8, jv880, obxd, clap) are in separate repos
mkdir -p ./build/modules/controller/
mkdir -p ./build/modules/chain/
mkdir -p ./build/modules/store/

echo "Building host..."

# Build host with module manager and settings
"${CROSS_PREFIX}gcc" -g -O3 \
    src/move_anything.c \
    src/host/module_manager.c \
    src/host/settings.c \
    -o build/move-anything \
    -Isrc -Isrc/lib \
    -Ilibs/quickjs/quickjs-2025-04-26 \
    -Llibs/quickjs/quickjs-2025-04-26 \
    -lquickjs -lm -ldl

# Build shim
"${CROSS_PREFIX}gcc" -g3 -shared -fPIC \
    -o build/move-anything-shim.so \
    src/move_anything_shim.c -ldl

echo "Building Signal Chain module..."

# Build Signal Chain DSP plugin
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/chain/dsp/chain_host.c \
    -o build/modules/chain/dsp.so \
    -Isrc \
    -lm -ldl

echo "Building Audio FX plugins..."

# Build Freeverb audio FX
mkdir -p ./build/modules/chain/audio_fx/freeverb/
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/chain/audio_fx/freeverb/freeverb.c \
    -o build/modules/chain/audio_fx/freeverb/freeverb.so \
    -Isrc \
    -lm

echo "Building Sound Generator plugins..."

# Build Line In sound generator
mkdir -p ./build/modules/chain/sound_generators/linein/
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/chain/sound_generators/linein/linein.c \
    -o build/modules/chain/sound_generators/linein/dsp.so \
    -Isrc \
    -lm

# Copy shared utilities
cp ./src/shared/*.mjs ./build/shared/

# Copy host files
cp ./src/host/menu_ui.js ./build/host/
cp ./src/host/*.mjs ./build/host/ 2>/dev/null || true
cp ./src/host/version.txt ./build/host/

# Copy scripts and assets
cp ./src/shim-entrypoint.sh ./build/
cp ./src/start.sh ./build/ 2>/dev/null || true
cp ./src/stop.sh ./build/ 2>/dev/null || true

# Copy Controller module files
cp ./src/modules/controller/module.json ./build/modules/controller/
cp ./src/modules/controller/ui.js ./build/modules/controller/

# Copy Store module files
cp ./src/modules/store/module.json ./build/modules/store/
cp ./src/modules/store/ui.js ./build/modules/store/

# Copy Signal Chain module files
cp ./src/modules/chain/module.json ./build/modules/chain/
cp ./src/modules/chain/ui.js ./build/modules/chain/
mkdir -p ./build/modules/chain/midi_fx/
cp ./src/modules/chain/midi_fx/*.mjs ./build/modules/chain/midi_fx/ 2>/dev/null || true
mkdir -p ./build/modules/chain/patches/
cp ./src/modules/chain/patches/*.json ./build/modules/chain/patches/ 2>/dev/null || true

# Copy chain component module.json files for dynamic discovery
cp ./src/modules/chain/audio_fx/freeverb/module.json ./build/modules/chain/audio_fx/freeverb/ 2>/dev/null || true
cp ./src/modules/chain/sound_generators/linein/module.json ./build/modules/chain/sound_generators/linein/ 2>/dev/null || true
mkdir -p ./build/modules/chain/midi_fx/chord/
cp ./src/modules/chain/midi_fx/chord/module.json ./build/modules/chain/midi_fx/chord/ 2>/dev/null || true
mkdir -p ./build/modules/chain/midi_fx/arp/
cp ./src/modules/chain/midi_fx/arp/module.json ./build/modules/chain/midi_fx/arp/ 2>/dev/null || true

# Copy curl binary for store module (if present)
if [ -f "./libs/curl/curl" ]; then
    mkdir -p ./build/bin/
    cp ./libs/curl/curl ./build/bin/
    echo "Bundled curl binary"
else
    echo "Warning: libs/curl/curl not found - store module will not work without it"
fi

echo "Build complete!"
echo "Host binary: build/move-anything"
echo "Modules: build/modules/"
