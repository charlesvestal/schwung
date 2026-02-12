#!/usr/bin/env bash

# Detect quiet mode early (for screen reader users)
quiet_mode=false
for arg in "$@"; do
  case "$arg" in
    --enable-screen-reader) enable_screen_reader_arg=true ;;
    --disable-shadow-ui) disable_shadow_ui_arg=true ;;
    --disable-standalone) disable_standalone_arg=true ;;
  esac
done
if [ "${enable_screen_reader_arg:-false}" = true ] && \
   [ "${disable_shadow_ui_arg:-false}" = true ] && \
   [ "${disable_standalone_arg:-false}" = true ]; then
  quiet_mode=true
fi

# Skip ASCII art in quiet mode (screen reader friendly)
if [ "$quiet_mode" = false ]; then
  cat << 'EOM'
 __  __                      _                _   _     _
|  \/  | _____   _____      / \   _ __  _   _| |_| |__ (_)_ __   __ _
| |\/| |/ _ \ \ / / _ \    / _ \ | '_ \| | | | __| '_ \| | '_ \ / _` |
| |  | | (_) \ V /  __/   / ___ \| | | | |_| | |_| | | | | | | | (_| |
|_|  |_|\___/ \_/ \___|  /_/   \_\_| |_|\__, |\__|_| |_|_|_| |_|\__, |
                                        |___/                   |___/
EOM
else
  echo "Move Everything installer (screen reader mode)"
fi

# uncomment to debug
# set -x

set -euo pipefail

fail() {
  echo
  echo "Error: $*"
  exit 1
}

# Echo only if not in quiet mode (for screen reader friendly output)
qecho() {
  if [ "$quiet_mode" = false ]; then
    echo "$@"
  fi
}

# Echo always (for important messages even in quiet mode)
iecho() {
  echo "$@"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Retry wrapper for scp (network operations can be flaky)
# ═══════════════════════════════════════════════════════════════════════════════

scp_with_retry() {
  local src="$1"
  local dest="$2"
  local max_retries=3
  local retry=0
  while [ $retry -lt $max_retries ]; do
    if $scp_ableton "$src" "$dest" 2>/dev/null; then
      return 0
    fi
    retry=$((retry + 1))
    if [ $retry -lt $max_retries ]; then
      qecho "    Retry $retry/$max_retries..."
      sleep 2
    fi
  done
  qecho "    Failed to copy after $max_retries attempts"
  return 1
}

# ═══════════════════════════════════════════════════════════════════════════════
# Retry wrapper for SSH commands (Windows mDNS can be flaky)
# ═══════════════════════════════════════════════════════════════════════════════

ssh_root_with_retry() {
  local cmd="$1"
  local max_retries=3
  local retry=0
  while [ $retry -lt $max_retries ]; do
    if $ssh_root "$cmd" 2>/dev/null; then
      return 0
    fi
    retry=$((retry + 1))
    if [ $retry -lt $max_retries ]; then
      qecho "  Connection retry $retry/$max_retries..."
      sleep 2
    fi
  done
  qecho "  SSH command failed after $max_retries attempts"
  return 1
}

ssh_ableton_with_retry() {
  local cmd="$1"
  local max_retries=3
  local retry=0
  while [ $retry -lt $max_retries ]; do
    if $ssh_ableton "$cmd" 2>/dev/null; then
      return 0
    fi
    retry=$((retry + 1))
    if [ $retry -lt $max_retries ]; then
      qecho "  Connection retry $retry/$max_retries..."
      sleep 2
    fi
  done
  qecho "  SSH command failed after $max_retries attempts"
  return 1
}

# ═══════════════════════════════════════════════════════════════════════════════
# SSH Setup Wizard
# ═══════════════════════════════════════════════════════════════════════════════

ssh_test_ableton() {
  ssh -o ConnectTimeout=5 -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o LogLevel=ERROR -n ableton@move.local true 2>&1
}

ssh_test_root() {
  ssh -o ConnectTimeout=5 -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o LogLevel=ERROR -n root@move.local true 2>&1
}

ssh_get_configured_key() {
  # Check if SSH config specifies an IdentityFile for move.local
  if [ -f "$HOME/.ssh/config" ]; then
    # Extract IdentityFile from move.local config block
    awk 'BEGIN{found=0} /^Host move\.local/{found=1; next} found && /^Host /{found=0} found && /IdentityFile/{gsub(/.*IdentityFile[ \t]+/,""); gsub(/[ \t]*$/,""); print; exit}' "$HOME/.ssh/config" | sed "s|~|$HOME|g"
  fi
}

ssh_find_public_key() {
  # First check if SSH config specifies a key for move.local
  configured_key=$(ssh_get_configured_key)
  if [ -n "$configured_key" ]; then
    # Check that BOTH private and public key exist
    if [ -f "$configured_key" ] && [ -f "${configured_key}.pub" ]; then
      echo "${configured_key}.pub"
      return 0
    fi
    # Config specifies a key but it doesn't exist - return empty to trigger generation
    return 1
  fi

  # No config entry - check for default keys (check private key exists too)
  for keyfile in "$HOME/.ssh/id_ed25519" "$HOME/.ssh/id_rsa" "$HOME/.ssh/id_ecdsa"; do
    if [ -f "$keyfile" ] && [ -f "${keyfile}.pub" ]; then
      echo "${keyfile}.pub"
      return 0
    fi
  done
  return 1
}

ssh_generate_key() {
  # Check if SSH config specifies a key path for move.local
  configured_key=$(ssh_get_configured_key)
  if [ -n "$configured_key" ]; then
    keypath="$configured_key"
    echo "Generating SSH key at configured path: $keypath"
  else
    keypath="$HOME/.ssh/id_ed25519"
    echo "No SSH key found. Generating one now..."
  fi
  echo
  ssh-keygen -t ed25519 -N "" -f "$keypath" -C "$(whoami)@$(hostname)"
  echo
  echo "SSH key generated successfully."
}

ssh_copy_to_clipboard() {
  pubkey="$1"
  # Try macOS clipboard
  if command -v pbcopy >/dev/null 2>&1; then
    cat "$pubkey" | pbcopy
    return 0
  fi
  # Try Windows clipboard (Git Bash)
  if command -v clip >/dev/null 2>&1; then
    cat "$pubkey" | clip
    return 0
  fi
  # Try Linux clipboard (xclip)
  if command -v xclip >/dev/null 2>&1; then
    cat "$pubkey" | xclip -selection clipboard
    return 0
  fi
  # Try Linux clipboard (xsel)
  if command -v xsel >/dev/null 2>&1; then
    cat "$pubkey" | xsel --clipboard
    return 0
  fi
  return 1
}

ssh_remove_known_host() {
  echo "Removing old entry for move.local..."
  ssh-keygen -R move.local 2>/dev/null || true
  # Also remove by IP if we can resolve it (getent not available on Windows)
  if command -v getent >/dev/null 2>&1; then
    move_ip=$(getent hosts move.local 2>/dev/null | awk '{print $1}')
    if [ -n "$move_ip" ]; then
      ssh-keygen -R "$move_ip" 2>/dev/null || true
    fi
  fi
}

ssh_fix_permissions() {
  echo "Updating /data/authorized_keys permissions..."
  ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new -o LogLevel=ERROR -n root@move.local "chmod 644 /data/authorized_keys"
}

ssh_wizard() {
  echo
  echo "═══════════════════════════════════════════════════════════════════════════════"
  echo "  SSH Setup Wizard for Ableton Move"
  echo "═══════════════════════════════════════════════════════════════════════════════"
  echo

  # Step 1: Find or generate SSH key
  echo "Checking for existing SSH keys..."
  echo
  if pubkey=$(ssh_find_public_key); then
    echo "Found: $pubkey"
    echo "Using your existing SSH key."
  else
    ssh_generate_key
    # Use configured key path if set, otherwise default
    configured_key=$(ssh_get_configured_key)
    if [ -n "$configured_key" ]; then
      pubkey="${configured_key}.pub"
    else
      pubkey="$HOME/.ssh/id_ed25519.pub"
    fi
  fi
  echo

  # Step 2: Display and copy the key
  echo "═══════════════════════════════════════════════════════════════════════════════"
  echo "  Step 1: Copy your public key"
  echo "═══════════════════════════════════════════════════════════════════════════════"
  echo
  echo "Your public SSH key is:"
  echo
  cat "$pubkey"
  echo
  if ssh_copy_to_clipboard "$pubkey"; then
    echo "(The key has been copied to your clipboard)"
  else
    echo "(Copy the key above - clipboard copy not available)"
  fi
  echo

  # Step 3: Guide them to add it
  echo "═══════════════════════════════════════════════════════════════════════════════"
  echo "  Step 2: Add the key to your Move"
  echo "═══════════════════════════════════════════════════════════════════════════════"
  echo
  echo "1. Open your web browser to:  http://move.local/development/ssh"
  echo "2. Paste the key into the text area"
  echo "3. Click 'Save'"
  echo
  printf "Press ENTER when you've added the key..."
  read -r dummy </dev/tty
  echo
}

ssh_ensure_connection() {
  max_retries=3
  retry_count=0

  while [ $retry_count -lt $max_retries ]; do
    echo "Testing SSH connection..."
    ssh_result=$(ssh_test_ableton) || true

    # Check for success
    if [ -z "$ssh_result" ]; then
      echo "✓ SSH connection successful!"
      echo
      return 0
    fi

    # Check for host key verification failure
    if echo "$ssh_result" | grep -qi "host key verification failed\|known_hosts\|REMOTE HOST IDENTIFICATION HAS CHANGED"; then
      echo
      echo "Your Move's fingerprint has changed (this happens after firmware updates)."
      printf "Remove old fingerprint and retry? (y/N): "
      read -r fix_hosts </dev/tty
      if [ "$fix_hosts" = "y" ] || [ "$fix_hosts" = "Y" ]; then
        ssh_remove_known_host
        echo
        retry_count=$((retry_count + 1))
        continue
      fi
    fi

    # Check if root works but ableton doesn't (permissions issue)
    echo
    echo "Connection as 'ableton' failed. Checking root access..."
    root_result=$(ssh_test_root) || true

    if [ -z "$root_result" ] || echo "$root_result" | grep -qi "authenticity"; then
      # Root works (or just needs host key acceptance)
      echo
      echo "Connection as 'ableton' failed, but 'root' works."
      echo "This is usually a permissions issue with the authorized_keys file."
      printf "Fix it automatically? (y/N): "
      read -r fix_perms </dev/tty
      if [ "$fix_perms" = "y" ] || [ "$fix_perms" = "Y" ]; then
        ssh_fix_permissions
        echo
        retry_count=$((retry_count + 1))
        continue
      fi
    fi

    # Connection failed - offer setup wizard or retry
    echo
    echo "SSH connection failed."
    echo
    echo "Troubleshooting:"
    echo "  - Make sure you clicked 'Save' after pasting the key"
    echo "  - Try refreshing http://move.local/development/ssh and adding again"
    echo "  - Verify your Move is connected: can you reach http://move.local ?"
    echo
    printf "Retry connection? (y/N): "
    read -r do_retry </dev/tty
    if [ "$do_retry" = "y" ] || [ "$do_retry" = "Y" ]; then
      retry_count=$((retry_count + 1))
      continue
    else
      return 1
    fi
  done

  echo "Maximum retries reached."
  return 1
}

# ═══════════════════════════════════════════════════════════════════════════════

remote_filename=move-anything.tar.gz
hostname=move.local
username=ableton
ssh_ableton="ssh -o LogLevel=QUIET -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new -n $username@$hostname"
scp_ableton="scp -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new"
ssh_root="ssh -o LogLevel=QUIET -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new -n root@$hostname"

# Parse arguments
use_local=false
skip_modules=false
skip_confirmation=false
enable_screen_reader=false
disable_shadow_ui=false
disable_standalone=false
screen_reader_runtime_available=true
for arg in "$@"; do
  case "$arg" in
    local) use_local=true ;;
    -skip-modules|--skip-modules) skip_modules=true ;;
    -skip-confirmation|--skip-confirmation) skip_confirmation=true ;;
    --enable-screen-reader) enable_screen_reader=true ;;
    --disable-shadow-ui) disable_shadow_ui=true ;;
    --disable-standalone) disable_standalone=true ;;
    -h|--help)
      echo "Usage: install.sh [options]"
      echo ""
      echo "Options:"
      echo "  local                    Use local build instead of GitHub release"
      echo "  --skip-modules           Skip module installation prompt"
      echo "  --skip-confirmation      Skip unsupported/liability confirmation prompt"
      echo "  --enable-screen-reader   Enable screen reader (TTS) by default"
      echo "  --disable-shadow-ui      Disable shadow UI (slot configuration interface)"
      echo "  --disable-standalone     Disable standalone mode (shift+vol+knob8)"
      echo ""
      echo "Examples:"
      echo "  install.sh                                    # Install from GitHub, all features enabled"
      echo "  install.sh local --enable-screen-reader       # Install local build with screen reader on"
      echo "  install.sh --disable-shadow-ui --disable-standalone --enable-screen-reader"
      echo "                                                # Screen reader only, no UI"
      echo ""
      exit 0
      ;;
  esac
