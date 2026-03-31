#!/bin/sh
# Launch a standalone module, then restart Move when it exits.
# Usage: launch-standalone.sh /path/to/standalone/binary
#
# Called via host_launch_standalone() from the host process.
# This process inherits Move's file descriptors including /dev/ablspi0.0.
# We MUST close them before killing Move.

BINARY="$1"
if [ -z "$BINARY" ] || [ ! -x "$BINARY" ]; then
    echo "launch-standalone: invalid binary: $BINARY" >&2
    exit 1
fi

setsid sh -c '
    BINARY="$1"
    LOG_HELPER=/data/UserData/schwung/unified-log

    log() {
        if [ -x "$LOG_HELPER" ]; then
            "$LOG_HELPER" standalone "$*"
        elif [ -f /data/UserData/schwung/debug_log_on ]; then
            printf "%s\n" "$*" >> /data/UserData/schwung/debug.log
        fi
    }

    # Close ALL inherited file descriptors (3+)
    i=3; while [ $i -lt 1024 ]; do eval "exec ${i}>&-" 2>/dev/null; i=$((i+1)); done

    exec >/dev/null 2>&1
    log "=== launch-standalone.sh started at $(date) ==="
    log "Binary: $BINARY"
    sleep 1

    # Two-phase kill
    for name in MoveMessageDisplay MoveLauncher Move MoveOriginal schwung shadow_ui; do
        pids=$(pidof $name 2>/dev/null || true)
        if [ -n "$pids" ]; then
            log "SIGTERM $name: $pids"
            kill $pids 2>/dev/null || true
        fi
    done
    sleep 0.5

    for name in MoveMessageDisplay MoveLauncher Move MoveOriginal schwung shadow_ui; do
        pids=$(pidof $name 2>/dev/null || true)
        if [ -n "$pids" ]; then
            log "SIGKILL $name: $pids"
            kill -9 $pids 2>/dev/null || true
        fi
    done
    sleep 0.2

    # Free SPI device
    pids=$(fuser /dev/ablspi0.0 2>/dev/null || true)
    if [ -n "$pids" ]; then
        log "Killing SPI holders: $pids"
        kill -9 $pids 2>/dev/null || true
        sleep 0.5
    fi

    # Run standalone binary (blocks until exit)
    log "Launching: $BINARY"
    "$BINARY"
    EXIT_CODE=$?
    log "Standalone exited with code $EXIT_CODE"

    # Restart Move
    log "Restarting Move..."
    sleep 0.5
    if [ -x "$LOG_HELPER" ]; then
        nohup sh -c "/opt/move/Move 2>&1 | /data/UserData/schwung/unified-log move-shim" >/dev/null 2>&1 &
    else
        nohup /opt/move/Move >/dev/null 2>&1 &
    fi
    log "Move restarted with PID $!"
' _ "$BINARY" &
