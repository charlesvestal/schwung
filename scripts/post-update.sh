#!/bin/sh
# post-update.sh — Run after extracting a host update tarball.
# Called by the on-device Module Store (via host_system_cmd) and
# can also be run manually: ssh ableton@move.local "sh /data/UserData/schwung/scripts/post-update.sh"
#
# This script is idempotent — safe to run multiple times.
# It runs as root (shim is setuid), so it can update /usr/lib and /opt/move.

BASE="/data/UserData/schwung"

# --- Shim setup ---

# Copy shim to /usr/lib (glibc 2.35+ rejects symlinked .so under AT_SECURE)
rm -f /usr/lib/schwung-shim.so
cp "$BASE/schwung-shim.so" /usr/lib/schwung-shim.so 2>/dev/null || \
    ln -sf "$BASE/schwung-shim.so" /usr/lib/schwung-shim.so
chmod u+s /usr/lib/schwung-shim.so
chmod u+s "$BASE/schwung-shim.so"

# Remove web shim symlink (no longer used as of 0.9.2)
rm -f /usr/lib/schwung-web-shim.so

# TTS library symlinks
if [ -d "$BASE/lib" ]; then
    cd "$BASE/lib"
    for lib in *.so.*; do
        [ -e "$lib" ] || continue
        rm -f "/usr/lib/$lib"
        ln -sf "$BASE/lib/$lib" "/usr/lib/$lib"
    done
fi

# --- Entrypoint ---

chmod +x "$BASE/shim-entrypoint.sh" 2>/dev/null

# Backup original Move binary (only once)
if [ ! -f /opt/move/MoveOriginal ] && [ -f /opt/move/Move ]; then
    mv /opt/move/Move /opt/move/MoveOriginal
fi

# Install shimmed entrypoint
if [ -f /opt/move/MoveOriginal ] && [ -f "$BASE/shim-entrypoint.sh" ]; then
    cp "$BASE/shim-entrypoint.sh" /opt/move/Move
    chmod +x /opt/move/Move
    echo "post-update: entrypoint installed"
fi

# --- Restore stock MoveWebService if previously wrapped (pre-0.9.2 cleanup) ---

WEB_SVC_PATH=$(grep 'service_path=' /etc/init.d/move-web-service 2>/dev/null | head -n 1 | sed 's/.*service_path=//' | tr -d '[:space:]')
if [ -n "$WEB_SVC_PATH" ] && [ -f "${WEB_SVC_PATH}Original" ]; then
    killall MoveWebService MoveWebServiceOriginal 2>/dev/null
    sleep 1
    cp "${WEB_SVC_PATH}Original" "$WEB_SVC_PATH"
    chmod +x "$WEB_SVC_PATH"
    echo "post-update: MoveWebService restored to stock"
fi

# --- Executables ---

chmod +x "$BASE/schwung" 2>/dev/null
chmod +x "$BASE/start.sh" 2>/dev/null
chmod +x "$BASE/stop.sh" 2>/dev/null
chmod +x "$BASE/bin/display_ctl" 2>/dev/null
chmod +x "$BASE/bin/jack_midi_connect" 2>/dev/null

# --- RNBO integration symlinks ---

mkdir -p /data/UserData/rnbo/lib/jack
ln -sf "$BASE/lib/jack/jack_shadow.so" /data/UserData/rnbo/lib/jack/jack_shadow.so 2>/dev/null

mkdir -p /data/UserData/rnbo/scripts
ln -sf "$BASE/bin/display_ctl" /data/UserData/rnbo/scripts/display_ctl

mkdir -p /data/UserData/rnbo/config
ln -sf "$BASE/modules/overtake/rnbo-runner/control-startup-shadow.json" \
    /data/UserData/rnbo/config/control-startup-shadow.json 2>/dev/null

# --- Clean stale ld.so.preload entries ---

if [ -f /etc/ld.so.preload ] && grep -q 'schwung-shim.so' /etc/ld.so.preload; then
    grep -v 'schwung-shim.so' /etc/ld.so.preload > /etc/ld.so.preload.tmp || true
    if [ -s /etc/ld.so.preload.tmp ]; then
        cat /etc/ld.so.preload.tmp > /etc/ld.so.preload
    else
        rm -f /etc/ld.so.preload
    fi
    rm -f /etc/ld.so.preload.tmp
fi

# Write breadcrumb so we can verify the script ran
echo "$(date '+%Y-%m-%d %H:%M:%S') uid=$(id -u)" > "$BASE/post-update-ran"

echo "post-update: done"