done

if [ "$skip_confirmation" = false ]; then
  echo
  echo "**************************************************************"
  echo "*                                                            *"
  echo "*   WARNING:                                                 *"
  echo "*                                                            *"
  echo "*   Are you sure you want to install Move Everything on your *"
  echo "*   Move? This is UNSUPPORTED by Ableton.                    *"
  echo "*                                                            *"
  echo "*   The authors of this project accept no liability for      *"
  echo "*   any damage you incur by proceeding.                      *"
  echo "*                                                            *"
  echo "**************************************************************"
  echo
  echo "Type 'yes' to proceed: "
  read -r response </dev/tty
  if [ "$response" != "yes" ]; then
    echo "Installation aborted."
    exit 1
  fi
fi

if [ "$use_local" = true ]; then
  SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
  REPO_ROOT="$(dirname "$SCRIPT_DIR")"
  local_file="$REPO_ROOT/$remote_filename"
  echo "Using local build: $local_file"
  if [ ! -f "$local_file" ]; then
    fail "Local build not found. Run ./scripts/build.sh first."
  fi
else
  # Find latest binary release (v* tag, not installer-v*)
  tag=$(curl -fsSL https://api.github.com/repos/charlesvestal/move-anything/releases \
    | grep '"tag_name"' | grep -v installer | head -1 | sed 's/.*"tag_name": "//;s/".*//' ) \
    || fail "Failed to query GitHub releases API"
  if [ -z "$tag" ]; then
    fail "Could not find a binary release. Check https://github.com/charlesvestal/move-anything/releases"
  fi
  url="https://github.com/charlesvestal/move-anything/releases/download/${tag}/"
  qecho "Downloading release $tag from $url$remote_filename"
  # Use silent curl in quiet mode (screen reader friendly)
  if [ "$quiet_mode" = true ]; then
    curl -fsSLO "$url$remote_filename" || fail "Failed to download release. Check https://github.com/charlesvestal/move-anything/releases"
  else
    curl -fLO "$url$remote_filename" || fail "Failed to download release. Check https://github.com/charlesvestal/move-anything/releases"
  fi
  local_file="$remote_filename"
