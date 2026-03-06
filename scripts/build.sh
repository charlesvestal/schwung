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

    # Fetch Move Manual on host (Docker has no network access)
    # Skip if already cached (delete .cache/move_manual.json to force refresh)
    if [ -f ".cache/move_manual.json" ]; then
        echo "Move Manual cached ($(wc -c < .cache/move_manual.json) bytes)"
    else
        echo "Fetching Move Manual..."
        if ./scripts/fetch_move_manual.sh 2>/dev/null && [ -f ".cache/move_manual.json" ]; then
            echo "Move Manual fetched ($(wc -c < .cache/move_manual.json) bytes)"
        else
            echo "Warning: Could not fetch Move Manual"
        fi
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
if [ "${BUILD_VERBOSE:-0}" = "1" ]; then
    set -x
fi

cd "$REPO_ROOT"

# Incremental build helper: skip compilation if target is newer than all sources
needs_rebuild() {
    local target="$1"; shift
    [ ! -f "$target" ] && return 0
    for src in "$@"; do
        [ "$src" -nt "$target" ] && return 0
    done
    return 1
}

SCREEN_READER_ENABLED=1
if [ "$DISABLE_SCREEN_READER" = "1" ]; then
    SCREEN_READER_ENABLED=0
fi

    if [ "$SCREEN_READER_ENABLED" = "1" ]; then
        missing_deps=0
        for dep in \
            /usr/include/dbus-1.0/dbus/dbus.h \
            /usr/lib/aarch64-linux-gnu/dbus-1.0/include/dbus/dbus-arch-deps.h \
            /usr/include/espeak-ng/speak_lib.h \
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
    CC="${CROSS_PREFIX}gcc" AR="${CROSS_PREFIX}ar" make -C ./libs/quickjs/quickjs-2025-04-26 libquickjs.a
fi

# Prepare build directories (incremental: no clean unless explicitly requested)
mkdir -p ./build/
mkdir -p ./build/host/
mkdir -p ./build/shared/
mkdir -p ./build/shadow/
mkdir -p ./build/bin/
mkdir -p ./build/lib/
mkdir -p ./build/licenses/
mkdir -p ./build/modules/chain/
mkdir -p ./build/modules/audio_fx/freeverb/
mkdir -p ./build/modules/midi_fx/chord/
mkdir -p ./build/modules/midi_fx/arp/
mkdir -p ./build/modules/midi_fx/velocity_scale/
mkdir -p ./build/modules/sound_generators/linein/
mkdir -p ./build/modules/tools/wav-player/

# Generate bitmap font for host display (single source of truth: scripts/generate_font.py)
if needs_rebuild build/host/font.png scripts/generate_font.py; then
    echo "Generating host bitmap font..."
    python3 scripts/generate_font.py --deploy-png build/host/font.png
else
    echo "Skipping font generation (up to date)"
fi

if [ "$SCREEN_READER_ENABLED" = "1" ]; then
    echo "Screen reader build: enabled (dual engine: eSpeak-NG + Flite)"
    SHIM_TTS_SRC="src/host/tts_engine_dispatch.c src/host/tts_engine_espeak.c src/host/tts_engine_flite.c"
    SHIM_DEFINES="-DENABLE_SCREEN_READER=1"
    SHIM_INCLUDES="-Isrc -I/usr/include -I/usr/include/dbus-1.0 -I/usr/lib/aarch64-linux-gnu/dbus-1.0/include -I/usr/include/flite"
    SHIM_LIBS="-L/usr/lib/aarch64-linux-gnu -ldl -lrt -lpthread -ldbus-1 -lsystemd -lm -lespeak-ng -lflite -lflite_cmu_us_kal -lflite_usenglish -lflite_cmulex"
