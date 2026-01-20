# Move Anything Build Environment
# Targets: Ableton Move (aarch64 Linux)

FROM debian:bookworm

# Enable arm64 architecture for cross-compilation libraries
RUN dpkg --add-architecture arm64

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    make \
    file \
    libdbus-1-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Set cross-compilation environment
ENV CROSS_PREFIX=aarch64-linux-gnu-
ENV CC=aarch64-linux-gnu-gcc
ENV CXX=aarch64-linux-gnu-g++

# Build script embedded in container
CMD set -e && \
    echo "=== Move Anything Build ===" && \
    echo "Target: aarch64-linux-gnu" && \
    echo "" && \
    echo "Building QuickJS..." && \
    cd /build/libs/quickjs/quickjs-2025-04-26 && \
    make clean 2>/dev/null || true && \
    CC=aarch64-linux-gnu-gcc make libquickjs.a && \
    echo "QuickJS built successfully" && \
    echo "" && \
    echo "Building Move Anything..." && \
    cd /build && \
    CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh && \
    echo "" && \
    echo "Packaging..." && \
    CROSS_PREFIX=aarch64-linux-gnu- ./scripts/package.sh && \
    echo "" && \
    echo "=== Build Artifacts ===" && \
    file /build/build/move-anything && \
    file /build/build/move-anything-shim.so && \
    file /build/build/modules/sf2/dsp.so 2>/dev/null || echo "SF2 module DSP: not found" && \
    echo "" && \
    echo "=== Package Created ===" && \
    ls -lh /build/move-anything.tar.gz && \
    echo "" && \
    echo "Build complete!"
