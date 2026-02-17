#!/usr/bin/env bash
# Set library path for bundled TTS libraries
export LD_LIBRARY_PATH=/data/UserData/move-anything/lib:$LD_LIBRARY_PATH

# Note: link-subscriber is launched by the shim (auto-recovery lifecycle)

# Start live display server if present
DISPLAY_SRV="/data/UserData/move-anything/display-server"
if [ -x "$DISPLAY_SRV" ]; then
    "$DISPLAY_SRV" > /tmp/display-server.log 2>&1 &
fi

exec env LD_PRELOAD=move-anything-shim.so /opt/move/MoveOriginal
