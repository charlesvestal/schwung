#!/usr/bin/env bash
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

# Create tarball directly from build directory to avoid cp issues with ExtFS
# The build/ directory already has the correct structure

# Create the tarball with the correct directory name
cd ./build

# Build list of items to package
# start.sh/stop.sh (standalone-host launchers) no longer ship — the
# standalone runtime never runs on device (shadow mode only).
ITEMS="./schwung ./schwung-shim.so ./move-anything ./move-anything-shim.so ./shim-entrypoint.sh ./restart-move.sh ./launch-standalone.sh ./host ./shared ./modules ./shadow ./patches ./presets ./unified-log ./scripts"

# Dev/test artifacts must not ride along in wholesale-packaged dirs.
rm -f ./shadow/shadow_poc ./bin/midi_inject_test

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

# Add schwung-manager if it was built
if [ -f "./schwung-manager" ]; then
    ITEMS="$ITEMS ./schwung-manager"
fi

if tar --version 2>/dev/null | grep -q GNU; then
    # Use POSIX format to avoid GNUSparseFile.0 entries that BusyBox tar
    # on Move cannot extract (can happen with Docker volume mounts)
    tar --format=posix -czf ../schwung.tar.gz \
        --transform 's,^\.,schwung,' \
        $ITEMS
else
    # bsdtar: force ustar format. bsdtar's default pax writer auto-detects
    # holes (SEEK_HOLE) and emits GNU-sparse entries that BusyBox tar on
    # Move extracts to literal GNUSparseFile.0/<name> paths, silently
    # leaving the real file stale on device (bit us 2026-06-12: a dsp.so
    # with a Docker-virtiofs hole). ustar cannot encode sparse, so files
    # are stored dense.
    tar --format ustar -czf ../schwung.tar.gz \
        -s ',^\.,schwung,' \
        $ITEMS
fi
