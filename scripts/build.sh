#!/usr/bin/env bash
# Build Move Anything for Ableton Move (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"
DISABLE_SCREEN_READER="${DISABLE_SCREEN_READER:-0}"
REBUILD_DOCKER_IMAGE="${REBUILD_DOCKER_IMAGE:-0}"
REQUIRE_SCREEN_READER="${REQUIRE_SCREEN_READER:-0}"
BOOTSTRAP_SCRIPT="./scripts/bootstrap-build-deps.sh"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Move Anything Build (via Docker) ==="
    echo ""

    # Build/rebuild Docker image if needed
    if [ "$REBUILD_DOCKER_IMAGE" = "1" ]; then
        echo "Rebuilding Docker image..."
        docker build --pull -t "$IMAGE_NAME" "$REPO_ROOT"
        echo ""
    elif ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -e DISABLE_SCREEN_READER="$DISABLE_SCREEN_READER" \
        -e REQUIRE_SCREEN_READER="$REQUIRE_SCREEN_READER" \
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

SCREEN_READER_ENABLED=1
if [ "$DISABLE_SCREEN_READER" = "1" ]; then
    SCREEN_READER_ENABLED=0
fi

    if [ "$SCREEN_READER_ENABLED" = "1" ]; then
        missing_deps=0
        for dep in \
            /usr/include/dbus-1.0/dbus/dbus.h \
            /usr/lib/aarch64-linux-gnu/dbus-1.0/include/dbus/dbus-arch-deps.h \
        /usr/include/flite/flite.h; do
        if [ ! -f "$dep" ]; then
            echo "Missing screen reader dependency: $dep"
            missing_deps=1
        fi
    done

        if [ "$missing_deps" -ne 0 ]; then
            if [ "$REQUIRE_SCREEN_READER" = "1" ]; then
                echo "Error: screen reader dependencies are required but missing"
                echo "Hint: run $BOOTSTRAP_SCRIPT"
                exit 1
            fi
            echo "Warning: screen reader dependencies not found, building without screen reader"
            echo "Hint: run $BOOTSTRAP_SCRIPT to build with screen reader support"
            SCREEN_READER_ENABLED=0
        fi
    fi

if ! command -v "${CROSS_PREFIX}gcc" >/dev/null 2>&1; then
    echo "Error: missing compiler '${CROSS_PREFIX}gcc'"
    echo "Hint: run $BOOTSTRAP_SCRIPT"
    exit 1
fi

if [ ! -f "./libs/quickjs/quickjs-2025-04-26/libquickjs.a" ]; then
    echo "QuickJS static library not found, building it..."
    make -C ./libs/quickjs/quickjs-2025-04-26 clean >/dev/null 2>&1 || true
    CC="${CROSS_PREFIX}gcc" make -C ./libs/quickjs/quickjs-2025-04-26 libquickjs.a
fi

# Clean and prepare
./scripts/clean.sh
mkdir -p ./build/
mkdir -p ./build/host/
mkdir -p ./build/shared/
# Module directories are created automatically when copying files
# External modules (sf2, dexed, m8, minijv, obxd, clap) are in separate repos

if [ "$SCREEN_READER_ENABLED" = "1" ]; then
    echo "Screen reader build: enabled"
    SHIM_TTS_SRC="src/host/tts_engine_flite.c"
    SHIM_DEFINES="-DENABLE_SCREEN_READER=1"
    SHIM_INCLUDES="-Isrc -I/usr/include -I/usr/include/dbus-1.0 -I/usr/lib/aarch64-linux-gnu/dbus-1.0/include"
    SHIM_LIBS="-L/usr/lib/aarch64-linux-gnu -ldl -lrt -lpthread -ldbus-1 -lsystemd -lm -lflite -lflite_cmu_us_kal -lflite_usenglish -lflite_cmulex"
else
    echo "Screen reader build: disabled"
    SHIM_TTS_SRC="src/host/tts_engine_stub.c"
    SHIM_DEFINES="-DENABLE_SCREEN_READER=0"
    SHIM_INCLUDES="-Isrc -I/usr/include"
    SHIM_LIBS="-ldl -lrt -lpthread -lm"
fi

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
# D-Bus/TTS are optional and can be disabled with DISABLE_SCREEN_READER=1
"${CROSS_PREFIX}gcc" -g3 -shared -fPIC \
    -o build/move-anything-shim.so \
    src/move_anything_shim.c \
    src/host/unified_log.c \
    "$SHIM_TTS_SRC" \
    $SHIM_DEFINES \
    $SHIM_INCLUDES \
    $SHIM_LIBS

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
# TTS settings are written to shared memory, actual TTS happens in shim
"${CROSS_PREFIX}gcc" -g -O3 \
    src/shadow/shadow_ui.c \
    src/host/js_display.c \
    src/host/unified_log.c \
    -o build/shadow/shadow_ui \
    -Isrc -Isrc/lib \
    -Ilibs/quickjs/quickjs-2025-04-26 \
    -Llibs/quickjs/quickjs-2025-04-26 \
    -lquickjs -lm -ldl -lrt

mkdir -p ./build/test/
if [ "$SCREEN_READER_ENABLED" = "1" ]; then
    echo "Building TTS test program..."

    # Build Flite test program for testing TTS - using dynamic linking
    "${CROSS_PREFIX}gcc" -g -O3 \
        test/test_flite.c \
        -o build/test/test_flite \
        -I/usr/include \
        -L/usr/lib/aarch64-linux-gnu \
        -lflite -lflite_cmu_us_kal -lflite_usenglish -lflite_cmulex \
        -lm -lpthread || echo "Warning: TTS test build failed"

    echo "Copying Flite libraries for deployment..."

    # Copy Flite .so files to build/lib/ for deployment to Move
    mkdir -p ./build/lib/
    cp -L /usr/lib/aarch64-linux-gnu/libflite.so.* ./build/lib/ 2>/dev/null || true
    cp -L /usr/lib/aarch64-linux-gnu/libflite_cmu_us_kal.so.* ./build/lib/ 2>/dev/null || true
    cp -L /usr/lib/aarch64-linux-gnu/libflite_usenglish.so.* ./build/lib/ 2>/dev/null || true
    cp -L /usr/lib/aarch64-linux-gnu/libflite_cmulex.so.* ./build/lib/ 2>/dev/null || true
    ./scripts/verify-flite-bundle.sh ./build/lib

    # Copy Flite copyright notice (required by BSD-style license)
    mkdir -p ./build/licenses/
    cp /usr/share/doc/libflite1/copyright ./build/licenses/FLITE_LICENSE.txt 2>/dev/null || echo "Warning: Flite license file not found"
else
    echo "Skipping TTS/Flite artifacts (screen reader disabled)"
fi

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
cp ./src/restart-move.sh ./build/ 2>/dev/null || true
cp ./src/start.sh ./build/ 2>/dev/null || true
cp ./src/stop.sh ./build/ 2>/dev/null || true

# Copy all module files (js, mjs, json) - preserves directory structure
# Compiled .so files are built separately above
echo "Copying module files..."
find ./src/modules -type f \( -name "*.js" -o -name "*.mjs" -o -name "*.json" \) | while IFS= read -r src; do
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

# Flite voice data is built into the Flite libraries - no separate data directory needed

echo "Build complete!"
echo "Host binary: build/move-anything"
echo "Modules: build/modules/"
