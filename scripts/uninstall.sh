#!/usr/bin/env bash
# Uninstall Move Anything from Move and restore stock firmware
set -euo pipefail

HOST=${MOVE_HOST:-move.local}
SSH="ssh -o LogLevel=QUIET -o ConnectTimeout=5"
SET_PAGE_BACKUP_ROOT="/data/UserData/UserLibrary/Move Everything Backups/Set Pages"
purge_data=false

log() { printf "[uninstall] %s\n" "$*"; }

usage() {
    cat <<EOF
Usage: uninstall.sh [--purge-data]

Options:
  --purge-data   Permanently delete Move Anything data instead of exporting set-page backups
EOF
}

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

parse_args() {
    if [[ "${MOVE_PURGE_DATA:-}" == "1" ]]; then
        purge_data=true
    fi

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --purge-data)
                purge_data=true
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                echo "Unknown option: $1" >&2
                usage >&2
                exit 1
                ;;
        esac
        shift
    done
}

confirm() {
    [[ "${MOVE_FORCE_UNINSTALL:-}" == "1" ]] && return
    if $purge_data; then
        read -r -p "Remove Move Anything, restore stock firmware, and permanently delete Move Anything data? [y/N] " answer
    else
        read -r -p "Remove Move Anything and restore stock firmware? Inactive Set Pages will be backed up to ${SET_PAGE_BACKUP_ROOT}. [y/N] " answer
    fi
    [[ "$answer" =~ ^[Yy] ]] || { log "Aborted."; exit 0; }
}

backup_set_pages() {
    $purge_data && return 0

    log "Exporting inactive Set Pages backup..."
    local backup_cmd='
backup_root="'"${SET_PAGE_BACKUP_ROOT}"'"
src="/data/UserData/move-anything/set_pages"
if [ ! -d "$src" ]; then
    exit 0
fi
ts=$(date +%Y%m%d-%H%M%S)
dest="$backup_root/uninstall-$ts"
mkdir -p "$dest"
cp -a "$src/." "$dest/"
'
    ssh_with_retry "ableton" "$backup_cmd" || log "Warning: Failed to export inactive Set Pages backup"
}

main() {
    parse_args "$@"
    confirm

    log "Stopping processes..."
    ssh_with_retry "ableton" "killall move-anything MoveLauncher Move MoveOriginal 2>/dev/null || true" || true
    sleep 1

    log "Restoring stock Move binary..."
    ssh_with_retry "root" 'test -f /opt/move/MoveOriginal && mv /opt/move/MoveOriginal /opt/move/Move' || log "Warning: Could not restore stock binary (may already be restored)"

    backup_set_pages

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
