#!/usr/bin/env bash
set -x

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

# Create tarball directly from build directory to avoid cp issues with ExtFS
# The build/ directory already has the correct structure

# Create the tarball with the correct directory name
cd ./build
tar -czvf ../move-anything.tar.gz \
    --transform 's,^\.,move-anything,' \
    ./move-anything \
    ./move-anything-shim.so \
    ./shim-entrypoint.sh \
    ./start.sh \
    ./stop.sh \
    ./host \
    ./shared \
    ./modules
