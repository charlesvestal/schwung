#!/usr/bin/env sh
cat << 'EOM'
 __  __                      _                _   _     _
|  \/  | _____   _____      / \   _ __  _   _| |_| |__ (_)_ __   __ _
| |\/| |/ _ \ \ / / _ \    / _ \ | '_ \| | | | __| '_ \| | '_ \ / _` |
| |  | | (_) \ V /  __/   / ___ \| | | | |_| | |_| | | | | | | | (_| |
|_|  |_|\___/ \_/ \___|  /_/   \_\_| |_|\__, |\__|_| |_|_|_| |_|\__, |
                                        |___/                   |___/
EOM

# uncomment to debug
# set -x

set -euo pipefail

fail() {
  echo
  echo "Error: $*"
  exit 1
}

remote_filename=move-anything.tar.gz
hostname=move.local
username=ableton
ssh_ableton="ssh -o LogLevel=QUIET -n $username@$hostname"
scp_ableton="scp -o ConnectTimeout=1"
ssh_root="ssh -o LogLevel=QUIET -n root@$hostname"

if [ "${1:-}" = "local" ]; then
  SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
  REPO_ROOT="$(dirname "$SCRIPT_DIR")"
  local_file="$REPO_ROOT/$remote_filename"
  echo "Using local build: $local_file"
  if [ ! -f "$local_file" ]; then
    fail "Local build not found. Run ./scripts/build.sh first."
  fi
else
  url=https://github.com/charlesvestal/move-anything/raw/main/
  echo "Downloading build from $url$remote_filename"
  curl -LO "$url$remote_filename"
  local_file="$remote_filename"
fi
echo "Build MD5: $(md5sum "$local_file")"

echo "Connecting via ssh to $ssh_ableton..."
if ! $ssh_ableton -o ConnectTimeout=1 ls &> /dev/null
then
    echo
    echo "Error: Could not connect to $hostname using SSH."
    echo "Check that your Move is connected to the same network as this device"
    echo "and that you have added your keys at http://move.local/development/ssh"
    echo
    echo "If your Move was updated, or its keys changed, you may have to remove"
    echo "entries for it in your known_hosts file."
    exit
fi

$scp_ableton "$local_file" "$username@$hostname:./$remote_filename"
$ssh_ableton "tar -xzvf ./$remote_filename"

# Verify expected payload exists before making system changes
$ssh_ableton "test -f /data/UserData/move-anything/move-anything-shim.so" || fail "Payload missing: move-anything-shim.so"
$ssh_ableton "test -f /data/UserData/move-anything/shim-entrypoint.sh" || fail "Payload missing: shim-entrypoint.sh"

# Verify modules directory exists
if $ssh_ableton "test -d /data/UserData/move-anything/modules"; then
  echo "Modules directory found"
  $ssh_ableton "ls /data/UserData/move-anything/modules/"
else
  echo "Warning: No modules directory found"
fi

# killall returns non-zero if a process isn't running; don't treat that as fatal
$ssh_ableton "killall MoveLauncher Move MoveOriginal move-anything || true"

$ssh_root cp -aL /data/UserData/move-anything/move-anything-shim.so /usr/lib/
$ssh_root chmod u+s /usr/lib/move-anything-shim.so

# Ensure the replacement Move script exists and is executable
$ssh_root chmod +x /data/UserData/move-anything/shim-entrypoint.sh

# Backup original only once, and only if current Move exists
if $ssh_root "test ! -f /opt/move/MoveOriginal"; then
  $ssh_root "test -f /opt/move/Move" || fail "Missing /opt/move/Move; refusing to proceed"
  $ssh_root mv /opt/move/Move /opt/move/MoveOriginal
  $ssh_ableton "cp /opt/move/MoveOriginal ~/"
fi

# Install the shimmed Move entrypoint
$ssh_root cp /data/UserData/move-anything/shim-entrypoint.sh /opt/move/Move

$ssh_root md5sum /opt/move/Move
$ssh_root md5sum /opt/move/MoveOriginal
$ssh_root md5sum /usr/lib/move-anything-shim.so

echo "Restarting Move binary with shim installed..."

$ssh_ableton "test -x /opt/move/MoveLauncher" || fail "Missing /opt/move/MoveLauncher"
$ssh_ableton "nohup /opt/move/MoveLauncher 2>/dev/null 1>/dev/null &" &

echo
echo "Done!"
echo
echo "Move Anything is now installed with the modular plugin system."
echo "Modules are located in: /data/UserData/move-anything/modules/"
echo
echo "To launch Move Anything: hold Shift + touch Volume knob + Knob 8"
echo "To return to Move: hold Shift + click Jog wheel"
