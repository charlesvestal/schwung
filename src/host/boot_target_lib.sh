#!/bin/sh
# boot_target_lib.sh — boot-selector decision logic: target resolution plus a
# two-strike boot watchdog. Sourced by shim-entrypoint.sh (the boot selector
# that replaces /opt/move/Move) and by tests/host/test_boot_*.sh.
#
# BusyBox-sh compatible: no arrays, no [[ ]], no top-level `local`. Every path
# is derived from BOOT_TARGETS_DIR so a test can point this at a fixture
# directory. Sourcing this file must have NO side effects — it only defines
# functions and one variable default.
#
# Registry shape:
#   $BOOT_TARGETS_DIR/default              bare target id, one line
#   $BOOT_TARGETS_DIR/<id>/boot.json       flat JSON, one field per line
#   $BOOT_TARGETS_DIR/<id>/healthy         opt-in "boot was good" touch-file
#   $BOOT_TARGETS_DIR/.boot-attempt        watchdog stamp: "<id> <count>"
#   "stock" is a built-in id with no directory ever.

BOOT_TARGETS_DIR="${BOOT_TARGETS_DIR:-/data/UserData/boot-targets}"

# bt_json_field FILE KEY — echo the value of the first `"KEY" : "value"`
# occurrence in FILE. rc 1 (nothing echoed) if the file is missing or the key
# is not found. awk single-pass with an explicit exit on match: a
# `sed -n '...p' | head -n 1` pipeline SIGPIPEs its producer under
# `set -o pipefail`, which this library must never assume its caller lacks.
bt_json_field() {
    [ -f "$1" ] || return 1
    awk -v key="$2" '
        {
            idx = index($0, "\"" key "\"")
            if (idx == 0) next
            rest = substr($0, idx)
            if (match(rest, "^\"" key "\"[ \t]*:[ \t]*\"[^\"]*\"") == 0) next
            val = rest
            sub("^\"" key "\"[ \t]*:[ \t]*\"", "", val)
            sub("\".*$", "", val)
            print val
            found = 1
            exit
        }
        END { if (!found) exit 1 }
    ' "$1"
}

# bt_is_registered ID — true iff ID has a boot.json under the registry.
bt_is_registered() {
    [ -f "$BOOT_TARGETS_DIR/$1/boot.json" ]
}

# bt_resolve_default — echo the target id to boot:
#   the id named in the "default" file, if it is "stock" or registered;
#   else "schwung", if "schwung" is registered;
#   else "stock".
bt_resolve_default() {
    default_file="$BOOT_TARGETS_DIR/default"
    if [ -f "$default_file" ]; then
        want=$(head -n 1 "$default_file")
        if [ "$want" = "stock" ] || bt_is_registered "$want"; then
            echo "$want"
            return 0
        fi
    fi
    if bt_is_registered "schwung"; then
        echo "schwung"
        return 0
    fi
    echo "stock"
}

# bt_exec_path ID — echo the "exec" field from ID/boot.json. rc 1 (nothing
# echoed) for the built-in "stock" id, a missing boot.json, or a boot.json
# with no "exec" field.
bt_exec_path() {
    [ "$1" = "stock" ] && return 1
    bt_json_field "$BOOT_TARGETS_DIR/$1/boot.json" "exec"
}

# bt_stamp_file — echo the path to the watchdog's boot-attempt stamp.
bt_stamp_file() {
    echo "$BOOT_TARGETS_DIR/.boot-attempt"
}

# bt_watchdog_clear — remove the boot-attempt stamp (a clean boot resets it).
bt_watchdog_clear() {
    rm -f "$(bt_stamp_file)"
}

# bt_watchdog_enter ID — record one more boot attempt at ID; echo the new
# strike count. The built-in "stock" target is never watched: echoes 0 and
# writes nothing. A ID/healthy marker from a previous successful boot clears
# the strike count before this attempt is counted. A stamp naming a DIFFERENT
# id resets the count to 0 (switching targets is not a strike against the new
# one).
bt_watchdog_enter() {
    id="$1"
    if [ "$id" = "stock" ]; then
        echo 0
        return 0
    fi

    healthy_file="$BOOT_TARGETS_DIR/$id/healthy"
    if [ -f "$healthy_file" ]; then
        rm -f "$healthy_file"
        bt_watchdog_clear
    fi

    stamp="$(bt_stamp_file)"
    count=0
    if [ -f "$stamp" ]; then
        # `read` avoids `set -- $(cat ...)`, which is subject to pathname
        # expansion and word splitting on the stamp's content (a corrupted
        # or torn-write stamp must never glob-expand against files in the
        # caller's cwd). The trailing `_` swallows any extra fields; a read
        # failure (empty file) must not error under `set -e` callers, hence
        # the `|| { ... }` fallback that still leaves both vars defined.
        IFS=' ' read -r prev_id prev_count _ < "$stamp" 2>/dev/null || {
            prev_id=""
            prev_count=""
        }
        # A non-numeric or empty count must never reach arithmetic: dash and
        # BusyBox ash abort the whole sourcing shell on `$((...))` with a
        # non-numeric operand.
        case "$prev_count" in
            ''|*[!0-9]*) prev_count=0 ;;
        esac
        if [ "$prev_id" = "$id" ] && [ "$prev_count" -gt 0 ]; then
            count="$prev_count"
        fi
    fi
    count=$((count + 1))
    echo "$id $count" > "$stamp"
    echo "$count"
}

# bt_watchdog_forced ID — rc 0 iff the stamp exists, names ID, and its count
# is >= 2 (two failed attempts at ID with no intervening healthy boot).
bt_watchdog_forced() {
    id="$1"
    stamp="$(bt_stamp_file)"
    [ -f "$stamp" ] || return 1
    # Same `set --` / glob-expansion hazard as bt_watchdog_enter: read
    # instead of splitting via `set --`.
    IFS=' ' read -r want_id want_count _ < "$stamp" 2>/dev/null || {
        want_id=""
        want_count=""
    }
    [ "$want_id" = "$id" ] || return 1
    # Validate before arithmetic comparison (belt and braces even though the
    # 2>/dev/null on `[ -ge ]` already guards a non-numeric operand from
    # aborting the shell).
    case "$want_count" in
        ''|*[!0-9]*) want_count=0 ;;
    esac
    [ "$want_count" -ge 2 ] 2>/dev/null
}
