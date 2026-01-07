#!/usr/bin/env bash
set -x

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

rm -fr ./dist/
mkdir -p ./dist/move-anything/

cp -rv \
    ./build/move-anything \
    ./build/move-anything-shim.so \
    ./build/shim-entrypoint.sh \
    ./build/start.sh \
    ./build/stop.sh \
    ./build/font.png \
    ./build/font.png.dat \
    ./build/host \
    ./build/shared \
    ./build/modules \
    ./dist/move-anything/

# Strip binaries
strip ./dist/move-anything/move-anything 2>/dev/null || true
strip ./dist/move-anything/move-anything-shim.so 2>/dev/null || true
find ./dist/move-anything/modules -name "*.so" -exec strip {} \; 2>/dev/null || true

tar -C ./dist/ -czvf move-anything.tar.gz move-anything
