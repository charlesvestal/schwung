#!/usr/bin/env bash
# Build Nam A2 module for Move Anything (ARM64)
#
# Two-phase build:
#   1. Build NeuralAudio as a static library via CMake cross-compilation
#   2. Compile nam_a2_plugin.cpp and link against libNeuralAudio.a
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-nam-a2-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Nam A2 Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building Nam A2 Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories. Wipe dist/nam-a2 first so stale files from a
# previous build (e.g. removed/renamed models) don't get repackaged.
mkdir -p build/neuralaudio
rm -rf dist/nam-a2
mkdir -p dist/nam-a2

# --- Phase 1: Build NeuralAudio static library via CMake ---
echo ""
echo "--- Phase 1: Building NeuralAudio static library ---"

# Create CMake toolchain file for cross-compilation
cat > build/aarch64-toolchain.cmake << 'TOOLCHAIN_EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
TOOLCHAIN_EOF

cmake -S deps/NeuralAudio -B build/neuralaudio \
    -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/build/aarch64-toolchain.cmake" \
    -DCMAKE_CXX_STANDARD=20 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-Ofast -march=armv8-a -mtune=cortex-a72 -DNDEBUG" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_UTILS=OFF \
    -DBUILD_NAMCORE=OFF \
    -DBUILD_STATIC_RTNEURAL=OFF \
    -DWAVENET_FRAMES=128 \
    -DBUFFER_PADDING=8

cmake --build build/neuralaudio -j"$(nproc)"

echo "NeuralAudio static library built."

# --- Phase 2: Compile Nam A2 plugin and link ---
echo ""
echo "--- Phase 2: Compiling Nam A2 plugin ---"

# Find the static libraries we need to link
NA_LIB="build/neuralaudio/NeuralAudio/libNeuralAudio.a"
RT_LIB=$(find build/neuralaudio -name "libRTNeural.a" | head -1)

# Newer NeuralAudio versions declare the NeuralAudio CMake target as an
# OBJECT library (no linkable .a of its own - just .o files under
# CMakeFiles/NeuralAudio.dir/) rather than a STATIC one. Archive those
# object files ourselves so Phase 2's direct g++ link line keeps working
# whichever form the pinned deps/NeuralAudio checkout uses.
if [ ! -f "$NA_LIB" ]; then
    NA_OBJDIR="build/neuralaudio/NeuralAudio/CMakeFiles/NeuralAudio.dir"
    NA_OBJS=$(find "$NA_OBJDIR" -name "*.o" 2>/dev/null)
    if [ -n "$NA_OBJS" ]; then
        echo "NeuralAudio built as an OBJECT library; archiving its .o files into $NA_LIB"
        ${CROSS_PREFIX}ar rcs "$NA_LIB" $NA_OBJS
    fi
fi

if [ ! -f "$NA_LIB" ]; then
    echo "ERROR: NeuralAudio library not found: $NA_LIB"
    find build/neuralaudio -name "*.a" 2>/dev/null
    exit 1
fi
if [ -z "$RT_LIB" ] || [ ! -f "$RT_LIB" ]; then
    echo "ERROR: RTNeural library not found"
    find build/neuralaudio -name "*.a" 2>/dev/null
    exit 1
fi
echo "Found NeuralAudio: $NA_LIB"
echo "Found RTNeural: $RT_LIB"

${CROSS_PREFIX}g++ -Ofast -shared -fPIC \
    -std=c++20 \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    -DNAM_SAMPLE_FLOAT \
    -DDSP_SAMPLE_FLOAT \
    -DLSTM_MATH=FastMath \
    -DWAVENET_MATH=FastMath \
    -DWAVENET_MAX_NUM_FRAMES=128 \
    -DLAYER_ARRAY_BUFFER_PADDING=8 \
    src/dsp/nam_a2_plugin.cpp \
    -o build/nam-a2.so \
    -Isrc/dsp \
    -Ideps/NeuralAudio \
    -Ideps/NeuralAudio/NeuralAudio \
    -Ideps/NeuralAudio/deps/RTNeural/modules/Eigen \
    -Ideps/NeuralAudio/deps/RTNeural/modules/json \
    -Ideps/NeuralAudio/deps/RTNeural/modules/json/single_include \
    -Ideps/NeuralAudio/deps/RTNeural \
    -Ideps/NeuralAudio/deps/math_approx/include \
    -Ideps/NeuralAudio/deps/RTNeural-NAM/wavenet \
    -Ideps/NeuralAudio/deps/NeuralAmpModelerCore \
    "$NA_LIB" \
    "$RT_LIB" \
    -lm -lpthread

echo "Plugin compiled: build/nam-a2.so"

# --- Package ---
echo ""
echo "--- Packaging ---"

cat src/module.json > dist/nam-a2/module.json
[ -f src/help.json ] && cat src/help.json > dist/nam-a2/help.json
cat build/nam-a2.so > dist/nam-a2/nam-a2.so
chmod +x dist/nam-a2/nam-a2.so

# Always create models/ and cabs/ in the tarball so the Module Store install
# lands the expected directory layout. Existing files are preserved on
# extract; tar only overwrites same-named files (these dirs are empty by
# default - no models/cabs are bundled).
mkdir -p dist/nam-a2/models dist/nam-a2/cabs

# find, not a glob: src/models and src/cabs normally hold only a .gitkeep
# placeholder (dotfile) so any bundled real models/cabs stay opt-in, and a
# bare `src/models/*` glob doesn't match dotfiles - it matches nothing, so
# on a .gitkeep-only tree the literal unexpanded pattern was passed to cp
# and it failed with "cannot stat 'src/models/*'".
if [ -d "src/models" ]; then
    find src/models -maxdepth 1 -type f ! -name '.gitkeep' -exec cp {} dist/nam-a2/models/ \;
fi
if [ -d "src/cabs" ]; then
    find src/cabs -maxdepth 1 -type f ! -name '.gitkeep' -exec cp {} dist/nam-a2/cabs/ \;
fi

# Create tarball for release
cd dist
tar -czvf nam-a2-module.tar.gz nam-a2/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/nam-a2/"
echo "Tarball: dist/nam-a2-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
