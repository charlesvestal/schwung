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

# === Factory-reset / missing-payload safety net ===
# A factory reset (or any wipe of /data) removes the entire Schwung payload,
# but our root-partition hooks survive: this script IS /opt/move/Move, the
# stock binary is at /opt/move/MoveOriginal, and on glibc 2.35+ images
# /usr/lib/schwung-shim.so is a real copy (not a symlink). If we went ahead
# and exec'd MoveOriginal with LD_PRELOAD=schwung-shim.so, the shim would load
# and immediately fail — it needs /data for SHM, config, modules, and the
# link-subscriber — crashing MoveOriginal on every boot. That is exactly the
# "Move doesn't boot after a factory reset" failure. So if the payload on
# /data is gone, launch stock Move with no shim. The device always boots; a
# later reinstall restores Schwung. Needs no root, so it works under the
# `start-stop-daemon -c ableton` launch context.
if [ ! -f "$SCHWUNG_DIR/schwung-shim.so" ] && [ -x /opt/move/MoveOriginal ]; then
    exec /opt/move/MoveOriginal
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

# ── Boot selector ─────────────────────────────────────────────────────────
# Resolve a target, offer the 2 s Back window, watchdog-count the attempt,
# then exec the target's entry script. Every failure path lands on stock
# MoveOriginal: the selector's own failure mode is always "Move boots".
BT_LIB="$SCHWUNG_DIR/host/boot_target_lib.sh"
BOOT_SELECT="$SCHWUNG_DIR/bin/boot-select"

if [ ! -f "$BT_LIB" ]; then
    exec env LD_PRELOAD=schwung-shim.so /opt/move/MoveOriginal
fi
. "$BT_LIB"

# Self-registration: Schwung owns its own registry entry, refreshed every
# boot so a wiped or hand-edited registry heals itself.
mkdir -p "$BOOT_TARGETS_DIR/schwung"
printf '{\n  "name": "Schwung",\n  "exec": "%s"\n}\n' \
    "$SCHWUNG_DIR/schwung-entry.sh" > "$BOOT_TARGETS_DIR/schwung/boot.json"

target=$(bt_resolve_default)
attempt=$(bt_watchdog_enter "$target")

# boot-select: window + picker. Prints the chosen id on stdout; a selection
# rewrites $BOOT_TARGETS_DIR/default itself. Missing or failing binary ->
# silent fallthrough to the resolved default.
if [ -x "$BOOT_SELECT" ]; then
    if bt_watchdog_forced "$target"; then
        chosen=$("$BOOT_SELECT" --forced "$target" 2>/dev/null) || chosen=""
    else
        chosen=$("$BOOT_SELECT" --window "$target" 2>/dev/null) || chosen=""
    fi
    if [ -n "$chosen" ] && [ "$chosen" != "$target" ]; then
        target="$chosen"
        attempt=$(bt_watchdog_enter "$target")
    fi
fi

if [ "$target" = "stock" ]; then
    bt_watchdog_clear
    exec /opt/move/MoveOriginal
fi

entry=$(bt_exec_path "$target") || entry=""
if [ -z "$entry" ] || [ ! -x "$entry" ]; then
    exec /opt/move/MoveOriginal
fi

# Liveness watcher: exec preserves the pid, so checking our own pid after
# 30 s checks the target. A target that forks-and-exits is not covered —
# that is what the healthy touch-file is for (see docs/BOOT_TARGETS.md).
self=$$
(
    sleep 30
    if kill -0 "$self" 2>/dev/null; then
        BOOT_TARGETS_DIR="$BOOT_TARGETS_DIR" . "$BT_LIB" && bt_watchdog_clear
    fi
) &

exec "$entry"
