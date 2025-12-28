#!/usr/bin/env bash
killall MoveLauncher MoveMessageDisplay Move
echo "Waiting 1 second for Move binaries to exit..."
sleep 0.5
echo "Launching!"
cd /data/UserData/control_surface_move
./control_surface_move ./move_menu.js
/opt/move/MoveLauncher
