#!/usr/bin/env bash
# Set library path for bundled TTS libraries
export LD_LIBRARY_PATH=/data/UserData/move-anything/lib:$LD_LIBRARY_PATH

# Start Link Audio subscriber if present (feature gate is inside the binary)
LINK_SUB="/data/UserData/move-anything/link-subscriber"
if [ -x "$LINK_SUB" ]; then
    "$LINK_SUB" >> /tmp/link-subscriber.log 2>&1 &
fi

exec env LD_PRELOAD=move-anything-shim.so /opt/move/MoveOriginal
