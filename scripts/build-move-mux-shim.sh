#!/usr/bin/env bash
# Cross-compile move-mux-shim.so for Move (aarch64 Linux).
#
# The full host build (scripts/build.sh) is a giant pipeline with TTS deps,
# QuickJS, modules, etc. This shim is a 200-line .so for the dual-Move
# instance experiment and only needs the cross-compiler — no external libs.
# Reuses the existing schwung-builder Docker image (which already has
# gcc-aarch64-linux-gnu installed) so we don't add a second image.
#
# Usage:
#   ./scripts/build-move-mux-shim.sh                    # build via Docker
#   CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build-move-mux-shim.sh
#                                                       # build directly
#
# Output: build/move-mux-shim.so
#
# Sanity check: the .so should be ~10-20KB and export `open`, `openat`,
# `stat`, `lstat`, `access`, plus `mux_remap_path`/`mux_resolve`.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-builder"

# If we're not already inside a container and CROSS_PREFIX isn't set,
# delegate to Docker. Mirrors scripts/build.sh's pattern.
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Error: Docker image '$IMAGE_NAME' not found."
        echo "Run scripts/build.sh once first to build the image, or set CROSS_PREFIX."
        exit 1
    fi

    echo "=== Building move-mux-shim.so via Docker ==="
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        --entrypoint /bin/sh \
        "$IMAGE_NAME" \
        -c "CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build-move-mux-shim.sh"
    echo ""
    echo "Output: $REPO_ROOT/build/move-mux-shim.so"
    exit 0
fi

# === Inside container (or with explicit CROSS_PREFIX) ===
cd "$REPO_ROOT"

if ! command -v "${CROSS_PREFIX}gcc" >/dev/null 2>&1; then
    echo "Error: missing compiler '${CROSS_PREFIX}gcc'"
    exit 1
fi

mkdir -p build/

echo "Building move-mux-shim.so..."
"${CROSS_PREFIX}gcc" -g -O2 -shared -fPIC -std=c11 \
    -Wall -Wextra -Werror \
    -fvisibility=hidden \
    src/move_mux_shim.c \
    -o build/move-mux-shim.so \
    -ldl

echo "Built: build/move-mux-shim.so ($(wc -c < build/move-mux-shim.so | tr -d ' ') bytes)"

# Quick sanity check: confirm the wrapper symbols are exported.
if command -v "${CROSS_PREFIX}objdump" >/dev/null 2>&1; then
    echo ""
    echo "Exported wrapper symbols:"
    "${CROSS_PREFIX}objdump" -T build/move-mux-shim.so 2>/dev/null \
        | grep -E '\b(open|openat|stat|lstat|access|faccessat|mux_)' \
        | grep -v '\*UND\*' \
        | awk '{print "  " $NF}' \
        | sort -u
fi