fi
if [ "$quiet_mode" = false ]; then
  if command -v md5sum >/dev/null 2>&1; then
    echo "Build MD5: $(md5sum "$local_file")"
  elif command -v md5 >/dev/null 2>&1; then
    echo "Build MD5: $(md5 -q "$local_file")"
  fi
fi

# Check SSH connection, run setup wizard if needed
qecho "Checking SSH connection to $hostname..."
ssh_result=$(ssh_test_ableton) || true

if [ -n "$ssh_result" ]; then
  # SSH failed - check if it's a network issue first
  if echo "$ssh_result" | grep -qi "Could not resolve\|No route to host\|Connection timed out\|Network is unreachable"; then
    echo
    echo "Cannot reach move.local on the network."
    echo
    echo "Please check that:"
    echo "  - Your Move is powered on"
    echo "  - Your Move is connected to the same WiFi network as this computer"
    echo "  - You can access http://move.local in your browser"
    echo
    fail "Network connection to Move failed"
  fi

  # SSH failed for auth/key reasons - offer wizard (interactive only)
  echo
  echo "SSH connection failed."

  if [ "$skip_confirmation" = true ]; then
    # Non-interactive mode (GUI installer) - fail immediately
    fail "SSH connection to $hostname failed (non-interactive mode)"
  fi

  printf "Would you like help setting up SSH access? (y/N): "
  read -r run_wizard </dev/tty

  if [ "$run_wizard" = "y" ] || [ "$run_wizard" = "Y" ]; then
    ssh_wizard
    if ! ssh_ensure_connection; then
      fail "Could not establish SSH connection to Move"
    fi
  else
    iecho ""
    iecho "To set up SSH manually:"
    iecho "  1. Generate a key: ssh-keygen -t ed25519"
    iecho "  2. Add your public key at: http://move.local/development/ssh"
    iecho "  3. Run this install script again"
    fail "SSH connection required for installation"
  fi
else
  qecho "✓ SSH connection OK"
  iecho "Installing Move Everything..."
fi

# Copy and extract main tarball with retry (Windows mDNS can be flaky)
scp_with_retry "$local_file" "$username@$hostname:./$remote_filename" || fail "Failed to copy tarball to device"
# Validate tar payload layout before extraction.
# Some host-side tar variants can encode large files under GNUSparseFile.0 paths
# that BusyBox tar on Move does not restore correctly.
ssh_ableton_with_retry "tar -tzf ./$remote_filename | grep -qx 'move-anything/move-anything-shim.so'" || \
    fail "Invalid tar payload: missing move-anything/move-anything-shim.so entry"
