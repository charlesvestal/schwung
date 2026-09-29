#!/bin/bash
# Launch a standalone module, then restart Move when it exits.
# Usage: launch-standalone.sh /path/to/standalone/binary
#
# Called via host_launch_standalone() from the host process.
# This process inherits Move's file descriptors including /dev/ablspi0.0.
# We MUST close them before killing Move.
#
# bash (not /bin/sh): some custom Move images symlink /bin/sh to dash, which
# uses FDs 10-19 internally for builtin redirections. The "close all FDs 3+"
# loop below silently breaks the shell mid-script on dash. Force bash to
# avoid the issue while keeping the script behavior identical on stock Move.

BINARY="$1"
if [ -z "$BINARY" ] || [ ! -x "$BINARY" ]; then
    echo "launch-standalone: invalid binary: $BINARY" >&2
    exit 1
fi

setsid bash -c '
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

    # Let shadow_ui SAVE before anything else dies. SIGTERM makes it raise
    # should_exit and run the same save-and-leave path a restart uses, which
    # also stops the shim respawning it -- so this must happen while the shim
    # (which serves its param reads) is still alive. Bounded: a wedged
    # shadow_ui is killed below like everything else.
    pids=$(pidof shadow_ui 2>/dev/null || true)
    if [ -n "$pids" ]; then
        log "SIGTERM shadow_ui (save): $pids"
        kill $pids 2>/dev/null || true
        n=0
        while [ $n -lt 30 ] && pidof shadow_ui >/dev/null 2>&1; do
            sleep 0.1; n=$((n+1))
        done
        log "shadow_ui quiesced after $((n*100)) ms"
    fi

    # Stand the supervisor down BEFORE the sweep. move-launcher.service is
    # Restart=on-failure, so killing MoveLauncher by name reads to systemd as a
    # failure: ~2 s later it brings the whole stock stack back ALONGSIDE the
    # standalone binary, both driving /dev/ablspi0.0. Stopping the unit first
    # means the sweep below kills an unsupervised stack. We are ableton and
    # cannot stop a unit; schwung-heal can (a closed verb, hardcoded unit name).
    #
    # Only when the unit is KillMode=process: `systemctl stop` then signals the
    # unit main process (MoveLauncher) alone. Under a cgroup kill mode it would
    # take this script -- a descendant of MoveOriginal -- down with it, and a
    # stopped unit is never restarted, leaving a device running nothing. So
    # anything else keeps the old behaviour.
    HEAL=/data/UserData/schwung/bin/schwung-heal
    PAUSED=0
    if [ -u "$HEAL" ] && [ -x /usr/bin/systemctl ]; then
        KM=$(/usr/bin/systemctl show -p KillMode --value move-launcher.service 2>/dev/null)
        if [ "$KM" = "process" ]; then
            if "$HEAL" --pause-launcher; then
                PAUSED=1
                log "move-launcher paused"
            else
                log "WARNING: could not pause move-launcher; stock may respawn alongside"
            fi
        else
            log "move-launcher KillMode=${KM:-unknown}; not pausing it"
        fi
    fi

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

    # Restart Move. Through its supervisor when we paused it -- that is the
    # real boot path (MoveLauncher, the selector, supervision restored); the
    # bare exec below runs Move with no supervisor at all.
    log "Restarting Move..."
    sleep 0.5
    if [ "$PAUSED" = "1" ] && "$HEAL" --resume-launcher; then
        log "move-launcher resumed"
    else
        if [ -x "$LOG_HELPER" ]; then
            nohup sh -c "/opt/move/Move 2>&1 | /data/UserData/schwung/unified-log move-shim" >/dev/null 2>&1 &
        else
            nohup /opt/move/Move >/dev/null 2>&1 &
        fi
        log "Move restarted with PID $!"
    fi
' _ "$BINARY" &
