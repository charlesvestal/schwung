#!/bin/sh
# Restart rnbomovecontrol with JACK already running
kill $(pgrep rnbomovecontrol) 2>/dev/null
sleep 2
export HOME=/data/UserData
export LD_LIBRARY_PATH=/data/UserData/rnbo/lib:$LD_LIBRARY_PATH
nohup /data/UserData/rnbo/bin/rnbomovecontrol -s /data/UserData/rnbo/config/control-startup-shadow-nojack.json > /tmp/schwung-rnbomovecontrol.log 2>&1 &
