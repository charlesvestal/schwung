#!/bin/bash
# Install Nam A2 module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/nam-a2" ]; then
    echo "Error: dist/nam-a2 not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing Nam A2 Module ==="

# Deploy to Move - audio_fx subdirectory
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/schwung/modules/audio_fx/nam-a2/models /data/UserData/schwung/modules/audio_fx/nam-a2/cabs"
scp -r dist/nam-a2/* ableton@move.local:/data/UserData/schwung/modules/audio_fx/nam-a2/

# Set permissions so Schwung Manager / Module Store can update later
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/schwung/modules/audio_fx/nam-a2"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/schwung/modules/audio_fx/nam-a2/"
echo ""
echo "Place .nam model files in: /data/UserData/schwung/modules/audio_fx/nam-a2/models/"
echo "Place .wav cab IR files in: /data/UserData/schwung/modules/audio_fx/nam-a2/cabs/"
echo "Restart Schwung (or call host_rescan_modules()) to load the new module."
