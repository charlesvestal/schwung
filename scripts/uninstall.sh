#!/usr/bin/env bash
# Uninstall Schwung from Move and restore stock firmware
set -euo pipefail

HOST=${MOVE_HOST:-move.local}
SET_PAGE_BACKUP_ROOT="/data/UserData/UserLibrary/Schwung Backups/Set Pages"
purge_data=false

log() { printf "[uninstall] %s\n" "$*"; }

# Resolve an explicit identity file (-i) for $HOST so SSH does not rely solely on
# ~/.ssh/config inference, which fails silently for bare-IP hosts (e.g.
# MOVE_HOST=192.168.x.x) that have no matching "Host <ip>" block.
resolve_ssh_identity() {
    local key="" cand h
    if [ -f "$HOME/.ssh/config" ]; then
        for h in "$HOST" "move.local"; do
            key=$(awk -v want="$h" '
                BEGIN{found=0}
                /^[ \t]*[Hh]ost[ \t]/{
                    found=0
                    line=$0; sub(/^[ \t]*[Hh]ost[ \t]+/,"",line)
                    n=split(line, pats, /[ \t]+/)
                    for(i=1;i<=n;i++) if(pats[i]==want){found=1}
                    next
                }
                found && /^[ \t]*[Ii]dentity[Ff]ile[ \t]/{
                    sub(/^[ \t]*[Ii]dentity[Ff]ile[ \t]+/,""); gsub(/[ \t]*$/,"")
                    print; exit
                }' "$HOME/.ssh/config" | sed "s|~|$HOME|g")
            [ -n "$key" ] && break
        done
    fi
    if [ -z "$key" ]; then
        for cand in "$HOME/.ssh/move_key" "$HOME/.ssh/id_ed25519" "$HOME/.ssh/id_rsa" "$HOME/.ssh/id_ecdsa"; do
            [ -f "$cand" ] && { key="$cand"; break; }
        done
    fi
    # Only emit -i when the path has NO whitespace: ${SSH} is expanded unquoted,
    # so a space in the key path (Git Bash $HOME "/c/Users/First Last") would
    # split the argument and corrupt the command. Fall back to ssh's own config
    # handling in that case. The case-block also guarantees a 0 return so
    # `SSH_IDENTITY=$(...)` never aborts under `set -e`.
    case "$key" in
        '')           ;;  # no key found
        *[[:space:]]*|*[*?[]*) ;; # whitespace OR glob chars unsafe to inline
        *) [ -f "$key" ] && printf -- '-i %s' "$key" ;;
    esac
    return 0
}

SSH_IDENTITY=$(resolve_ssh_identity)
# LogLevel=ERROR (not QUIET): preserves SSH error messages so the preflight's
# stderr capture and ssh_with_retry's "Detail:" line are populated. QUIET would
# silence them and the user would see "Detail: no response" with no actual diagnosis.
SSH="ssh -o LogLevel=ERROR -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new ${SSH_IDENTITY}"

usage() {
    cat <<EOF
Usage: uninstall.sh [--purge-data]

Options:
  --purge-data   Permanently delete Schwung data instead of exporting set-page backups

Environment:
  MOVE_HOST              Override the device hostname (default: move.local).
                         Use this when mDNS doesn't resolve, e.g. MOVE_HOST=192.168.1.50.
  MOVE_FORCE_UNINSTALL=1 Skip all interactive prompts (for piped/CI use).
  MOVE_PURGE_DATA=1      Equivalent to --purge-data via env var.
EOF
}

