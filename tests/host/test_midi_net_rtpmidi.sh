#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

: "${CROSS_PREFIX:=}"
CC="${CROSS_PREFIX}gcc"
CXX="${CROSS_PREFIX}g++"
out=build/tests/test_midi_net_rtpmidi
objdir=build/tests/rtpmidi-objects

if [ ! -f build/lib/libschwung-rtpmidi.so ]; then
    echo "error: build/lib/libschwung-rtpmidi.so is missing; run scripts/build.sh first" >&2
    exit 1
fi

mkdir -p "$objdir"
common=(-DMIDI_NET_TESTING -Isrc/host -Wall -Wextra -Werror)
"$CC" -std=gnu11 "${common[@]}" -c src/host/midi_net.c -o "$objdir/midi_net.o"
"$CC" -std=gnu11 "${common[@]}" -c src/host/midi_net_ipmidi.c -o "$objdir/midi_net_ipmidi.o"
"$CC" -std=gnu11 "${common[@]}" -c src/host/unified_log.c -o "$objdir/unified_log.o"
"$CXX" -std=c++17 "${common[@]}" tests/host/test_midi_net_rtpmidi.cpp \
    "$objdir/midi_net.o" "$objdir/midi_net_ipmidi.o" "$objdir/unified_log.o" \
    -Lbuild/lib -Wl,-rpath,'$ORIGIN/../lib' \
    -lschwung-rtpmidi -ldl -lpthread -o "$out"

if [ -z "$CROSS_PREFIX" ]; then
    LD_LIBRARY_PATH=build/lib "$out"
else
    echo "Built $out for the target; run it on Linux with build/lib on LD_LIBRARY_PATH"
fi
