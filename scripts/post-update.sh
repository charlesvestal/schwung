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

# Web shim symlink (for MoveWebService PIN readout)
if [ -f "$BASE/schwung-web-shim.so" ]; then
    rm -f /usr/lib/schwung-web-shim.so
    ln -sf "$BASE/schwung-web-shim.so" /usr/lib/schwung-web-shim.so
fi

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
echo "$(date '+%Y-%m-%d %H:%M:%S')" > "$BASE/post-update-ran"

echo "post-update: done"
