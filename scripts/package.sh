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
ITEMS="./move-anything ./move-anything-shim.so ./shim-entrypoint.sh ./start.sh ./stop.sh ./host ./shared ./modules ./shadow ./patches ./test"

# Add bin directory if it exists (contains curl for store module)
if [ -d "./bin" ]; then
    ITEMS="$ITEMS ./bin"
fi

# Add lib directory if it exists (contains Flite .so files for TTS)
if [ -d "./lib" ]; then
    ITEMS="$ITEMS ./lib"
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

tar -czvf ../move-anything.tar.gz \
    --transform 's,^\.,move-anything,' \
    $ITEMS
