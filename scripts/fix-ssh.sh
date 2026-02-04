#!/usr/bin/env sh
# Ensure SSH access works for the ableton user on Move.
#
# The device's /data/authorized_keys is shared by all users via sshd_config's
# AuthorizedKeysFile directive, but is owned by root with mode 0600 so only
# root can read it.  This script makes it world-readable (0644) so sshd can
# use it for the ableton user too.  SSH authorized_keys are public keys, so
# 0644 is safe.

set -euo pipefail

hostname=move.local
ssh_root="ssh -o ConnectTimeout=5 -o LogLevel=QUIET -n root@$hostname"

echo "Connecting to $hostname as root..."
if ! $ssh_root true 2>/dev/null; then
  echo "Error: Cannot connect as root@$hostname."
  echo "Make sure your Move is on the network and root SSH works."
  exit 1
fi

echo "Fixing /data/authorized_keys permissions..."
$ssh_root chmod 644 /data/authorized_keys

# Verify
echo "Verifying ableton SSH access..."
if ssh -o ConnectTimeout=5 -o BatchMode=yes -o LogLevel=QUIET ableton@$hostname true 2>/dev/null; then
  echo "Success! ssh ableton@$hostname is working."
else
  echo "Warning: ableton SSH still failing."
  echo "Check that your local key matches one in /data/authorized_keys on the device."
  exit 1
fi
