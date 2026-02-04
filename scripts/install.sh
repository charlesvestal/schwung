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

    # Parse catalog with python3 and install each module to correct subdirectory
    echo "$catalog" | python3 -c "
import json, sys
catalog = json.loads(sys.stdin.read())
for m in catalog['modules']:
    ctype = m.get('component_type', '')
    if ctype == 'sound_generator':
        subdir = 'sound_generators'
    elif ctype == 'audio_fx':
        subdir = 'audio_fx'
    elif ctype == 'midi_fx':
        subdir = 'midi_fx'
    elif ctype == 'utility':
        subdir = 'utilities'
    elif ctype == 'overtake':
        subdir = 'overtake'
    else:
        subdir = 'other'
    print(m['id'] + '|' + m['github_repo'] + '|' + m['asset_name'] + '|' + m['name'] + '|' + subdir)
" | while IFS='|' read -r id repo asset name subdir; do
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
    echo
    echo "NOTE: Some modules require or benefit from additional assets:"
    echo "  - Dexed: Optional additional .syx patch banks"
    echo "  - Mini-JV: REQUIRES ROM files"
    echo "  - SF2: REQUIRES SoundFont files (.sf2)"
    echo "Modules are also available in the Module Store."
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