else
    echo "Screen reader build: disabled"
    SHIM_TTS_SRC="src/host/tts_engine_stub.c"
    SHIM_DEFINES="-DENABLE_SCREEN_READER=0"
    SHIM_INCLUDES="-Isrc -I/usr/include"
    SHIM_LIBS="-ldl -lrt -lpthread -lm"
fi

# Build host with module manager and settings
if needs_rebuild build/move-anything \
    src/move_anything.c src/host/module_manager.c src/host/settings.c src/host/unified_log.c \
    src/host/module_manager.h src/host/settings.h src/host/plugin_api_v1.h src/host/unified_log.h; then
    echo "Building host..."
    "${CROSS_PREFIX}gcc" -g -O3 \
        src/move_anything.c \
        src/host/module_manager.c \
        src/host/settings.c \
        src/host/unified_log.c \
        -o build/move-anything \
        -Isrc -Isrc/lib \
        -Ilibs/quickjs/quickjs-2025-04-26 \
        -Llibs/quickjs/quickjs-2025-04-26 \
        -lquickjs -lm -ldl -lrt -lpthread
else
    echo "Skipping host (up to date)"
fi

# Build shim (with shared memory support for shadow instrument)
if needs_rebuild build/move-anything-shim.so \
    src/move_anything_shim.c \
    src/host/shadow_sampler.c src/host/shadow_set_pages.c src/host/shadow_dbus.c \
    src/host/shadow_chain_mgmt.c src/host/shadow_link_audio.c src/host/shadow_process.c \
    src/host/shadow_resample.c src/host/shadow_overlay.c src/host/shadow_pin_scanner.c \
    src/host/shadow_led_queue.c src/host/shadow_fd_trace.c src/host/shadow_state.c \
    src/host/shadow_midi.c src/host/unified_log.c \
    $SHIM_TTS_SRC \
    src/host/shadow_constants.h src/host/shadow_midi.h src/host/shadow_sampler.h \
    src/host/shadow_set_pages.h src/host/shadow_dbus.h src/host/shadow_chain_mgmt.h \
    src/host/shadow_chain_types.h src/host/shadow_link_audio.h src/host/shadow_process.h \
    src/host/shadow_resample.h src/host/shadow_overlay.h src/host/shadow_pin_scanner.h \
    src/host/shadow_led_queue.h src/host/shadow_fd_trace.h src/host/shadow_state.h \
    src/host/plugin_api_v1.h src/host/unified_log.h src/host/tts_engine.h \
    src/host/link_audio.h; then
    echo "Building shim..."
    "${CROSS_PREFIX}gcc" -g3 -shared -fPIC \
        -o build/move-anything-shim.so \
        src/move_anything_shim.c \
        src/host/shadow_sampler.c \
        src/host/shadow_set_pages.c \
        src/host/shadow_dbus.c \
        src/host/shadow_chain_mgmt.c \
        src/host/shadow_link_audio.c \
        src/host/shadow_process.c \
        src/host/shadow_resample.c \
        src/host/shadow_overlay.c \
        src/host/shadow_pin_scanner.c \
        src/host/shadow_led_queue.c \
        src/host/shadow_fd_trace.c \
        src/host/shadow_state.c \
        src/host/shadow_midi.c \
        src/host/unified_log.c \
        $SHIM_TTS_SRC \
        $SHIM_DEFINES \
        $SHIM_INCLUDES \
        $SHIM_LIBS
else
    echo "Skipping shim (up to date)"
fi

# Build web shim (tiny LD_PRELOAD for MoveWebService PIN challenge detection)
if needs_rebuild build/move-anything-web-shim.so \
    src/host/web_shim.c src/host/unified_log.c src/host/unified_log.h; then
    echo "Building web shim..."
    "${CROSS_PREFIX}gcc" -g -shared -fPIC \
        -o build/move-anything-web-shim.so \
        src/host/web_shim.c \
        src/host/unified_log.c \
        -Isrc -Isrc/host \
        -ldl -lrt
else
    echo "Skipping web shim (up to date)"
