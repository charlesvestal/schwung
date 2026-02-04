#!/usr/bin/env bash
set -x

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

# Create tarball directly from build directory to avoid cp issues with ExtFS
# The build/ directory already has the correct structure

# Create the tarball with the correct directory name
cd ./build

# Build list of items to package
ITEMS="./move-anything ./move-anything-shim.so ./shim-entrypoint.sh ./restart-move.sh ./start.sh ./stop.sh ./host ./shared ./modules ./shadow ./patches"

# Add bin directory if it exists (contains curl for store module)
if [ -d "./bin" ]; then
    ITEMS="$ITEMS ./bin"
fi

tar -czvf ../move-anything.tar.gz \
    --transform 's,^\.,move-anything,' \
    $ITEMS
