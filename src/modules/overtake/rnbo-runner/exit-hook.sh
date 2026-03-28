#!/bin/sh
# RNBO Runner exit cleanup — called by shim on overtake clean exit
# Kill everything RNBO-related. rnbomovecontrol ignores SIGTERM.
killall -9 rnbomovecontrol 2>/dev/null
killall -9 jackd 2>/dev/null
killall -9 rnbooscquery 2>/dev/null
killall -9 jack_transport_link 2>/dev/null
killall -9 rnbo-runner-panel 2>/dev/null
# Disable RNBO display override
/data/UserData/rnbo/scripts/display_ctl 0 2>/dev/null
# Remove running flag (suspend detection)
rm -f /data/UserData/schwung/jack_running