fi

if needs_rebuild build/unified-log \
    src/host/unified_log_cli.c src/host/unified_log.c src/host/unified_log.h; then
    echo "Building unified log CLI..."
    "${CROSS_PREFIX}gcc" -g -O3 \
        src/host/unified_log_cli.c \
        src/host/unified_log.c \
        -o build/unified-log \
        -Isrc -Isrc/host
else
    echo "Skipping unified log CLI (up to date)"
fi

# Build Shadow Instrument POC (reference example - not used in production)
if needs_rebuild build/shadow/shadow_poc \
    examples/shadow_poc.c src/host/shadow_constants.h; then
    echo "Building Shadow POC..."
    "${CROSS_PREFIX}gcc" -g -O3 \
        examples/shadow_poc.c \
        -o build/shadow/shadow_poc \
        -Isrc -Isrc/host \
        -lm -ldl -lrt
else
    echo "Skipping Shadow POC (up to date)"
fi

# Build Shadow UI host (uses shared display bindings from js_display.c)
if needs_rebuild build/shadow/shadow_ui \
    src/shadow/shadow_ui.c src/host/js_display.c src/host/unified_log.c \
    src/host/js_display.h src/host/shadow_constants.h src/host/unified_log.h; then
    echo "Building Shadow UI..."
    "${CROSS_PREFIX}gcc" -g -O3 \
        src/shadow/shadow_ui.c \
        src/host/js_display.c \
        src/host/unified_log.c \
        -o build/shadow/shadow_ui \
        -Isrc -Isrc/lib \
        -Ilibs/quickjs/quickjs-2025-04-26 \
        -Llibs/quickjs/quickjs-2025-04-26 \
        -lquickjs -lm -ldl -lrt -lpthread
else
    echo "Skipping Shadow UI (up to date)"
fi

# Build Link Audio subscriber (C++17, requires Link SDK)
if [ -d "./libs/link/include/ableton" ]; then
    if needs_rebuild build/link-subscriber \
        src/host/link_subscriber.cpp src/host/arc4random_compat.c src/host/unified_log.c \
        src/host/link_audio.h src/host/unified_log.h src/host/shadow_constants.h; then
        echo "Building Link Audio subscriber..."
        # Build arc4random compat shim (Move's glibc 2.34 lacks arc4random from 2.36)
        "${CROSS_PREFIX}gcc" -c -g -O0 \
            src/host/arc4random_compat.c \
            -o build/arc4random_compat.o
        "${CROSS_PREFIX}gcc" -c -g -O3 \
            src/host/unified_log.c \
            -o build/unified_log.o \
            -Isrc -Isrc/host
        "${CROSS_PREFIX}g++" -std=c++17 -O3 -DNDEBUG \
            -DLINK_PLATFORM_UNIX=1 \
            -DLINK_PLATFORM_LINUX=1 \
            -Wno-multichar \
            -I./libs/link/include \
            -I./libs/link/modules/asio-standalone/asio/include \
            -Isrc -Isrc/host \
            src/host/link_subscriber.cpp \
            build/arc4random_compat.o \
            build/unified_log.o \
            -o build/link-subscriber \
            -lpthread -lrt -latomic \
            -static-libstdc++ \
            -Wl,--wrap=arc4random
        echo "Link Audio subscriber built"
    else
        echo "Skipping Link Audio subscriber (up to date)"
    fi
else
    echo "Warning: Link SDK not found at libs/link/, skipping link-subscriber"
fi

# Build MIDI inject test tool
if needs_rebuild build/bin/midi_inject_test \
    tests/shadow/midi_inject_test.c src/host/shadow_constants.h; then
    echo "Building MIDI inject test tool..."
    "${CROSS_PREFIX}gcc" -g -O3 \
        tests/shadow/midi_inject_test.c \
        -o build/bin/midi_inject_test \
        -Isrc \
        -lrt || echo "Warning: midi_inject_test build failed"
