#!/usr/bin/env bash
# uninstall.sh - Remove Move Anything from the Move and restore stock behavior.
#
# Usage:
#   ./uninstall.sh
#
# Requirements:
#   - SSH access to move.local for both ableton@ and root@
#   - The same prerequisites as install.sh (SSH keys, etc.)

set -euo pipefail

HOST=${MOVE_HOST:-move.local}
ABLETON_USER=${MOVE_USER:-ableton}
ROOT_USER=root
SSH="ssh -o LogLevel=QUIET"
SCP="scp -o LogLevel=QUIET"

log() { printf "[uninstall] %s\n" "$*"; }

confirm() {
    if [[ "${MOVE_FORCE_UNINSTALL:-}" == "1" ]]; then
        return
    fi
    read -r -p "This will remove Move Anything and restore stock firmware state. Continue? [y/N] " answer
    case "${answer}" in
        [Yy]*) ;;
        *)
            log "Aborted."
            exit 0
            ;;
    esac
}

check_connection() {
    if ! ${SSH} "${ABLETON_USER}@${HOST}" exit >/dev/null 2>&1; then
        log "Unable to reach ${HOST} as ${ABLETON_USER}. Ensure SSH is configured."
        exit 1
    fi
    if ! ${SSH} "${ROOT_USER}@${HOST}" exit >/dev/null 2>&1; then
        log "Unable to reach ${HOST} as ${ROOT_USER}. Ensure root SSH access works."
        exit 1
    fi
}

stop_processes() {
    log "Stopping Move processes..."
    ${SSH} "${ABLETON_USER}@${HOST}" "killall MoveLauncher Move MoveOriginal MoveMessageDisplay MoveSentryRunProcessor MoveControlModeHandler MoveFirmwareAutoUpdater >/dev/null 2>&1 || true"

    log "Waiting for processes to exit..."
    ${SSH} "${ABLETON_USER}@${HOST}" 'for attempt in {1..40}; do
        if ! pgrep MoveLauncher >/dev/null 2>&1 \
           && ! pgrep Move >/dev/null 2>&1 \
           && ! pgrep MoveOriginal >/dev/null 2>&1 \
           && ! pgrep MoveMessageDisplay >/dev/null 2>&1; then
            break
        fi
        sleep 0.25
    done'
}

restore_move_binary() {
    log "Restoring stock Move binary..."
    ${SSH} "${ROOT_USER}@${HOST}" 'if [ -f /opt/move/MoveOriginal ]; then
        rm -f /opt/move/Move
        mv /opt/move/MoveOriginal /opt/move/Move
    fi'
}

remove_shim() {
    log "Removing control surface shim..."
    ${SSH} "${ROOT_USER}@${HOST}" 'rm -f /usr/lib/control_surface_move_shim.so'
}

cleanup_installation_dirs() {
    log "Removing Move Anything directories..."
    ${SSH} "${ABLETON_USER}@${HOST}" 'rm -rf ~/control_surface_move ~/control_surface_move.tar.gz'
}

restore_user_library() {
    log "Restoring UserLibrary symlink..."
    ${SSH} "${ABLETON_USER}@${HOST}" 'if [ -L ~/UserLibrary ]; then
        rm ~/UserLibrary
    fi

    if [ -d ~/UserLibrary_base ]; then
        mv ~/UserLibrary_base ~/UserLibrary
    else
        mkdir -p ~/UserLibrary
    fi'
}

finalise() {
    log "Rebooting Move to apply changes..."
    ${SSH} "${ROOT_USER}@${HOST}" reboot
    log "Move is rebooting. Allow a minute for it to come back online."
}

main() {
    confirm
    check_connection
    stop_processes
    restore_move_binary
    remove_shim
    cleanup_installation_dirs
    restore_user_library
    finalise
}

main "$@"
