# curl for Move Anything

This directory contains a statically-linked curl binary for ARM (Move hardware).

## Building curl

To build a static ARM curl binary:

```bash
# In Docker with ARM cross-compiler
apt-get install -y libssl-dev

# Download curl source
curl -LO https://curl.se/download/curl-8.5.0.tar.gz
tar xzf curl-8.5.0.tar.gz
cd curl-8.5.0

# Configure for static ARM build
CC="${CROSS_PREFIX}gcc" \
./configure --host=arm-linux-gnueabihf \
    --disable-shared \
    --enable-static \
    --with-openssl \
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
    --without-librtmp \
    --without-libidn2 \
    --without-libpsl

make -j$(nproc)

# The static binary is in src/curl
cp src/curl /path/to/move-anything/libs/curl/
```

## Pre-built Binary

A pre-built static ARM curl binary should be placed in this directory as `curl`.

The binary will be installed to `/data/UserData/move-anything/bin/curl` on the device.

## License

curl is licensed under the curl license (MIT/X derivative). See COPYING file.
