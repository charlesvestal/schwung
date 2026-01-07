#!/usr/bin/env bash
# Uninstall Move Anything from Move and restore stock firmware
set -euo pipefail

HOST=${MOVE_HOST:-move.local}
SSH="ssh -o LogLevel=QUIET"

log() { printf "[uninstall] %s\n" "$*"; }

confirm() {
    [[ "${MOVE_FORCE_UNINSTALL:-}" == "1" ]] && return
    read -r -p "Remove Move Anything and restore stock firmware? [y/N] " answer
    [[ "$answer" =~ ^[Yy] ]] || { log "Aborted."; exit 0; }
}

main() {
    confirm
    
    log "Stopping processes..."
    ${SSH} "ableton@${HOST}" "killall move-anything MoveLauncher Move MoveOriginal 2>/dev/null || true"
    sleep 1
    
    log "Restoring stock Move binary..."
    ${SSH} "root@${HOST}" 'test -f /opt/move/MoveOriginal && mv /opt/move/MoveOriginal /opt/move/Move'
    
    log "Removing shim and files..."
    ${SSH} "root@${HOST}" 'rm -f /usr/lib/move-anything-shim.so'
    ${SSH} "ableton@${HOST}" 'rm -rf ~/move-anything ~/move-anything.tar.gz'
    
    log "Rebooting Move..."
    ${SSH} "root@${HOST}" reboot || true
    log "Done. Move will restart with stock firmware."
}

main "$@"
