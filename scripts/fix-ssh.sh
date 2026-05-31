#!/usr/bin/env bash
# Ensure SSH access works for the ableton user on Move.
#
# The device's /data/authorized_keys is shared by all users via sshd_config's
# AuthorizedKeysFile directive, but is owned by root with mode 0600 so only
# root can read it.  This script makes it world-readable (0644) so sshd can
# use it for the ableton user too.  SSH authorized_keys are public keys, so
# 0644 is safe.

set -euo pipefail

# Host may be overridden when move.local doesn't resolve: MOVE_HOST=<ip> fix-ssh.sh
hostname="${MOVE_HOST:-move.local}"

# Resolve an explicit identity file (-i) for the target so this works with a
# bare IP that has no matching "Host <ip>" config block. Space-guarded because
# the command strings are expanded unquoted; returns "" (and 0) when unsafe/none.
_resolve_identity() {
  _k=""
  if [ -f "$HOME/.ssh/config" ]; then
    for _h in "$hostname" "move.local"; do
      _k=$(awk -v want="$_h" '
        BEGIN{found=0}
        /^[ \t]*[Hh]ost[ \t]/{found=0; line=$0; sub(/^[ \t]*[Hh]ost[ \t]+/,"",line); n=split(line,pats,/[ \t]+/); for(i=1;i<=n;i++) if(pats[i]==want){found=1}; next}
        found && /^[ \t]*[Ii]dentity[Ff]ile[ \t]/{sub(/^[ \t]*[Ii]dentity[Ff]ile[ \t]+/,""); gsub(/[ \t]*$/,""); print; exit}' "$HOME/.ssh/config" | sed "s|~|$HOME|g")
      [ -n "$_k" ] && break
    done
  fi
  if [ -z "$_k" ]; then
    for _c in "$HOME/.ssh/move_key" "$HOME/.ssh/id_ed25519" "$HOME/.ssh/id_rsa" "$HOME/.ssh/id_ecdsa"; do
      [ -f "$_c" ] && { _k="$_c"; break; }
    done
  fi
  case "$_k" in
    '') ;;
    *[[:space:]]*|*[*?[]*) ;;
    *) [ -f "$_k" ] && printf -- '-i %s' "$_k" ;;
  esac
  return 0
}
ssh_identity_opt=$(_resolve_identity)
# Use LogLevel=ERROR (not QUIET) so SSH error messages reach our stderr-capture
# in the diagnostic prints below. QUIET silences them and turns the "Detail:"
# line into an empty string — defeating the whole point of capturing stderr.
ssh_root="ssh $ssh_identity_opt -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new -o LogLevel=ERROR -n root@$hostname"

echo "Connecting to $hostname as root..."
_root_err=$(mktemp)
if ! $ssh_root true 2>"$_root_err"; then
  echo "Error: Cannot connect as root@$hostname."
  echo "Detail: $(tr '\n' ' ' < "$_root_err" | cut -c1-200)"
  rm -f "$_root_err"
  echo "Make sure your Move is on the network and root SSH works."
  echo "If 'move.local' does not resolve, retry with: MOVE_HOST=<ip> fix-ssh.sh"
  exit 1
fi
rm -f "$_root_err"

echo "Fixing /data/authorized_keys permissions..."
$ssh_root chmod 644 /data/authorized_keys

# Verify
echo "Verifying ableton SSH access..."
_verify_err=$(mktemp)
if ssh $ssh_identity_opt -o ConnectTimeout=5 -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o LogLevel=ERROR ableton@$hostname true 2>"$_verify_err"; then
  echo "Success! ssh ableton@$hostname is working."
  rm -f "$_verify_err"
else
  echo "Warning: ableton SSH still failing."
  echo "Detail: $(tr '\n' ' ' < "$_verify_err" | cut -c1-200)"
  rm -f "$_verify_err"
  echo "Check that your local key matches one in /data/authorized_keys on the device."
  exit 1
fi
