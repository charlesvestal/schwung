#!/bin/sh
# Restart Move with shim cleanly
# Detaches, kills all Move processes (including MoveLauncher to prevent
# race conditions), then starts Move fresh.
#
# Called via system() from the shim, so this process inherits all of
# MoveOriginal's file descriptors (including /dev/ablspi0.0).
# We MUST close them before killing MoveOriginal, otherwise this script
# itself holds SPI open and fuser will find us as a holder.

setsid sh -c '
    LOG_HELPER=/data/UserData/move-anything/unified-log

    log() {
        if [ -x "$LOG_HELPER" ]; then
            "$LOG_HELPER" restart-move "$*"
        elif [ -f /data/UserData/move-anything/debug_log_on ]; then
            printf "%s\n" "$*" >> /data/UserData/move-anything/debug.log
        fi
    }

    # Close ALL inherited file descriptors (3+) before doing anything.
    # The SPI device fd inherited from MoveOriginal would otherwise
    # prevent the new Move from opening it.
    i=3; while [ $i -lt 1024 ]; do eval "exec ${i}>&-" 2>/dev/null; i=$((i+1)); done

    exec >/dev/null 2>&1
    log "=== restart-move.sh started at $(date) ==="
    sleep 1

    # Two-phase kill (matching install.sh): SIGTERM first, then SIGKILL
    for name in MoveMessageDisplay MoveLauncher Move MoveOriginal move-anything shadow_ui; do
        pids=$(pidof $name 2>/dev/null || true)
        if [ -n "$pids" ]; then
            log "SIGTERM $name: $pids"
            kill $pids 2>/dev/null || true
        fi
    done
    sleep 0.5

    for name in MoveMessageDisplay MoveLauncher Move MoveOriginal move-anything shadow_ui; do
        pids=$(pidof $name 2>/dev/null || true)
        if [ -n "$pids" ]; then
            log "SIGKILL $name: $pids"
            kill -9 $pids 2>/dev/null || true
        fi
    done
    sleep 0.2

    # Free the SPI device if anything still holds it
    pids=$(fuser /dev/ablspi0.0 2>/dev/null || true)
    if [ -n "$pids" ]; then
        log "Killing SPI holders: $pids"
        kill -9 $pids 2>/dev/null || true
        sleep 0.5
    fi

    log "Starting Move..."
    if [ -x "$LOG_HELPER" ]; then
        nohup sh -c "/opt/move/Move 2>&1 | /data/UserData/move-anything/unified-log move-shim" >/dev/null 2>&1 &
    else
        nohup /opt/move/Move >/dev/null 2>&1 &
    fi
    log "Move started with PID $!"
' &
