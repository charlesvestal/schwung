#!/bin/sh
# Schwung diagnostic capture — for users hitting the "hollow / phasey audio" bug.
#
# Run this on your Mac via SSH (the script lives on the Move device):
#   ssh ableton@move.local /data/UserData/schwung/scripts/collect-diagnostics.sh
#
# The script captures 60 seconds of SPI/MIDI traffic plus system state, then
# packages it into a tarball under /data/UserData/schwung/.
# DURING the 60-second capture, plug and replug your headphones two times.
#
# When done, copy the tarball back to your Mac:
#   scp ableton@move.local:/data/UserData/schwung/diagnostics-*.tgz ~/Desktop/
#
# The tarball contains nothing personal — only MIDI traffic, kernel info, and
# Schwung's debug log. Send it to Charles.

set -e

OUT_DIR=/data/UserData/schwung/diagnostics
STAMP=$(date +%Y%m%d-%H%M%S)
TARBALL=/data/UserData/schwung/diagnostics-${STAMP}.tgz
LOG=/data/UserData/schwung/xmos_sysex.txt
FLAG=/data/UserData/schwung/log_xmos_sysex_on
CAPTURE_SECONDS=60

mkdir -p "$OUT_DIR"
rm -f "$LOG"

echo "==== Schwung diagnostic capture ===="
echo "Stamp:    $STAMP"
echo "Output:   $TARBALL"
echo
echo "Arming SysEx + jack-detect logger..."
touch "$FLAG"

echo
echo ">>> CAPTURE WINDOW: $CAPTURE_SECONDS seconds <<<"
echo ">>> While this runs, UNPLUG and REPLUG headphones 2 times <<<"
echo

i=0
while [ $i -lt $CAPTURE_SECONDS ]; do
    sleep 1
    i=$((i + 1))
    if [ $((i % 10)) -eq 0 ]; then
        echo "  ${i}s elapsed..."
    fi
done

echo
echo "Disabling logger..."
rm -f "$FLAG"
sleep 2  # allow shim to close fd on next 1s flag check

echo "Gathering system info..."
{
    echo "==== diagnostic capture $STAMP ===="
    echo
    echo "==== uname ===="
    uname -a
    echo
    echo "==== schwung version ===="
    cat /data/UserData/schwung/host/version.txt 2>/dev/null || echo "(no version.txt)"
    echo
    echo "==== uptime ===="
    uptime
    echo
    echo "==== free ===="
    free 2>/dev/null
    echo
    echo "==== df -h ===="
    df -h
    echo
    echo "==== running schwung-related processes ===="
    ps | grep -iE 'Move|schwung' | grep -v grep
    echo
    echo "==== loaded modules ===="
    ls /data/UserData/schwung/modules/ 2>/dev/null
    echo
    echo "==== current patch ===="
    cat /data/UserData/schwung/current_patch.json 2>/dev/null || echo "(no current_patch.json)"
    echo
    echo "==== shadow control ===="
    ls -la /dev/shm/ 2>/dev/null | grep -i schwung
    echo
    echo "==== features ===="
    cat /data/UserData/schwung/features.json 2>/dev/null || echo "(no features.json)"
} > "$OUT_DIR/system-info.txt"

echo "Copying logs..."
[ -f "$LOG" ] && cp "$LOG" "$OUT_DIR/xmos_sysex.txt"
[ -f /data/UserData/schwung/debug.log ] && tail -c 200000 /data/UserData/schwung/debug.log > "$OUT_DIR/debug.log.tail"

echo "Bundling..."
cd /data/UserData/schwung
tar -czf "$TARBALL" diagnostics/

# Cleanup intermediate files (keep tarball)
rm -rf "$OUT_DIR"
rm -f "$LOG"

SIZE=$(ls -lh "$TARBALL" | awk '{print $5}')
echo
echo "==== DONE ===="
echo "Bundle: $TARBALL ($SIZE)"
echo
echo "Copy to your Mac with:"
echo "  scp ableton@move.local:$TARBALL ~/Desktop/"
echo
