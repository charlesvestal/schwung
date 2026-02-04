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
      echo "    Retry $retry/$max_retries..."
      sleep 2
    fi
  done
  echo "    Failed to copy after $max_retries attempts"
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
ssh_ableton="ssh -o LogLevel=QUIET -o StrictHostKeyChecking=accept-new -n $username@$hostname"
scp_ableton="scp -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new"
ssh_root="ssh -o LogLevel=QUIET -o StrictHostKeyChecking=accept-new -n root@$hostname"

# Parse arguments
use_local=false
skip_modules=false
for arg in "$@"; do
  case "$arg" in
    local) use_local=true ;;
    -skip-modules|--skip-modules) skip_modules=true ;;
  esac
done

if [ "$use_local" = true ]; then
  SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
  REPO_ROOT="$(dirname "$SCRIPT_DIR")"
  local_file="$REPO_ROOT/$remote_filename"
  echo "Using local build: $local_file"
  if [ ! -f "$local_file" ]; then
    fail "Local build not found. Run ./scripts/build.sh first."
  fi
else
  url=https://github.com/charlesvestal/move-anything/releases/latest/download/
  echo "Downloading latest release from $url$remote_filename"
  curl -fLO "$url$remote_filename" || fail "Failed to download release. Check https://github.com/charlesvestal/move-anything/releases"
  local_file="$remote_filename"
fi
if command -v md5sum >/dev/null 2>&1; then
  echo "Build MD5: $(md5sum "$local_file")"
elif command -v md5 >/dev/null 2>&1; then
  echo "Build MD5: $(md5 -q "$local_file")"
fi

# Check SSH connection, run setup wizard if needed
echo "Checking SSH connection to $hostname..."
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

  # SSH failed for auth/key reasons - offer wizard
  echo
  echo "SSH connection failed."
  printf "Would you like help setting up SSH access? (y/N): "
  read -r run_wizard </dev/tty

  if [ "$run_wizard" = "y" ] || [ "$run_wizard" = "Y" ]; then
    ssh_wizard
    if ! ssh_ensure_connection; then
      fail "Could not establish SSH connection to Move"
    fi
  else
    echo
    echo "To set up SSH manually:"
    echo "  1. Generate a key: ssh-keygen -t ed25519"
    echo "  2. Add your public key at: http://move.local/development/ssh"
    echo "  3. Run this install script again"
    fail "SSH connection required for installation"
  fi
else
  echo "✓ SSH connection OK"
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
  printf "Proceed with migration? [Y/n] "
  read -r do_migrate </dev/tty

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
$ssh_root "if [ -f /etc/ld.so.preload ] && grep -q 'move-anything-shim.so' /etc/ld.so.preload; then ts=\$(date +%Y%m%d-%H%M%S); cp /etc/ld.so.preload /etc/ld.so.preload.bak-move-anything-\$ts; grep -v 'move-anything-shim.so' /etc/ld.so.preload > /tmp/ld.so.preload.new || true; if [ -s /tmp/ld.so.preload.new ]; then cat /tmp/ld.so.preload.new > /etc/ld.so.preload; else rm -f /etc/ld.so.preload; fi; rm -f /tmp/ld.so.preload.new; fi"

echo
echo "Stopping Move to install shim (your Move screen will go dark briefly)..."

# Use root to stop running Move processes cleanly, then force if needed.
$ssh_root "for name in MoveMessageDisplay MoveLauncher Move MoveOriginal move-anything shadow_ui; do pids=\$(pidof \$name 2>/dev/null || true); if [ -n \"\$pids\" ]; then kill \$pids || true; fi; done"
$ssh_root "sleep 0.5"
$ssh_root "for name in MoveMessageDisplay MoveLauncher Move MoveOriginal move-anything shadow_ui; do pids=\$(pidof \$name 2>/dev/null || true); if [ -n \"\$pids\" ]; then kill -9 \$pids || true; fi; done"
$ssh_root "sleep 0.2"
# Free the SPI device if anything still holds it (prevents \"communication error\")
$ssh_root "pids=\$(fuser /dev/ablspi0.0 2>/dev/null || true); if [ -n \"\$pids\" ]; then kill -9 \$pids || true; fi"

# Symlink shim to /usr/lib/ (root partition has no free space for copies)
$ssh_root "rm -f /usr/lib/move-anything-shim.so && ln -s /data/UserData/move-anything/move-anything-shim.so /usr/lib/move-anything-shim.so"
$ssh_root chmod u+s /data/UserData/move-anything/move-anything-shim.so

# Ensure the replacement Move script exists and is executable
$ssh_root chmod +x /data/UserData/move-anything/shim-entrypoint.sh

