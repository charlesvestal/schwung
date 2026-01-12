#!/usr/bin/env bash
set -x

# Get repo root (parent of scripts/)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

# Clean and prepare
./scripts/clean.sh
mkdir -p ./build/
mkdir -p ./build/host/
mkdir -p ./build/shared/
mkdir -p ./build/modules/seqomd/

echo "Building host..."

# Build host with module manager
"${CROSS_PREFIX}gcc" -g -O3 \
    src/move_anything.c \
    src/host/module_manager.c \
    -o build/move-anything \
    -Isrc -Isrc/lib \
    -Ilibs/quickjs/quickjs-2025-04-26 \
    -Llibs/quickjs/quickjs-2025-04-26 \
    -lquickjs -lm -ldl

# Build shim
"${CROSS_PREFIX}gcc" -g3 -shared -fPIC \
    -o build/move-anything-shim.so \
    src/move_anything_shim.c -ldl

# Copy shared utilities
cp ./src/shared/*.mjs ./build/shared/

# Copy host files
cp ./src/host/menu_ui.js ./build/host/

# Copy scripts and assets
cp ./src/shim-entrypoint.sh ./build/
cp ./src/start.sh ./build/ 2>/dev/null || true
cp ./src/stop.sh ./build/ 2>/dev/null || true

echo "Building Sequencer module..."

# Build Sequencer DSP plugin
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/seqomd/dsp/seq_plugin.c \
    -o build/modules/seqomd/dsp.so \
    -Isrc \
    -lm

# Copy Sequencer module files
cp ./src/modules/seqomd/module.json ./build/modules/seqomd/
cp ./src/modules/seqomd/ui.js ./build/modules/seqomd/

# Copy seqomd lib and views (if they exist)
if [ -d ./src/modules/seqomd/lib ]; then
    mkdir -p ./build/modules/seqomd/lib/
    cp ./src/modules/seqomd/lib/*.js ./build/modules/seqomd/lib/
fi
if [ -d ./src/modules/seqomd/views ]; then
    cp -r ./src/modules/seqomd/views ./build/modules/seqomd/
fi

# Strip binaries to reduce size
echo "Stripping binaries..."
"${CROSS_PREFIX}strip" build/move-anything
"${CROSS_PREFIX}strip" build/move-anything-shim.so
"${CROSS_PREFIX}strip" build/modules/seqomd/dsp.so

echo "Build complete!"
echo "Host binary: build/move-anything"
echo "Modules: build/modules/"
