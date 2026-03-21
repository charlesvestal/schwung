#!/usr/bin/env bash
killall MoveLauncher MoveMessageDisplay Move
echo "Waiting for Move binaries to exit..."
sleep 0.5
echo "Launching Schwung..."
cd /data/UserData/schwung
# Ensure tmp directory exists for store module downloads
mkdir -p /data/UserData/schwung/tmp
LOG=/data/UserData/schwung/schwung.log
./schwung ./host/menu_ui.js > "$LOG" 2>&1
# schwung has exited, restart Move with shim (avoid MoveLauncher error screen)
echo "Schwung exited, restarting Move..."
killall MoveLauncher MoveMessageDisplay Move || true
exec /opt/move/Move
