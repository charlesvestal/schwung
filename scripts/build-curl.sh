#!/usr/bin/env bash
# Build static curl for ARM (aarch64)
# Run this script to build curl via Docker

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CURL_VERSION="8.5.0"

cd "$REPO_ROOT"

echo "=== Building curl $CURL_VERSION for ARM ==="

# Create a temporary Dockerfile for curl build
cat > /tmp/Dockerfile.curl << 'EOF'
FROM debian:bookworm

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    make \
    curl \
    ca-certificates \
    xz-utils \
    bzip2 \
    cmake \
    python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Download curl source
RUN curl -fsSL https://curl.se/download/curl-8.5.0.tar.xz -o curl.tar.xz && \
    tar xJf curl.tar.xz

# Download and build mbedTLS for cross-compilation (static libraries)
RUN curl -fsSL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.5/mbedtls-3.6.5.tar.bz2 -o mbedtls.tar.bz2 && \
    tar xjf mbedtls.tar.bz2 && \
    cd mbedtls-3.6.5 && \
    mkdir build && cd build && \
    cmake .. \
        -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
        -DCMAKE_AR=/usr/bin/aarch64-linux-gnu-ar \
        -DCMAKE_RANLIB=/usr/bin/aarch64-linux-gnu-ranlib \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        -DENABLE_TESTING=OFF \
        -DENABLE_PROGRAMS=OFF \
        -DCMAKE_INSTALL_PREFIX=/build/mbedtls-install \
        -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) && \
    make install

# Build curl with mbedTLS - use full static linking
RUN cd curl-8.5.0 && \
    ./configure \
        --host=aarch64-linux-gnu \
        --disable-shared \
        --enable-static \
        --with-mbedtls=/build/mbedtls-install \
        --without-libpsl \
        --without-libidn2 \
        --without-nghttp2 \
        --without-brotli \
        --without-zstd \
        --without-zlib \
        --disable-ldap \
        --disable-ldaps \
        --disable-rtsp \
        --disable-dict \
        --disable-telnet \
        --disable-tftp \
        --disable-pop3 \
        --disable-imap \
        --disable-smb \
        --disable-smtp \
        --disable-gopher \
        --disable-mqtt \
        --disable-manual \
        --disable-docs \
        CPPFLAGS="-I/build/mbedtls-install/include" \
        LDFLAGS="-L/build/mbedtls-install/lib" \
        LIBS="-lmbedtls -lmbedx509 -lmbedcrypto" \
        CC=aarch64-linux-gnu-gcc \
        LD=aarch64-linux-gnu-ld && \
    make -j$(nproc) && \
    aarch64-linux-gnu-strip src/curl

CMD ["cp", "/build/curl-8.5.0/src/curl", "/output/curl"]
EOF

echo "Building Docker image for curl..."
docker build -t curl-arm-builder -f /tmp/Dockerfile.curl /tmp

echo "Extracting curl binary..."
mkdir -p "$REPO_ROOT/libs/curl"
docker run --rm -v "$REPO_ROOT/libs/curl:/output" curl-arm-builder

echo ""
echo "=== Done ==="
file "$REPO_ROOT/libs/curl/curl"
ls -lh "$REPO_ROOT/libs/curl/curl"
echo ""
echo "curl binary saved to: libs/curl/curl"
