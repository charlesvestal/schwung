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
mkdir -p ./build/modules/sf2/
mkdir -p ./build/modules/dx7/
mkdir -p ./build/modules/m8/
mkdir -p ./build/modules/controller/
mkdir -p ./build/modules/chain/
mkdir -p ./build/modules/jv880/
mkdir -p ./build/modules/jv880/roms/

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

echo "Building SF2 module..."

# Build SF2 DSP plugin
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/sf2/dsp/sf2_plugin.c \
    -o build/modules/sf2/dsp.so \
    -Isrc -Isrc/modules/sf2/dsp \
    -lm

echo "Building DX7 module..."

# Build DX7 DSP plugin (C++)
"${CROSS_PREFIX}g++" -g -O3 -shared -fPIC -std=c++14 \
    src/modules/dx7/dsp/dx7_plugin.cpp \
    src/modules/dx7/dsp/msfa/dx7note.cc \
    src/modules/dx7/dsp/msfa/env.cc \
    src/modules/dx7/dsp/msfa/exp2.cc \
    src/modules/dx7/dsp/msfa/fm_core.cc \
    src/modules/dx7/dsp/msfa/fm_op_kernel.cc \
    src/modules/dx7/dsp/msfa/freqlut.cc \
    src/modules/dx7/dsp/msfa/lfo.cc \
    src/modules/dx7/dsp/msfa/pitchenv.cc \
    src/modules/dx7/dsp/msfa/sin.cc \
    src/modules/dx7/dsp/msfa/porta.cpp \
    -o build/modules/dx7/dsp.so \
    -Isrc -Isrc/modules/dx7/dsp \
    -lm

# JV-880 module is built from separate repo (move-anything-jv880)
# Skip if source doesn't exist
if [ -d "src/modules/jv880/dsp" ]; then
    echo "Building JV-880 module..."
    "${CROSS_PREFIX}g++" -Ofast -shared -fPIC -std=c++11 \
        -march=armv8-a -mtune=cortex-a72 \
        -fno-exceptions -fno-rtti \
        -fomit-frame-pointer -fno-stack-protector \
        -DNDEBUG \
        src/modules/jv880/dsp/jv880_plugin.cpp \
        src/modules/jv880/dsp/mcu.cpp \
        src/modules/jv880/dsp/mcu_opcodes.cpp \
        src/modules/jv880/dsp/pcm.cpp \
        -o build/modules/jv880/dsp.so \
        -Isrc -Isrc/modules/jv880/dsp \
        -lm -lpthread
else
    echo "Skipping JV-880 module (external repo)"
fi

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

# Copy scripts and assets
cp ./src/shim-entrypoint.sh ./build/
cp ./src/start.sh ./build/ 2>/dev/null || true
cp ./src/stop.sh ./build/ 2>/dev/null || true

# Copy SF2 module files
cp ./src/modules/sf2/module.json ./build/modules/sf2/
cp ./src/modules/sf2/ui.js ./build/modules/sf2/

# Copy DX7 module files
cp ./src/modules/dx7/module.json ./build/modules/dx7/
cp ./src/modules/dx7/ui.js ./build/modules/dx7/
[ -f ./src/modules/dx7/patches.syx ] && cp ./src/modules/dx7/patches.syx ./build/modules/dx7/

# Copy JV-880 module files (if present - external repo)
if [ -d "src/modules/jv880" ]; then
    cp ./src/modules/jv880/module.json ./build/modules/jv880/
    cp ./src/modules/jv880/ui.js ./build/modules/jv880/
fi

# Copy M8 module files
cp ./src/modules/m8/module.json ./build/modules/m8/
cp ./src/modules/m8/ui.js ./build/modules/m8/

# Copy Controller module files
cp ./src/modules/controller/module.json ./build/modules/controller/
cp ./src/modules/controller/ui.js ./build/modules/controller/

# Copy Signal Chain module files
cp ./src/modules/chain/module.json ./build/modules/chain/
cp ./src/modules/chain/ui.js ./build/modules/chain/
mkdir -p ./build/modules/chain/midi_fx/
cp ./src/modules/chain/midi_fx/*.mjs ./build/modules/chain/midi_fx/ 2>/dev/null || true
mkdir -p ./build/modules/chain/patches/
cp ./src/modules/chain/patches/*.json ./build/modules/chain/patches/ 2>/dev/null || true

echo "Build complete!"
echo "Host binary: build/move-anything"
echo "Modules: build/modules/"
