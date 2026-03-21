#!/usr/bin/env bash
# Set library path for bundled TTS libraries
export LD_LIBRARY_PATH=/data/UserData/schwung/lib:$LD_LIBRARY_PATH

# Note: link-subscriber is launched by the shim (auto-recovery lifecycle)

# Start live display server if present
DISPLAY_SRV="/data/UserData/schwung/display-server"
if [ -x "$DISPLAY_SRV" ]; then
    "$DISPLAY_SRV" >/dev/null 2>&1 &
fi

# Start filebrowser for file management (port 404, no auth) if enabled
FB="/data/UserData/schwung/bin/filebrowser"
FB_FLAG="/data/UserData/schwung/filebrowser_enabled"
if [ -x "$FB" ] && [ -f "$FB_FLAG" ]; then
    "$FB" \
        --noauth \
        --address 0.0.0.0 \
        --port 404 \
        --root /data/UserData \
        --database /data/UserData/schwung/filebrowser.db \
        --disableThumbnails \
        --disablePreviewResize \
        --disableExec \
        --disableTypeDetectionByHeader \
        >/dev/null 2>&1 &
fi

exec env LD_PRELOAD=schwung-shim.so /opt/move/MoveOriginal