# Retry wrapper for SSH commands (Windows mDNS can be flaky)
ssh_with_retry() {
    local user="$1"
    local cmd="$2"
    local max_retries=3
    local retry=0
    local out=""
    while [ $retry -lt $max_retries ]; do
        # Capture combined output so we can surface *why* it failed instead of
        # swallowing the error (the old code hid stderr with 2>/dev/null).
        if out=$(${SSH} "${user}@${HOST}" "$cmd" 2>&1); then
            return 0
        fi
        retry=$((retry + 1))
        if [ $retry -lt $max_retries ]; then
            log "  Connection retry $retry/$max_retries..."
            sleep 2
        fi
    done
    log "  SSH command failed after $max_retries attempts"
    [ -n "$out" ] && log "  Detail: $(printf '%s' "$out" | tr '\n' ' ' | cut -c1-200)"
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
    if [ ! -t 0 ]; then
        log "Aborted: stdin is not a terminal. For piped use (e.g. curl | bash), set MOVE_FORCE_UNINSTALL=1 to skip prompts."
        exit 1
    fi
    if $purge_data; then
        read -r -p "Remove Schwung, restore stock firmware, and permanently delete Schwung data? [y/N] " answer || answer=""
    else
        read -r -p "Remove Schwung and restore stock firmware? Inactive Set Pages will be backed up to ${SET_PAGE_BACKUP_ROOT}. [y/N] " answer || answer=""
    fi
    [[ "$answer" =~ ^[Yy] ]] || { log "Aborted."; exit 0; }
}

backup_set_pages() {
    $purge_data && return 0

    log "Exporting inactive Set Pages backup..."
    local backup_cmd='
backup_root="'"${SET_PAGE_BACKUP_ROOT}"'"
src="/data/UserData/schwung/set_pages"
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

preflight() {
    if [ -n "$SSH_IDENTITY" ]; then
        log "Target: $HOST (identity: ${SSH_IDENTITY#-i })"
    else
        log "Target: $HOST (no explicit key found; relying on ssh defaults)"
    fi
    local errf
    errf=$(mktemp)
    if ${SSH} -o BatchMode=yes "ableton@${HOST}" true 2>"$errf"; then
        rm -f "$errf"
        return 0
    fi
    local err
    err=$(tr '\n' ' ' < "$errf" | cut -c1-200)
    rm -f "$errf"
    log "Warning: cannot SSH to ableton@${HOST}."
    log "  Detail: ${err:-no response}"
    log "  - If 'move.local' does not resolve, re-run with the IP:  MOVE_HOST=<ip> bash uninstall.sh"
    log "  - Make sure your SSH key is authorized on the Move and present in THIS shell/filesystem."
    if [[ "${MOVE_FORCE_UNINSTALL:-}" == "1" ]]; then
        log "  (continuing anyway: MOVE_FORCE_UNINSTALL=1)"
        return 0
    fi
    if [ ! -t 0 ]; then
        log "  stdin is not a terminal — cannot prompt. Set MOVE_FORCE_UNINSTALL=1 to bypass, or run interactively."
        exit 1
    fi
    printf "Continue anyway? [y/N] "
    read -r ans || ans=""  # don't let EOF (no TTY) abort via set -e
    [[ "$ans" =~ ^[Yy] ]] || { log "Aborted."; exit 1; }
}

main() {
    parse_args "$@"
    confirm
    preflight

    log "Stopping processes..."
    ssh_with_retry "ableton" "killall schwung MoveLauncher Move MoveOriginal 2>/dev/null || true" || true
    sleep 1

    log "Restoring stock Move binary..."
    ssh_with_retry "root" 'test -f /opt/move/MoveOriginal && mv /opt/move/MoveOriginal /opt/move/Move' || log "Warning: Could not restore stock binary (may already be restored)"

    backup_set_pages

    log "Removing shim and files..."
    ssh_with_retry "root" 'rm -f /usr/lib/schwung-shim.so' || true
    ssh_with_retry "root" 'rm -f /usr/lib/schwung-web-shim.so' || true
    ssh_with_retry "ableton" 'rm -rf ~/schwung ~/schwung.tar.gz' || true
    ssh_with_retry "root" 'rm -f /data/UserData/move-anything' || true  # Remove backwards-compat symlink

    log "Restoring MoveWebService..."
    ssh_with_retry "root" 'for svc in /opt/move/MoveWebServiceOriginal /opt/move-web-service/MoveWebServiceOriginal; do if [ -f "$svc" ]; then dir=$(dirname "$svc"); base=$(basename "$svc" Original); rm -f "$dir/$base"; mv "$svc" "$dir/$base"; fi; done' || true

    log "Rebooting Move..."
    ssh_with_retry "root" "reboot" || true
    log "Done. Move will restart with stock firmware."
}

main "$@"
