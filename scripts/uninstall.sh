#!/usr/bin/env bash
# Uninstall Move Anything from Move and restore stock firmware
set -euo pipefail

HOST=${MOVE_HOST:-move.local}
SSH="ssh -o LogLevel=QUIET -o ConnectTimeout=5"

log() { printf "[uninstall] %s\n" "$*"; }

# Retry wrapper for SSH commands (Windows mDNS can be flaky)
ssh_with_retry() {
    local user="$1"
    local cmd="$2"
    local max_retries=3
    local retry=0
    while [ $retry -lt $max_retries ]; do
        if ${SSH} "${user}@${HOST}" "$cmd" 2>/dev/null; then
            return 0
        fi
        retry=$((retry + 1))
        if [ $retry -lt $max_retries ]; then
            log "  Connection retry $retry/$max_retries..."
            sleep 2
        fi
    done
    log "  SSH command failed after $max_retries attempts"
    return 1
}

confirm() {
    [[ "${MOVE_FORCE_UNINSTALL:-}" == "1" ]] && return
    read -r -p "Remove Move Anything and restore stock firmware? [y/N] " answer
    [[ "$answer" =~ ^[Yy] ]] || { log "Aborted."; exit 0; }
}

main() {
    confirm

    log "Stopping processes..."
    ssh_with_retry "ableton" "killall move-anything MoveLauncher Move MoveOriginal 2>/dev/null || true" || true
    sleep 1

    log "Restoring stock Move binary..."
    ssh_with_retry "root" 'test -f /opt/move/MoveOriginal && mv /opt/move/MoveOriginal /opt/move/Move' || log "Warning: Could not restore stock binary (may already be restored)"

    log "Removing shim and files..."
    ssh_with_retry "root" 'rm -f /usr/lib/move-anything-shim.so' || true
    ssh_with_retry "root" 'rm -f /usr/lib/move-anything-web-shim.so' || true
    ssh_with_retry "ableton" 'rm -rf ~/move-anything ~/move-anything.tar.gz' || true

    log "Restoring MoveWebService..."
    ssh_with_retry "root" 'for svc in /opt/move/MoveWebServiceOriginal /opt/move-web-service/MoveWebServiceOriginal; do if [ -f "$svc" ]; then dir=$(dirname "$svc"); base=$(basename "$svc" Original); mv "$svc" "$dir/$base"; fi; done' || true

    log "Rebooting Move..."
    ssh_with_retry "root" "reboot" || true
    log "Done. Move will restart with stock firmware."
}

main "$@"
