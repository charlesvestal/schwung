#!/bin/bash
# Build Move Anything using Docker (cross-compiles for Ableton Move)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

echo "=== Move Anything Docker Build ==="
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
