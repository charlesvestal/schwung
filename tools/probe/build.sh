#!/usr/bin/env bash
# Build schwung-probe for ARM64 Linux, in the SAME image modules were built in.
#
# That is not incidental: the probe dlopens release tarballs, so it needs the
# same glibc and the same libdbus/libsystemd they were linked against. On an
# Apple-silicon Mac this container runs natively rather than emulated.
#
#   tools/probe/build.sh              -> build/probe
#   CROSS_PREFIX=aarch64-linux-gnu-   -> skip Docker (already on/for ARM)
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=build/probe
mkdir -p build

if [ -n "${CROSS_PREFIX:-}" ] || [ -f /.dockerenv ]; then
    "${CROSS_PREFIX:-}gcc" -O2 -std=c11 -D_GNU_SOURCE \
        -Isrc/host -Wall -Wextra -Wno-unused-parameter \
        -o "$OUT" tools/probe/probe.c -ldl -lm
    echo "built $OUT"
    exit 0
fi

if ! docker info >/dev/null 2>&1; then
    echo "error: docker is not running (or set CROSS_PREFIX to build natively)" >&2
    exit 1
fi

docker run --rm --platform linux/arm64 -v "$PWD:/w" -w /w debian:bookworm bash -c '
    set -e
    if ! command -v gcc >/dev/null; then
        apt-get update -qq >/dev/null && apt-get install -y -qq gcc >/dev/null
    fi
    gcc -O2 -std=c11 -D_GNU_SOURCE -Isrc/host -Wall -Wextra -Wno-unused-parameter \
        -o build/probe tools/probe/probe.c -ldl -lm
'
echo "built $OUT"