# Use verbose tar only in non-quiet mode (screen reader friendly)
if [ "$quiet_mode" = true ]; then
    ssh_ableton_with_retry "tar -xzf ./$remote_filename" || fail "Failed to extract tarball"
else
    ssh_ableton_with_retry "tar -xzvf ./$remote_filename" || fail "Failed to extract tarball"
fi

# Verify expected payload exists before making system changes
ssh_ableton_with_retry "test -f /data/UserData/move-anything/move-anything-shim.so" || fail "Payload missing: move-anything-shim.so"
ssh_ableton_with_retry "test -f /data/UserData/move-anything/shim-entrypoint.sh" || fail "Payload missing: shim-entrypoint.sh"

# Verify modules directory exists
if ssh_ableton_with_retry "test -d /data/UserData/move-anything/modules"; then
  qecho "Modules directory found"
  if [ "$quiet_mode" = false ]; then
    ssh_ableton_with_retry "ls /data/UserData/move-anything/modules/" || true
  fi
else
  echo "Warning: No modules directory found"
fi

# Migration: Check for patches and modules in old locations
echo
echo "Checking for migration needs..."

# Track deleted modules for reinstall option
deleted_modules=""

# Find external modules in old location (modules/<id>/ instead of modules/<type>/<id>/)
old_modules=$($ssh_ableton "cd /data/UserData/move-anything/modules 2>/dev/null && for d in */; do
  d=\${d%/}
  case \"\$d\" in
    chain|controller|store|text-test|sound_generators|audio_fx|midi_fx|utilities|other) continue ;;
  esac
  if [ -f \"\$d/module.json\" ]; then
    echo \"\$d\"
  fi
done" 2>/dev/null || true)

# Check for patches in old location
old_patches=false
if $ssh_ableton "test -d /data/UserData/move-anything/modules/chain/patches && ls /data/UserData/move-anything/modules/chain/patches/*.json >/dev/null 2>&1"; then
  old_patches=true
fi

# Check for modules in old chain subdirectories
old_chain_modules=$($ssh_ableton "cd /data/UserData/move-anything/modules/chain 2>/dev/null && for subdir in audio_fx midi_fx sound_generators; do
  if [ -d \"\$subdir\" ]; then
    for d in \"\$subdir\"/*/; do
      d=\${d%/}
      if [ -f \"\$d/module.json\" ] 2>/dev/null; then
        echo \"\$d\"
      fi
    done
  fi
done" 2>/dev/null || true)

if [ -n "$old_modules" ] || [ "$old_patches" = true ] || [ -n "$old_chain_modules" ]; then
  echo
  echo "========================================"
  echo "Migration Required (v0.3.0)"
  echo "========================================"
  echo
  echo "This update restructures the module directory."
  echo
  if [ -n "$old_modules" ]; then
    echo "External modules found in old location:"
    echo "  $old_modules"
    echo
  fi
  if [ -n "$old_chain_modules" ]; then
    echo "Chain modules found in old location:"
    echo "  $old_chain_modules"
    echo
  fi
  if [ "$old_patches" = true ]; then
    echo "Patches found in old location:"
    echo "  modules/chain/patches/"
    echo
  fi
  echo "Migration will:"
  echo "  1. Move your patches to the new /patches/ directory"
  echo "  2. DELETE old external modules (they need fresh install)"
  echo
  echo "After migration, reinstall modules via the Module Store."
  echo
  if [ "$skip_confirmation" = true ]; then
    do_migrate="y"
  else
    printf "Proceed with migration? [Y/n] "
    read -r do_migrate </dev/tty
  fi

  if [ "$do_migrate" != "n" ] && [ "$do_migrate" != "N" ]; then
    echo "Migrating..."

    # Migrate patches first
    if [ "$old_patches" = true ]; then
      echo "  Moving patches to /patches/..."
      $ssh_ableton "mkdir -p /data/UserData/move-anything/patches && mv /data/UserData/move-anything/modules/chain/patches/*.json /data/UserData/move-anything/patches/ 2>/dev/null || true"
    fi

    # Delete old external modules (they have wrong import paths)
    for mod in $old_modules; do
      echo "  Removing old module: $mod"
      $ssh_ableton "rm -rf /data/UserData/move-anything/modules/$mod"
      deleted_modules="$deleted_modules $mod"
    done

    # Delete old chain subdirectory modules and track them
    if [ -n "$old_chain_modules" ]; then
      echo "  Removing old chain modules..."
      for chain_mod in $old_chain_modules; do
        # Extract just the module id (e.g., "audio_fx/cloudseed" -> "cloudseed")
        mod_id=$(basename "$chain_mod")
        deleted_modules="$deleted_modules $mod_id"
      done
      $ssh_ableton "rm -rf /data/UserData/move-anything/modules/chain/audio_fx /data/UserData/move-anything/modules/chain/midi_fx /data/UserData/move-anything/modules/chain/sound_generators 2>/dev/null || true"
    fi

    echo
    echo "Migration complete!"
    echo
    echo "Your patches have been preserved."
    echo "Please reinstall external modules via the Module Store"
    echo "or use the install option below."
  else
    echo "Skipping migration. Old modules may not work correctly."
  fi
else
  echo "No migration needed."
fi

# Preflight: clean stale debug/tmp artifacts that can fill root on dev-heavy setups.
# Keep runtime sockets and only remove known one-off files/directories.
ssh_root_with_retry "rm -rf /var/volatile/tmp/_MEI* 2>/dev/null || true; rm -f /var/volatile/tmp/*.pcm /var/volatile/tmp/*.out /var/volatile/tmp/*.err /var/volatile/tmp/yt* /var/volatile/tmp/ytdlp* /var/volatile/tmp/ytmod* /var/volatile/tmp/ytsearch* /var/volatile/tmp/clap_* /var/volatile/tmp/chain_* /var/volatile/tmp/lddebug_* /var/volatile/tmp/preload_* /var/volatile/tmp/verify-* /var/volatile/tmp/auxv_* /var/volatile/tmp/test_shadow.js /var/volatile/tmp/trigger /var/volatile/tmp/surge_debug.log 2>/dev/null || true" || true

# Safety: check root partition has enough free space (< 10MB free = danger zone)
root_avail=$($ssh_root "df / | tail -1 | awk '{print \$4}'" 2>/dev/null || echo "0")
if [ "$root_avail" -lt 10240 ] 2>/dev/null; then
  echo
  echo "Warning: Root partition has less than 10MB free (${root_avail}KB available)"
  echo "Cleaning up any stale backup files..."
  $ssh_root "rm -f /opt/move/Move.bak /opt/move/Move.shim /opt/move/Move.orig 2>/dev/null || true"
  root_avail=$($ssh_root "df / | tail -1 | awk '{print \$4}'" 2>/dev/null || echo "0")
  if [ "$root_avail" -lt 5120 ] 2>/dev/null; then
    fail "Root partition critically low (${root_avail}KB free). Cannot safely proceed."
  fi
  echo "Root partition now has ${root_avail}KB free"
fi

# Ensure shim isn't globally preloaded (breaks XMOS firmware check and causes communication error)
ssh_root_with_retry "if [ -f /etc/ld.so.preload ] && grep -q 'move-anything-shim.so' /etc/ld.so.preload; then ts=\$(date +%Y%m%d-%H%M%S); cp /etc/ld.so.preload /etc/ld.so.preload.bak-move-anything-\$ts; grep -v 'move-anything-shim.so' /etc/ld.so.preload > /tmp/ld.so.preload.new || true; if [ -s /tmp/ld.so.preload.new ]; then cat /tmp/ld.so.preload.new > /etc/ld.so.preload; else rm -f /etc/ld.so.preload; fi; rm -f /tmp/ld.so.preload.new; fi" || true

# Symlink shim to /usr/lib/ (root partition has no free space for copies)
ssh_root_with_retry "rm -f /usr/lib/move-anything-shim.so && ln -s /data/UserData/move-anything/move-anything-shim.so /usr/lib/move-anything-shim.so" || fail "Failed to install shim after retries"
ssh_root_with_retry "chmod u+s /data/UserData/move-anything/move-anything-shim.so" || fail "Failed to set shim permissions"
ssh_root_with_retry "test -u /data/UserData/move-anything/move-anything-shim.so" || fail "Shim setuid bit missing after install"

# Deploy Flite libraries (for TTS support) from /data to /usr/lib via symlink.
# Root partition is nearly full, so symlink instead of copying.
# Use direct predicate checks so expected test failures don't print misleading
# "Connection retry" messages from the retry wrapper.
if $ssh_ableton "test ! -d /data/UserData/move-anything/lib" 2>/dev/null; then
  screen_reader_runtime_available=false
  iecho "Screen reader runtime not bundled; skipping Flite deployment."
elif $ssh_ableton "test -d /data/UserData/move-anything/lib" 2>/dev/null; then
  ssh_ableton_with_retry "for lib in libflite.so.1 libflite_cmu_us_kal.so.1 libflite_usenglish.so.1 libflite_cmulex.so.1; do test -e /data/UserData/move-anything/lib/\$lib || exit 1; done" || fail "Payload missing required Flite libraries (screen reader dependency)"
  qecho "Deploying Flite libraries..."
  ssh_root_with_retry "cd /data/UserData/move-anything/lib && set -- libflite*.so.* && [ \"\$1\" != 'libflite*.so.*' ] || exit 1; for lib in \"\$@\"; do rm -f /usr/lib/\$lib && ln -s /data/UserData/move-anything/lib/\$lib /usr/lib/\$lib; done" || fail "Failed to install Flite libraries"
else
  fail "Failed to check screen reader runtime payload on device (SSH error)"
fi

# Ensure the replacement Move script exists and is executable
ssh_root_with_retry "chmod +x /data/UserData/move-anything/shim-entrypoint.sh" || fail "Failed to set entrypoint permissions"

# Backup original only once, and only if current Move exists
# IMPORTANT: Use mv (not cp) on root partition — it's nearly full (~460MB, <25MB free).
# Never create extra copies of large files under /opt/move/ or anywhere on /.
if $ssh_root "test ! -f /opt/move/MoveOriginal" 2>/dev/null; then
  ssh_root_with_retry "test -f /opt/move/Move" || fail "Missing /opt/move/Move; refusing to proceed"
  ssh_root_with_retry "mv /opt/move/Move /opt/move/MoveOriginal" || fail "Failed to backup original Move"
  ssh_ableton_with_retry "cp /opt/move/MoveOriginal ~/" || true
elif ! $ssh_root "test -f /opt/move/MoveOriginal" 2>/dev/null; then
  fail "Failed to verify /opt/move/MoveOriginal on device (SSH error)"
fi

# Install the shimmed Move entrypoint
ssh_root_with_retry "cp /data/UserData/move-anything/shim-entrypoint.sh /opt/move/Move" || fail "Failed to install shim entrypoint"


# Create feature configuration file
qecho ""
qecho "Configuring features..."
ssh_ableton_with_retry "mkdir -p /data/UserData/move-anything/config" || true

# Preserve link_audio_enabled from existing config (defaults to false)
link_audio_val=$(ssh_ableton_with_retry "grep -o '\"link_audio_enabled\"[[:space:]]*:[[:space:]]*[a-z]*' /data/UserData/move-anything/config/features.json 2>/dev/null | grep -o 'true\|false'" 2>/dev/null || echo "false")

# Build features.json content
features_json="{
  \"shadow_ui_enabled\": $([ "$disable_shadow_ui" = false ] && echo "true" || echo "false"),
  \"standalone_enabled\": $([ "$disable_standalone" = false ] && echo "true" || echo "false"),
  \"link_audio_enabled\": $link_audio_val
}"

# Write features.json
ssh_ableton_with_retry "cat > /data/UserData/move-anything/config/features.json << 'EOF'
$features_json
EOF" || echo "Warning: Failed to create features.json"

# Create screen reader state file if --enable-screen-reader was passed
if [ "$enable_screen_reader" = true ]; then
    if [ "$screen_reader_runtime_available" = true ]; then
      qecho "Enabling screen reader..."
      ssh_ableton_with_retry "echo '1' > /data/UserData/move-anything/config/screen_reader_state.txt" || true
    else
      iecho "Screen reader requested, but this build does not include TTS runtime support."
      enable_screen_reader=false
    fi
fi

if [ "$quiet_mode" = false ]; then
    echo "Features configured:"
    echo "  Shadow UI: $([ "$disable_shadow_ui" = false ] && echo "enabled" || echo "disabled")"
    echo "  Standalone: $([ "$disable_standalone" = false ] && echo "enabled" || echo "disabled")"
    echo "  Screen Reader: $([ "$enable_screen_reader" = true ] && echo "enabled" || echo "disabled (toggle with shift+vol+menu)")"
fi

# Optional: Install modules from the Module Store (before restart so they're available immediately)
# Skip if both shadow UI and standalone are disabled (no way to use modules)
echo
install_mode=""
deleted_modules=$(echo "$deleted_modules" | xargs)  # trim whitespace

if [ "$disable_shadow_ui" = true ] && [ "$disable_standalone" = true ]; then
    echo "Skipping module installation (shadow UI and standalone both disabled)"
    skip_modules=true
elif [ "$skip_modules" = true ]; then
    echo "Skipping module installation (--skip-modules)"
elif [ -n "$deleted_modules" ]; then
    # Migration happened - offer three choices
    echo "Module installation options:"
    echo "  (a) Install ALL available modules"
    echo "  (m) Install only MIGRATED modules: $deleted_modules"
    echo "  (n) Install NONE (use Module Store later)"
    echo
    printf "Choice [a/m/N]: "
    read -r install_choice </dev/tty
    case "$install_choice" in
        a|A) install_mode="all" ;;
        m|M) install_mode="missing" ;;
        *) install_mode="" ;;
    esac
