#!/bin/sh
# schwung-entry.sh — the Schwung boot target. Exec'd by the selector
# (/opt/move/Move, i.e. shim-entrypoint.sh). Launches Schwung's sidecar
# services, then execs MoveOriginal under the shim. Registered at
# /data/UserData/boot-targets/schwung/boot.json.
SCHWUNG_DIR="/data/UserData/schwung"

# Set library path for bundled TTS libraries
export LD_LIBRARY_PATH=$SCHWUNG_DIR/lib:$LD_LIBRARY_PATH

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
    # Skip if already running.
    #
    # `kill -0` alone is NOT enough: it asks whether SOMETHING holds that pid,
    # not whether the manager does. The pid file survives a reboot, and Linux
    # hands the number out again — observed 2026-08-20, where the stale pid 928
    # came back as `display-server`, so this test passed, the manager was never
    # started, and port 7700 was simply dead until someone noticed. It fails
    # silently and only after a reboot, which is the worst combination.
    #
    # So confirm the pid is actually the manager by reading its cmdline.
    SCHWUNG_MGR_RUNNING=0
    if [ -f "$SCHWUNG_MGR_PID" ]; then
        mgr_pid="$(cat "$SCHWUNG_MGR_PID" 2>/dev/null)"
        # A non-numeric or empty pid file must not turn into a bare `/proc//cmdline`.
        case "$mgr_pid" in
            ''|*[!0-9]*) mgr_pid="" ;;
        esac
        # SCHWUNG_PROC_DIR is /proc on the device; the host test overrides it,
        # since the dev machines have no /proc to build a fixture in.
        if [ -n "$mgr_pid" ] && kill -0 "$mgr_pid" 2>/dev/null &&
           tr '\0' ' ' < "${SCHWUNG_PROC_DIR:-/proc}/$mgr_pid/cmdline" 2>/dev/null |
               grep -q "schwung-manager"; then
            SCHWUNG_MGR_RUNNING=1
        fi
    fi
    if [ "$SCHWUNG_MGR_RUNNING" = "1" ]; then
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

# The standalone :404 file browser is GONE and is not started here any more.
#
# It was a bundled third-party binary serving all of /data/UserData with
# --noauth, gated on a flag file that Global Settings -> Services wrote. Schwung
# Manager already serves the same tree at :7700/files, with keyboard and
# screen-reader access this one never had -- so the toggle amounted to a second
# unauthenticated web server for a job already done.
#
# The flag file on an upgraded device is removed by retireFilebrowserService()
# in shadow_ui.js, which also kills anything still listening. Deleting this
# block alone would have been enough to stop it at the NEXT boot and would have
# left the flag behind for a reinstall to find.

exec env LD_PRELOAD=schwung-shim.so /opt/move/MoveOriginal
