#!/usr/bin/env bash
killall MoveLauncher MoveMessageDisplay Move
echo "Waiting for Move binaries to exit..."
sleep 0.5
echo "Launching Move Anything..."
cd /data/UserData/move-anything
# Ensure tmp directory exists for store module downloads
mkdir -p /data/UserData/move-anything/tmp
LOG=/data/UserData/move-anything/move-anything.log
./move-anything ./host/menu_ui.js > "$LOG" 2>&1
# move-anything has exited, restart Move with shim (avoid MoveLauncher error screen)
echo "Move Anything exited, restarting Move..."
killall MoveLauncher MoveMessageDisplay Move || true
exec /opt/move/Move
