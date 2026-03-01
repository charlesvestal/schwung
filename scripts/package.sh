#!/usr/bin/env bash
set -eo pipefail
set -x

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

# Create tarball directly from build directory to avoid cp issues with ExtFS
# The build/ directory already has the correct structure

# Create the tarball with the correct directory name
cd ./build

# Build list of items to package
ITEMS="./move-anything ./move-anything-shim.so ./shim-entrypoint.sh ./restart-move.sh ./start.sh ./stop.sh ./host ./shared ./modules ./shadow ./patches ./presets ./test ./unified-log"

# Add bin directory if it exists (contains curl for store module)
if [ -d "./bin" ]; then
    ITEMS="$ITEMS ./bin"
fi

# Add lib directory if it exists (contains eSpeak-NG .so files for TTS)
if [ -d "./lib" ]; then
    ITEMS="$ITEMS ./lib"
fi

# Add eSpeak-NG data directory if it exists (phoneme data, voice definitions)
if [ -d "./espeak-ng-data" ]; then
    ITEMS="$ITEMS ./espeak-ng-data"
fi

# Add licenses directory if it exists (third-party license files)
if [ -d "./licenses" ]; then
    ITEMS="$ITEMS ./licenses"
fi

# Add link-subscriber if it was built
if [ -f "./link-subscriber" ]; then
    ITEMS="$ITEMS ./link-subscriber"
fi

# Add display-server if it was built
if [ -f "./display-server" ]; then
    ITEMS="$ITEMS ./display-server"
fi

# Add web shim if it was built (PIN challenge TTS readout for MoveWebService)
if [ -f "./move-anything-web-shim.so" ]; then
    ITEMS="$ITEMS ./move-anything-web-shim.so"
fi

if tar --version 2>/dev/null | grep -q GNU; then
    tar -czvf ../move-anything.tar.gz \
        --transform 's,^\.,move-anything,' \
        $ITEMS
else
    tar -czvf ../move-anything.tar.gz \
        -s ',^\.,move-anything,' \
        $ITEMS
fi
