#!/usr/bin/env bash
set -x
echo "Killing control_surface_move process on Move..."
ssh ableton@move.local killall control_surface_move
echo "Copying build to Move..."
scp control_surface_move.tar.gz ableton@move.local:./
ssh root@move.local tar -xf /data/UserData/control_surface_move.tar.gz -C /data/UserData
ssh root@move.local cp -aL /data/UserData/control_surface_move/control_surface_move_shim.so /usr/lib/
ssh root@move.local chmod u+s /usr/lib/control_surface_move_shim.so
if ssh root@move.local "test ! -f /opt/move/MoveOriginal"; then
  ssh root@move.local mv /opt/move/Move /opt/move/MoveOriginal
fi

ssh root@move.local chmod +x /data/UserData/control_surface_move/Move.sh
ssh root@move.local cp /data/UserData/control_surface_move/Move.sh /opt/move/Move

# cp /data/UserData/control_surface_move/Move.sh /opt/move/Move
# /etc/suid-debug
# ssh ableton@move.local "nohup ~/control_surface_move/control_surface_move ~/control_surface_move/move_default.js 1>/dev/null 2>/dev/null &"
