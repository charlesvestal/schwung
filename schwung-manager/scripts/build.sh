#!/usr/bin/env bash
# Build schwung-manager for Ableton Move (ARM64)
#
# Requires Go installed on the build machine. No Docker needed since
# schwung-manager is pure Go (no cgo).
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MANAGER_ROOT="$(dirname "$SCRIPT_DIR")"
REPO_ROOT="$(dirname "$MANAGER_ROOT")"

cd "$MANAGER_ROOT"

echo "=== Building schwung-manager ==="

# Cross-compile for Move (aarch64 Linux)
GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build \
    -ldflags="-s -w" \
    -o schwung-manager-arm64 \
    .

echo "Built: schwung-manager-arm64 ($(wc -c < schwung-manager-arm64 | tr -d ' ') bytes)"

# Copy to main build directory if it exists (for packaging into schwung.tar.gz)
if [ -d "$REPO_ROOT/build" ]; then
    cp schwung-manager-arm64 "$REPO_ROOT/build/schwung-manager"
    echo "Copied to build/schwung-manager"
fi

echo "Done."
