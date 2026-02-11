#!/usr/bin/env bash
# Build SEQOMD module for Move Anything (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== SEQOMD Module Build (via Docker) ==="
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
    echo "Output: $REPO_ROOT/seqomd-module.tar.gz"
    echo ""
    echo "To install on Move:"
    echo "  ./scripts/install.sh local"
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building SEQOMD Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories
mkdir -p build/
mkdir -p dist/seqomd

# Compile DSP plugin
echo "Compile DSP plugin..."
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/dsp/seq_plugin.c \
    src/dsp/midi.c \
    src/dsp/scheduler.c \
    src/dsp/transpose.c \
    src/dsp/scale.c \
    src/dsp/arpeggiator.c \
    src/dsp/track.c \
    src/dsp/params.c \
    -o build/dsp.so \
    -Isrc \
    -lm

# Copy all module files (js, mjs, json) - preserves directory structure
# Compiled .so files are built separately above
echo "Copying module files..."
find ./src/ -type f \( -name "*.js" -o -name "*.mjs" -o -name "*.json" \) | while read src; do
    dest="./dist/seqomd/${src#./src/}"
    mkdir -p "$(dirname "$dest")"
    cp "$src" "$dest"
done

# Copy files to dist (use cat to avoid ExtFS deallocation issues with Docker)
echo "Packaging..."
cat build/dsp.so > dist/seqomd/dsp.so
chmod +x dist/seqomd/dsp.so

# Create tarball for release
cd dist
tar -czvf seqomd-module.tar.gz seqomd/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/seqomd/"
echo "Tarball: dist/seqomd-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
