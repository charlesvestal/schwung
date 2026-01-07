#!/usr/bin/env bash
killall MoveLauncher MoveMessageDisplay Move
echo "Waiting for Move binaries to exit..."
sleep 0.5
echo "Launching Move Anything..."
cd /data/UserData/move-anything
LOG=/data/UserData/move-anything/move-anything.log
./move-anything ./host/menu_ui.js > "$LOG" 2>&1 &
PID=$!
echo "move-anything pid=$PID (logging to $LOG)"
/opt/move/MoveLauncher
