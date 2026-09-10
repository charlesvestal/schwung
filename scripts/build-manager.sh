#!/bin/bash
#
# Build schwung-manager (Go) for the device: linux/arm64, static.
#
# THE SINGLE PLACE THIS IS BUILT.  It used to be built twice, and the two
# copies did not agree: build.sh preferred a local `go` and fell back to a
# golang container, while install.sh's rebuild was guarded on
# `command -v go` alone.  On a machine with Docker but no Go the install
# block evaluated to false and was skipped in silence — no warning, because
# the only warning sat on the build-FAILED branch inside an if that never
# ran — so `install.sh local` uploaded whatever schwung-manager happened to
# be in the existing tarball and reported success.  A manager fix could then
# be deployed, verified as deployed, and still not be running.
#
# That is the same defect already recorded for the link sidecar in
# CLAUDE.md: a build step that can be skipped silently defeats every bisect
# that follows.  So this script is loud and it is shared — a caller gets a
# fresh binary or a non-zero exit, never a quiet no-op.
#
# Usage: scripts/build-manager.sh [output_path]
#   Default output: <repo>/build/schwung-manager
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$REPO_ROOT/build/schwung-manager}"
SRC="$REPO_ROOT/schwung-manager"

if [ ! -d "$SRC" ]; then
    echo "ERROR: $SRC not found" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

if command -v go &>/dev/null; then
    echo "Building schwung-manager with local go..."
    (cd "$SRC" && GOOS=linux GOARCH=arm64 CGO_ENABLED=0 \
        go build -o "$OUT" -ldflags="-s -w" .)
elif command -v docker &>/dev/null; then
    echo "Local 'go' not found — building schwung-manager via golang:1.26-bookworm container"
    mkdir -p "$REPO_ROOT/.cache/go-cache" "$REPO_ROOT/.cache/go-mod-cache"
    OUT_DIR="$(cd "$(dirname "$OUT")" && pwd)"
    docker run --rm \
        -v "$SRC:/src" \
        -v "$OUT_DIR:/out" \
        -v "$REPO_ROOT/.cache/go-cache:/gocache" \
        -v "$REPO_ROOT/.cache/go-mod-cache:/go-mod-cache" \
        -u "$(id -u):$(id -g)" \
        -w /src \
        -e GOOS=linux -e GOARCH=arm64 -e CGO_ENABLED=0 \
        -e GOCACHE=/gocache -e GOMODCACHE=/go-mod-cache \
        golang:1.26-bookworm \
        go build -buildvcs=false -o "/out/$(basename "$OUT")" -ldflags="-s -w" .
else
    echo "ERROR: neither 'go' nor 'docker' available — cannot build schwung-manager." >&2
    exit 1
fi

[ -s "$OUT" ] || { echo "ERROR: $OUT was not produced" >&2; exit 1; }
echo "Built: schwung-manager ($(wc -c < "$OUT" | tr -d ' ') bytes)"
