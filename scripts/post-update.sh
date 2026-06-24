#!/bin/sh
# post-update.sh — Run after extracting a host update tarball.
# Called by the on-device Module Store (via host_system_cmd) and
# can also be run manually: ssh ableton@move.local "sh /data/UserData/schwung/scripts/post-update.sh"
#
# This script is idempotent — safe to run multiple times.
#
# Privilege note: this runs as root ONLY on the retired on-device store path
# (host_system_cmd inherited the shim's setuid privileges). Under schwung-manager
# (the web installer, the live path) it runs as the unprivileged `ableton` user,
# so every direct /usr/lib and /opt/move write below SILENTLY FAILS. The
# privileged mirror is therefore delegated to schwung-heal (setuid-root) — see
# the "Privileged mirror" block below. The direct writes are kept only for the
# legacy root path and as a no-op fallback.

BASE="/data/UserData/schwung"

# --- Shim setup ---

# Copy shim to /usr/lib (glibc 2.35+ rejects symlinked .so under AT_SECURE).
# Effective only when run as root (legacy store path); a no-op under the web
# manager, where the Privileged mirror block below does the real work.
rm -f /usr/lib/schwung-shim.so
cp "$BASE/schwung-shim.so" /usr/lib/schwung-shim.so 2>/dev/null || \
    ln -sf "$BASE/schwung-shim.so" /usr/lib/schwung-shim.so
chmod u+s /usr/lib/schwung-shim.so 2>/dev/null
chmod u+s "$BASE/schwung-shim.so" 2>/dev/null

# --- Privileged mirror (web-installer path) ---
#
# schwung-manager runs this script as `ableton`, so the /usr/lib writes above
# do nothing. schwung-heal is setuid-root and is the privileged path that
# mirrors $BASE/schwung-shim.so -> /usr/lib and shim-entrypoint.sh -> /opt/move/Move.
# Invoking it here mirrors the shim SYNCHRONOUSLY during the upgrade, so the live
# shim is current even if the manager's later detached `schwung-heal --reboot`
# leg fails to fire (its reboot has historically been the flaky part; the mirror
# from a clean synchronous shell context like this is reliable). Mirror only — the
# manager (or a manual reboot) owns the reboot that swaps the new shim into the
# already-running MoveOriginal.
if [ -u "$BASE/bin/schwung-heal" ]; then
    "$BASE/bin/schwung-heal" 2>&1 | sed 's/^/post-update: heal: /'
elif [ -x "$BASE/bin/schwung-heal" ]; then
    echo "post-update: schwung-heal present but not setuid-root — shim mirror skipped (run install.sh to re-bless)"
else
    echo "post-update: schwung-heal missing — shim mirror skipped"
fi

# Remove web shim symlink (no longer used as of 0.9.2)
rm -f /usr/lib/schwung-web-shim.so

# Remove the retired on-device store module (schwung-manager at
# move.local:7700 is the single install/update path; new tarballs no
# longer ship it, but updates extract over the old tree)
rm -rf "$BASE/modules/store"

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
    /etc/init.d/move-web-service start >/dev/null 2>&1 || true
    echo "post-update: MoveWebService restored to stock"
fi

# --- Executables ---

chmod +x "$BASE/schwung" 2>/dev/null
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
