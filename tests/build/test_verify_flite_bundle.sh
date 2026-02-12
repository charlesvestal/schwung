#!/usr/bin/env bash
set -euo pipefail

SCRIPT="scripts/verify-flite-bundle.sh"

if [ ! -x "$SCRIPT" ]; then
  echo "FAIL: Missing executable $SCRIPT" >&2
  exit 1
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

incomplete="$tmpdir/incomplete"
mkdir -p "$incomplete"
touch "$incomplete/libflite.so.1"

if "$SCRIPT" "$incomplete" >/dev/null 2>&1; then
  echo "FAIL: verifier passed incomplete Flite bundle" >&2
  exit 1
fi

complete="$tmpdir/complete"
mkdir -p "$complete"
touch "$complete/libflite.so.1"
touch "$complete/libflite_cmu_us_kal.so.1"
touch "$complete/libflite_usenglish.so.1"
touch "$complete/libflite_cmulex.so.1"

"$SCRIPT" "$complete" >/dev/null

echo "PASS: Flite bundle verifier rejects incomplete bundles and accepts complete bundles"
