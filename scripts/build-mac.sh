#!/usr/bin/env bash
# Native macOS arm64 build of schwung-host with the in-process sim backend.
# Compiles only what's needed to boot into the host tick loop — no display,
# no audio, no shadow features. See docs/plans for the full Sim A plan.
#
# Outputs: build/mac/schwung-host
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
SCHWUNG_SPI_ROOT="${SCHWUNG_SPI_ROOT:-$(dirname "$REPO_ROOT")/schwung-spi}"

OUT_DIR="$REPO_ROOT/build/mac"
mkdir -p "$OUT_DIR"

if [[ ! -d "$SCHWUNG_SPI_ROOT" ]]; then
    echo "error: SCHWUNG_SPI_ROOT='$SCHWUNG_SPI_ROOT' does not exist" >&2
    echo "       (need it for protocol constants in schwung_spi_lib.h)" >&2
    exit 1
fi

# Build native QuickJS if it's stale or missing/wrong-arch.
QJS_DIR="$REPO_ROOT/libs/quickjs/quickjs-2025-04-26"
QJS_LIB="$QJS_DIR/libquickjs.a"
if [[ ! -f "$QJS_LIB" ]] || ! file "$QJS_LIB" 2>/dev/null | grep -q "random library"; then
    echo "Building QuickJS for native macOS..."
    (cd "$QJS_DIR" && make clean >/dev/null 2>&1 && make -j libquickjs.a CC=clang AR=ar >/dev/null)
fi

CC="${CC:-clang}"
CFLAGS=(
    -arch arm64
    -O2 -g
    -Wall -Wextra
    -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare
    -DSCHWUNG_SIM_BACKEND
    -I "$REPO_ROOT/src"
    -I "$REPO_ROOT/src/host"
    -I "$QJS_DIR"
    -I "$SCHWUNG_SPI_ROOT"
)

SOURCES=(
    "src/schwung_host.c"
    "src/host/sim_backend.c"
    "src/host/module_manager.c"
    "src/host/settings.c"
    "src/host/analytics.c"
    "src/host/arc4random_compat.c"
    "src/host/unified_log.c"
)

OBJECTS=()
echo "Compiling sources..."
for src in "${SOURCES[@]}"; do
    obj="$OUT_DIR/$(basename "${src%.c}").o"
    OBJECTS+=("$obj")
    echo "  $src"
    "$CC" "${CFLAGS[@]}" -c "$REPO_ROOT/$src" -o "$obj"
done

echo "Linking schwung-host..."
"$CC" -arch arm64 \
    -o "$OUT_DIR/schwung-host" \
    "${OBJECTS[@]}" \
    "$QJS_LIB" \
    -lm -lpthread -ldl

ls -la "$OUT_DIR/schwung-host"
echo "Done."