else
    # No migration - offer yes/no for all
    echo "Would you like to install all available modules from the Module Store?"
    echo "(Sound Generators, Audio FX, MIDI FX, and Utilities)"
    printf "Install modules? [y/N] "
    read -r install_choice </dev/tty
    if [ "$install_choice" = "y" ] || [ "$install_choice" = "Y" ]; then
        install_mode="all"
    fi
fi

if [ -n "$install_mode" ]; then
    echo
    echo "Fetching module catalog..."
    catalog_url="https://raw.githubusercontent.com/charlesvestal/move-anything/main/module-catalog.json"
    catalog=$(curl -fsSL "$catalog_url") || { echo "Failed to fetch module catalog"; exit 1; }

    # Parse catalog with awk (no Python needed, works on Windows Git Bash)
    echo "$catalog" | awk '
BEGIN { id=""; name=""; repo=""; asset=""; ctype="" }
/"id":/ { gsub(/.*"id": *"|".*/, ""); id=$0 }
/"name":/ { gsub(/.*"name": *"|".*/, ""); name=$0 }
/"github_repo":/ { gsub(/.*"github_repo": *"|".*/, ""); repo=$0 }
/"asset_name":/ { gsub(/.*"asset_name": *"|".*/, ""); asset=$0 }
/"component_type":/ { gsub(/.*"component_type": *"|".*/, ""); ctype=$0 }
/\}/ {
  if (length(id) > 0 && length(repo) > 0 && length(asset) > 0) {
    if (ctype == "sound_generator") subdir = "sound_generators"
    else if (ctype == "audio_fx") subdir = "audio_fx"
    else if (ctype == "midi_fx") subdir = "midi_fx"
    else if (ctype == "utility") subdir = "utilities"
    else if (ctype == "overtake") subdir = "overtake"
    else subdir = "other"
    print id "|" repo "|" asset "|" name "|" subdir
  }
  id=""; name=""; repo=""; asset=""; ctype=""
}
' | while IFS='|' read -r id repo asset name subdir; do
        # If mode is "missing", only install modules that were deleted
        if [ "$install_mode" = "missing" ]; then
            case " $deleted_modules " in
                *" $id "*) ;;  # Module was deleted, continue to install
                *) continue ;; # Module wasn't deleted, skip
            esac
        fi

        echo
        if [ -n "$subdir" ]; then
            dest="modules/$subdir"
            echo "Installing $name ($id) to $dest/..."
        else
            dest="modules"
            echo "Installing $name ($id)..."
        fi
        url="https://github.com/${repo}/releases/latest/download/${asset}"
        if curl -fsSLO "$url"; then
            # Use retry for scp/ssh because Windows mDNS can be flaky
            if scp_with_retry "$asset" "$username@$hostname:./move-anything/"; then
                ssh_ableton_with_retry "cd move-anything && mkdir -p $dest && tar -xzf $asset -C $dest/ && rm $asset" || echo "  Warning: Failed to extract $name"
            else
                echo "  Warning: Failed to copy $name to device"
            fi
            rm -f "$asset"
            echo "  Installed: $name"
        else
            echo "  Failed to download $name (may not have a release yet)"
        fi
    done

    echo
    echo "========================================"
    echo "Module Installation Complete"
    echo "========================================"
