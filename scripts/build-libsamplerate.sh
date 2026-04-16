#!/usr/bin/env bash
# Build static libsamplerate for ARM (aarch64) — used by link-subscriber for
# async sample-rate conversion between Link's audio-thread clock and Move's
# SPI-callback clock.
#
# Run this once after a fresh checkout (or after bumping LIBSAMPLERATE_VERSION).
# Output lands in libs/libsamplerate/:
#   libs/libsamplerate/lib/libsamplerate.a
#   libs/libsamplerate/include/samplerate.h
#
# scripts/build.sh picks these up and links link-subscriber against them.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
LIBSAMPLERATE_VERSION="0.2.2"

cd "$REPO_ROOT"

echo "=== Building libsamplerate $LIBSAMPLERATE_VERSION for aarch64 ==="

cat > /tmp/Dockerfile.libsamplerate <<'EOF'
FROM debian:bookworm

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    make \
    cmake \
    curl \
    ca-certificates \
    xz-utils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

ARG LIBSAMPLERATE_VERSION
RUN curl -fsSL "https://github.com/libsndfile/libsamplerate/releases/download/${LIBSAMPLERATE_VERSION}/libsamplerate-${LIBSAMPLERATE_VERSION}.tar.xz" -o src.tar.xz && \
    tar xJf src.tar.xz && \
    mv libsamplerate-${LIBSAMPLERATE_VERSION} libsamplerate

WORKDIR /build/libsamplerate

RUN mkdir build && cd build && \
    cmake .. \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
        -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
        -DCMAKE_AR=/usr/bin/aarch64-linux-gnu-ar \
        -DCMAKE_RANLIB=/usr/bin/aarch64-linux-gnu-ranlib \
        -DCMAKE_INSTALL_PREFIX=/build/install \
        -DBUILD_SHARED_LIBS=OFF \
        -DLIBSAMPLERATE_EXAMPLES=OFF \
        -DLIBSAMPLERATE_INSTALL=ON \
        -DBUILD_TESTING=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON && \
    make -j"$(nproc)" && \
    make install

# Verify static lib + header
RUN ls -la /build/install/lib/libsamplerate.a && \
    ls -la /build/install/include/samplerate.h
EOF

docker build --pull -f /tmp/Dockerfile.libsamplerate \
    --build-arg LIBSAMPLERATE_VERSION="$LIBSAMPLERATE_VERSION" \
    -t libsamplerate-build .

# Extract artifacts
rm -rf libs/libsamplerate
mkdir -p libs/libsamplerate
docker create --name libsamplerate-tmp libsamplerate-build
docker cp libsamplerate-tmp:/build/install/lib/libsamplerate.a libs/libsamplerate/
docker cp libsamplerate-tmp:/build/install/include/samplerate.h libs/libsamplerate/
docker rm libsamplerate-tmp

echo ""
echo "=== Done ==="
ls -la libs/libsamplerate/
