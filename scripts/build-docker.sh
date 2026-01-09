#!/bin/bash
# Build Move Anything using Docker (cross-compiles for Ableton Move)
# Uses docker cp to avoid OrbStack/Docker volume mount corruption issues
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CONTAINER_NAME="move-anything-builder-$$"

echo "=== Move Anything Docker Build ==="
echo ""

# Remove any stale container
docker rm -f "$CONTAINER_NAME" 2>/dev/null || true

echo "Building inside Docker container..."
docker run --name "$CONTAINER_NAME" \
    -v "$REPO_ROOT:/src:ro" \
    debian:bookworm bash -c "
set -e

# Install build dependencies
apt-get update -qq
apt-get install -y -qq gcc-aarch64-linux-gnu g++-aarch64-linux-gnu make >/dev/null 2>&1

# Copy source to build directory (avoids volume mount write issues)
mkdir -p /build
cp -r /src/* /build/

# Build QuickJS
echo 'Building QuickJS...'
cd /build/libs/quickjs/quickjs-2025-04-26
make clean >/dev/null 2>&1 || true
CC=aarch64-linux-gnu-gcc make libquickjs.a >/dev/null 2>&1
echo 'QuickJS built successfully'

# Build Move Anything
echo 'Building Move Anything...'
cd /build
CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh

# Package
echo 'Packaging...'
./scripts/package.sh

echo ''
echo '=== Build Artifacts ==='
ls -lh /build/build/move-anything /build/build/move-anything-shim.so
echo ''
echo '=== Package Created ==='
ls -lh /build/move-anything.tar.gz
"

# Copy tarball out using docker cp (bypasses volume mount issues)
echo ""
echo "Extracting build artifact..."
docker cp "$CONTAINER_NAME:/build/move-anything.tar.gz" "$REPO_ROOT/move-anything.tar.gz"

# Cleanup container
docker rm "$CONTAINER_NAME" >/dev/null

echo ""
echo "=== Done ==="
echo "Output: $REPO_ROOT/move-anything.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh local"
