#!/bin/bash
# Install SEQOMD module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/seqomd" ]; then
    echo "Error: dist/seqomd not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing SEQOMD Module ==="

# Deploy to Move - utilities subdirectory
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/other/seqomd"
scp -r dist/seqomd/* ableton@move.local:/data/UserData/move-anything/modules/other/seqomd/

# Set permissions so Module Store can update later
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/move-anything/modules/other/seqomd"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/move-anything/modules/other/seqomd/"
echo ""
echo "Restart Move Anything to load the new module."
