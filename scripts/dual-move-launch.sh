#!/usr/bin/env bash
#
# dual-move-launch.sh — spawn / launch / clean up the dual-MoveOriginal PoC.
#
# Verified working 2026-04-27: instance B successfully boots through D-Bus
# service registration, audio engine init, and XMOS state read on a
# per-instance bus, *provided* /dev/ablspi0.0 is free (i.e. stock instance
# A is stopped). Two-Moves-simultaneously needs the schwung-shim SPI
# broker work — see docs/dual-move-spi-broker-design.md.
#
# Subcommands:
#   spawn-bus   — start the per-instance dbus-daemon if not running
#   launch-b    — kill stock, launch instance B in foreground (ssh blocks)
#   restore     — kill any test processes, run /etc/init.d/move start
#   status      — show what's running
#
# Pairs with dual-move/instance-b.conf (deployed to
# /data/UserData/schwung/dual-move/instance-b.conf on device).

set -euo pipefail

DEVICE="${MOVE_HOST:-move.local}"
DEPLOY_DIR=/data/UserData/schwung/dual-move
BUS_CONF="$DEPLOY_DIR/instance-b.conf"
BUS_ADDR="unix:abstract=move-b-bus"
PID_FILE="$DEPLOY_DIR/dbus-b.pid"
LOG_FILE="$DEPLOY_DIR/move-b.log"

remote_root() { ssh -o ConnectTimeout=5 root@"$DEVICE" "$@"; }
remote_user() { ssh -o ConnectTimeout=5 ableton@"$DEVICE" "$@"; }

ensure_deploy() {
    local local_conf
    local_conf="$(dirname "$0")/../dual-move/instance-b.conf"
    [ -f "$local_conf" ] || { echo "missing $local_conf"; exit 1; }
    remote_root "mkdir -p $DEPLOY_DIR && chown ableton:users $DEPLOY_DIR"
    scp -q "$local_conf" root@"$DEVICE":"$BUS_CONF"
    remote_root "chown ableton:users $BUS_CONF"
}

cmd_spawn_bus() {
    ensure_deploy
    if remote_root "[ -f $PID_FILE ] && kill -0 \$(cat $PID_FILE) 2>/dev/null"; then
        echo "instance-b dbus-daemon already running, address $BUS_ADDR"
        return 0
    fi
    remote_root "rm -f $PID_FILE"
    remote_user "dbus-daemon --config-file=$BUS_CONF --fork --print-address"
    sleep 0.3
    local pid
    pid=$(remote_root "cat $PID_FILE 2>/dev/null || true")
    [ -n "$pid" ] || { echo "dbus-daemon failed to start"; exit 1; }
    echo "spawned dbus-daemon pid=$pid address=$BUS_ADDR"
}

cmd_launch_b() {
    cmd_spawn_bus
    echo "WARN: this will SIGSTOP MoveLauncher and SIGKILL stock MoveOriginal."
    echo "WARN: Move display will show 'Move crashed' until 'restore' is run."
    echo "WARN: ctrl-C this script then run '$0 restore' to recover."
    read -r -p "proceed? [y/N] " ans
    [ "$ans" = "y" ] || { echo "aborted"; exit 1; }

    remote_root "
        LP=\$(pgrep -f /opt/move/MoveLauncher | head -n 1)
        MOP=\$(pgrep -f /opt/move/MoveOriginal | head -n 1)
        echo \"suspending MoveLauncher pid=\$LP\"
        kill -STOP \$LP
        echo \"killing MoveOriginal pid=\$MOP\"
        kill -9 \$MOP
        sleep 1
    "
    echo "launching instance B (env DBUS_SYSTEM_BUS_ADDRESS=$BUS_ADDR)..."
    echo "log: $LOG_FILE"
    remote_user "
        rm -f $LOG_FILE
        DBUS_SYSTEM_BUS_ADDRESS=$BUS_ADDR /opt/move/MoveOriginal > $LOG_FILE 2>&1
    " || true
    echo "instance B exited"
    cmd_restore
}

cmd_restore() {
    remote_root "
        pkill -9 -f MoveMessageDisplay 2>/dev/null || true
        PID=\$(cat $PID_FILE 2>/dev/null || true)
        [ -n \"\$PID\" ] && kill \$PID 2>/dev/null && echo \"killed dbus-b pid=\$PID\"
        rm -f $PID_FILE
        pkill -9 -f /opt/move/MoveOriginal 2>/dev/null || true
        pkill -9 -f /opt/move/MoveLauncher 2>/dev/null || true
        sleep 1
        /etc/init.d/move start
        sleep 4
        pgrep -af /opt/move/MoveOriginal | head -n 3
    "
}

cmd_status() {
    remote_root "
        echo '=== stock MoveOriginal ==='
        pgrep -af /opt/move/MoveOriginal | head -n 3
        echo '=== instance-b dbus-daemon ==='
        pgrep -af 'dbus-daemon.*instance-b' | head -n 3
        echo '=== last instance B log lines ==='
        [ -f $LOG_FILE ] && tail -n 8 $LOG_FILE
    "
}

case "${1:-}" in
    spawn-bus) cmd_spawn_bus ;;
    launch-b)  cmd_launch_b ;;
    restore)   cmd_restore ;;
    status)    cmd_status ;;
    *)
        cat <<USAGE
usage: $0 {spawn-bus|launch-b|restore|status}

  spawn-bus   start per-instance dbus-daemon on $BUS_ADDR
  launch-b    stop stock, launch instance B with private bus (interactive,
              auto-restores on exit)
  restore     kill test processes, /etc/init.d/move start
  status      show running stock + dbus-b + recent instance-B log
USAGE
        exit 2 ;;
esac