else
    echo "Skipping MIDI inject test (up to date)"
fi


# Always bundle TTS runtime libraries and data (even when screen reader is compiled
# as disabled) so the screen reader can be enabled at runtime without rebuilding.
# Skip if already bundled (libs don't change between builds).
if [ ! -f ./build/lib/.tts_bundled ]; then
    echo "Bundling TTS runtime libraries..."

    # eSpeak-NG libraries
    cp -L /usr/lib/aarch64-linux-gnu/libespeak-ng.so.* ./build/lib/ 2>/dev/null || true
    # libsonic (needed by eSpeak-NG for time-stretching/pitch-shifting)
    cp -L /usr/lib/aarch64-linux-gnu/libsonic.so.* ./build/lib/ 2>/dev/null || true

    # Flite libraries
    cp -L /usr/lib/aarch64-linux-gnu/libflite.so.* ./build/lib/ 2>/dev/null || true
    cp -L /usr/lib/aarch64-linux-gnu/libflite_cmu_us_kal.so.* ./build/lib/ 2>/dev/null || true
    cp -L /usr/lib/aarch64-linux-gnu/libflite_usenglish.so.* ./build/lib/ 2>/dev/null || true
    cp -L /usr/lib/aarch64-linux-gnu/libflite_cmulex.so.* ./build/lib/ 2>/dev/null || true

    # eSpeak-NG data (English only, ~1.6MB instead of ~13MB)
    echo "Bundling eSpeak-NG data files..."
    ESPEAK_SRC=""
    if [ -d /usr/lib/aarch64-linux-gnu/espeak-ng-data ]; then
        ESPEAK_SRC=/usr/lib/aarch64-linux-gnu/espeak-ng-data
    elif [ -d /usr/share/espeak-ng-data ]; then
        ESPEAK_SRC=/usr/share/espeak-ng-data
    fi

    if [ -n "$ESPEAK_SRC" ]; then
        mkdir -p ./build/espeak-ng-data/
        cp "$ESPEAK_SRC"/phontab "$ESPEAK_SRC"/phonindex "$ESPEAK_SRC"/phondata ./build/espeak-ng-data/
        cp "$ESPEAK_SRC"/phondata-manifest ./build/espeak-ng-data/ 2>/dev/null || true
        cp "$ESPEAK_SRC"/intonations ./build/espeak-ng-data/
        cp "$ESPEAK_SRC"/en_dict ./build/espeak-ng-data/
        cp -r "$ESPEAK_SRC"/voices ./build/espeak-ng-data/
        mkdir -p ./build/espeak-ng-data/lang/gmw/
        cp "$ESPEAK_SRC"/lang/gmw/en* ./build/espeak-ng-data/lang/gmw/ 2>/dev/null || true
    else
        echo "Warning: eSpeak-NG data directory not found"
    fi

    # Verify eSpeak-NG bundle if script is available
    if [ -f ./scripts/verify-espeak-bundle.sh ]; then
        ./scripts/verify-espeak-bundle.sh ./build/lib ./build/espeak-ng-data || true
    fi

    # License files
    cp /usr/share/doc/libespeak-ng1/copyright ./build/licenses/ESPEAK_NG_LICENSE.txt 2>/dev/null || true
    cp /usr/share/doc/libflite1/copyright ./build/licenses/FLITE_LICENSE.txt 2>/dev/null || true

    touch ./build/lib/.tts_bundled
else
    echo "Skipping TTS bundle (already present)"
fi

# pcaudio stub (satisfies eSpeak-NG's libpcaudio symbols without pulling in
# the full libpcaudio->libpulse->libX11 dependency chain)
if needs_rebuild build/lib/libpcaudio.so.0 src/host/pcaudio_stub.c; then
    echo "Building pcaudio stub library..."
    "${CROSS_PREFIX}gcc" -shared -fPIC -o ./build/lib/libpcaudio.so.0 src/host/pcaudio_stub.c
