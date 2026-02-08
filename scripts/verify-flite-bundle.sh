#!/usr/bin/env bash
set -euo pipefail

BUNDLE_DIR="${1:-./build/lib}"

if [ ! -d "$BUNDLE_DIR" ]; then
  echo "Error: Flite bundle directory not found: $BUNDLE_DIR" >&2
  exit 1
fi

required_libs=(
  "libflite.so.1"
  "libflite_cmu_us_kal.so.1"
  "libflite_usenglish.so.1"
  "libflite_cmulex.so.1"
)

missing=0
for lib in "${required_libs[@]}"; do
  if [ ! -e "$BUNDLE_DIR/$lib" ]; then
    echo "Error: Missing required Flite library: $BUNDLE_DIR/$lib" >&2
    missing=1
  fi
done

# Detect a broken "no-match glob" symlink (literal wildcard path).
if [ -e "$BUNDLE_DIR/libflite*.so.*" ]; then
  echo "Error: Invalid wildcard symlink found: $BUNDLE_DIR/libflite*.so.*" >&2
  missing=1
fi

if [ "$missing" -ne 0 ]; then
  echo >&2
  echo "Flite bundle verification failed." >&2
  echo "Build with Docker (./scripts/build.sh) or install arm64 Flite dev packages for manual cross-builds." >&2
  exit 1
fi

echo "Flite bundle verified: required libraries are present in $BUNDLE_DIR"
