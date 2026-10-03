#!/usr/bin/env bash
#
# Mirror the Move's /data/UserData tree on macOS, so modules that hardcode
# device-absolute paths find their content.
#
#   desktop/setup-data-mirror.sh          create the tree, print the one
#                                         privileged step
#   desktop/setup-data-mirror.sh --check  report status only
#
# WHY THIS EXISTS. 21 of 78 module repos hardcode "/data/UserData/..." --
# overwhelmingly Move's user library:
#
#     /data/UserData/UserLibrary/Wavetables   10 uses
#     /data/UserData/UserLibrary/Samples       6
#     /data/UserData/UserLibrary/Tablor        3
#
# plus per-module stores like breakbeat's sample library. These are not a
# configuration mistake -- on the device that path is simply where content
# lives -- so the fix is to make the path exist here rather than to patch 21
# repos and every future one. breakbeat with a clock and a real sample renders
# at -19.6 dBFS; the same module with neither reads as broken.
#
# HOW, ON macOS. The root filesystem is read-only and SIP-protected, so
# /data cannot just be mkdir'd. /etc/synthetic.conf is Apple's supported
# mechanism for exactly this: it declares symlinks to be created at / on
# boot, and apfs.util -B applies it without rebooting.
#
# THE PRIVILEGED STEP IS NOT RUN FOR YOU. It edits a system file, and that is
# the user's call -- this script prints the exact command and stops.
#
# The target deliberately contains NO SPACES. synthetic.conf is a
# tab-separated format and a path with spaces in it is a good way to get a
# silently ignored line, which would leave /data missing with nothing to say
# why. The modules live under "Application Support"; their DATA lives here.
set -uo pipefail

MIRROR="$HOME/.schwung/data"
USERDATA="$MIRROR/UserData"
CONF=/etc/synthetic.conf
LINE="data	$MIRROR"

check_only=0
[ "${1:-}" = "--check" ] && check_only=1

status() {
    echo "mirror tree : $([ -d "$USERDATA" ] && echo "present  $USERDATA" || echo "absent")"
    if [ -L /data ]; then
        echo "/data       : symlink -> $(readlink /data)"
    elif [ -e /data ]; then
        echo "/data       : exists, but is not a symlink"
    else
        echo "/data       : absent"
    fi
    if [ -f "$CONF" ] && grep -q "^data	" "$CONF" 2>/dev/null; then
        echo "synthetic   : configured"
    else
        echo "synthetic   : not configured"
    fi
}

if [ "$check_only" = 1 ]; then
    status
    exit 0
fi

if [ "$(uname -s)" != "Darwin" ]; then
    echo "setup-data-mirror.sh: macOS only" >&2
    exit 1
fi

# The directories the fleet actually reaches for, from the survey above.
mkdir -p \
    "$USERDATA/UserLibrary/Samples" \
    "$USERDATA/UserLibrary/Wavetables" \
    "$USERDATA/UserLibrary/Tablor" \
    "$USERDATA/schwung" \
    "$USERDATA/breakbeat-samples/Built-in"

echo "created the mirror under $USERDATA"
echo

if [ -L /data ] && [ "$(readlink /data)" = "$MIRROR" ]; then
    echo "/data already points here -- nothing else to do."
    exit 0
fi

if [ -e /data ] && [ ! -L /data ]; then
    echo "WARNING: /data already exists and is not a symlink. Leaving it alone;" >&2
    echo "something else on this machine owns it." >&2
    exit 1
fi

cat <<EOF
One privileged step remains, and it is not run for you because it edits a
system file. To make /data point at the mirror:

  printf '%s\\n' 'data\t$MIRROR' | sudo tee -a $CONF
  sudo /System/Library/Filesystems/apfs.fs/Contents/Resources/apfs.util -B

The second command applies it immediately; without it the link appears at the
next boot. To undo, remove the line from $CONF and reboot.

Then put content where the modules look, e.g.

  $USERDATA/UserLibrary/Samples/
  $USERDATA/breakbeat-samples/Built-in/

and re-run  desktop/setup-data-mirror.sh --check  to confirm.
EOF