else
    echo "Skipping pcaudio stub (up to date)"
fi

echo "Building Signal Chain module..."

# Build Signal Chain DSP plugin
if needs_rebuild build/modules/chain/dsp.so \
    src/modules/chain/dsp/chain_host.c src/host/unified_log.c \
    src/host/unified_log.h src/host/plugin_api_v1.h src/host/audio_fx_api_v1.h \
    src/host/audio_fx_api_v2.h src/host/midi_fx_api_v1.h; then
    echo "Building chain DSP..."
    "${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
        src/modules/chain/dsp/chain_host.c \
        src/host/unified_log.c \
        -o build/modules/chain/dsp.so \
        -Isrc \
        -lm -ldl -lpthread
else
    echo "Skipping chain DSP (up to date)"
fi

echo "Building Audio FX plugins..."

# Build Freeverb audio FX
if needs_rebuild build/modules/audio_fx/freeverb/freeverb.so \
    src/modules/audio_fx/freeverb/freeverb.c src/host/audio_fx_api_v1.h; then
    echo "Building freeverb..."
    "${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
        src/modules/audio_fx/freeverb/freeverb.c \
        -o build/modules/audio_fx/freeverb/freeverb.so \
        -Isrc \
        -lm
else
    echo "Skipping freeverb (up to date)"
fi

echo "Building MIDI FX plugins..."

# Build Chord MIDI FX
if needs_rebuild build/modules/midi_fx/chord/dsp.so \
    src/modules/midi_fx/chord/dsp/chord.c src/host/midi_fx_api_v1.h; then
    echo "Building chord MIDI FX..."
    "${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
        src/modules/midi_fx/chord/dsp/chord.c \
        -o build/modules/midi_fx/chord/dsp.so \
        -Isrc
else
    echo "Skipping chord MIDI FX (up to date)"
fi

# Build Arpeggiator MIDI FX
if needs_rebuild build/modules/midi_fx/arp/dsp.so \
    src/modules/midi_fx/arp/dsp/arp.c src/host/midi_fx_api_v1.h; then
    echo "Building arp MIDI FX..."
    "${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
        src/modules/midi_fx/arp/dsp/arp.c \
        -o build/modules/midi_fx/arp/dsp.so \
        -Isrc
else
    echo "Skipping arp MIDI FX (up to date)"
fi

# Build Velocity Scale MIDI FX
if needs_rebuild build/modules/midi_fx/velocity_scale/dsp.so \
    src/modules/midi_fx/velocity_scale/dsp/velocity_scale.c src/host/midi_fx_api_v1.h; then
    echo "Building velocity scale MIDI FX..."
    "${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
        src/modules/midi_fx/velocity_scale/dsp/velocity_scale.c \
        -o build/modules/midi_fx/velocity_scale/dsp.so \
        -Isrc
else
    echo "Skipping velocity scale MIDI FX (up to date)"
fi

echo "Building Sound Generator plugins..."

# Build Line In sound generator
if needs_rebuild build/modules/sound_generators/linein/dsp.so \
    src/modules/sound_generators/linein/linein.c src/host/plugin_api_v1.h; then
    echo "Building line-in generator..."
    "${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
        src/modules/sound_generators/linein/linein.c \
        -o build/modules/sound_generators/linein/dsp.so \
        -Isrc \
        -lm
else
    echo "Skipping line-in generator (up to date)"
fi

# Build WAV Player tool DSP
if needs_rebuild build/modules/tools/wav-player/dsp.so \
    src/modules/tools/wav-player/wav_player.c src/host/plugin_api_v1.h; then
    echo "Building WAV Player tool DSP..."
    "${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
        src/modules/tools/wav-player/wav_player.c \
        -o build/modules/tools/wav-player/dsp.so \
        -Isrc
else
    echo "Skipping WAV Player tool DSP (up to date)"
