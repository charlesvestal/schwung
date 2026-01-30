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
# Module directories are created automatically when copying files
# External modules (sf2, dexed, m8, minijv, obxd, clap) are in separate repos

echo "Building host..."

# Build host with module manager and settings
"${CROSS_PREFIX}gcc" -g -O3 \
    src/move_anything.c \
    src/host/module_manager.c \
    src/host/settings.c \
    src/host/unified_log.c \
    -o build/move-anything \
    -Isrc -Isrc/lib \
    -Ilibs/quickjs/quickjs-2025-04-26 \
    -Llibs/quickjs/quickjs-2025-04-26 \
    -lquickjs -lm -ldl

# Build shim (with shared memory support for shadow instrument)
# D-Bus support requires cross-compiled libdbus headers
"${CROSS_PREFIX}gcc" -g3 -shared -fPIC \
    -o build/move-anything-shim.so \
    src/move_anything_shim.c \
    src/host/unified_log.c \
    -I/usr/include/dbus-1.0 \
    -I/usr/lib/aarch64-linux-gnu/dbus-1.0/include \
    -ldl -lrt -lpthread -ldbus-1 -lm

echo "Building Shadow POC..."

# Build Shadow Instrument POC (reference example - not used in production)
mkdir -p ./build/shadow/
"${CROSS_PREFIX}gcc" -g -O3 \
    examples/shadow_poc.c \
    -o build/shadow/shadow_poc \
    -Isrc -Isrc/host \
    -lm -ldl -lrt

echo "Building Shadow UI..."

# Build Shadow UI host (uses shared display bindings from js_display.c)
"${CROSS_PREFIX}gcc" -g -O3 \
    src/shadow/shadow_ui.c \
    src/host/js_display.c \
    src/host/unified_log.c \
    -o build/shadow/shadow_ui \
    -Isrc -Isrc/lib \
    -Ilibs/quickjs/quickjs-2025-04-26 \
    -Llibs/quickjs/quickjs-2025-04-26 \
    -lquickjs -lm -ldl -lrt

echo "Building Signal Chain module..."

# Build Signal Chain DSP plugin
mkdir -p ./build/modules/chain/
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/chain/dsp/chain_host.c \
    src/host/unified_log.c \
    -o build/modules/chain/dsp.so \
    -Isrc \
    -lm -ldl -lpthread

echo "Building Audio FX plugins..."

# Build Freeverb audio FX
mkdir -p ./build/modules/audio_fx/freeverb/
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/audio_fx/freeverb/freeverb.c \
    -o build/modules/audio_fx/freeverb/freeverb.so \
    -Isrc \
    -lm

echo "Building MIDI FX plugins..."

# Build Chord MIDI FX
mkdir -p ./build/modules/midi_fx/chord/
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/midi_fx/chord/dsp/chord.c \
    -o build/modules/midi_fx/chord/dsp.so \
    -Isrc

# Build Arpeggiator MIDI FX
mkdir -p ./build/modules/midi_fx/arp/
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/midi_fx/arp/dsp/arp.c \
    -o build/modules/midi_fx/arp/dsp.so \
    -Isrc

echo "Building Sound Generator plugins..."

# Build Line In sound generator
mkdir -p ./build/modules/sound_generators/linein/
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/sound_generators/linein/linein.c \
    -o build/modules/sound_generators/linein/dsp.so \
    -Isrc \
    -lm

# Copy shared utilities
cp ./src/shared/*.mjs ./build/shared/

# Copy host files
cp ./src/host/menu_ui.js ./build/host/
cp ./src/host/*.mjs ./build/host/ 2>/dev/null || true
cp ./src/host/version.txt ./build/host/

# Copy shadow UI files
cp ./src/shadow/shadow_ui.js ./build/shadow/

# Copy scripts and assets
cp ./src/shim-entrypoint.sh ./build/
cp ./src/start.sh ./build/ 2>/dev/null || true
cp ./src/stop.sh ./build/ 2>/dev/null || true

# Copy all module files (js, mjs, json) - preserves directory structure
# Compiled .so files are built separately above
echo "Copying module files..."
find ./src/modules -type f \( -name "*.js" -o -name "*.mjs" -o -name "*.json" \) | while read src; do
    dest="./build/${src#./src/}"
    mkdir -p "$(dirname "$dest")"
    cp "$src" "$dest"
done

# Copy patches directory
echo "Copying patches..."
mkdir -p ./build/patches
cp -r ./src/patches/*.json ./build/patches/ 2>/dev/null || true

# Copy master presets directory

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
