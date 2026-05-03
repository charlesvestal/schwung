#!/usr/bin/env bash

# === Migration from move-anything → schwung ===
# When upgrading from 0.7.x via the Module Store, files land in
# /data/UserData/move-anything/ but with new schwung binary names.
# Detect this and migrate before proceeding.
SCHWUNG_DIR="/data/UserData/schwung"
OLD_DIR="/data/UserData/move-anything"

if [ ! -d "$SCHWUNG_DIR" ] && [ -d "$OLD_DIR" ] && [ ! -L "$OLD_DIR" ]; then
    # Old directory exists, new one doesn't — need to migrate
    mv "$OLD_DIR" "$SCHWUNG_DIR"
    ln -s "$SCHWUNG_DIR" "$OLD_DIR"

    # Migrate sample/preset directories
    OLD_SAMPLES="/data/UserData/UserLibrary/Samples/Move Everything"
    NEW_SAMPLES="/data/UserData/UserLibrary/Samples/Schwung"
    if [ -d "$OLD_SAMPLES" ] && [ ! -d "$NEW_SAMPLES" ] && [ ! -L "$OLD_SAMPLES" ]; then
        mv "$OLD_SAMPLES" "$NEW_SAMPLES"
        ln -s "$NEW_SAMPLES" "$OLD_SAMPLES"
    fi

    OLD_PRESETS="/data/UserData/UserLibrary/Track Presets/Move Everything"
    NEW_PRESETS="/data/UserData/UserLibrary/Track Presets/Schwung"
    if [ -d "$OLD_PRESETS" ] && [ ! -d "$NEW_PRESETS" ] && [ ! -L "$OLD_PRESETS" ]; then
        mv "$OLD_PRESETS" "$NEW_PRESETS"
        ln -s "$NEW_PRESETS" "$OLD_PRESETS"
    fi
fi

# === Fix /usr/lib/ shim symlink if stale ===
# After migration, ensure the shim symlink points to the right file
if [ -f "$SCHWUNG_DIR/schwung-shim.so" ]; then
    SHIM_TARGET=$(readlink /usr/lib/schwung-shim.so 2>/dev/null || true)
    if [ "$SHIM_TARGET" != "$SCHWUNG_DIR/schwung-shim.so" ]; then
        rm -f /usr/lib/schwung-shim.so
        ln -s "$SCHWUNG_DIR/schwung-shim.so" /usr/lib/schwung-shim.so
    fi
    # Remove old-name symlink if present
    rm -f /usr/lib/move-anything-shim.so
fi

# === Update /opt/move/Move entrypoint if stale ===
# If /opt/move/Move still references the old name, replace it with this script
if grep -q 'move-anything-shim.so' /opt/move/Move 2>/dev/null; then
    cp "$SCHWUNG_DIR/shim-entrypoint.sh" /opt/move/Move
    chmod +x /opt/move/Move
fi

# === Self-heal /usr/lib shim and /opt/move entrypoint at every boot ===
# /etc/init.d/move launches us via `start-stop-daemon -c ableton`, so this
# script (and everything it spawns) runs as ableton — which can't write
# /usr/lib or /opt/move directly. schwung-heal is a tiny setuid-root
# helper (no CLI input, hardcoded paths) that mirrors the data-partition
# shim and entrypoint to their system locations when stale. Idempotent.
# Runs BEFORE LD_PRELOAD exec so the corrected shim is what MoveOriginal
# loads — no reboot needed.
SCHWUNG_HEAL="$SCHWUNG_DIR/bin/schwung-heal"
if [ -x "$SCHWUNG_HEAL" ]; then
    "$SCHWUNG_HEAL" >>"$SCHWUNG_DIR/heal-boot.log" 2>&1
fi

# Set library path for bundled TTS libraries
export LD_LIBRARY_PATH=$SCHWUNG_DIR/lib:$LD_LIBRARY_PATH

# Note: link-subscriber is launched by the shim (auto-recovery lifecycle)

# Start live display server if present
DISPLAY_SRV="$SCHWUNG_DIR/display-server"
if [ -x "$DISPLAY_SRV" ]; then
    "$DISPLAY_SRV" >/dev/null 2>&1 &
fi

# Start schwung-manager web UI if present (skip if already running)
SCHWUNG_MGR="$SCHWUNG_DIR/schwung-manager"
SCHWUNG_MGR_LOG="$SCHWUNG_DIR/schwung-manager.log"
SCHWUNG_MGR_PID="$SCHWUNG_DIR/schwung-manager.pid"
if [ -x "$SCHWUNG_MGR" ]; then
    # Skip if already running
    if [ -f "$SCHWUNG_MGR_PID" ] && kill -0 "$(cat "$SCHWUNG_MGR_PID")" 2>/dev/null; then
        : # already running
    else
        # Rotate log if over 100KB
        if [ -f "$SCHWUNG_MGR_LOG" ]; then
            log_size=$(wc -c < "$SCHWUNG_MGR_LOG" 2>/dev/null || echo 0)
            if [ "$log_size" -gt 102400 ]; then
                tail -c 102400 "$SCHWUNG_MGR_LOG" > "$SCHWUNG_MGR_LOG.tmp" 2>/dev/null
                mv "$SCHWUNG_MGR_LOG.tmp" "$SCHWUNG_MGR_LOG"
            fi
        fi
        "$SCHWUNG_MGR" -port 7700 -roots /data/UserData/ >>"$SCHWUNG_MGR_LOG" 2>&1 &
        echo $! > "$SCHWUNG_MGR_PID"
    fi
fi

# Start filebrowser for file management (port 404, no auth) if enabled
FB="$SCHWUNG_DIR/bin/filebrowser"
FB_FLAG="$SCHWUNG_DIR/filebrowser_enabled"
if [ -x "$FB" ] && [ -f "$FB_FLAG" ]; then
    "$FB" \
        --noauth \
        --address 0.0.0.0 \
        --port 404 \
        --root /data/UserData \
        --database "$SCHWUNG_DIR/filebrowser.db" \
        --disableThumbnails \
        --disablePreviewResize \
        --disableExec \
        --disableTypeDetectionByHeader \
        >/dev/null 2>&1 &
fi

exec env LD_PRELOAD=schwung-shim.so /opt/move/MoveOriginal