fi

# Copy shared utilities (only if source is newer)
for f in ./src/shared/*.mjs; do
    cp -u "$f" ./build/shared/
done
cp -u ./src/shared/*.json ./build/shared/ 2>/dev/null || true

# Bundle Move Manual (fetched on host before Docker, or from prior build)
if [ -f ".cache/move_manual.json" ]; then
    cp -u .cache/move_manual.json ./build/shared/move_manual_bundled.json
    echo "Bundled Move Manual"
else
    echo "Warning: .cache/move_manual.json not found - no bundled manual"
fi

# Copy host files (only if source is newer)
cp -u ./src/host/menu_ui.js ./build/host/
cp -u ./src/host/*.mjs ./build/host/ 2>/dev/null || true
# Derive version: prefer src/host/version.txt (set by CI), fall back to git tag
SRC_VERSION=$(cat ./src/host/version.txt 2>/dev/null | tr -d '[:space:]')
GIT_VERSION=$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//')
if [ -n "$SRC_VERSION" ]; then
    BUILD_VERSION="$SRC_VERSION"
elif [ -n "$GIT_VERSION" ]; then
    BUILD_VERSION="$GIT_VERSION"
else
    BUILD_VERSION="0.0.0"
fi
if [ ! -f ./build/host/version.txt ] || [ "$(cat ./build/host/version.txt)" != "$BUILD_VERSION" ]; then
    echo "$BUILD_VERSION" > ./build/host/version.txt
fi

# Build display server (live display SSE streaming to browser)
if needs_rebuild build/display-server \
    src/host/display_server.c src/host/unified_log.c src/host/unified_log.h; then
    echo "Building display server..."
    "${CROSS_PREFIX}gcc" -g -O3 \
        src/host/display_server.c \
        src/host/unified_log.c \
        -o build/display-server \
        -Isrc -Isrc/host \
        -lrt
else
    echo "Skipping display server (up to date)"
fi

# Copy shadow UI files (only if source is newer)
cp -u ./src/shadow/shadow_ui.js ./build/shadow/
cp -u ./src/shadow/*.mjs ./build/shadow/ 2>/dev/null || true

# Copy scripts and assets (only if source is newer)
cp -u ./src/shim-entrypoint.sh ./build/
cp -u ./src/restart-move.sh ./build/ 2>/dev/null || true
cp -u ./src/start.sh ./build/ 2>/dev/null || true
cp -u ./src/stop.sh ./build/ 2>/dev/null || true

# Copy all module files (js, mjs, json, sh) - preserves directory structure
# Compiled .so files are built separately above
echo "Copying module files..."
find ./src/modules -type f \( -name "*.js" -o -name "*.mjs" -o -name "*.json" -o -name "*.sh" \) | while IFS= read -r src; do
    dest="./build/${src#./src/}"
    mkdir -p "$(dirname "$dest")"
    cp -u "$src" "$dest"
done
# Make shell scripts in modules executable
find ./build/modules -type f -name "*.sh" -exec chmod +x {} \;

# Copy patches directory (only if source is newer)
mkdir -p ./build/patches
cp -u ./src/patches/*.json ./build/patches/ 2>/dev/null || true

# Copy track presets (only if source is newer)
mkdir -p ./build/presets/track_presets
cp -u ./src/presets/track_presets/*.json ./build/presets/track_presets/ 2>/dev/null || true

# Copy curl binary for store module (if present)
if [ -f "./libs/curl/curl" ]; then
    mkdir -p ./build/bin/
    cp -u ./libs/curl/curl ./build/bin/
    echo "Bundled curl binary"
else
    echo "Warning: libs/curl/curl not found - store module will not work without it"
fi

# eSpeak-NG data directory is copied to build/espeak-ng-data/ above

echo "Build complete!"
echo "Host binary: build/move-anything"
echo "Modules: build/modules/"