# Backup original only once, and only if current Move exists
# IMPORTANT: Use mv (not cp) on root partition — it's nearly full (~460MB, <25MB free).
# Never create extra copies of large files under /opt/move/ or anywhere on /.
if $ssh_root "test ! -f /opt/move/MoveOriginal"; then
  $ssh_root "test -f /opt/move/Move" || fail "Missing /opt/move/Move; refusing to proceed"
  $ssh_root mv /opt/move/Move /opt/move/MoveOriginal
  $ssh_ableton "cp /opt/move/MoveOriginal ~/"
fi

# Install the shimmed Move entrypoint
$ssh_root cp /data/UserData/move-anything/shim-entrypoint.sh /opt/move/Move


# Optional: Install modules from the Module Store (before restart so they're available immediately)
echo
install_mode=""
deleted_modules=$(echo "$deleted_modules" | xargs)  # trim whitespace

if [ "$skip_modules" = true ]; then
    echo "Skipping module installation (-skip-modules)"
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
            $scp_ableton "$asset" "$username@$hostname:./move-anything/" && \
            $ssh_ableton "cd move-anything && mkdir -p $dest && tar -xzf $asset -C $dest/ && rm $asset"
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
        # Expand ~ to home directory and handle escaped spaces
        rom_path=$(echo "$rom_path" | sed "s|^~|$HOME|" | sed 's/\\ / /g' | sed "s/^['\"]//;s/['\"]$//")
        if [ -d "$rom_path" ]; then
            rom_count=0
            $ssh_ableton "mkdir -p move-anything/modules/sound_generators/minijv/roms"
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
                $ssh_ableton "mkdir -p move-anything/modules/sound_generators/minijv/roms/expansions"
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
        # Expand ~ to home directory and handle escaped spaces
        sf2_path=$(echo "$sf2_path" | sed "s|^~|$HOME|" | sed 's/\\ / /g' | sed "s/^['\"]//;s/['\"]$//")
        if [ -d "$sf2_path" ]; then
            sf2_count=0
            $ssh_ableton "mkdir -p move-anything/modules/sound_generators/sf2/soundfonts"
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
        # Expand ~ to home directory and handle escaped spaces
        syx_path=$(echo "$syx_path" | sed "s|^~|$HOME|" | sed 's/\\ / /g' | sed "s/^['\"]//;s/['\"]$//")
        if [ -d "$syx_path" ]; then
            syx_count=0
            $ssh_ableton "mkdir -p move-anything/modules/sound_generators/dexed/banks"
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

echo
echo "Restarting Move binary with shim installed..."

# Stop Move via init service (kills MoveLauncher + Move + all children cleanly)
$ssh_root "/etc/init.d/move stop >/dev/null 2>&1 || true"
$ssh_root "for name in MoveOriginal Move MoveLauncher MoveMessageDisplay shadow_ui move-anything; do pids=\$(pidof \$name 2>/dev/null || true); if [ -n \"\$pids\" ]; then kill -9 \$pids 2>/dev/null || true; fi; done"
# Clean up stale shared memory so it's recreated with correct permissions
$ssh_root "rm -f /dev/shm/move-shadow-*"
# Free the SPI device if anything still holds it (prevents "communication error" on restart)
$ssh_root "pids=\$(fuser /dev/ablspi0.0 2>/dev/null || true); if [ -n \"\$pids\" ]; then kill -9 \$pids || true; fi"
$ssh_ableton "sleep 2"

$ssh_ableton "test -x /opt/move/Move" || fail "Missing /opt/move/Move"

# Restart via init service (starts MoveLauncher which starts Move with proper lifecycle)
$ssh_root "/etc/init.d/move start >/dev/null 2>&1"

# Wait for MoveOriginal to appear with shim loaded (retry up to 15 seconds)
shim_ok=false
for i in $(seq 1 15); do
    $ssh_ableton "sleep 1"
    if $ssh_root "pid=\$(pidof MoveOriginal 2>/dev/null | awk '{print \$1}'); test -n \"\$pid\" && tr '\\0' '\\n' < /proc/\$pid/environ | grep -q 'LD_PRELOAD=move-anything-shim.so'" 2>/dev/null; then
        shim_ok=true
        break
    fi
done
$shim_ok || fail "Move started without shim (LD_PRELOAD missing)"

echo
echo "Done!"
echo
echo "Move Anything is now installed with the modular plugin system."
echo "Modules are located in: /data/UserData/move-anything/modules/"
echo
echo "Shift+Vol+Track or Shift+Vol+Menu: Access Move Anything's slot configurations"
echo "Shift+Vol+Knob8: Access Move Anything's standalone mode and module store"
echo
echo "Logging commands:"
echo "  Enable:  ssh ableton@move.local 'touch /data/UserData/move-anything/debug_log_on'"
echo "  Disable: ssh ableton@move.local 'rm -f /data/UserData/move-anything/debug_log_on'"
echo "  View:    ssh ableton@move.local 'tail -f /data/UserData/move-anything/debug.log'"
echo