fi

# Offer to copy assets for modules that need them (skip if --skip-modules was used)
if [ "$skip_modules" = false ]; then
    echo
    echo "Some modules require or benefit from additional assets:"
    echo "  - Mini-JV: ROM files + optional SR-JV80 expansions"
    echo "  - SF2: SoundFont files (.sf2)"
    echo "  - Dexed: Additional .syx patch banks (optional - defaults included)"
    echo "  - NAM: .nam model files (free models at tonehunt.org and tone3000.com)"
    echo
    printf "Would you like to copy assets to your Move now? (y/N): "
    read -r copy_assets </dev/tty
else
    copy_assets="n"
fi

if [ "$copy_assets" = "y" ] || [ "$copy_assets" = "Y" ]; then
    echo
    echo "═══════════════════════════════════════════════════════════════════════════════"
    echo "  Asset Copy"
    echo "═══════════════════════════════════════════════════════════════════════════════"

    # Track if any copy failed
    asset_copy_failed=false

    # JV880 ROMs
    echo
    echo "Mini-JV ROMs: Enter the folder containing your JV880 ROM files."
    echo "Expected structure:"
    echo "  your_folder/"
    echo "    jv880_rom1.bin"
    echo "    jv880_rom2.bin        (must be v1.0.0)"
    echo "    jv880_waverom1.bin"
    echo "    jv880_waverom2.bin"
    echo "    jv880_nvram.bin"
    echo "    expansions/           (optional SR-JV80 expansion .bin files)"
    echo
    echo "(Press ENTER to skip)"
    printf "Enter or drag folder path: "
    read -r rom_path </dev/tty

    if [ -n "$rom_path" ]; then
        # Expand ~ to home directory and handle escaped spaces/quotes from drag-and-drop
        rom_path=$(echo "$rom_path" | sed "s|^~|$HOME|" | sed "s/\\\\ / /g; s/\\\\'/'/g" | sed "s/^['\"]//;s/['\"]$//")
        if [ -d "$rom_path" ]; then
            rom_count=0
            ssh_ableton_with_retry "mkdir -p move-anything/modules/sound_generators/minijv/roms" || true
            for rom in jv880_rom1.bin jv880_rom2.bin jv880_waverom1.bin jv880_waverom2.bin jv880_nvram.bin; do
                if [ -f "$rom_path/$rom" ]; then
                    echo "  Copying $rom..."
                    if scp_with_retry "$rom_path/$rom" "$username@$hostname:./move-anything/modules/sound_generators/minijv/roms/"; then
                        rom_count=$((rom_count + 1))
                    else
                        asset_copy_failed=true
                    fi
                fi
            done
            # Copy expansion ROMs if present
            if [ -d "$rom_path/expansions" ]; then
                exp_count=0
                ssh_ableton_with_retry "mkdir -p move-anything/modules/sound_generators/minijv/roms/expansions" || true
                for exp in "$rom_path/expansions"/*.bin "$rom_path/expansions"/*.BIN; do
                    if [ -f "$exp" ]; then
                        echo "  Copying expansion: $(basename "$exp")..."
                        if scp_with_retry "$exp" "$username@$hostname:./move-anything/modules/sound_generators/minijv/roms/expansions/"; then
                            exp_count=$((exp_count + 1))
                        else
                            asset_copy_failed=true
                        fi
                    fi
                done
                if [ $exp_count -gt 0 ]; then
                    echo "  Copied $exp_count expansion ROM(s)"
                fi
            fi
            if [ $rom_count -gt 0 ]; then
                echo "  Copied $rom_count base ROM file(s)"
            else
                echo "  No ROM files found in $rom_path"
            fi
        else
            echo "  Directory not found: $rom_path"
        fi
    fi

    # SoundFonts
    echo
    echo "SF2 SoundFonts: Enter the folder containing your .sf2 files."
    echo "(Press ENTER to skip)"
    printf "Enter or drag folder path: "
    read -r sf2_path </dev/tty

    if [ -n "$sf2_path" ]; then
        # Expand ~ to home directory and handle escaped spaces/quotes from drag-and-drop
        sf2_path=$(echo "$sf2_path" | sed "s|^~|$HOME|" | sed "s/\\\\ / /g; s/\\\\'/'/g" | sed "s/^['\"]//;s/['\"]$//")
        if [ -d "$sf2_path" ]; then
            sf2_count=0
            ssh_ableton_with_retry "mkdir -p move-anything/modules/sound_generators/sf2/soundfonts" || true
            for sf2 in "$sf2_path"/*.sf2 "$sf2_path"/*.SF2; do
                if [ -f "$sf2" ]; then
                    echo "  Copying $(basename "$sf2")..."
                    if scp_with_retry "$sf2" "$username@$hostname:./move-anything/modules/sound_generators/sf2/soundfonts/"; then
                        sf2_count=$((sf2_count + 1))
                    else
                        asset_copy_failed=true
                    fi
                fi
            done
            if [ $sf2_count -gt 0 ]; then
                echo "  Copied $sf2_count SoundFont file(s)"
            else
                echo "  No .sf2 files found in $sf2_path"
            fi
        else
            echo "  Directory not found: $sf2_path"
        fi
    fi

    # DX7 patches
    echo
    echo "Dexed (DX7): Enter the folder containing your .syx patch banks."
    echo "(Defaults are included - this adds additional banks. Press ENTER to skip)"
    printf "Enter or drag folder path: "
    read -r syx_path </dev/tty

    if [ -n "$syx_path" ]; then
        # Expand ~ to home directory and handle escaped spaces/quotes from drag-and-drop
        syx_path=$(echo "$syx_path" | sed "s|^~|$HOME|" | sed "s/\\\\ / /g; s/\\\\'/'/g" | sed "s/^['\"]//;s/['\"]$//")
        if [ -d "$syx_path" ]; then
            syx_count=0
            ssh_ableton_with_retry "mkdir -p move-anything/modules/sound_generators/dexed/banks" || true
            for syx in "$syx_path"/*.syx "$syx_path"/*.SYX; do
                if [ -f "$syx" ]; then
                    echo "  Copying $(basename "$syx")..."
                    if scp_with_retry "$syx" "$username@$hostname:./move-anything/modules/sound_generators/dexed/banks/"; then
                        syx_count=$((syx_count + 1))
                    else
                        asset_copy_failed=true
                    fi
                fi
            done
            if [ $syx_count -gt 0 ]; then
                echo "  Copied $syx_count patch bank(s)"
            else
                echo "  No .syx files found in $syx_path"
            fi
        else
            echo "  Directory not found: $syx_path"
        fi
    fi

    # NAM models
    echo
    echo "NAM: Enter the folder containing your .nam model files."
    echo "Free models available at: https://tonehunt.org and https://tone3000.com"
    echo "(Press ENTER to skip)"
    printf "Enter or drag folder path: "
    read -r nam_path </dev/tty

    if [ -n "$nam_path" ]; then
        # Expand ~ to home directory and handle escaped spaces/quotes from drag-and-drop
        nam_path=$(echo "$nam_path" | sed "s|^~|$HOME|" | sed "s/\\\\ / /g; s/\\\\'/'/g" | sed "s/^['\"]//;s/['\"]$//")
        if [ -d "$nam_path" ]; then
            nam_count=0
            ssh_ableton_with_retry "mkdir -p move-anything/modules/audio_fx/nam/models" || true
            for nam in "$nam_path"/*.nam "$nam_path"/*.NAM; do
                if [ -f "$nam" ]; then
                    echo "  Copying $(basename "$nam")..."
                    if scp_with_retry "$nam" "$username@$hostname:./move-anything/modules/audio_fx/nam/models/"; then
                        nam_count=$((nam_count + 1))
                    else
                        asset_copy_failed=true
                    fi
                fi
            done
            if [ $nam_count -gt 0 ]; then
                echo "  Copied $nam_count NAM model(s)"
            else
                echo "  No .nam files found in $nam_path"
            fi
        else
            echo "  Directory not found: $nam_path"
        fi
    fi

    echo
    if [ "$asset_copy_failed" = true ]; then
        echo "Asset copy completed with some errors. You may need to copy failed files manually."
    else
        echo "Asset copy complete."
    fi
    if [ -z "$install_mode" ]; then
        echo "Note: Install the modules via the Module Store to use these assets."
    fi
fi

qecho ""
iecho "Restarting Move..."

# Stop Move via init service (kills MoveLauncher + Move + all children cleanly)
# Use retry wrappers because Windows mDNS resolution can be flaky.
ssh_root_with_retry "/etc/init.d/move stop >/dev/null 2>&1 || true" || true
ssh_root_with_retry "for name in MoveOriginal Move MoveLauncher MoveMessageDisplay shadow_ui move-anything; do pids=\$(pidof \$name 2>/dev/null || true); if [ -n \"\$pids\" ]; then kill -9 \$pids 2>/dev/null || true; fi; done" || true
# Clean up stale shared memory so it's recreated with correct permissions
ssh_root_with_retry "rm -f /dev/shm/move-shadow-*" || true
# Free the SPI device if anything still holds it (prevents "communication error" on restart)
ssh_root_with_retry "pids=\$(fuser /dev/ablspi0.0 2>/dev/null || true); if [ -n \"\$pids\" ]; then kill -9 \$pids || true; fi" || true
ssh_ableton_with_retry "sleep 2" || true

ssh_ableton_with_retry "test -x /opt/move/Move" || fail "Missing /opt/move/Move"

# Restart via init service (starts MoveLauncher which starts Move with proper lifecycle)
ssh_root_with_retry "/etc/init.d/move start >/dev/null 2>&1" || fail "Failed to restart Move service"

# Wait for MoveOriginal to appear with shim loaded (retry up to 15 seconds)
shim_ok=false
for i in $(seq 1 15); do
    ssh_ableton_with_retry "sleep 1" || true
    # Verify both env and actual mapped shim (env alone can be present while loader ignores preload).
    if $ssh_root "pid=\$(pidof MoveOriginal 2>/dev/null | awk '{print \$1}'); test -n \"\$pid\" && tr '\\0' '\\n' < /proc/\$pid/environ | grep -q 'LD_PRELOAD=move-anything-shim.so' && grep -q 'move-anything-shim.so' /proc/\$pid/maps" 2>/dev/null; then
        shim_ok=true
        break
    fi
done
$shim_ok || fail "Move started without active shim mapping (LD_PRELOAD env/maps check failed)"

iecho ""
iecho "Installation complete!"

# Concise output in quiet mode (screen reader friendly)
if [ "$quiet_mode" = true ]; then
    iecho ""
    iecho "Screen reader enabled. Press Shift+Menu on Move to toggle on/off."
    iecho "Visit http://move.local for web interface."
else
    # Verbose output for visual users
    echo
    echo "Move Everything is now installed with the modular plugin system."
    echo "Modules are located in: /data/UserData/move-anything/modules/"
    echo

    # Show active features
    if [ "$disable_shadow_ui" = false ] || [ "$disable_standalone" = false ]; then
        echo "Active features:"
        if [ "$disable_shadow_ui" = false ]; then
            echo "  Shift+Vol+Track or Shift+Menu: Access slot configurations and Master FX"
        fi
        if [ "$disable_standalone" = false ]; then
            echo "  Shift+Vol+Knob8: Access standalone mode and module store"
        fi
        echo
    fi

    # Show screen reader shortcut based on shadow UI state
    if [ "$disable_shadow_ui" = true ]; then
        echo "Screen Reader:"
        echo "  Shift+Menu: Toggle screen reader on/off"
        echo
    else
        echo "Screen Reader:"
        echo "  Toggle via Shadow UI settings menu"
        echo
    fi

    echo "Logging commands:"
    echo "  Enable:  ssh ableton@move.local 'touch /data/UserData/move-anything/debug_log_on'"
    echo "  Disable: ssh ableton@move.local 'rm -f /data/UserData/move-anything/debug_log_on'"
    echo "  View:    ssh ableton@move.local 'tail -f /data/UserData/move-anything/debug.log'"
    echo
fi
